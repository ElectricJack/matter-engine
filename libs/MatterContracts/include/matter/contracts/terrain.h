#pragma once
#include <matter/contracts/generation.h>

namespace matter::services {
enum class BoundaryFace : uint32_t { negative_x, positive_x, negative_y, positive_y,
                                    negative_z, positive_z };
struct TerrainBoundaryEnvelope {
  BoundaryFace face = BoundaryFace::positive_x;
  std::array<int32_t, 3> neighbor_key{};
  float sample_spacing_metres = 1, neighbor_spacing_metres = 1;
  data::Capability policy; // Algorithm-specific seam/transition semantics.
  std::optional<data::SemanticId> seam_resource;
};
struct TerrainRequest {
  JobEnvelope job;
  data::SourceIdentity field; // Frozen field program identity, not a runtime pointer.
  std::vector<data::AssetDependency> dependencies;
  ResolvedInputs resolved;
  data::SemanticId region;
  std::array<int32_t, 3> region_key{};
  std::array<double, 3> origin_metres{};
  data::Bounds sample_domain; // Local metres; high precision origin stays separate.
  std::array<uint32_t, 3> cell_counts{{1, 1, 1}};
  uint32_t detail_level = 0;
  std::vector<data::Capability> required_capabilities;
  std::vector<TerrainBoundaryEnvelope> boundaries; // Explicit requested neighbors.
  EvaluationOptions options;
};
// Distinct from data::TerrainProduct (the existing serialized asset extension).
// This service product carries the job identity and separately owned CPU data;
// it never contains resident sectors, collision solvers or graphics resources.
struct TerrainProduct {
  JobEnvelope job;
  data::ContentDigest field_content;
  data::SemanticId region;
  std::array<int32_t, 3> region_key{};
  std::array<double, 3> origin_metres{};
  std::array<uint32_t, 3> cell_counts{{1, 1, 1}};
  uint32_t detail_level = 0;
  data::Bounds bounds;
  AssetOwner geometry;
  ResolvedInputs resources; // Includes optional field/collision/seam payload bytes.
  std::vector<TerrainBoundaryEnvelope> boundaries;
  std::vector<data::ExtensionEnvelope> products;
};
using TerrainResult = JobResult<TerrainProduct>;
data::ValidationResult validate(const TerrainRequest &, const std::vector<data::Capability> &);
data::ValidationResult validate(const TerrainProduct &, const TerrainRequest &,
                                const std::vector<data::Capability> &);
// Checks product and result envelopes before replacing a consumer's owner.
data::ValidationResult admit_terrain(const TerrainResult &, const TerrainRequest &,
                                     const std::vector<data::Capability> &,
                                     std::shared_ptr<const TerrainProduct> &);
class TerrainService {
public:
  virtual ~TerrainService() = default;
  virtual std::vector<data::Capability> capabilities() const = 0;
  virtual TerrainResult generate(const TerrainRequest &, const JobControl &) = 0;
};
} // namespace matter::services
