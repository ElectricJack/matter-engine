#pragma once
#include <matter/contracts/terrain.h>

namespace matter::terrain {
namespace data = matter::contracts;
namespace services = matter::services;

// CPU is the portable C++ implementation. GPU lowering and authoring callbacks
// are separate backends and fail explicitly here. WASM build qualification is
// separate from sharing this implementation.
enum class FieldTarget : uint32_t { cpu, gpu, script_callback };
struct EncodedField {
  data::Capability encoding{"matter.terrain.field.text", 1};
  std::string text; // Exact canonical legacy tape bytes, SHA-256 in request.field.
  FieldTarget target = FieldTarget::cpu;
};

namespace detail { struct RegionState; }
class FrozenRegion;
using FreezeResult = services::JobResult<FrozenRegion>;
// Accepted TerrainRequest envelope capabilities; this is a field stage, not
// an implementation of TerrainService::generate or a promise of mesh seams.
std::vector<data::Capability> field_capabilities();
FreezeResult freeze_region(const services::TerrainRequest &, const EncodedField &,
                           const services::JobControl & = {});

// Only the factory constructs this value. The request, program and dependencies
// are copied; no mutable input alias, WorldBinding, or legacy runtime escapes.
// Copies retain immutable owned storage. Const sampling may run concurrently.
class FrozenRegion {
public:
  FrozenRegion(const FrozenRegion &) = default;
  FrozenRegion &operator=(const FrozenRegion &) = default;
  const services::TerrainRequest &request() const;
  bool is_heightfield() const;
  uint64_t legacy_text_hash() const; // FNV identity, never relabeled as SHA-256.
private:
  explicit FrozenRegion(std::shared_ptr<const detail::RegionState> state);
  std::shared_ptr<const detail::RegionState> state_;
  friend FreezeResult freeze_region(const services::TerrainRequest &, const EncodedField &,
                                    const services::JobControl &);
  friend struct FieldAccess;
};

// Local metres <-> double world metres; origin is never baked into a vertex.
// Output is unchanged on invalid/nonfinite/overflowing conversion.
data::ValidationResult local_to_world(const std::array<double, 3> &origin,
                                     const std::array<double, 3> &local,
                                     std::array<double, 3> &world);
data::ValidationResult world_to_local(const std::array<double, 3> &origin,
                                     const std::array<double, 3> &world,
                                     std::array<double, 3> &local);
// Closed lattice: indices [0, cell_counts], exact endpoints, both neighbors
// include their common face. Interior is interpolated in double, then converted
// once to the legacy float32 world-evaluation frame with checked precision.
data::ValidationResult lattice_position(const FrozenRegion &, const std::array<uint32_t, 3> &,
                                       std::array<double, 3> &local);
struct FieldSample {
  float height_metres = 0; // WORLD altitude, not region-local y.
  float density = 0;      // Positive solid; implicit heightfield = height - world y.
  float moisture = 0, relief = 0;
};
using PointResult = services::JobResult<FieldSample>;
PointResult sample_local(const FrozenRegion &, const std::array<double, 3> &local,
                         const services::JobControl & = {});
struct RegionSamples {
  FrozenRegion input; // Retains the immutable request/program/dependencies.
  // x fastest, then y, then z: x + (nx+1) * (y + (ny+1) * z).
  std::vector<FieldSample> samples;
};
using SamplesResult = services::JobResult<RegionSamples>;
SamplesResult sample_region(const FrozenRegion &, const services::JobControl & = {});
} // namespace matter::terrain
