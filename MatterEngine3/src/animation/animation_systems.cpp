#include "animation/animation_systems.h"

#include "matter/ecs.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace matter::animation {
namespace {

struct AnimationSystemsContext {
    AnimationSystems* value = nullptr;
};

uint64_t animator_key(AnimatorInstanceHandle instance) {
    return (uint64_t(instance.slot_index) << 32u) | instance.generation;
}

bool inverse(const Mat4f& source, Mat4f& out) {
    float a[4][8]{};
    for (int row = 0; row < 4; ++row) for (int column = 0; column < 4; ++column) {
        a[row][column] = source.m[row * 4 + column];
        a[row][column + 4] = row == column ? 1.0f : 0.0f;
    }
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (std::fabs(a[row][column]) > std::fabs(a[pivot][column])) pivot = row;
        if (!std::isfinite(a[pivot][column]) || std::fabs(a[pivot][column]) < 1e-8f) return false;
        for (int item = 0; item < 8; ++item) std::swap(a[column][item], a[pivot][item]);
        const float divisor = a[column][column];
        for (int item = 0; item < 8; ++item) a[column][item] /= divisor;
        for (int row = 0; row < 4; ++row) if (row != column) {
            const float factor = a[row][column];
            for (int item = 0; item < 8; ++item) a[row][item] -= factor * a[column][item];
        }
    }
    for (int row = 0; row < 4; ++row) for (int column = 0; column < 4; ++column)
        out.m[row * 4 + column] = a[row][column + 4];
    return true;
}

AnimationTransform runtime_root_delta(const AnimationTransform& previous, const AnimationTransform& current) {
    AnimationTransform result{};
    result.translation = {current.translation.x - previous.translation.x, current.translation.y - previous.translation.y,
                          current.translation.z - previous.translation.z};
    return result;
}

void emit_runtime_markers(AnimatorInstanceHandle instance, const std::vector<RuntimeClipMarker>& markers,
                          float duration, bool loop, float previous, float current,
                          std::vector<AnimationMarkerEvent>& out) {
    if (duration <= 0.0f || previous == current) return;
    const bool forward = current > previous;
    struct Item { float absolute; RuntimeClipMarker marker; };
    std::vector<Item> items;
    for (const auto& marker : markers) {
        const int first = loop ? int(std::floor(std::min(previous, current) / duration)) - 1 : 0;
        const int last = loop ? int(std::ceil(std::max(previous, current) / duration)) + 1 : 0;
        for (int cycle = first; cycle <= last; ++cycle) {
            const float absolute = loop ? marker.time + cycle * duration : marker.time;
            if ((forward && absolute > previous && absolute <= current) || (!forward && absolute >= current && absolute < previous))
                items.push_back({absolute, marker});
        }
    }
    std::stable_sort(items.begin(), items.end(), [forward](const Item& a, const Item& b) {
        if (a.absolute != b.absolute) return forward ? a.absolute < b.absolute : a.absolute > b.absolute;
        return a.marker.marker_index < b.marker.marker_index;
    });
    for (const auto& item : items) out.push_back({instance, item.marker.marker_index, item.marker.time});
}

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

bool AnimationSystems::publish_desired_root_motion(AnimatorInstanceHandle instance,
                                                    const DesiredRootMotion& motion,
                                                    uint64_t fixed_tick) {
    if (!instance.valid() || !motion.valid) return false;
    const uint64_t slot_key = animator_key(instance);
    const auto existing = desired_root_motion_.find(slot_key);
    if (existing != desired_root_motion_.end() && existing->second.tick == fixed_tick) return false;
    RootMotionSlot& slot = desired_root_motion_[slot_key];
    slot.tick = fixed_tick;
    slot.motion = motion;
    slot.consumed = false;
    return true;
}

bool AnimationSystems::register_fixed_work(const AnimationFixedWork& work) {
    if (!work.instance.valid() || !std::isfinite(work.clip.duration) || work.clip.duration <= 0.0f ||
        !std::isfinite(work.clip.time) || !std::isfinite(work.clip.rate)) return false;
    for (const auto& marker : work.clip.markers)
        if (!std::isfinite(marker.time) || marker.time < 0.0f || marker.time > work.clip.duration) return false;
    for (const auto& query : work.queries)
        if (animator_key(query.instance) != animator_key(work.instance) || !std::isfinite(query.max_distance) || query.max_distance < 0.0f) return false;
    fixed_work_[animator_key(work.instance)] = work;
    return true;
}

void AnimationSystems::remove_fixed_work(AnimatorInstanceHandle instance) { if (instance.valid()) fixed_work_.erase(animator_key(instance)); }
std::vector<AnimationMarkerEvent> AnimationSystems::take_marker_events() { std::vector<AnimationMarkerEvent> result; result.swap(marker_events_); return result; }
std::vector<DesiredRootMotion> AnimationSystems::take_consumed_root_motion() { std::vector<DesiredRootMotion> result; result.swap(consumed_root_motion_); return result; }

bool AnimationSystems::consume_desired_root_motion(AnimatorInstanceHandle instance,
                                                    uint64_t fixed_tick,
                                                    DesiredRootMotion& out) {
    out = {};
    const auto found = desired_root_motion_.find(animator_key(instance));
    if (!instance.valid() || found == desired_root_motion_.end() || found->second.tick != fixed_tick ||
        found->second.consumed || !found->second.motion.valid) return false;
    found->second.consumed = true;
    out = found->second.motion;
    return true;
}

std::vector<AnimationWorldQueryResult> AnimationSystems::execute_fixed_world_queries(
    std::vector<AnimationWorldQueryRequest> requests) {
    std::vector<AnimationWorldQueryResult> results(requests.size());
    std::vector<uint32_t> order;
    order.reserve(requests.size());
    for (uint32_t index = 0; index < requests.size(); ++index) {
        results[index].instance = requests[index].instance;
        results[index].controller_order = requests[index].controller_order;
        order.push_back(index);
    }
    std::stable_sort(order.begin(), order.end(), [&requests](uint32_t a, uint32_t b) {
        const auto& left = requests[a]; const auto& right = requests[b];
        if (left.priority != right.priority) return left.priority < right.priority;
        if (left.instance.slot_index != right.instance.slot_index) return left.instance.slot_index < right.instance.slot_index;
        return left.controller_order < right.controller_order;
    });
    uint32_t admitted = 0;
    for (uint32_t index : order) {
        const auto& request = requests[index];
        if (!request.instance.valid() || !std::isfinite(request.max_distance) || request.max_distance < 0.0f) continue;
        if (admitted >= kMaxAnimationWorldQueries) { ++world_query_overflow_count_; continue; }
        ++admitted;
        if (world_queries_ != nullptr)
            results[index].hit = world_queries_->ray_cast(request.origin, request.direction,
                                                           request.max_distance, request.mask, results[index].value);
    }
    return results;
}

bool resolve_world_target(const Mat4f& current_root_world,
                          const AnimationTransform& desired_world,
                          AnimationTransform& out_root_relative) {
    Mat4f inverse_root{};
    if (!inverse(current_root_world, inverse_root)) return false;
    const Float3& point = desired_world.translation;
    const float x = inverse_root.m[0] * point.x + inverse_root.m[4] * point.y + inverse_root.m[8] * point.z + inverse_root.m[12];
    const float y = inverse_root.m[1] * point.x + inverse_root.m[5] * point.y + inverse_root.m[9] * point.z + inverse_root.m[13];
    const float z = inverse_root.m[2] * point.x + inverse_root.m[6] * point.y + inverse_root.m[10] * point.z + inverse_root.m[14];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    out_root_relative = desired_world;
    out_root_relative.translation = {x, y, z};
    return true;
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

void AnimationSystems::run_fixed_update(flecs::world& world, double fixed_delta) {
    trace(AnimationScheduleEvent::FixedSampleRootChannels, fixed_delta);
    struct OrderedMarker { AnimatorInstanceHandle instance; uint16_t node; uint16_t clip; AnimationMarkerEvent event; };
    std::vector<OrderedMarker> emitted;
    for (auto& pair : fixed_work_) {
        AnimationFixedWork& work = pair.second;
        const float previous = work.clip.time;
        const float current = previous + work.clip.rate * static_cast<float>(fixed_delta);
        std::vector<AnimationMarkerEvent> local;
        emit_runtime_markers(work.instance, work.clip.markers, work.clip.duration, work.clip.loop, previous, current, local);
        for (const auto& event : local) emitted.push_back({work.instance, work.clip.node_index, work.clip.clip_index, event});
        work.clip.time = current;
    }
    std::stable_sort(emitted.begin(), emitted.end(), [](const OrderedMarker& a, const OrderedMarker& b) {
        if (a.instance.slot_index != b.instance.slot_index) return a.instance.slot_index < b.instance.slot_index;
        if (a.clip != b.clip) return a.clip < b.clip;
        if (a.node != b.node) return a.node < b.node;
        if (a.event.time != b.event.time) return a.event.time < b.event.time;
        return a.event.marker_index < b.event.marker_index;
    });
    for (const auto& marker : emitted) marker_events_.push_back(marker.event);
    trace(AnimationScheduleEvent::FixedPublishDesiredRootMotion, fixed_delta);
    const uint64_t tick = world.get<ecs::AnimationFixedState>().current_tick;
    for (const auto& pair : fixed_work_) {
        const AnimationFixedWork& work = pair.second;
        DesiredRootMotion motion{};
        motion.delta = runtime_root_delta(work.root_previous, work.root_current);
        motion.valid = true;
        (void)publish_desired_root_motion(work.instance, motion, tick);
    }
    trace(AnimationScheduleEvent::FixedEmitMarkers, fixed_delta);
}

void AnimationSystems::run_pre_physics(flecs::world& world, double fixed_delta) {
    const uint64_t tick = world.get<ecs::AnimationFixedState>().current_tick;
    for (const auto& pair : fixed_work_) {
        DesiredRootMotion motion{};
        if (consume_desired_root_motion(pair.second.instance, tick, motion)) consumed_root_motion_.push_back(motion);
    }
    trace(AnimationScheduleEvent::PrePhysicsAuthority, fixed_delta);
}

void AnimationSystems::run_physics(double fixed_delta) {
    trace(AnimationScheduleEvent::PhysicsStep, fixed_delta);
}

void AnimationSystems::run_post_physics(double fixed_delta) {
    trace(AnimationScheduleEvent::PostPhysicsHierarchy, fixed_delta);
}

void AnimationSystems::run_fixed_post(flecs::world& world, double fixed_delta) {
    trace(AnimationScheduleEvent::FixedEvaluateControllers, fixed_delta);
    trace(AnimationScheduleEvent::FixedWorldQueries, fixed_delta);
    std::vector<AnimationWorldQueryRequest> queries;
    for (const auto& pair : fixed_work_)
        queries.insert(queries.end(), pair.second.queries.begin(), pair.second.queries.end());
    (void)execute_fixed_world_queries(std::move(queries));
    trace(AnimationScheduleEvent::FixedSmoothTargets, fixed_delta);
    for (auto& pair : fixed_work_) {
        AnimationFixedWork& work = pair.second;
        if (work.root_entity == 0) continue;
        const flecs::entity root = world.entity(work.root_entity);
        const ecs::WorldTransform* transform = root.try_get<ecs::WorldTransform>();
        if (transform == nullptr || !resolve_world_target(transform->matrix, work.desired_target_world,
                                                           work.evaluated_target_root_relative)) continue;
    }
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
        [](AnimationSystems& instance, flecs::world& runtime_world, double delta) { instance.run_fixed_update(runtime_world, delta); });
    register_system<ecs::PrePhysics>(world, "MatterAnimationPrePhysics",
        [](AnimationSystems& instance, flecs::world& runtime_world, double delta) { instance.run_pre_physics(runtime_world, delta); });
    register_system<ecs::Physics>(world, "MatterAnimationPhysicsTrace",
        [](AnimationSystems& instance, flecs::world&, double delta) { instance.run_physics(delta); });
    register_system<ecs::PostPhysicsHierarchy>(world, "MatterAnimationPostPhysicsHierarchy",
        [](AnimationSystems& instance, flecs::world&, double delta) { instance.run_post_physics(delta); });
    register_system<ecs::FixedPostUpdate>(world, "MatterAnimationFixedPostUpdate",
        [](AnimationSystems& instance, flecs::world& runtime_world, double delta) { instance.run_fixed_post(runtime_world, delta); });
    register_system<ecs::FrameUpdate>(world, "MatterAnimationFrameUpdate",
        [](AnimationSystems& instance, flecs::world& runtime_world, double delta) {
            instance.run_frame(runtime_world, delta);
        });
}

} // namespace matter::animation
