#pragma once
#include <matter/contracts/graphics.h>
#include <matter/contracts/terrain.h>
#include <memory>

namespace matter::terrain_coordinator {
namespace data = matter::contracts;
namespace services = matter::services;
using ProductOwner = std::shared_ptr<const services::TerrainProduct>;

struct CacheKey { data::ContentDigest content; };
struct CacheEntry {
  services::TerrainRequest request;
  ProductOwner product;
};
struct CacheHit { std::shared_ptr<const CacheEntry> entry; };
using CacheResult = services::JobResult<CacheHit>;
struct Acknowledgement {};
using Status = services::JobResult<Acknowledgement>;
// Synchronous application-lane services. Store is atomic and retains immutable
// ownership. A miss is successful with a null entry. No implicit GPU cache.
class Cache {
public:
  virtual ~Cache() = default;
  virtual CacheResult lookup(const services::JobEnvelope &, const CacheKey &) = 0;
  virtual Status store(const services::JobEnvelope &, const CacheKey &,
                       std::shared_ptr<const CacheEntry>) = 0;
};
// Namespaced canonical SHA-256, including the implementation bundle, field,
// resolved dependencies, region/domain/detail/boundaries, capabilities and
// evaluation limits. Only the request ID and generation epoch are excluded.
data::ValidationResult cache_key(const services::TerrainRequest &,
                                const data::ContentDigest &implementation, CacheKey &);

// Bounded FIFO cache; byte capacity reserves each entry's declared input,
// metadata and output limits (not an allocator/RSS measurement). Oversized
// entries fail without evicting anything. Consumers' owners survive eviction.
class MemoryCache final : public Cache {
public:
  MemoryCache(size_t max_entries, uint64_t max_reserved_bytes);
  ~MemoryCache() override;
  CacheResult lookup(const services::JobEnvelope &, const CacheKey &) override;
  Status store(const services::JobEnvelope &, const CacheKey &,
               std::shared_ptr<const CacheEntry>) override;
  void clear();
  size_t size() const;
  uint64_t reserved_bytes() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct Limits {
  size_t regions = 128, queued = 32, running = 4, uploads = 8, observations = 128;
  uint64_t input_bytes_per_region = 64ull * 1024 * 1024;
  uint64_t output_bytes_per_region = 64ull * 1024 * 1024;
  uint64_t metadata_bytes_per_region = 64ull * 1024 * 1024;
  uint64_t working_bytes_per_job = 64ull * 1024 * 1024;
};
struct Options {
  data::ContentDigest implementation; // Required; includes generator/algorithm versions.
  std::vector<data::Capability> generation_capabilities;
  Limits limits;
};
struct Demand {
  bool cpu = false, collision = false, graphics = false;
};
struct GenerationWork {
  std::shared_ptr<const services::TerrainRequest> request;
  services::JobControl control;
};
// Run on the host's chosen worker. No Coordinator pointer/callback is retained.
// Every taken work item must reach complete(), even if cancelled before running.
services::TerrainResult execute(const GenerationWork &, services::TerrainService &);
struct Statistics { size_t regions = 0, queued = 0, running = 0, uploads = 0; };

// All methods, cache calls and graphics calls are confined to ONE application
// lane and must not re-enter the coordinator. Only execute() runs off-lane.
// Injected services outlive this object. Cancelled jobs keep their running credit
// until complete(); edits/retries can never create an unbounded abandoned queue.
// Scene placement is a separate consumer of gpu(): this adapter owns upload
// resources, not scene instances. GraphicsObservation.instance names a region.
class Coordinator {
public:
  Coordinator(Options, Cache * = nullptr, services::GraphicsService * = nullptr);
  ~Coordinator();
  Coordinator(const Coordinator &) = delete;
  Coordinator &operator=(const Coordinator &) = delete;

  // Replaces a definition only after validation/admission capacity checks.
  // The caller supplies source_revision; this coordinator replaces request_id
  // and generation_epoch with unique values on every edit/retry/recreation.
  data::ValidationResult define(services::TerrainRequest);
  data::ValidationResult demand(const data::SemanticId &, Demand);
  data::ValidationResult retry(const data::SemanticId &);
  data::ValidationResult evict(const data::SemanticId &); // Entire region.
  data::ValidationResult evict_gpu(const data::SemanticId &); // Clears graphics demand only.
  data::ValidationResult drop_cpu(const data::SemanticId &); // Clears CPU demand only.
  // Bounded batch, validated before applying. Does not invoke a generator.
  // A representation absent from the frozen request fails explicitly; a detail
  // or algorithm change must arrive as a new definition from world policy.
  data::ValidationResult observe(const std::vector<services::GraphicsObservation> &);
  data::ValidationResult observe_graphics();

  // Cache hits are admitted here; at most one owned job is returned per call.
  // A successful empty result means no work or running-credit backpressure.
  data::ValidationResult take(std::optional<GenerationWork> &);
  // Work is the receipt for the running credit; even a malformed backend
  // result returns that credit. Duplicate/foreign receipts never consume one.
  data::ValidationResult complete(const GenerationWork &, const services::TerrainResult &);
  // Starts bounded uploads and polls each pending ticket once per call.
  data::ValidationResult pump_graphics();

  std::shared_ptr<const services::TerrainRequest> definition(const data::SemanticId &) const;
  ProductOwner cpu(const data::SemanticId &) const;
  // Collision may refer to mesh/extension resources, so its independent lease
  // retains the complete immutable product. Dropping CPU/GPU does not clear it.
  ProductOwner collision(const data::SemanticId &) const;
  std::optional<services::GraphicsHandle> gpu(const data::SemanticId &) const;
  Statistics statistics() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace matter::terrain_coordinator
