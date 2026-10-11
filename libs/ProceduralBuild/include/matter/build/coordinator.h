#pragma once
#include <matter/contracts/hosts.h>
#include <mutex>

namespace matter::build {
// A graph node is a prototype request, not an instance placement. The same
// prototype may be bound many times; authored instance IDs remain untouched.
struct ChildBinding {
  contracts::SemanticId node;
  contracts::SemanticId asset; // Name exposed in the parent's resolved inputs.
};
struct NodeRequest {
  contracts::SemanticId id;
  services::BuildRequest build;
  std::vector<ChildBinding> children;
};
struct GraphRequest {
  std::vector<NodeRequest> nodes;
  std::vector<contracts::SemanticId> roots; // Order and repeated placements retained.
  services::ResourceBudget budget; // Aggregate coordinator budget, in addition to per-node budgets.
};
struct NodeProduct {
  contracts::SemanticId id;
  services::CacheKey key;
  std::shared_ptr<const services::AssetProduct> asset;
};
struct GraphProduct {
  std::vector<NodeProduct> nodes; // Reachable nodes, children first; aliases share owners.
  std::vector<NodeProduct> roots; // Same order as GraphRequest::roots.
  uint64_t stages = 0, generated = 0, cache_hits = 0, deduplicated = 0;
};
using GraphResult = services::JobResult<GraphProduct>;

struct CoordinatorOptions {
  // Required content identity of the authoring SDK/host and geometry/processing
  // implementation bundle. Change it when service semantics change. For QuickJS
  // include quickjs_sdk_content() in this bundle's identity.
  contracts::ContentDigest implementation;
  uint32_t max_nodes = 1024, max_source_modules = 1024;
  uint64_t max_source_bytes = 8ull * 1024 * 1024, max_stages = 8192;
  // Explicit host-provided imports, e.g. "matter:authoring". Their bytes/version
  // must be covered by implementation. No ambient filesystem/import fallback.
  std::vector<contracts::SemanticId> host_modules;
};

// Implements the resolver/baker separation of the legacy part_graph without
// linking the engine. The caller freezes the prototype graph before invocation;
// resolver services supply frozen source-import graphs for each prototype.
// Services/options outlive the coordinator. All builds are thread-confined;
// activate/invalidate/published may be called from a different thread. There is
// exactly one synchronous service job in flight (also during graph traversal).
// A caller can schedule independent coordinators on its own bounded executor.
class Coordinator final : public services::BuildService {
public:
  Coordinator(services::ResolverService &, services::AuthoringHost &,
              services::GeometryService &, CoordinatorOptions,
              services::CacheService *cache = nullptr);
  // Every activation supersedes even an identical envelope. It cancels the
  // previous run; invalid input leaves the existing activation unchanged.
  contracts::ValidationResult activate(const services::JobEnvelope &);
  void invalidate();
  std::shared_ptr<const GraphProduct> published() const;
  std::vector<contracts::Capability> capabilities() const override;
  services::AssetResult build(const services::JobEnvelope &, const services::BuildRequest &,
                              const services::JobControl &) override;
  GraphResult build_graph(const services::JobEnvelope &, const GraphRequest &,
                           const services::JobControl & = {});
private:
  struct Activation;
  services::ResolverService &resolver_;
  services::AuthoringHost &host_;
  services::GeometryService &geometry_;
  CoordinatorOptions options_;
  services::CacheService *cache_;
  mutable std::mutex mutex_;
  std::shared_ptr<Activation> active_;
  std::shared_ptr<const GraphProduct> published_;
};

// Deliberately distinct from portable SHA-256 keys; only these frozen legacy
// namespaces are accepted. This does not reinterpret a legacy hash as SHA-256.
contracts::ValidationResult legacy_cache_key(const contracts::LegacyHash &, services::CacheKey &);
} // namespace matter::build
