#pragma once

#include "animation/animation_evaluator.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace flecs { class world; }

namespace matter::animation {

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

private:
    friend void register_animation_systems(flecs::world&, AnimationSystems&);
    void run_fixed_pre(flecs::world& world, double fixed_delta);
    void run_fixed_update(double fixed_delta);
    void run_pre_physics(double fixed_delta);
    void run_physics(double fixed_delta);
    void run_post_physics(double fixed_delta);
    void run_fixed_post(double fixed_delta);
    void run_frame(flecs::world& world, double frame_delta);
    void trace(AnimationScheduleEvent event, double delta_seconds);

    double interpolation_alpha_ = 0.0;
    std::vector<AnimationScheduleTraceEntry> trace_;
    AnimationPoseSnapshotStore pose_snapshots_;
};

// Installs the B3 fixed/frame scheduling seam into an already initialized ECS
// world. Runtime owns the AnimationSystems object for the lifetime of systems.
void register_animation_systems(flecs::world& world, AnimationSystems& systems);

} // namespace matter::animation
