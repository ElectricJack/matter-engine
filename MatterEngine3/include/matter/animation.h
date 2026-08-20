#pragma once

// MatterEngine3/include/matter/animation.h
//
// The public face of the animation runtime. Everything an outside caller (the
// ECS phase, the editor, tests) needs to own animators lives here: opaque
// handles, the recoverable-failure status codes, the runtime stats block, and
// `AnimationService` itself.
//
// HOW IT FITS
//
// The implementation is `animation::AnimationServiceImpl` in
// `MatterEngine3/src/animation/animation_store.cpp`; the rest of the animation
// subsystem (`anim_asset`, `animation_evaluator`, `animation_systems`,
// `ozz_adapter`, ...) sits under `MatterEngine3/src/animation/`. Every private
// type this header mentions — `AnimAsset`, `AnimationRuntimeDefinition`,
// `AnimationRuntimeBindingDescriptor`, `AnimatorCheckpoint`,
// `AnimationSystems` — is only FORWARD-DECLARED in namespace
// `matter::animation`. That is deliberate: it keeps ozz, the animation IR and
// the evaluator out of every translation unit that merely wants to drive an
// animator, so this header can be included from the editor. Do not include a
// private animation header from here.
//
// LIFECYCLE / TYPICAL CALL SEQUENCE
//
//   AnimationService service{config};        // 0 in a config field means
//                                            // "use the runtime default"
//   const AnimAsset* asset = service.insert_asset(std::move(loaded));
//   Animator animator = service.create(asset, definition);
//   if (!animator.valid()) { ... }           // budget/validation failure
//   auto speed = service.input(animator.instance, "speed");
//   service.set(speed, 4.0f);                // staged, not applied yet
//   ...                                      // sample_*_controls() applies it
//   service.remove(animator.instance);
//   service.release_asset(asset);            // only after the last instance
//
// The ECS runtime binds a service to the runtime bridge with
// `attach_runtime_systems()` and detaches it by passing `nullptr` before
// teardown (`MatterEngine3/src/ecs/ecs_runtime.cpp`); once attached,
// `AnimationSystems` calls `sample_fixed_controls()` from the fixed-tick phase
// and `sample_frame_controls()` from the per-frame phase
// (`MatterEngine3/src/animation/animation_systems.cpp`).
//
// GOTCHAS
//
//   - API writes through `set*()` are STAGED. They do not take effect until
//     the input's authored cadence samples them; a value written and read back
//     in the same statement need not agree with what the evaluator sees.
//   - Failure is reported in-band, never by exception: `create()` /
//     `replace_asset()` hand back an `Animator` whose `status` and
//     `bind_pose_fallback` describe what went wrong, and every mutator returns
//     `bool`. Budget failures are RECOVERABLE — the owner keeps its static
//     part visible in bind pose and may retry later.
//   - `AnimationService` is move-only (the copy operations are deleted) and
//     the destructor is out-of-line because `AnimationServiceImpl` is
//     incomplete here.
//   - A handle's `valid()` is a shape check on the handle's own fields. It
//     does not consult the service, so it cannot tell you the referenced
//     animator still exists.

#include "matter/animation_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace matter {

// Which phase samples a staged control write.
//   Fixed   — picked up by AnimationService::sample_fixed_controls(), driven
//             from the fixed simulation tick.
//   Frame   — picked up by sample_frame_controls(), once per rendered frame.
//   Invalid — the default-constructed sentinel (0xff). A zero-initialized
//             handle therefore fails valid() rather than silently reading as
//             a Fixed control.
enum class AnimationCadence : uint8_t { Fixed, Frame, Invalid = 0xff };

// A generation-qualified reference to one authored control input on one
// animator, obtained from AnimationService::input(name).
//
//   slot_index    — runtime slot; UINT32_MAX = unset.
//   generation    — bumped on slot reuse so a stale handle cannot be mistaken
//                   for a live one.
//   schema_index  — index into the asset's authored input schema;
//                   UINT32_MAX = unset.
//   value_type    — the authored type; the matching set() overload must be
//                   used or the write is rejected.
//   cadence       — which sample_*_controls() phase applies writes to it.
//
// valid() is a pure shape check: it confirms the four sentinels are filled in
// and that value_type is in range. It does NOT ask the service whether the
// animator is still alive, so treat a `false` from a set*() call as the real
// liveness answer.
struct AnimationInputHandle {
    uint32_t slot_index = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t schema_index = UINT32_MAX;
    AnimationValueType value_type = static_cast<AnimationValueType>(0xff);
    AnimationCadence cadence = AnimationCadence::Invalid;
    bool valid() const {
        return slot_index != UINT32_MAX && schema_index != UINT32_MAX &&
               static_cast<uint32_t>(value_type) <= static_cast<uint32_t>(AnimationValueType::Symbol) &&
               (cadence == AnimationCadence::Fixed || cadence == AnimationCadence::Frame);
    }
};

// A generation-qualified reference to one authored TARGET (an IK goal or
// similar driven transform) on one animator, from AnimationService::target().
// Same field meanings as AnimationInputHandle, with one extra invariant:
// valid() additionally requires `value_type == AnimationValueType::Transform`,
// because a target is always a transform. Targets are driven through
// set_enabled/set_weight/set_transform/snap rather than the set() overloads.
struct AnimationTargetHandle {
    uint32_t slot_index = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t schema_index = UINT32_MAX;
    AnimationValueType value_type = static_cast<AnimationValueType>(0xff);
    AnimationCadence cadence = AnimationCadence::Invalid;
    bool valid() const {
        return slot_index != UINT32_MAX && schema_index != UINT32_MAX &&
               value_type == AnimationValueType::Transform &&
               (cadence == AnimationCadence::Fixed || cadence == AnimationCadence::Frame);
    }
};

// A generation-qualified reference to a whole animator instance — the handle
// carried on `Animator::instance` and passed to every per-animator call.
//
// NOTE THE INVERTED VALIDITY RULE. Unlike the two handles above, an instance
// handle names no slot INSIDE an animator, so valid() requires that
// `schema_index`, `value_type` and `cadence` all still hold their unset
// sentinels (UINT32_MAX / 0xff / Invalid). Filling any of them in makes the
// handle invalid rather than more specific; only `slot_index` and
// `generation` carry meaning here.
struct AnimatorInstanceHandle {
    uint32_t slot_index = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t schema_index = UINT32_MAX;
    AnimationValueType value_type = static_cast<AnimationValueType>(0xff);
    AnimationCadence cadence = AnimationCadence::Invalid;
    bool valid() const {
        return slot_index != UINT32_MAX && schema_index == UINT32_MAX &&
               static_cast<uint32_t>(value_type) == 0xffu && cadence == AnimationCadence::Invalid;
    }
};

// Why an Animator is not usable. Only `Ok` makes Animator::valid() true.
//   BudgetExceeded — a runtime cap (instances, mutable bytes, joints, graph
//                    or controller nodes) was hit. Recoverable: see
//                    Animator::bind_pose_fallback.
//   InvalidHandle  — the instance handle did not resolve.
//   LoadFailed     — the asset/definition could not be turned into a runtime
//                    animator.
enum class AnimationStatus : uint8_t { Ok, BudgetExceeded, InvalidHandle, LoadFailed };

// The result of create()/replace_asset(): an instance handle plus why it may
// not be usable. Failure is reported here, not thrown — always test valid()
// (which requires both a well-formed handle and status == Ok) before using
// `instance`.
struct Animator {
    AnimatorInstanceHandle instance{};
    AnimationStatus status = AnimationStatus::Ok;
    // Allocation caps are recoverable: the owner keeps its static .part visible
    // in bind pose and may retry allocation later.
    bool bind_pose_fallback = false;
    bool valid() const { return instance.valid() && status == AnimationStatus::Ok; }
};

// Root motion the animator wants applied to its owner. `valid == false` means
// the animator produced none this step; `delta` is then meaningless, not
// identity-by-contract, so gate on the flag.
struct DesiredRootMotion { AnimationTransform delta{}; bool valid = false; };
// One authored marker firing on one animator. `marker_index` indexes the
// asset's authored marker list; UINT32_MAX = unset. `time` is the clip time
// at which it fired.
struct AnimationMarkerEvent { AnimatorInstanceHandle instance{}; uint32_t marker_index = UINT32_MAX; float time = 0.0f; };

// Stable public reason codes for every recoverable animation degradation.  Do
// not expose the runtime evaluator or renderer types through diagnostics.
enum class AnimationRuntimeFallbackReason : uint8_t {
    AssetLimit,
    RuntimeInstanceLimit,
    EvaluationBudget,
    SkinWorkBudget,
    SkinVertexBudget,
    InvalidSkinSubmission,
    InvalidEvaluationRequest,
    EvaluationFailure,
    Count
};

// A by-value snapshot of runtime occupancy and evaluation work, returned by
// AnimationService::stats(). Two kinds of member are mixed here:
//
//   - instantaneous: `active_*`, `*_capacity`, `max_*`, `mutable_bytes`,
//     `mutable_budget_bytes` — what the store holds right now.
//   - counters (the uint64 `*_count` members) — evaluation and degradation
//     tallies.
//
// `fallback_counts` is indexed by AnimationRuntimeFallbackReason and sized by
// its `Count` sentinel, so adding a reason automatically widens the array;
// keep the enum's `Count` last.
struct AnimationRuntimeStats {
    uint32_t active_instances = 0;
    uint32_t active_assets = 0;
    uint32_t instance_capacity = 0;
    // Bytes of mutable (per-instance) animation state currently held, and the
    // ceiling it is measured against. Exceeding the ceiling degrades new
    // animators to bind pose rather than failing the world.
    size_t mutable_bytes = 0;
    size_t mutable_budget_bytes = 0;
    uint32_t asset_capacity = 0;
    uint32_t max_joints_per_asset = 0;
    uint32_t max_graph_nodes = 0;
    uint32_t max_controller_nodes = 0;
    uint64_t evaluated_pose_count = 0;
    uint64_t evaluated_joint_count = 0;
    uint64_t evaluated_presentation_pose_count = 0;
    uint64_t frozen_pose_count = 0;
    uint64_t resampled_pose_count = 0;
    uint64_t world_query_count = 0;
    uint64_t world_query_overflow_count = 0;
    uint64_t submitted_skin_work_items = 0;
    uint64_t submitted_skinned_vertices = 0;
    uint64_t last_complete_fallback_count = 0;
    uint64_t bind_pose_fallback_count = 0;
    uint64_t fallback_count = 0;
    std::array<uint64_t, static_cast<size_t>(AnimationRuntimeFallbackReason::Count)> fallback_counts{};
};
// Zero means "use the centrally defined runtime default". This keeps the
// public API free of internal budget types and prevents defaults/hard caps
// from being copied into two headers.
struct AnimationStoreConfig {
    uint32_t instance_capacity = 0;
    size_t mutable_budget_bytes = 0;
    uint32_t asset_capacity = 0;
    uint32_t max_joints_per_asset = 0;
    uint32_t max_graph_nodes = 0;
    uint32_t max_controller_nodes = 0;
};

namespace animation {
struct AnimAsset;
struct AnimationRuntimeDefinition;
struct AnimationRuntimeBindingDescriptor;
struct AnimatorCheckpoint;
class AnimationServiceImpl;
class AnimationSystems;
}

// A read-only, generation-qualified service snapshot used only by the
// internal runtime bridge.  It deliberately carries values, not Slot pointers:
// a service redefinition or destroy cannot leave the ECS phase with a dangling
// reference into AnimationServiceImpl.
struct AnimationRuntimeBindingLease {
    AnimatorInstanceHandle instance{};
    uint64_t asset_identity = 0;
    uint64_t descriptor_identity = 0;
    std::shared_ptr<const animation::AnimationRuntimeBindingDescriptor> descriptor;
    struct Value {
        AnimationValueType type = AnimationValueType::Number;
        bool boolean = false;
        double number = 0.0;
        Float3 float3{};
        Quaternion quaternion{};
        AnimationTransform transform{};
        uint32_t symbol = 0;
    };
    std::vector<Value> fixed_previous;
    std::vector<Value> fixed_current;
    std::vector<Value> frame_controls;
    std::vector<AnimationTransform> target_transforms;
    std::vector<float> target_weights;
    std::vector<uint8_t> target_enabled;
    std::vector<uint8_t> target_snap_requested;
    bool valid() const { return instance.valid() && descriptor != nullptr; }
};

// The animation runtime's owning handle: immutable assets, live animator
// instances, their staged control values, and the bridge to the ECS runtime.
//
// OWNERSHIP. Pimpl over `animation::AnimationServiceImpl`; move-only (copy is
// deleted) and the destructor is defined out of line because the impl type is
// incomplete in this header. Assets handed to insert_asset() are owned by the
// service from that point and stay put — the returned pointer is the asset
// IDENTITY that create()/replace_asset()/release_asset() take — until
// release_asset() accepts them back.
//
// CALL ORDER. insert_asset -> create -> (input/target + set*) -> remove ->
// release_asset. release_asset() refuses while any live instance still
// references the asset, so removing every animator first is a precondition,
// not a courtesy.
//
// STAGED WRITES. The set*() family records values; nothing observes them
// until the matching cadence phase runs sample_fixed_controls() /
// sample_frame_controls(). Those two, plus attach_runtime_systems(), are the
// runtime's own hooks — they are not part of the gameplay-facing surface and
// gameplay code should not call them.
//
// BUDGETS. Construct with an AnimationStoreConfig whose zero fields mean
// "runtime default" (see the note on that struct). When a cap is hit, create()
// returns an Animator with status BudgetExceeded and, where applicable,
// bind_pose_fallback set, and stats() records the reason in `fallback_counts`.
class AnimationService {
public:
    explicit AnimationService(AnimationStoreConfig config = {});
    ~AnimationService();
    AnimationService(AnimationService&&) noexcept;
    AnimationService& operator=(AnimationService&&) noexcept;
    AnimationService(const AnimationService&) = delete;
    AnimationService& operator=(const AnimationService&) = delete;

    // Takes ownership of an immutable asset and returns the pointer that
    // henceforth IS that asset's identity to this service — pass it back to
    // create()/replace_asset()/release_asset(). The argument is consumed by
    // value; move into it rather than copying a large asset.
    const animation::AnimAsset* insert_asset(animation::AnimAsset asset);
    // Releases immutable asset/schema ownership after its last animator has
    // been removed. Returns false while any live instance still references it.
    bool release_asset(const animation::AnimAsset* asset);
    // Instantiate one animator over `asset` using `definition` (the authored
    // graph/controller/binding description). Never throws: a cap or a
    // malformed definition comes back as a non-Ok Animator::status, so always
    // check Animator::valid(). `replace_asset` swaps both the asset and the
    // definition under an existing instance, keeping its slot.
    Animator create(const animation::AnimAsset* asset, const animation::AnimationRuntimeDefinition& definition);
    Animator replace_asset(AnimatorInstanceHandle instance, const animation::AnimAsset* asset,
                          const animation::AnimationRuntimeDefinition& definition);
    bool remove(AnimatorInstanceHandle instance);

    // Resolve an authored control/target by name. An unknown name (or a dead
    // instance) yields a default-constructed handle whose valid() is false —
    // that is a normal outcome, not an error to be logged per frame; cache the
    // handle rather than resolving by name every tick.
    //
    // The set*() family below returns false when the handle does not resolve
    // or its value_type does not match the overload. Writes are STAGED: they
    // are not visible to the evaluator until the handle's cadence is sampled.
    AnimationInputHandle input(AnimatorInstanceHandle, std::string_view name) const;
    AnimationTargetHandle target(AnimatorInstanceHandle, std::string_view name) const;
    bool set(AnimationInputHandle, bool);
    bool set(AnimationInputHandle, float);
    bool set(AnimationInputHandle, const Float3&);
    bool set(AnimationInputHandle, const Quaternion&);
    bool set(AnimationInputHandle, const AnimationTransform&);
    bool set_enabled(AnimationTargetHandle, bool);
    bool set_weight(AnimationTargetHandle, float);
    bool set_transform(AnimationTargetHandle, const AnimationTransform&);
    bool snap(AnimationTargetHandle);

    // Internal runtime bridge.  Definitions with no runtime descriptor remain
    // intentionally unbound for compatibility; malformed descriptors fail at
    // create/replace rather than becoming partially active.
    bool runtime_binding(AnimatorInstanceHandle, AnimationRuntimeBindingLease&) const;
    // Runtime phase hooks.  API writes are staged until their authored
    // cadence samples them; these methods are intentionally not part of the
    // gameplay-facing control surface.
    bool sample_fixed_controls();
    bool sample_frame_controls();
    void attach_runtime_systems(animation::AnimationSystems* systems);

    // Editor play/stop seam.  These operate only on descriptor-bound runtime
    // animators and are transactional: a rejected restore leaves both the
    // service controls and its attached runtime bridge untouched.
    bool capture_runtime_checkpoints(std::vector<animation::AnimatorCheckpoint>& out) const;
    bool validate_runtime_checkpoints(const std::vector<animation::AnimatorCheckpoint>& checkpoints) const;
    bool restore_runtime_checkpoints(const std::vector<animation::AnimatorCheckpoint>& checkpoints);

    // Observation only — none of these mutate the service. stats() builds a
    // fresh AnimationRuntimeStats by value on each call.
    AnimationStatus status(AnimatorInstanceHandle) const;
    AnimationRuntimeStats stats() const;
    size_t mutable_bytes() const;
    float number_value(AnimationInputHandle) const;
    bool bool_value(AnimationInputHandle) const;

private:
    std::unique_ptr<animation::AnimationServiceImpl> impl_;
};

} // namespace matter
