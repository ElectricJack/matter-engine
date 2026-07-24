#pragma once

#include "animation/animation_evaluator.h"
#include "animation/animation_world_queries.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace flecs { class world; }

namespace matter::animation {

constexpr uint32_t kMaxAnimationWorldQueries = 2048;

struct AnimationWorldQueryRequest {
    AnimatorInstanceHandle instance{};
    uint16_t controller_order = 0;
    int32_t priority = 0;
    Float3 origin{};
    Float3 direction{};
    float max_distance = 0.0f;
    uint64_t mask = 0;
};

struct AnimationWorldQueryResult {
    AnimatorInstanceHandle instance{};
    uint16_t controller_order = 0;
    bool hit = false;
    WorldRayHit value{};
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

struct AnimationScheduleTraceEntry {
    AnimationScheduleEvent event;
    double delta_seconds = 0.0;
};

// Renderer-facing copies of complete evaluator output.  The store is wholly
// independent of Flecs and requires the consumer to request the frame serial
// it intends to render, preventing a stale pose from being mistaken as current.
class AnimationPoseSnapshotStore {
public:
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

class AnimationSystems {
public:
    AnimationPoseSnapshotStore& pose_snapshots() noexcept { return pose_snapshots_; }
    const AnimationPoseSnapshotStore& pose_snapshots() const noexcept { return pose_snapshots_; }

    // Runtime calls this exactly once, after its fixed-step accumulator loop
    // and before FrameUpdate.  It is consumed only by presentation state.
    void set_interpolation_alpha(double alpha) noexcept;
    std::vector<AnimationScheduleTraceEntry> take_trace();

    // Fixed root motion has exactly one consumer (the authority phase).  A
    // second consumer for the same animator/tick fails closed.
    bool publish_desired_root_motion(AnimatorInstanceHandle instance,
                                     const DesiredRootMotion& motion, uint64_t fixed_tick);
    bool consume_desired_root_motion(AnimatorInstanceHandle instance,
                                     uint64_t fixed_tick, DesiredRootMotion& out);

    void set_world_queries(const AnimationWorldQueries* queries) noexcept { world_queries_ = queries; }
    std::vector<AnimationWorldQueryResult> execute_fixed_world_queries(
        std::vector<AnimationWorldQueryRequest> requests);
    uint64_t world_query_overflow_count() const noexcept { return world_query_overflow_count_; }
    bool register_fixed_work(const AnimationFixedWork& work);
    void remove_fixed_work(AnimatorInstanceHandle instance);
    std::vector<AnimationMarkerEvent> take_marker_events();
    std::vector<DesiredRootMotion> take_consumed_root_motion();

    // Called by AnimationService lifecycle operations.  These entry points
    // are internal; direct register_fixed_work remains a narrow test/tool seam.
    bool refresh_service_binding(const AnimationRuntimeBindingLease& lease);
    void detach_service_binding(AnimatorInstanceHandle instance);
    void attach_service(AnimationService* service) noexcept { service_ = service; }
    bool has_service(const AnimationService* service) const noexcept { return service_ == service; }
    bool capture_service_checkpoint(AnimatorCheckpoint& checkpoint) const;
    bool validate_service_checkpoint(const AnimatorCheckpoint& checkpoint) const;
    bool restore_service_checkpoint(const AnimatorCheckpoint& checkpoint);

private:
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
    void evaluate_service_bindings(flecs::world& world, double delta_seconds);

    double interpolation_alpha_ = 0.0;
    std::vector<AnimationScheduleTraceEntry> trace_;
    AnimationPoseSnapshotStore pose_snapshots_;
    struct RootMotionSlot { uint64_t tick = 0; DesiredRootMotion motion{}; bool consumed = false; };
    std::map<uint64_t, RootMotionSlot> desired_root_motion_;
    const AnimationWorldQueries* world_queries_ = nullptr;
    uint64_t world_query_overflow_count_ = 0;
    std::map<uint64_t, AnimationFixedWork> fixed_work_;
    std::vector<AnimationMarkerEvent> marker_events_;
    std::vector<DesiredRootMotion> consumed_root_motion_;
    AnimationService* service_ = nullptr;
    AnimationEvaluator evaluator_;
    std::map<uint64_t, AnimationRuntimeBindingLease> service_bindings_;
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
