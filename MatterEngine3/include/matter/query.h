#pragma once
#include <cstdint>

namespace matter {

struct RayHit {
    float t = -1.0f;
    float normal[3] = {0, 0, 0};   // world-space, faces the ray origin
    uint32_t instance = 0;         // index usable with instance_info()
    uint64_t part_hash = 0;
    int material_id = -1;
};

struct InstanceInfo {
    float transform[16];           // row-major world placement
    uint64_t part_hash = 0;
    const char* module_name = nullptr;  // may be null; valid until next bake/reload
};

struct PartBounds {
    float aabb_min[3];
    float aabb_max[3];
};

// Bake Lab W4 (part-workbench.md SS-I.5): LOD Inspector grid data. Read-only
// PartStore reflection — mirrors InstanceInfo/PartBounds above, no bake or
// render side effects. See WorldSession::part_lod_level_info /
// part_child_summary.
struct PartLodLevelInfo {
    float    threshold = 0.0f;        // this level's screen-size selection threshold
    uint32_t triangle_count = 0;      // this level's own mesh triangle count
};

// One distinct child part referenced by a part's baked children table,
// aggregated by hash: repeated placements of the same module collapse into
// one entry with instance_count > 1 (e.g. "Leaf x340").
struct PartChildSummary {
    uint64_t    child_hash = 0;
    const char* module_name = nullptr;  // may be null; valid until next bake/reload
    uint32_t    instance_count = 0;
};

} // namespace matter
