#pragma once

// MatterEngine3/include/matter/query.h
//
// Result types for `WorldSession`'s read-only query and reflection API (see the
// "Query API" block in matter/world_session.h). Every struct here is a plain
// POD out-parameter — nothing owns memory, nothing is a handle you release.
//
// The session methods that fill them:
//   RayHit            WorldSession::raycast
//   InstanceInfo      WorldSession::instance_info / instance_info_by_hash
//   PartBounds        WorldSession::part_bounds
//   PartLodLevelInfo  WorldSession::part_lod_level_info
//   PartChildSummary  WorldSession::part_child_summary
//   PickIdentity      WorldSession::pick_at_pixel
//
// GOTCHAS.
//  * `raycast` is backed by a LAZILY BUILT CPU BVH: the first call after a bake
//    pays the build cost, so it is not a per-frame-cheap call the way the name
//    suggests.
//  * Every `const char* module_name` here is borrowed from engine-side storage
//    and is only valid until the next bake or world reload. Copy it if you need
//    to keep it.
//  * The part-addressed reflection calls key on `part_hash`, not on an instance,
//    and report nothing when the part has no loaded CPU-side representation
//    (not baked yet, or released) — which the callers use to tell "no data yet"
//    apart from "genuinely zero".

#include <cstdint>

namespace matter {

// One CPU raycast hit against the baked static world.
struct RayHit {
    float t = -1.0f;               // distance along the ray; -1 = no hit
    float normal[3] = {0, 0, 0};   // world-space, faces the ray origin
    uint32_t instance = 0;         // index usable with instance_info()
    uint64_t part_hash = 0;        // which part asset was hit
    int material_id = -1;          // -1 = none / unassigned
};

// One placement of a part in the baked world. `instance` in a RayHit indexes the
// same space as `WorldSession::instance_count()`, and that index is only stable
// until the next bake.
struct InstanceInfo {
    float transform[16];           // row-major world placement
    uint64_t part_hash = 0;
    const char* module_name = nullptr;  // may be null; valid until next bake/reload
};

// Axis-aligned bounds of a part asset, addressed by `part_hash` — a property of
// the part itself, not of any one placement of it. Uninitialized by default;
// only read it when `WorldSession::part_bounds` returned true.
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

// GPU identity-buffer pick result.
// `kind` selects WHICH of the two id fields is meaningful: `part_hash` for a
// baked static instance, `entity_id` for a dynamic ECS entity. `None` is the
// normal "the cursor was over background" answer, not an error.
enum class PickKind : uint8_t { None, StaticInstance, DynamicEntity };

struct PickIdentity {
    PickKind kind = PickKind::None;
    uint64_t part_hash = 0;   // valid when kind == StaticInstance
    uint64_t entity_id = 0;   // valid when kind == DynamicEntity
};

} // namespace matter
