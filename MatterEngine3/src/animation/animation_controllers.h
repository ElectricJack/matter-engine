#pragma once

// MatterEngine3/src/animation/animation_controllers.h
//
// The native-controller extension point of the animation runtime.
//
// A "native controller" is a C++ node in a compiled animation graph
// (`RuntimeGraphNodeKind::NativeController`) that writes IK target transforms
// during the fixed tick. The one shipped implementation is the procedural
// gait controller in `animation_controllers.cpp`, selected by the authored
// controller type string `"proceduralGait"` at
// `animation_runtime_asset.cpp: compile_controller`.
//
// How it fits
// -----------
// - `animation_runtime_asset.cpp` builds a `NativeControllerDescriptor` at
//   asset-decode time, validates it by constructing the controller once
//   through `NativeControllerRegistry::with_v1_controllers()`, and stores the
//   descriptor plus its target indices on the runtime binding descriptor.
// - `animation_systems` owns the live instances and drives `fixed_update`
//   once per fixed tick, then applies `NativeControllerContext::writes` to
//   the target set. The evaluator itself treats a NativeController graph node
//   as a pass-through of its single dependency.
// - The only world access a controller gets is the raycast interface in
//   `animation_world_queries.h`. Deliberately no Flecs world, no renderer, no
//   asset store.
//
// Conventions and gotchas
// -----------------------
// - `NativeControllerTypeId` is a stable 8-byte ASCII tag serialized into ANIM
//   metadata. Never derive it from RTTI or an address; an unrecognised id must
//   fail the load rather than silently fall back.
// - Controller parameter blobs are packed little-endian POD copied with
//   `memcpy`, so field order and padding are part of the on-disk format.
// - Every entry point returns `bool`/null on failure and is expected to leave
//   controller state untouched when it fails; the gait controller solves into
//   a scratch copy for exactly this reason.
// - Nothing here is thread-safe. A controller instance belongs to the fixed
//   tick that drives it.

#include "animation/animation_targets.h"
#include "animation/animation_world_queries.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace matter::animation {

// Stable strings are serialized in ANIM metadata. They must not be RTTI names
// or addresses; an unknown id is a load failure rather than a fallback.
using NativeControllerTypeId = uint64_t;
constexpr NativeControllerTypeId kGaitControllerTypeId = 0x474149545f563100ull; // "GAIT_V1\0"

// The serialized identity of one controller node: which implementation, its
// opaque packed parameter blob, and the cadence it runs at. Built at asset
// decode time and thereafter immutable. `NativeControllerRegistry::create`
// rejects any cadence other than `Fixed` or `Frame`.
struct NativeControllerDescriptor {
    NativeControllerTypeId type = 0;
    std::vector<uint8_t> parameters;
    EvaluationCadence cadence = EvaluationCadence::Fixed;
};
// Declared mutable-state footprint of one controller instance, in bytes. The
// factory fills it during `create` so the caller can charge the instance
// against the budget before committing to it; it is a declaration, not an
// allocation the caller performs.
struct NativeControllerLayout { size_t fixed_state_bytes = 0; size_t frame_state_bytes = 0; };
// One IK target write produced by a controller. `target_index` indexes the
// binding's canonical target list (`UINT16_MAX` = unset), `transform` is in
// the target solver's space, and `weight` is a normalized 0-1 blend factor.
struct ControllerTargetWrite { uint16_t target_index = UINT16_MAX; AnimationTransform transform{}; float weight = 1.0f; };

// A narrow, value-only host contract. Controllers receive neither Flecs world
// nor renderer objects; the one permitted world dependency is the fixed query
// interface defined by animation_world_queries.h.
struct NativeControllerContext {
    double fixed_delta_seconds = 0.0;
    const AnimationWorldQueries* world_queries = nullptr;
    // Controller rest/predicted coordinates are rig-relative.  Runtime
    // supplies the post-authority entity world pose at the fixed boundary so
    // native controllers can issue world queries without seeing Flecs.
    Mat4f entity_world{};
    bool has_entity_world = false;
    std::vector<AnimationValue> inputs;
    std::vector<ControllerTargetWrite> writes;
};

// Interface every native controller implements.
//
// Instances are owned by the caller through `std::unique_ptr` and created only
// via `NativeControllerRegistry::create`. Call order per instance is:
// `fixed_update` once per fixed tick, with `checkpoint`/`restore` used around
// an editor play/stop transaction.
//
// Contract for implementers:
// - `fixed_update` returns false to mean "this tick produced nothing usable".
//   It must then leave both its own state and the context's `writes` as it
//   found them, so the caller can drop the tick without corruption.
// - `checkpoint`/`restore` round-trip only durable state. `restore` must
//   reject a blob whose size or contents do not match (it returns false), and
//   must not partially apply one.
// - `layout()` and `type()` are constant for the lifetime of the instance.
class NativeController {
public:
    virtual ~NativeController() = default;
    virtual NativeControllerTypeId type() const noexcept = 0;
    virtual NativeControllerLayout layout() const noexcept = 0;
    virtual bool fixed_update(NativeControllerContext&) = 0;
    virtual bool checkpoint(std::vector<uint8_t>&) const = 0;
    virtual bool restore(const std::vector<uint8_t>&) = 0;
};

// Type-id -> factory table. Copyable and cheap; hold one per runtime that
// needs to instantiate controllers.
//
// `register_factory` rejects id 0 and null factories, and will not overwrite
// an existing registration (it returns false). `create` returns null for an
// unregistered id, an unsupported cadence, or a parameter blob the factory
// rejects -- an unknown controller is a load failure, never a silent
// no-op fallback.
class NativeControllerRegistry {
public:
    using Factory = std::unique_ptr<NativeController>(*)(const uint8_t*, size_t, NativeControllerLayout&);
    bool register_factory(NativeControllerTypeId, Factory);
    std::unique_ptr<NativeController> create(const NativeControllerDescriptor&, NativeControllerLayout&) const;
    static NativeControllerRegistry with_v1_controllers();
private:
    std::map<NativeControllerTypeId, Factory> factories_;
};

// Packed little-endian v1 blob. Values are explicitly finite/limited at the
// registry boundary before state allocation.
struct GaitControllerParameters {
    uint16_t left_target = UINT16_MAX;
    uint16_t right_target = UINT16_MAX;
    Float3 left_predicted{};
    Float3 right_predicted{};
    // Units for the block below. `left_predicted`/`right_predicted` above are
    // rig-relative rest positions of the two chain end effectors, taken from
    // the skeleton rest model pose at compile time.
    // - stride_seconds: seconds for one full gait cycle; must be > 0.
    // - swing_height: metres the foot arcs above its predicted path.
    // - ray_distance: metres of downward ray from the probe origin; must be > 0.
    // - step_height: metres above the predicted point the probe starts, and
    //   the maximum vertical deviation a hit may have to count as walkable.
    // - min_ground_normal_y: minimum world-space normal Y (-1..1) for a
    //   surface to be plantable; 0.5 is roughly a 60 degree slope limit.
    float stride_seconds = 1.0f;
    float swing_height = 0.1f;
    float ray_distance = 2.0f;
    float step_height = 0.25f;
    float min_ground_normal_y = 0.5f;
};
// Factory for `kGaitControllerTypeId`. Returns null unless the blob is exactly
// `sizeof(GaitControllerParameters)` and every field passes the finiteness and
// range checks; on success it also fills the out layout. Exposed by name so
// the runtime-asset decoder can probe-construct a controller during
// validation without hard-coding the registry.
std::unique_ptr<NativeController> create_gait_controller(const uint8_t*, size_t, NativeControllerLayout&);

} // namespace matter::animation
