#include "animation/animation_systems.h"

#include "matter/ecs.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

namespace matter::animation {
namespace {

struct AnimationSystemsContext {
    AnimationSystems* value = nullptr;
};

bool complete(const AnimationPoseSnapshot& snapshot) {
    const uint32_t count = snapshot.local_pose.count;
    return snapshot.instance.valid() &&
           snapshot.model_pose.count == count &&
           snapshot.previous_model_pose.count == count &&
           snapshot.skin_palette.count == count &&
           snapshot.previous_skin_palette.count == count &&
           (count == 0 ||
            (snapshot.local_pose.data != nullptr && snapshot.model_pose.data != nullptr &&
             snapshot.previous_model_pose.data != nullptr && snapshot.skin_palette.data != nullptr &&
             snapshot.previous_skin_palette.data != nullptr));
}

template <typename T>
void copy(ArrayView<T> source, std::vector<T>& destination) {
    if (source.count == 0) {
        destination.clear();
        return;
    }
    destination.assign(source.data, source.data + source.count);
}

template <typename Phase, typename Fn>
void register_system(flecs::world& world, const char* name, Fn&& fn) {
    flecs::system system = world.system<const AnimationSystemsContext>(name)
        .term_at(0).src<AnimationSystemsContext>()
        .kind<Phase>()
        .each([function = std::forward<Fn>(fn)](
            flecs::iter& iterator, size_t, const AnimationSystemsContext& context) {
            if (context.value != nullptr) {
                flecs::world runtime_world = iterator.world();
                function(*context.value, runtime_world, iterator.delta_time());
            }
        });
    if constexpr (std::is_same_v<Phase, ecs::FrameUpdate>) {
        system.add<ecs::FramePipelineSystem>();
    } else {
        system.add<ecs::FixedPipelineSystem>();
    }
}

} // namespace

uint64_t AnimationPoseSnapshotStore::key(AnimatorInstanceHandle instance) {
    return (uint64_t(instance.slot_index) << 32u) | instance.generation;
}

AnimationPoseSnapshot AnimationPoseSnapshotStore::view(
    AnimatorInstanceHandle instance, const PoseBuffer& buffer) {
    return {instance, buffer.fixed_tick, buffer.frame_serial,
            {buffer.local_pose.data(), static_cast<uint32_t>(buffer.local_pose.size())},
            {buffer.model_pose.data(), static_cast<uint32_t>(buffer.model_pose.size())},
            {buffer.previous_model_pose.data(), static_cast<uint32_t>(buffer.previous_model_pose.size())},
            {buffer.skin_palette.data(), static_cast<uint32_t>(buffer.skin_palette.size())},
            {buffer.previous_skin_palette.data(), static_cast<uint32_t>(buffer.previous_skin_palette.size())}};
}

bool AnimationPoseSnapshotStore::publish(const AnimationPoseSnapshot& snapshot) {
    if (!complete(snapshot)) {
        return false;
    }
    Slot& slot = slots_[key(snapshot.instance)];
    PoseBuffer& back = slot.buffers[slot.front ^ 1u];
    copy(snapshot.local_pose, back.local_pose);
    copy(snapshot.model_pose, back.model_pose);
    copy(snapshot.previous_model_pose, back.previous_model_pose);
    copy(snapshot.skin_palette, back.skin_palette);
    copy(snapshot.previous_skin_palette, back.previous_skin_palette);
    back.fixed_tick = snapshot.fixed_tick;
    back.frame_serial = snapshot.frame_serial;
    slot.front ^= 1u;
    slot.has_snapshot = true;
    return true;
}

AnimationPoseSnapshot AnimationPoseSnapshotStore::snapshot(
    AnimatorInstanceHandle instance, uint64_t frame_serial) const {
    const auto found = slots_.find(key(instance));
    if (!instance.valid() || found == slots_.end() || !found->second.has_snapshot) {
        return {};
    }
    const PoseBuffer& front = found->second.buffers[found->second.front];
    return front.frame_serial == frame_serial ? view(instance, front) : AnimationPoseSnapshot{};
}

AnimationPoseSnapshot AnimationPoseSnapshotStore::latest(AnimatorInstanceHandle instance) const {
    const auto found = slots_.find(key(instance));
    return !instance.valid() || found == slots_.end() || !found->second.has_snapshot
        ? AnimationPoseSnapshot{} : view(instance, found->second.buffers[found->second.front]);
}

void AnimationPoseSnapshotStore::forget(AnimatorInstanceHandle instance) {
    if (instance.valid()) {
        slots_.erase(key(instance));
    }
}

void AnimationSystems::set_interpolation_alpha(double alpha) noexcept {
    interpolation_alpha_ = std::isfinite(alpha)
        ? std::max(0.0, std::min(1.0, alpha)) : 0.0;
}

std::vector<AnimationScheduleTraceEntry> AnimationSystems::take_trace() {
    std::vector<AnimationScheduleTraceEntry> result;
    result.swap(trace_);
    return result;
}

void AnimationSystems::trace(AnimationScheduleEvent event, double delta_seconds) {
    trace_.push_back({event, delta_seconds});
}

void AnimationSystems::run_fixed_pre(flecs::world& world, double fixed_delta) {
    ecs::AnimationFixedState state = world.get<ecs::AnimationFixedState>();
    state.previous_tick = state.current_tick;
    ++state.current_tick;
    world.set<ecs::AnimationFixedState>(state);
    trace(AnimationScheduleEvent::FixedRotateState, fixed_delta);
    trace(AnimationScheduleEvent::FixedSampleApiWrites, fixed_delta);
    trace(AnimationScheduleEvent::FixedAdvanceClocks, fixed_delta);
}

void AnimationSystems::run_fixed_update(double fixed_delta) {
    trace(AnimationScheduleEvent::FixedSampleRootChannels, fixed_delta);
    trace(AnimationScheduleEvent::FixedPublishDesiredRootMotion, fixed_delta);
    trace(AnimationScheduleEvent::FixedEmitMarkers, fixed_delta);
}

void AnimationSystems::run_pre_physics(double fixed_delta) {
    trace(AnimationScheduleEvent::PrePhysicsAuthority, fixed_delta);
}

void AnimationSystems::run_physics(double fixed_delta) {
    trace(AnimationScheduleEvent::PhysicsStep, fixed_delta);
}

void AnimationSystems::run_post_physics(double fixed_delta) {
    trace(AnimationScheduleEvent::PostPhysicsHierarchy, fixed_delta);
}

void AnimationSystems::run_fixed_post(double fixed_delta) {
    trace(AnimationScheduleEvent::FixedEvaluateControllers, fixed_delta);
    trace(AnimationScheduleEvent::FixedWorldQueries, fixed_delta);
    trace(AnimationScheduleEvent::FixedSmoothTargets, fixed_delta);
    trace(AnimationScheduleEvent::FixedPublishSnapshot, fixed_delta);
}

void AnimationSystems::run_frame(flecs::world& world, double frame_delta) {
    ecs::AnimationFrameState state = world.get<ecs::AnimationFrameState>();
    ++state.frame_serial;
    state.interpolation_alpha = interpolation_alpha_;
    world.set<ecs::AnimationFrameState>(state);
    trace(AnimationScheduleEvent::FrameSampleApiWrites, frame_delta);
    trace(AnimationScheduleEvent::FrameInterpolateFixedState, frame_delta);
    trace(AnimationScheduleEvent::FrameEvaluatePresentationGraph, frame_delta);
    trace(AnimationScheduleEvent::FrameSolveTargetsAndIk, frame_delta);
    trace(AnimationScheduleEvent::FramePublishPoseSnapshot, frame_delta);
}

void register_animation_systems(flecs::world& world, AnimationSystems& systems) {
    world.component<AnimationSystemsContext>();
    world.set<AnimationSystemsContext>({&systems});
    register_system<ecs::FixedPreUpdate>(world, "MatterAnimationFixedPreUpdate",
        [](AnimationSystems& instance, flecs::world& runtime_world, double delta) {
            instance.run_fixed_pre(runtime_world, delta);
        });
    register_system<ecs::FixedUpdate>(world, "MatterAnimationFixedUpdate",
        [](AnimationSystems& instance, flecs::world&, double delta) { instance.run_fixed_update(delta); });
    register_system<ecs::PrePhysics>(world, "MatterAnimationPrePhysics",
        [](AnimationSystems& instance, flecs::world&, double delta) { instance.run_pre_physics(delta); });
    register_system<ecs::Physics>(world, "MatterAnimationPhysicsTrace",
        [](AnimationSystems& instance, flecs::world&, double delta) { instance.run_physics(delta); });
    register_system<ecs::PostPhysicsHierarchy>(world, "MatterAnimationPostPhysicsHierarchy",
        [](AnimationSystems& instance, flecs::world&, double delta) { instance.run_post_physics(delta); });
    register_system<ecs::FixedPostUpdate>(world, "MatterAnimationFixedPostUpdate",
        [](AnimationSystems& instance, flecs::world&, double delta) { instance.run_fixed_post(delta); });
    register_system<ecs::FrameUpdate>(world, "MatterAnimationFrameUpdate",
        [](AnimationSystems& instance, flecs::world& runtime_world, double delta) {
            instance.run_frame(runtime_world, delta);
        });
}

} // namespace matter::animation
