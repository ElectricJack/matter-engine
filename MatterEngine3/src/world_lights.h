#pragma once
// MatterEngine3/src/world_lights.h
//
// The RUNTIME form of a world's lighting and its renderer-neutral spatial
// publication. The exact authoring, packing and attenuation contracts are in
// docs/local-lighting.md.
//
// Authoring goes through the world definition, not through this header. A world
// declares `lights.sun` / `lights.sky` / `lights.points` / `lights.spots` in JS;
// src/script/world_definition_loader.cpp parses those into `WorldLight`
// (matter/world_definition.h), which is the AUTHORED form — cone half-angles in
// DEGREES plus a separate `intensity`. The provider converts each of those into
// the LocalLight below, which is the resolved form the renderer consumes: cone
// angles pre-converted to cosines so the shader compares against dot products
// directly.
//
// Who touches it:
//   - src/provider/local_provider.* builds and holds the authored + runtime sets
//   - src/provider/world_source.h carries a WorldLights per world source
//   - src/resolve_cache.cpp serializes WorldLights field-for-field into the
//     resolve cache (so any field added here needs a matching read/write pair
//     and a format-version bump there)
//   - the Vulkan renderer reads sun_dir/sun_color/sky_color per frame
//
// A default-constructed WorldLights is the valid "world authored no local
// lights" state. The publication owns CPU vectors and no OS/GPU resources; it
// is copied by value wherever it goes.
//
// Sun-direction convention is shared repo-wide; see include/matter/sun_angles.h,
// which lists this struct among the stores that hold the same vector.
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace world_lights {

enum class LocalLightKind : std::uint32_t {
    Point = 0u,
    Spot = 1u,
};

enum LocalLightFlags : std::uint32_t {
    kLocalLightCastsShadow = 1u << 0,
};

// One resolved GPU-compatible local light. This is four 16-byte lanes under
// std430/StructuredBuffer rules; do not reorder or add implicit fields. `color`
// is scene-linear RGB radiant intensity after authored intensity is folded in.
// Spot direction points FROM the light toward the cone target. Point direction
// is zero. source_radius/range/position are world metres.
struct alignas(16) LocalLight {
    float position[3];
    float range;
    float direction[3];
    float cos_outer;
    float color[3];
    float source_radius;
    float cos_inner;
    std::uint32_t kind;
    std::uint32_t flags;
    std::uint32_t reserved;
};

static_assert(sizeof(LocalLight) == 64, "LocalLight GPU stride must remain 64 bytes");
static_assert(alignof(LocalLight) == 16, "LocalLight GPU alignment must remain 16 bytes");
static_assert(std::is_standard_layout<LocalLight>::value,
              "LocalLight must remain a standard-layout GPU record");
static_assert(std::is_trivially_copyable<LocalLight>::value,
              "LocalLight must remain trivially uploadable");
static_assert(offsetof(LocalLight, range) == 12, "LocalLight lane 0 changed");
static_assert(offsetof(LocalLight, direction) == 16, "LocalLight lane 1 changed");
static_assert(offsetof(LocalLight, cos_outer) == 28, "LocalLight lane 1 changed");
static_assert(offsetof(LocalLight, color) == 32, "LocalLight lane 2 changed");
static_assert(offsetof(LocalLight, source_radius) == 44, "LocalLight lane 2 changed");
static_assert(offsetof(LocalLight, cos_inner) == 48, "LocalLight lane 3 changed");
static_assert(offsetof(LocalLight, kind) == 52, "LocalLight lane 3 changed");
static_assert(offsetof(LocalLight, flags) == 56, "LocalLight lane 3 changed");
static_assert(offsetof(LocalLight, reserved) == 60, "LocalLight lane 3 changed");

// One open-addressed hash bucket uploaded as two 16-byte lanes. count==0 is the
// empty sentinel. Occupied buckets reference contiguous light_indices.
struct alignas(16) LocalLightCell {
    std::int32_t cell[3];
    std::uint32_t offset;
    std::uint32_t count;
    std::uint32_t reserved[3];
};

static_assert(sizeof(LocalLightCell) == 32,
              "LocalLightCell GPU stride must remain 32 bytes");
static_assert(alignof(LocalLightCell) == 16,
              "LocalLightCell GPU alignment must remain 16 bytes");
static_assert(std::is_standard_layout<LocalLightCell>::value,
              "LocalLightCell must remain a standard-layout GPU bucket");
static_assert(std::is_trivially_copyable<LocalLightCell>::value,
              "LocalLightCell must remain trivially uploadable");
static_assert(offsetof(LocalLightCell, offset) == 12,
              "LocalLightCell lane 0 changed");
static_assert(offsetof(LocalLightCell, count) == 16,
              "LocalLightCell lane 1 changed");

struct LocalLightIndexConfig {
    float cell_size = 8.0f;
    std::uint32_t max_cells_per_light = 4096u;
};

struct LocalLightIndexStats {
    std::uint32_t occupied_cell_count = 0;
    std::uint32_t bucket_count = 0;
    std::uint64_t list_entry_count = 0;
    std::uint32_t oversized_light_count = 0;
    std::uint32_t max_candidates_per_cell = 0;
    std::uint64_t gpu_index_bytes = 0;
};

struct LocalLightSpatialIndex {
    float cell_size = 8.0f;
    std::uint32_t max_cells_per_light = 4096u;
    std::vector<LocalLightCell> cells;
    std::vector<std::uint32_t> light_indices;
    std::vector<std::uint32_t> oversized_light_indices;
    LocalLightIndexStats stats{};
};

// Immutable-at-publication renderer input. `revision` is a deterministic
// content fingerprint over records and index configuration. A renderer must
// replace all local-light buffers, including with an empty publication, when it
// differs from the currently accepted revision.
struct LocalLightPublication {
    std::vector<LocalLight> records;
    LocalLightSpatialIndex index;
    std::uint64_t revision = 0;
};

// A whole world's lighting environment: one directional sun, a flat sky ambient,
// and any number of local lights. The resolve cache stores the records and
// reconstructs the derived index/revision on load.
struct WorldLights {
    // Defaults reproduce the Phase-1 hardcoded raster look exactly, so worlds
    // without `light` lines render unchanged.
    float sun_dir[3]   = {-0.45f, -0.80f, -0.35f};  // normalized; FROM sun toward scene
    float sun_color[3] = {2.2f, 2.05f, 1.8f};
    float sky_color[3] = {0.38f, 0.43f, 0.52f};
    LocalLightPublication local;
};

// Fixed hash shared with the follow-up GLSL implementation. `cells.size()` is
// always zero or a power of two, so shaders use hash & (bucket_count - 1).
std::uint32_t local_light_cell_hash(std::int32_t x,
                                    std::int32_t y,
                                    std::int32_t z) noexcept;

// Transactionally rebuild the sparse index and revision for publication.records.
// On false, publication is unchanged and error describes invalid data or an
// allocation/size failure; no partial/truncated index is published.
bool rebuild_local_light_publication(
    LocalLightPublication& publication,
    const LocalLightIndexConfig& config,
    std::string& error);

inline bool rebuild_local_light_publication(LocalLightPublication& publication,
                                            std::string& error) {
    return rebuild_local_light_publication(
        publication, LocalLightIndexConfig{}, error);
}

// CPU reference lookup for arbitrary world positions, including off-screen RT
// hit positions. Returns the cell candidates followed by every oversized light;
// there is no fixed candidate cap and no silent truncation.
bool query_local_light_candidates(const LocalLightSpatialIndex& index,
                                  const float position[3],
                                  std::vector<std::uint32_t>& candidates,
                                  std::string& error);

// CPU reference shared by tests and the follow-up raster/RT shader work.
float local_light_attenuation(const LocalLight& light,
                              const float receiver_position[3]) noexcept;
void evaluate_local_light_irradiance(const LocalLight& light,
                                     const float receiver_position[3],
                                     float irradiance_rgb[3]) noexcept;

} // namespace world_lights
