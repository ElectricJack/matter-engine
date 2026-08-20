#pragma once

// MatterEngine3/src/animation/animation_systems.h
//
// The ECS-resident half of the animation runtime. `AnimationService`
// (`animation_store.h`) owns animator identity, schemas and control state;
// `AnimationSystems` owns everything that has to happen on a clock - graph
// evaluation, root motion, marker emission, world queries, controllers,
// target smoothing and IK, pose-LOD, and publication of poses to the
// renderer.
//
// Lifecycle. `MatterEngine3/src/ecs/ecs_runtime.cpp` constructs one
// `AnimationSystems`, calls `register_animation_systems(world, systems)` to
// install the phase systems, and calls
// `AnimationService::attach_runtime_systems` to connect the two. The service
// then pushes one `AnimationRuntimeBindingLease` per animator into
// `refresh_service_binding`, and withdraws it with `detach_service_binding`.
// Leases are value copies, so this object never dereferences service-owned
// storage.
//
// Phase order per fixed step, then once per frame (mirrored by
// `AnimationScheduleEvent`, which is what the trace records):
//   FixedPreUpdate  - advance the tick, sample fixed API writes
//   FixedUpdate     - evaluate the graph, emit markers, publish root motion
//   PrePhysics      - apply root motion to the root entity's LocalTransform
//   Physics / PostPhysicsHierarchy - trace-only placeholders here
//   FixedPostUpdate - controllers, world queries, target smoothing + fixed IK,
//                     then preserve the solved fixed pose as an interpolation
//                     endpoint
//   FrameUpdate     - sample frame API writes, pose-LOD admission, presentation
//                     evaluation, frame IK, publish pose snapshots
//
// Ownership and thread affinity: single-threaded, driven entirely by the ECS
// pipeline. Nothing here takes a lock, and none of the maps are safe to touch
// from a render or worker thread. The renderer's read path is
// `pose_snapshots()` plus `publish_presentation_for_render`, which is called
// on the same thread.
//
// Every per-animator map in this file is keyed by
// `(slot_index << 32) | generation`, so a recycled slot never collides with
// the animator it replaced.

#include "animation/animation_evaluator.h"
#include "animation/animation_controllers.h"
#include "animation/animation_world_queries.h"
#include "matter/animation_debug.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace flecs { class world; }

namespace matter::animation {

// The per-fixed-tick world-query cap the runtime enforces, which is always
// `AnimationBudgetConfig::max_world_queries_per_fixed_tick` -- derived from
// that default rather than restating the number, because the two used to be
// independent literals that could drift apart silently. No engine source
// reads this name; it exists so the animation test suites can assert the cap
// without hard-coding it.
constexpr uint32_t kMaxAnimationWorldQueries =
    AnimationBudgetConfig{}.max_world_queries_per_fixed_tick;

// One ray a controller wants cast during the fixed step. `origin` and
// `direction` are world space, `max_distance` is metres, and `mask` is the
// caller's collision filter. Requests are ordered deterministically by
// (`priority` ascending, animator slot, `controller_order`) before execution,
// so a tick that overflows the query budget drops the same rays every time.
struct AnimationWorldQueryRequest {
    AnimatorInstanceHandle instance{};
    uint16_t controller_order = 0;
    int32_t priority = 0;
    Float3 origin{};
    Float3 direction{};
    float max_distance = 0.0f;
    uint64_t mask = 0;
};

// Result parallel to the request that produced it: results are returned in
// *request* order even though execution is ordered by priority. `hit == false`
// covers all three of "no geometry", "no world-query provider attached" and
// "dropped because the tick's query budget was exhausted", so it is not by
// itself an error signal.
struct AnimationWorldQueryResult {
    AnimatorInstanceHandle instance{};
    uint16_t controller_order = 0;
    bool hit = false;
    WorldRayHit value{};
};

// Visibility is produced by a completed render submission and consumed by the
// following FrameUpdate. It is deliberately value-owned so animation never
// reads renderer/Flecs state through an unsafe cross-phase pointer.
struct AnimationPresentationObservation {
    AnimatorInstanceHandle instance{};
    bool visible = true;
    float distance = 0.0f;
    int32_t explicit_priority = 0;
};

// This is the explicit B4 bridge from graph evaluation to fixed simulation.
// It is value-owned by AnimationSystems, so API writes cannot race phase
// execution. B5 replaces the simple root values with controller output.
struct AnimationFixedClipWork {
    uint16_t node_index = 0;
    uint16_t clip_index = 0;
    float duration = 0.0f;
    bool loop = false;
    float time = 0.0f;
    float rate = 1.0f;
    std::vector<RuntimeClipMarker> markers;
};

// The per-animator record the fixed phases operate on, one per animator, held
// in `fixed_work_`. It is seeded from the descriptor's immutable
// `fixed_work` on every lease refresh, but the runtime-only fields below
// (clip time, sampled root transforms, evaluated target) are deliberately
// carried across a refresh so that an input or target write cannot restart
// the animator's clock.
//
// `root_entity` is a flecs entity id, or 0 for "this animator has no root in
// the world" - in which case root motion is published but never applied, and
// target transforms are used as-is instead of being resolved against a root.
struct AnimationFixedWork {
    AnimatorInstanceHandle instance{};
    AnimationFixedClipWork clip{};
    AnimationTransform root_previous{};
    AnimationTransform root_current{};
    // Runtime-only fixed sampling state.  It is not authored data and is
    // preserved across control refreshes.
    bool root_sampled = false;
    std::vector<AnimationWorldQueryRequest> queries;
    AnimationTransform desired_target_world{};
    AnimationTransform evaluated_target_root_relative{};
    float target_weight = 0.0f;
    bool target_enabled = false;
    uint64_t root_entity = 0;
};

// Immutable authored-to-runtime bridge.  It is owned by the definition (via a
// shared immutable descriptor), while AnimationSystems owns only copies of
// per-instance work.  Keeping the evaluator definition alive here prevents a
// hot reload from invalidating an in-flight runtime request.
struct AnimationRuntimeBindingDescriptor {
    std::shared_ptr<const AnimationEvaluationDefinition> evaluation;
    AnimationFixedWork fixed_work;
    // UINT16_MAX means this descriptor has no externally-driven target.
    uint16_t target_index = UINT16_MAX;
    // These declarations are immutable and are instantiated per animator.
    // Controllers may only name Controller-owned targets; duplicate target
    // ownership is rejected when the binding is admitted.
    std::vector<CanonicalTarget> targets;
    struct Controller {
        // The controller ABI is deliberately explicit: a controller only
        // receives these fixed inputs, in declaration order.  It never gets a
        // view of the animator's full control array.
        struct InputBinding {
            uint16_t input_index = UINT16_MAX;
            AnimationValueType type = static_cast<AnimationValueType>(0xff);
            EvaluationCadence cadence = EvaluationCadence::Fixed;
        };
        NativeControllerDescriptor descriptor;
        int32_t priority = 0;
        std::vector<uint16_t> target_indices;
        std::vector<InputBinding> inputs;
    };
    std::vector<Controller> controllers;
};

// B3 makes the boundary order observable.  Entries named for later features
// are scheduling stubs only: B4/B5 own markers, root motion, world queries,
// controllers, target solving, and IK behavior.
enum class AnimationScheduleEvent : uint8_t {
    FixedRotateState,
    FixedSampleApiWrites,
    FixedAdvanceClocks,
    FixedSampleRootChannels,
    FixedPublishDesiredRootMotion,
    FixedEmitMarkers,
    PrePhysicsAuthority,
    PhysicsStep,
    PostPhysicsHierarchy,
    FixedEvaluateControllers,
    FixedWorldQueries,
    FixedSmoothTargets,
    FixedPublishSnapshot,
    FrameSampleApiWrites,
    FrameInterpolateFixedState,
    FrameEvaluatePresentationGraph,
    FrameSolveTargetsAndIk,
    FramePublishPoseSnapshot,
};

// One recorded phase boundary. The trace is append-only within a tick and is
// drained by `take_trace()`; it exists so tests and tools can assert the
// schedule's exact order and deltas without instrumenting the phases.
// `delta_seconds` is the delta of the phase that recorded the entry - fixed
// delta for the fixed events, frame delta for the frame events.
struct AnimationScheduleTraceEntry {
    AnimationScheduleEvent event;
    double delta_seconds = 0.0;
};

// Renderer-facing copies of complete evaluator output.  The store is wholly
// independent of Flecs and requires the consumer to request the frame serial
// it intends to render, preventing a stale pose from being mistaken as current.
class AnimationPoseSnapshotStore {
public:
    static size_t slot_mutable_bytes() noexcept;
    // Copies a snapshot into the slot's back buffer and flips it to front.
    // Returns false without storing anything if the snapshot is incomplete -
    // all five streams must have the same joint count and non-null pointers
    // whenever that count is non-zero.
    //
    // IMPORTANT: `snapshot()` and `latest()` return views that point into this
    // store's buffers, not copies. A view is invalidated by the next
    // `publish` for the same animator (which overwrites the back buffer that
    // is two publishes old) and by `forget`. Consume or copy a view before
    // publishing again; `copy_animation_debug_pose` exists precisely so an
    // out-of-phase caller does not have to reason about this.
    //
    // `snapshot()` additionally requires an EXACT `frame_serial` match and
    // returns an empty snapshot otherwise - that is the mechanism that stops a
    // stale pose from being mistaken for the current frame's.
    bool publish(const AnimationPoseSnapshot& snapshot);
    AnimationPoseSnapshot snapshot(AnimatorInstanceHandle instance,
                                   uint64_t frame_serial) const;
    AnimationPoseSnapshot latest(AnimatorInstanceHandle instance) const;
    void forget(AnimatorInstanceHandle instance);

private:
    struct PoseBuffer {
        uint64_t fixed_tick = 0;
        uint64_t frame_serial = 0;
        std::vector<AnimationTransform> local_pose;
        std::vector<Mat4f> model_pose;
        std::vector<Mat4f> previous_model_pose;
        std::vector<Mat4f> skin_palette;
        std::vector<Mat4f> previous_skin_palette;
    };
    struct Slot {
        std::array<PoseBuffer, 2> buffers{};
        uint8_t front = 0;
        bool has_snapshot = false;
    };

    static uint64_t key(AnimatorInstanceHandle instance);
    static AnimationPoseSnapshot view(AnimatorInstanceHandle instance,
                                      const PoseBuffer& buffer);
    std::map<uint64_t, Slot> slots_;
};

// The animation runtime object. Constructed once by the ECS runtime and kept
// alive for as long as the phase systems it registered; not copyable in
// practice (it owns evaluators and controller instances), and touched only
// from the ECS pipeline thread.
//
// It holds four pose stores with distinct jobs:
// - `pose_snapshots_` is the renderer-facing publication point.
// - `fixed_pose_snapshots_` / `previous_fixed_pose_snapshots_` are the two
//   interpolation endpoints, written only by the fixed phases.
// - `last_complete_presentation_pose_snapshots_` is the fallback a
//   pose-LOD-frozen animator is re-published from.
// and two evaluators: `evaluator_` carries the authoritative simulation state
// (graph clocks, root-motion history, fixed IK) and is what checkpoints
// capture; `presentation_evaluator_` is an ephemeral per-frame copy that must
// never write back into simulation state.
//
// Public methods split into three groups: the service bridge
// (`refresh_service_binding`, `detach_service_binding`, checkpoint
// capture/validate/restore) called only by `AnimationService`; the runtime
// hooks (`set_interpolation_alpha`, `set_presentation_delta_seconds`,
// `publish_presentation_for_render`, the visibility staging trio) called by
// the engine loop; and a narrow test/tool seam (`register_fixed_work`,
// `take_trace`, `take_marker_events`).
class AnimationSystems {
public:
    static size_t binding_container_mutable_bytes() noexcept;
    AnimationPoseSnapshotStore& pose_snapshots() noexcept { return pose_snapshots_; }
    const AnimationPoseSnapshotStore& pose_snapshots() const noexcept { return pose_snapshots_; }
    // Explicit presentation-to-render handoff.  Runtime evaluation owns pose
    // contents; the render caller owns its serial.  Republishing the complete
    // immutable view under the submission serial lets adapters require an
    // exact serial without sampling or advancing animation at render time.
    void publish_presentation_for_render(uint64_t render_frame_serial);

    // Runtime calls this exactly once, after its fixed-step accumulator loop
    // and before FrameUpdate.  It is consumed only by presentation state.
    void set_interpolation_alpha(double alpha) noexcept;
    // Wall-clock delta for the coming FrameUpdate.  The pose-LOD refresh clock
    // (presentation_time_seconds_) advances by this instead of the pipeline's
    // possibly time-scaled frame delta, so slow motion does not throttle how
    // often presentation refreshes.  Runtime sets it every tick; when it was
    // never set (direct run_frame callers in tests/tools), run_frame falls
    // back to its frame delta, which is the historical behaviour.
    void set_presentation_delta_seconds(double delta) noexcept;
    std::vector<AnimationScheduleTraceEntry> take_trace();

    // Fixed root motion has exactly one consumer (the authority phase).  A
    // second consumer for the same animator/tick fails closed.
    bool publish_desired_root_motion(AnimatorInstanceHandle instance,
                                     const DesiredRootMotion& motion, uint64_t fixed_tick);
    bool consume_desired_root_motion(AnimatorInstanceHandle instance,
                                     uint64_t fixed_tick, DesiredRootMotion& out);

    void set_world_queries(const AnimationWorldQueries* queries) noexcept { world_queries_ = queries; }
    // Runs a batch of rays through the attached `AnimationWorldQueries`.
    // Results come back in request order; execution order is the deterministic
    // priority order described on `AnimationWorldQueryRequest`. Requests past
    // the tick's budget (`max_world_queries_per_fixed_tick`, counted by
    // `fixed_tick_query_admitted_`, which is reset once per fixed post phase)
    // are counted as overflow and left as a no-hit rather than executed.
    // With no provider attached, every admitted query still counts and
    // returns no hit.
    std::vector<AnimationWorldQueryResult> execute_fixed_world_queries(
        std::vector<AnimationWorldQueryRequest> requests);
    uint64_t world_query_overflow_count() const noexcept { return world_query_overflow_count_; }
    uint64_t world_query_count() const noexcept { return world_query_count_; }
    // Install or overwrite an animator's fixed work record, validating the
    // handle, clip timing and per-query finiteness. Service-bound animators
    // reach this through `refresh_service_binding`; calling it directly is the
    // narrow test/tool seam, and such records take the descriptor-free clock
    // path in `run_fixed_update` instead of the evaluator's graph clock.
    bool register_fixed_work(const AnimationFixedWork& work);
    void remove_fixed_work(AnimatorInstanceHandle instance);
    std::vector<AnimationMarkerEvent> take_marker_events();
    std::vector<DesiredRootMotion> take_consumed_root_motion();

    // Called by AnimationService lifecycle operations.  These entry points
    // are internal; direct register_fixed_work remains a narrow test/tool seam.
    bool refresh_service_binding(const AnimationRuntimeBindingLease& lease);
    void detach_service_binding(AnimatorInstanceHandle instance);
    // Precondition: no service bindings may exist. Returns false (changing
    // nothing) if any animator is bound or if the config is invalid, because
    // the budget determines the evaluator storage that live animators are
    // already sized against. In practice only
    // `AnimationService::attach_runtime_systems` calls it, before any lease is
    // published. Rebuilds the pose-LOD scheduler, discarding its history.
    bool set_budget_config(const AnimationBudgetConfig& config);
    // Two-phase handoff of render-side visibility into the pose-LOD
    // scheduler. `stage_completed_visibility` takes the observations for a
    // frame that is being submitted (rejecting duplicates or non-finite
    // distances outright); `commit_completed_visibility` promotes them once
    // that frame actually completed, and refuses a serial that does not match
    // the staged one or that moves backwards; `discard_completed_visibility`
    // drops the staged set for an abandoned frame. Only committed
    // observations influence which animators are evaluated next frame; an
    // animator with no observation is treated as visible at distance 0.
    bool stage_completed_visibility(
        uint64_t frame_serial,
        std::vector<AnimationPresentationObservation> observations);
    bool commit_completed_visibility(uint64_t frame_serial);
    void discard_completed_visibility(uint64_t frame_serial) noexcept;
    const AnimationBudgetRuntimeStats& presentation_budget_stats() const noexcept {
        return presentation_budget_stats_;
    }
    AnimationBudgetRuntimeStats runtime_stats() const noexcept;
    void attach_service(AnimationService* service) noexcept { service_ = service; }
    bool has_service(const AnimationService* service) const noexcept { return service_ == service; }
    bool capture_service_checkpoint(AnimatorCheckpoint& checkpoint) const;
    bool validate_service_checkpoint(const AnimatorCheckpoint& checkpoint) const;
    bool restore_service_checkpoint(const AnimatorCheckpoint& checkpoint);
    // Copies the latest presentation pose and actual evaluated target state.
    // The result owns its storage across runtime advances and destruction.
    bool copy_animation_debug_pose(AnimatorInstanceHandle instance,
                                   AnimationDebugPoseSnapshot& out) const;

    // Publish the rig's BIND pose as this animator's presentation pose, so an
    // animator that has never been evaluated still has something to show.
    //
    // A stopped editor never advances a fixed step, so nothing ever published a
    // pose and every pose-shaped query failed -- which is what left the Part
    // Workbench animation tabs blank in exactly the mode an author inspects a
    // rig in. The bind pose is the correct thing to show there: it is what the
    // rig looks like before any clip touches it.
    //
    // Idempotent and non-destructive: returns true immediately when a pose is
    // already published, so a real evaluated pose is NEVER clobbered by this.
    bool seed_bind_pose(AnimatorInstanceHandle instance, const CanonicalRig& rig);

private:
    // The phase bodies. `register_animation_systems` is a friend so the
    // registered systems can call these without exposing them publicly; the
    // order they appear in is the order the ECS pipeline runs them. `run_frame`
    // is the only one on the frame pipeline, all others are fixed-step.
    friend void register_animation_systems(flecs::world&, AnimationSystems&);
    void run_fixed_pre(flecs::world& world, double fixed_delta);
    void run_fixed_update(flecs::world& world, double fixed_delta);
    void run_pre_physics(flecs::world& world, double fixed_delta);
    void run_physics(double fixed_delta);
    void run_post_physics(double fixed_delta);
    void run_fixed_post(flecs::world& world, double fixed_delta);
    void run_frame(flecs::world& world, double frame_delta);
    void trace(AnimationScheduleEvent event, double delta_seconds);
    void sample_service_bindings();
    void evaluate_service_bindings(flecs::world& world, double delta_seconds,
                                   float accumulator_alpha);
    bool apply_targets(flecs::world&, AnimatorInstanceHandle, EvaluationCadence, double);

    double interpolation_alpha_ = 0.0;
    // < 0 means "never set": run_frame advances the presentation clock by its
    // own frame delta exactly as before the wall-clock split existed.
    double presentation_delta_seconds_ = -1.0;
    std::vector<AnimationScheduleTraceEntry> trace_;
    AnimationPoseSnapshotStore pose_snapshots_;
    // Fixed evaluation publishes into pose_snapshots_ before FrameUpdate. A
    // throttled frame must use the last completed presentation instead of
    // accidentally exposing that newly advanced fixed sample.
    AnimationPoseSnapshotStore last_complete_presentation_pose_snapshots_;
    // Owned solved fixed samples.  These are deliberately distinct from the
    // renderer-facing presentation store: frame layers may never overwrite a
    // simulation checkpoint or its previous-model history.
    AnimationPoseSnapshotStore fixed_pose_snapshots_;
    AnimationPoseSnapshotStore previous_fixed_pose_snapshots_;
    // Single-slot mailbox per animator: one publish and one consume per fixed
    // tick. `tick` is what makes a second publish in the same tick fail and a
    // consume from a stale tick fail; `consumed` enforces the single-consumer
    // rule within the tick.
    struct RootMotionSlot { uint64_t tick = 0; DesiredRootMotion motion{}; bool consumed = false; };
    std::map<uint64_t, RootMotionSlot> desired_root_motion_;
    const AnimationWorldQueries* world_queries_ = nullptr;
    uint64_t world_query_count_ = 0;
    uint64_t world_query_overflow_count_ = 0;
    uint32_t fixed_tick_query_admitted_ = 0;
    std::map<uint64_t, AnimationFixedWork> fixed_work_;
    std::vector<AnimationMarkerEvent> marker_events_;
    std::vector<DesiredRootMotion> consumed_root_motion_;
    AnimationService* service_ = nullptr;
    // Fixed evaluator state is checkpointed and is the only owner of graph
    // clocks/root-motion history.  Presentation is an ephemeral copy made
    // from that solved fixed pose once per frame, so a frame IK pass cannot
    // mutate simulation state or erase a fixed IK result.
    AnimationEvaluator evaluator_;
    AnimationEvaluator presentation_evaluator_;
    AnimationBudgetConfig budget_config_{};
    PoseLodScheduler pose_lod_scheduler_{budget_config_};
    AnimationBudgetRuntimeStats presentation_budget_stats_{};
    uint64_t staged_visibility_serial_ = 0;
    bool has_staged_visibility_ = false;
    uint64_t completed_visibility_serial_ = 0;
    std::map<uint64_t, AnimationPresentationObservation> staged_visibility_;
    std::map<uint64_t, AnimationPresentationObservation> completed_visibility_;
    double presentation_time_seconds_ = 0.0;
    std::map<uint64_t, AnimationRuntimeBindingLease> service_bindings_;
    // Per-animator target/controller state, parallel to the descriptor's
    // `targets` vector. `targets` holds the smoothed (evaluated) state that
    // the IK solve consumes; `desired_world` holds the raw world-space goals -
    // written by the public API for External targets and by the owning
    // controller for Controller targets. `controllers` are owned instances,
    // rebuilt only when the descriptor identity or controller count changes,
    // so a plain control write preserves controller state.
    // `controller_descriptor` is the identity used for that check and is
    // non-owning.
    struct TargetRuntime {
        std::vector<AnimationTargetState> targets;
        std::vector<AnimationTransform> desired_world;
        std::vector<std::unique_ptr<NativeController>> controllers;
        const AnimationRuntimeBindingDescriptor* controller_descriptor = nullptr;
    };
    std::map<uint64_t, TargetRuntime> target_runtime_;
};

// Target writes are intentionally stored in world coordinates.  This helper is
// called from the fixed post-physics boundary, so moving roots cannot stale an
// earlier API-write transform.
bool resolve_world_target(const Mat4f& current_root_world,
                          const AnimationTransform& desired_world,
                          AnimationTransform& out_root_relative);

// Installs the B3 fixed/frame scheduling seam into an already initialized ECS
// world. Runtime owns the AnimationSystems object for the lifetime of systems.
void register_animation_systems(flecs::world& world, AnimationSystems& systems);

} // namespace matter::animation
