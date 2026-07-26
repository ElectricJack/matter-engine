#include "animation/animation_systems.h"

#include "matter/ecs.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
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

Quaternion normalize_quaternion(Quaternion q) {
    const float length = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    return length > 1e-6f ? Quaternion{q.x/length, q.y/length, q.z/length, q.w/length} : Quaternion{};
}
Quaternion multiply_quaternion(Quaternion a, Quaternion b) {
    return normalize_quaternion({a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
                                 a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
                                 a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
                                 a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z});
}
Quaternion matrix_rotation(const Mat4f& m) {
    const float trace = m.m[0] + m.m[5] + m.m[10];
    if (trace > 0.0f) { const float s = std::sqrt(trace + 1.0f) * 2.0f; return normalize_quaternion({(m.m[9]-m.m[6])/s,(m.m[2]-m.m[8])/s,(m.m[4]-m.m[1])/s,0.25f*s}); }
    if (m.m[0] > m.m[5] && m.m[0] > m.m[10]) { const float s=std::sqrt(1.0f+m.m[0]-m.m[5]-m.m[10])*2.0f; return normalize_quaternion({0.25f*s,(m.m[1]+m.m[4])/s,(m.m[2]+m.m[8])/s,(m.m[9]-m.m[6])/s}); }
    if (m.m[5] > m.m[10]) { const float s=std::sqrt(1.0f+m.m[5]-m.m[0]-m.m[10])*2.0f; return normalize_quaternion({(m.m[1]+m.m[4])/s,0.25f*s,(m.m[6]+m.m[9])/s,(m.m[2]-m.m[8])/s}); }
    const float s=std::sqrt(1.0f+m.m[10]-m.m[0]-m.m[5])*2.0f; return normalize_quaternion({(m.m[2]+m.m[8])/s,(m.m[6]+m.m[9])/s,0.25f*s,(m.m[4]-m.m[1])/s});
}

AnimationTransform runtime_root_delta(const AnimationTransform& previous, const AnimationTransform& current) {
    return root_motion_delta(previous, current);
}

bool finite_transform(const AnimationTransform& value) {
    return std::isfinite(value.translation.x) && std::isfinite(value.translation.y) && std::isfinite(value.translation.z) &&
           std::isfinite(value.rotation.x) && std::isfinite(value.rotation.y) && std::isfinite(value.rotation.z) && std::isfinite(value.rotation.w) &&
           std::isfinite(value.scale.x) && std::isfinite(value.scale.y) && std::isfinite(value.scale.z);
}

bool controller_input_value(const AnimationRuntimeBindingLease::Value& source,
                            AnimationValue& out) {
    switch (source.type) {
        case AnimationValueType::Bool: out = AnimationValue(source.boolean); return true;
        case AnimationValueType::Number:
            if (!std::isfinite(source.number)) return false;
            out = AnimationValue(source.number); return true;
        case AnimationValueType::Float3:
            if (!std::isfinite(source.float3.x) || !std::isfinite(source.float3.y) || !std::isfinite(source.float3.z)) return false;
            out = AnimationValue(source.float3); return true;
        case AnimationValueType::Quaternion:
            if (!std::isfinite(source.quaternion.x) || !std::isfinite(source.quaternion.y) || !std::isfinite(source.quaternion.z) || !std::isfinite(source.quaternion.w)) return false;
            out = AnimationValue(source.quaternion); return true;
        case AnimationValueType::Transform:
            if (!finite_transform(source.transform)) return false;
            out = AnimationValue(source.transform); return true;
        case AnimationValueType::Symbol:
            out = {}; out.type = AnimationValueType::Symbol; out.symbol = std::to_string(source.symbol); return true;
    }
    return false;
}

class ControllerQueryBroker final : public AnimationWorldQueries {
public:
    ControllerQueryBroker(const AnimationWorldQueries* source, uint64_t& overflow)
        : source_(source), overflow_(overflow) {}
    bool ray_cast(const Float3& origin, const Float3& direction, float max_distance,
                  uint64_t mask, WorldRayHit& out) const override {
        out = {};
        if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
            !std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z) ||
            !std::isfinite(max_distance) || max_distance < 0.0f) return false;
        if (admitted_ >= kMaxAnimationWorldQueries) { ++overflow_; return false; }
        ++admitted_;
        return source_ != nullptr && source_->ray_cast(origin, direction, max_distance, mask, out);
    }
private:
    const AnimationWorldQueries* source_ = nullptr;
    uint64_t& overflow_;
    mutable uint32_t admitted_ = 0;
};

// Solver calls must not be allowed to alter a target whose cadence belongs to
// the other phase.  A zero-weight copy preserves descriptor/chain validation
// while preventing the Ozz solve from touching its local pose.
std::vector<AnimationTargetState> targets_for_cadence(
    const std::vector<CanonicalTarget>& declarations,
    const std::vector<AnimationTargetState>& state,
    EvaluationCadence cadence) {
    std::vector<AnimationTargetState> result = state;
    for (size_t i = 0; i < result.size() && i < declarations.size(); ++i)
        if (declarations[i].cadence != cadence) result[i].evaluated_weight = 0.0f;
    return result;
}

bool has_targets_for_cadence(const std::vector<CanonicalTarget>& declarations,
                             EvaluationCadence cadence) {
    return std::any_of(declarations.begin(), declarations.end(), [cadence](const CanonicalTarget& target) {
        return target.cadence == cadence;
    });
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

void AnimationSystems::publish_presentation_for_render(uint64_t render_frame_serial) {
    // Service bindings are the only production publishers.  Copying through
    // AnimationPoseSnapshotStore preserves its double-buffer ownership and
    // does not expose evaluator storage to the renderer.
    for (const auto& pair : service_bindings_) {
        const AnimationPoseSnapshot latest = pose_snapshots_.latest(pair.second.instance);
        if (!latest.instance.valid() || latest.frame_serial == render_frame_serial) continue;
        AnimationPoseSnapshot tagged = latest;
        tagged.frame_serial = render_frame_serial;
        (void)pose_snapshots_.publish(tagged);
    }
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

bool AnimationSystems::set_budget_config(const AnimationBudgetConfig& config) {
    if (!config.valid() || !service_bindings_.empty()) return false;
    if (!evaluator_.set_budget_config(config)) return false;
    if (!presentation_evaluator_.set_budget_config(config)) return false;
    budget_config_ = config;
    pose_lod_scheduler_ = PoseLodScheduler(config);
    return true;
}

bool AnimationSystems::stage_completed_visibility(
    uint64_t frame_serial,
    std::vector<AnimationPresentationObservation> observations) {
    std::map<uint64_t, AnimationPresentationObservation> candidate;
    for (const AnimationPresentationObservation& observation : observations) {
        if (!observation.instance.valid() || !std::isfinite(observation.distance) ||
            observation.distance < 0.0f ||
            !candidate.emplace(animator_key(observation.instance), observation).second) {
            return false;
        }
    }
    staged_visibility_serial_ = frame_serial;
    has_staged_visibility_ = true;
    staged_visibility_ = std::move(candidate);
    return true;
}

bool AnimationSystems::commit_completed_visibility(uint64_t frame_serial) {
    if (!has_staged_visibility_ || staged_visibility_serial_ != frame_serial ||
        frame_serial < completed_visibility_serial_) {
        return false;
    }
    completed_visibility_serial_ = frame_serial;
    completed_visibility_ = std::move(staged_visibility_);
    staged_visibility_.clear();
    has_staged_visibility_ = false;
    return true;
}

void AnimationSystems::discard_completed_visibility(uint64_t frame_serial) noexcept {
    if (!has_staged_visibility_ || staged_visibility_serial_ != frame_serial) return;
    staged_visibility_.clear();
    has_staged_visibility_ = false;
}

AnimationBudgetRuntimeStats AnimationSystems::runtime_stats() const noexcept {
    AnimationBudgetRuntimeStats result = evaluator_.stats();
    result.merge(presentation_evaluator_.stats());
    result.merge(presentation_budget_stats_);
    return result;
}

bool AnimationSystems::refresh_service_binding(const AnimationRuntimeBindingLease& lease) {
    if (!lease.valid() || !lease.descriptor || !lease.descriptor->evaluation) return false;
    const AnimationRuntimeBindingDescriptor& descriptor = *lease.descriptor;
    if(descriptor.targets.size()!=lease.target_transforms.size() || descriptor.targets.size()!=lease.target_weights.size() ||
       descriptor.targets.size()!=lease.target_enabled.size() || descriptor.targets.size()>kMaxTargets ||
       !validate_exclusive_target_chains(descriptor.targets)) return false;
    std::set<uint16_t> owned_targets;
    for(const auto& controller:descriptor.controllers) {
        if(controller.descriptor.cadence!=EvaluationCadence::Fixed || !std::isfinite(static_cast<double>(controller.priority))) return false;
        for (const auto& input : controller.inputs) {
            if (input.cadence != EvaluationCadence::Fixed ||
                input.input_index >= descriptor.evaluation->inputs.size() ||
                descriptor.evaluation->inputs[input.input_index].cadence != EvaluationCadence::Fixed ||
                descriptor.evaluation->inputs[input.input_index].type != input.type) return false;
        }
        for(uint16_t target:controller.target_indices) {
            if(target>=descriptor.targets.size() || descriptor.targets[target].driver!=TargetDriverKind::Controller || !owned_targets.insert(target).second) return false;
        }
    }
    AnimationFixedWork work = descriptor.fixed_work;
    work.instance = lease.instance;
    const uint64_t slot_key = animator_key(lease.instance);
    // A control write receives a fresh immutable lease, but it is not a graph
    // replacement.  Retain clock/root/marker runtime state so a target toggle
    // or input write cannot restart the animator.
    const auto existing = fixed_work_.find(slot_key);
    const auto old_lease = service_bindings_.find(slot_key);
    if (existing != fixed_work_.end() && old_lease != service_bindings_.end() &&
        old_lease->second.descriptor.get() == lease.descriptor.get() &&
        old_lease->second.asset_identity == lease.asset_identity) {
        work.clip.time = existing->second.clip.time;
        work.root_previous = existing->second.root_previous;
        work.root_current = existing->second.root_current;
        work.root_sampled = existing->second.root_sampled;
        work.evaluated_target_root_relative = existing->second.evaluated_target_root_relative;
    }
    for (AnimationWorldQueryRequest& query : work.queries) {
        if (query.instance.valid() && animator_key(query.instance) != animator_key(lease.instance)) return false;
        query.instance = lease.instance;
    }
    if (descriptor.target_index != UINT16_MAX) {
        if (descriptor.target_index >= lease.target_transforms.size() ||
            descriptor.target_index >= lease.target_enabled.size()) return false;
        work.desired_target_world = lease.target_transforms[descriptor.target_index];
        work.target_weight = descriptor.target_index < lease.target_weights.size()
            ? lease.target_weights[descriptor.target_index] : 0.0f;
        work.target_enabled = lease.target_enabled[descriptor.target_index] != 0;
    }
    TargetRuntime candidate;
    const auto prior=target_runtime_.find(slot_key);
    if(prior!=target_runtime_.end()) { candidate.targets=prior->second.targets; candidate.desired_world=prior->second.desired_world; }
    if(candidate.targets.size()!=descriptor.targets.size()) {
        candidate.targets.resize(descriptor.targets.size());
        candidate.desired_world.resize(descriptor.targets.size());
        for(size_t i=0;i<candidate.targets.size();++i) candidate.targets[i].enabled=descriptor.targets[i].enabled;
    }
    for(size_t i=0;i<candidate.targets.size();++i) {
        // Controller-owned values are preserved; public API owns external values.
        if(descriptor.targets[i].driver==TargetDriverKind::External) {
            candidate.desired_world[i]=lease.target_transforms[i];
            candidate.targets[i].desired_weight=lease.target_weights[i];
            candidate.targets[i].enabled=lease.target_enabled[i]!=0;
            candidate.targets[i].snap_requested=lease.target_snap_requested.size()==descriptor.targets.size() && lease.target_snap_requested[i]!=0;
        }
        if(!std::isfinite(candidate.targets[i].desired_weight) || candidate.targets[i].desired_weight<0 || candidate.targets[i].desired_weight>1) return false;
    }
    if(prior==target_runtime_.end() || prior->second.controllers.size()!=descriptor.controllers.size() ||
       prior->second.controller_descriptor!=lease.descriptor.get()) {
        NativeControllerRegistry registry=NativeControllerRegistry::with_v1_controllers();
        candidate.controllers.clear();
        for(const auto& controller:descriptor.controllers) {
            NativeControllerLayout layout{}; auto instance=registry.create(controller.descriptor,layout);
            if(!instance || layout.frame_state_bytes!=0 || layout.fixed_state_bytes>64u*1024u) return false;
            candidate.controllers.push_back(std::move(instance));
        }
    } else candidate.controllers=std::move(target_runtime_[slot_key].controllers);
    candidate.controller_descriptor=lease.descriptor.get();
    if (!register_fixed_work(work)) return false;
    service_bindings_[animator_key(lease.instance)] = lease;
    target_runtime_[slot_key]=std::move(candidate);
    return true;
}

bool AnimationSystems::apply_targets(flecs::world& world, AnimatorInstanceHandle instance, EvaluationCadence cadence, double delta_seconds) {
    const uint64_t slot_key=animator_key(instance);
    const auto binding=service_bindings_.find(slot_key); const auto runtime=target_runtime_.find(slot_key);
    if(binding==service_bindings_.end() || runtime==target_runtime_.end() || !binding->second.descriptor || !binding->second.descriptor->evaluation) return false;
    const auto& targets=binding->second.descriptor->targets;
    if(targets.size()!=runtime->second.targets.size()) return false;
    std::vector<AnimationTargetState> candidate=runtime->second.targets;
    const auto work=fixed_work_.find(slot_key);
    const ecs::WorldTransform* root_transform=nullptr;
    if (work != fixed_work_.end() && work->second.root_entity != 0)
        root_transform=world.entity(work->second.root_entity).try_get<ecs::WorldTransform>();
    for(size_t i=0;i<targets.size();++i) {
        const bool mismatch=targets[i].cadence!=cadence;
        if(mismatch) continue;
        if (i >= runtime->second.desired_world.size()) return false;
        if (root_transform != nullptr && !resolve_world_target(root_transform->matrix, runtime->second.desired_world[i], candidate[i].desired)) return false;
        if (root_transform == nullptr) candidate[i].desired=runtime->second.desired_world[i];
        if(!smooth_animation_target(targets[i],candidate[i],delta_seconds,cadence)) return false;
    }
    const ecs::AnimationFrameState frame{}; // frame serial is filled by caller's current snapshot path below.
    (void)frame;
    runtime->second.targets=std::move(candidate);
    return true;
}

bool AnimationSystems::copy_animation_debug_pose(AnimatorInstanceHandle instance,
                                                 AnimationDebugPoseSnapshot& out) const {
    out = {};
    if (!instance.valid()) return false;
    const auto binding = service_bindings_.find(animator_key(instance));
    const auto runtime = target_runtime_.find(animator_key(instance));
    if (binding == service_bindings_.end() || runtime == target_runtime_.end() ||
        !binding->second.descriptor ||
        binding->second.instance.slot_index != instance.slot_index ||
        binding->second.instance.generation != instance.generation ||
        runtime->second.targets.size() != binding->second.descriptor->targets.size()) {
        return false;
    }
    const AnimationPoseSnapshot pose = pose_snapshots_.latest(instance);
    if (!pose.instance.valid() || pose.instance.generation != instance.generation ||
        !pose.model_pose.data || !pose.skin_palette.data) {
        return false;
    }
    AnimationDebugPoseSnapshot candidate{};
    candidate.instance = instance;
    candidate.fixed_tick = pose.fixed_tick;
    candidate.frame_serial = pose.frame_serial;
    if (pose.local_pose.data) {
        candidate.local_pose.assign(pose.local_pose.data,
                                    pose.local_pose.data + pose.local_pose.count);
    }
    candidate.model_pose.assign(pose.model_pose.data,
                                pose.model_pose.data + pose.model_pose.count);
    candidate.skin_palette.assign(pose.skin_palette.data,
                                  pose.skin_palette.data + pose.skin_palette.count);
    candidate.targets.reserve(runtime->second.targets.size());
    for (const AnimationTargetState& target : runtime->second.targets) {
        candidate.targets.push_back(
            {target.evaluated, target.evaluated_weight, target.enabled, true});
    }
    out = std::move(candidate);
    return true;
}

void AnimationSystems::detach_service_binding(AnimatorInstanceHandle instance) {
    if (!instance.valid()) return;
    remove_fixed_work(instance);
    pose_snapshots_.forget(instance);
    last_complete_presentation_pose_snapshots_.forget(instance);
    fixed_pose_snapshots_.forget(instance);
    previous_fixed_pose_snapshots_.forget(instance);
    evaluator_.forget(instance);
    presentation_evaluator_.forget(instance);
    pose_lod_scheduler_.forget(animator_key(instance));
    completed_visibility_.erase(animator_key(instance));
    staged_visibility_.erase(animator_key(instance));
    service_bindings_.erase(animator_key(instance));
    target_runtime_.erase(animator_key(instance));
}

bool AnimationSystems::capture_service_checkpoint(AnimatorCheckpoint& checkpoint) const {
    const auto binding=service_bindings_.find(animator_key(checkpoint.instance));
    const auto work=fixed_work_.find(animator_key(checkpoint.instance));
    if(binding==service_bindings_.end() || work==fixed_work_.end() ||
       binding->second.asset_identity!=checkpoint.asset_identity ||
       binding->second.descriptor_identity!=checkpoint.descriptor_identity) return false;
    checkpoint.fixed_clip_time=work->second.clip.time;
    checkpoint.fixed_root_previous=work->second.root_previous;
    checkpoint.fixed_root_current=work->second.root_current;
    checkpoint.fixed_root_sampled=work->second.root_sampled;
    checkpoint.desired_target=work->second.desired_target_world;
    checkpoint.evaluated_target=work->second.evaluated_target_root_relative;
    checkpoint.target_weight=work->second.target_weight;
    checkpoint.target_enabled=work->second.target_enabled;
    const auto runtime=target_runtime_.find(animator_key(checkpoint.instance));
    if(runtime==target_runtime_.end()) return false;
    checkpoint.target_desired.clear(); checkpoint.target_weights.clear(); checkpoint.target_enabled_states.clear(); checkpoint.target_snap_requested_states.clear();
    checkpoint.target_evaluated_states.clear(); checkpoint.target_evaluated_weights.clear(); checkpoint.native_controller_checkpoints.clear();
    for(const auto& target:runtime->second.targets) { checkpoint.target_desired.push_back(target.desired); checkpoint.target_weights.push_back(target.desired_weight); checkpoint.target_enabled_states.push_back(target.enabled?1u:0u); checkpoint.target_snap_requested_states.push_back(target.snap_requested?1u:0u); checkpoint.target_evaluated_states.push_back(target.evaluated); checkpoint.target_evaluated_weights.push_back(target.evaluated_weight); }
    for(const auto& controller:runtime->second.controllers) { std::vector<uint8_t> state; if(!controller || !controller->checkpoint(state) || state.size()>64u*1024u) return false; checkpoint.native_controller_checkpoints.push_back(std::move(state)); }
    // Marker crossings are reconstructed from evaluator previous/current graph
    // time.  A descriptor cursor would be a second, stale marker clock.
    checkpoint.marker_cursors.clear();
    // An evaluator may not have run yet when Play begins.  That is a valid
    // bind-pose checkpoint; restore reconstructs transient contexts lazily.
    (void)evaluator_.capture_checkpoint(checkpoint.instance, checkpoint);
    return checkpoint.bounded();
}

bool AnimationSystems::validate_service_checkpoint(const AnimatorCheckpoint& checkpoint) const {
    const auto binding=service_bindings_.find(animator_key(checkpoint.instance));
    const auto work=fixed_work_.find(animator_key(checkpoint.instance));
    if(binding==service_bindings_.end() || work==fixed_work_.end() || !binding->second.valid() ||
       !binding->second.descriptor || !binding->second.descriptor->evaluation ||
       binding->second.asset_identity!=checkpoint.asset_identity ||
       binding->second.descriptor_identity!=checkpoint.descriptor_identity || !checkpoint.bounded()) return false;
    if(!std::isfinite(checkpoint.fixed_clip_time) || !std::isfinite(checkpoint.target_weight) ||
       !finite_transform(checkpoint.fixed_root_previous) || !finite_transform(checkpoint.fixed_root_current) ||
       !finite_transform(checkpoint.desired_target) || !finite_transform(checkpoint.evaluated_target)) return false;
    const auto runtime=target_runtime_.find(animator_key(checkpoint.instance));
    if(runtime==target_runtime_.end() || !checkpoint.marker_cursors.empty() || checkpoint.target_evaluated_states.size()!=runtime->second.targets.size() || checkpoint.target_evaluated_weights.size()!=runtime->second.targets.size() || checkpoint.native_controller_checkpoints.size()!=runtime->second.controllers.size()) return false;
    for(size_t i=0;i<runtime->second.targets.size();++i) if(!finite_transform(checkpoint.target_evaluated_states[i]) || !std::isfinite(checkpoint.target_evaluated_weights[i]) || checkpoint.target_evaluated_weights[i]<0||checkpoint.target_evaluated_weights[i]>1) return false;
    return evaluator_.validate_checkpoint(checkpoint.instance,*binding->second.descriptor->evaluation,checkpoint);
}

bool AnimationSystems::restore_service_checkpoint(const AnimatorCheckpoint& checkpoint) {
    if(!validate_service_checkpoint(checkpoint)) return false;
    const uint64_t slot_key=animator_key(checkpoint.instance);
    const AnimationRuntimeBindingLease& binding=service_bindings_.at(slot_key);
    const AnimationFixedWork previous_work=fixed_work_.at(slot_key);
    AnimationFixedWork restored=previous_work;
    restored.clip.time=checkpoint.fixed_clip_time;
    restored.root_previous=checkpoint.fixed_root_previous;
    restored.root_current=checkpoint.fixed_root_current;
    restored.root_sampled=checkpoint.fixed_root_sampled;
    restored.desired_target_world=checkpoint.desired_target;
    restored.evaluated_target_root_relative=checkpoint.evaluated_target;
    restored.target_weight=checkpoint.target_weight;
    restored.target_enabled=checkpoint.target_enabled;
    TargetRuntime& candidate=target_runtime_.at(slot_key);
    std::vector<std::vector<uint8_t>> previous_controller_state; previous_controller_state.reserve(candidate.controllers.size());
    for(const auto& controller:candidate.controllers) { std::vector<uint8_t> bytes; if(!controller || !controller->checkpoint(bytes)) return false; previous_controller_state.push_back(std::move(bytes)); }
    for(size_t i=0;i<candidate.controllers.size();++i) if(!candidate.controllers[i]->restore(checkpoint.native_controller_checkpoints[i])) { for(size_t j=0;j<i;++j) (void)candidate.controllers[j]->restore(previous_controller_state[j]); return false; }
    const std::vector<AnimationTargetState> previous_targets=candidate.targets;
    for(size_t i=0;i<candidate.targets.size();++i) { candidate.targets[i].desired=checkpoint.target_desired[i]; candidate.targets[i].desired_weight=checkpoint.target_weights[i]; candidate.targets[i].enabled=checkpoint.target_enabled_states[i]!=0; candidate.targets[i].snap_requested=checkpoint.target_snap_requested_states[i]!=0; candidate.targets[i].evaluated=checkpoint.target_evaluated_states[i]; candidate.targets[i].evaluated_weight=checkpoint.target_evaluated_weights[i]; }
    if(!register_fixed_work(restored)) { candidate.targets=previous_targets; for(size_t i=0;i<candidate.controllers.size();++i) (void)candidate.controllers[i]->restore(previous_controller_state[i]); return false; }
    if(!evaluator_.restore_checkpoint(checkpoint.instance,*binding.descriptor->evaluation,checkpoint)) { (void)register_fixed_work(previous_work); candidate.targets=previous_targets; for(size_t i=0;i<candidate.controllers.size();++i) (void)candidate.controllers[i]->restore(previous_controller_state[i]); return false; }
    const AnimationPoseSnapshot pose=evaluator_.snapshot(checkpoint.instance);
    if(pose.instance.valid()) {
        // Checkpoints contain the fixed evaluator state, not a transient
        // frame layer.  Re-seed both interpolation endpoints transactionally
        // so replay cannot present a pose from the abandoned timeline.
        (void)fixed_pose_snapshots_.publish(pose);
        (void)previous_fixed_pose_snapshots_.publish(pose);
        presentation_evaluator_.forget(checkpoint.instance);
        (void)pose_snapshots_.publish(pose);
    } else {
        fixed_pose_snapshots_.forget(checkpoint.instance);
        previous_fixed_pose_snapshots_.forget(checkpoint.instance);
        presentation_evaluator_.forget(checkpoint.instance);
        pose_snapshots_.forget(checkpoint.instance);
    }
    return true;
}

void AnimationSystems::sample_service_bindings() {
    // Fixed API writes become observable only at this boundary.  The service
    // publishes both the prior and newly captured samples in one immutable
    // lease, so interpolation never races a gameplay write.
    if (service_ != nullptr) (void)service_->sample_fixed_controls();
}

void AnimationSystems::evaluate_service_bindings(flecs::world& world, double delta_seconds,
                                                 float accumulator_alpha) {
    const ecs::AnimationFixedState fixed = world.get<ecs::AnimationFixedState>();
    const ecs::AnimationFrameState frame = world.get<ecs::AnimationFrameState>();
    std::vector<AnimationEvaluationRequest> requests;
    std::vector<std::vector<AnimationValue>> previous_values;
    std::vector<std::vector<AnimationValue>> current_values;
    std::vector<std::vector<AnimationValue>> frame_values;
    requests.reserve(service_bindings_.size());
    previous_values.reserve(service_bindings_.size()); current_values.reserve(service_bindings_.size()); frame_values.reserve(service_bindings_.size());
    const auto convert = [](const std::vector<AnimationRuntimeBindingLease::Value>& values) {
        std::vector<AnimationValue> result; result.reserve(values.size());
        for (const auto& value : values) {
            switch (value.type) {
                case AnimationValueType::Bool: result.emplace_back(value.boolean); break;
                case AnimationValueType::Number: result.emplace_back(value.number); break;
                case AnimationValueType::Float3: result.emplace_back(value.float3); break;
                case AnimationValueType::Quaternion: result.emplace_back(value.quaternion); break;
                case AnimationValueType::Transform: result.emplace_back(value.transform); break;
                case AnimationValueType::Symbol: { AnimationValue symbol{}; symbol.type = AnimationValueType::Symbol; symbol.symbol = std::to_string(value.symbol); result.push_back(std::move(symbol)); break; }
            }
        }
        return result;
    };
    for (const auto& item : service_bindings_) {
        const AnimationRuntimeBindingLease& lease = item.second;
        if (!lease.valid() || !lease.descriptor || !lease.descriptor->evaluation) continue;
        AnimationEvaluationRequest request{};
        request.instance = lease.instance;
        request.definition = lease.descriptor->evaluation.get();
        previous_values.push_back(convert(lease.fixed_previous)); current_values.push_back(convert(lease.fixed_current)); frame_values.push_back(convert(lease.frame_controls));
        request.fixed_previous = {previous_values.back().data(), static_cast<uint32_t>(previous_values.back().size())};
        request.fixed_current = {current_values.back().data(), static_cast<uint32_t>(current_values.back().size())};
        request.frame_controls = {frame_values.back().data(), static_cast<uint32_t>(frame_values.back().size())};
        request.fixed_tick = fixed.current_tick;
        request.frame_serial = frame.frame_serial;
        request.fixed_delta_seconds = static_cast<float>(delta_seconds);
        request.accumulator_alpha = accumulator_alpha;
        const auto work = fixed_work_.find(item.first);
        request.root_lock = work != fixed_work_.end() && work->second.root_entity != 0;
        requests.push_back(request);
    }
    if (!requests.empty()) (void)evaluator_.evaluate(requests);
    for (const AnimationEvaluationRequest& request : requests) {
        const AnimationPoseSnapshot snapshot = evaluator_.snapshot(request.instance);
        if (snapshot.instance.valid() && snapshot.frame_serial == frame.frame_serial) (void)pose_snapshots_.publish(snapshot);
    }
}

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
    // MatterEngine matrices are row-major and store translation in the last
    // column (m[3], m[7], m[11]).  Keep this conversion in the engine's
    // convention rather than silently interpreting WorldTransform as GLM.
    const float x = inverse_root.m[0] * point.x + inverse_root.m[1] * point.y + inverse_root.m[2] * point.z + inverse_root.m[3];
    const float y = inverse_root.m[4] * point.x + inverse_root.m[5] * point.y + inverse_root.m[6] * point.z + inverse_root.m[7];
    const float z = inverse_root.m[8] * point.x + inverse_root.m[9] * point.y + inverse_root.m[10] * point.z + inverse_root.m[11];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    out_root_relative = desired_world;
    out_root_relative.translation = {x, y, z};
    const Quaternion root = matrix_rotation(current_root_world);
    out_root_relative.rotation = multiply_quaternion({-root.x, -root.y, -root.z, root.w}, desired_world.rotation);
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
    sample_service_bindings();
    trace(AnimationScheduleEvent::FixedSampleApiWrites, fixed_delta);
    trace(AnimationScheduleEvent::FixedAdvanceClocks, fixed_delta);
}

void AnimationSystems::run_fixed_update(flecs::world& world, double fixed_delta) {
    trace(AnimationScheduleEvent::FixedSampleRootChannels, fixed_delta);
    struct OrderedMarker { AnimatorInstanceHandle instance; uint16_t node; uint16_t clip; AnimationMarkerEvent event; };
    std::vector<OrderedMarker> emitted;
    // FixedUpdate always samples the current fixed state.  Presentation
    // interpolation belongs exclusively to FrameUpdate.
    evaluate_service_bindings(world, fixed_delta, 1.0f);
    for (auto& pair : fixed_work_) {
        AnimationFixedWork& work = pair.second;
        std::vector<RuntimeGraphClipAdvance> advances;
        if (service_bindings_.find(pair.first) != service_bindings_.end() &&
            evaluator_.fixed_graph_clips(work.instance, advances)) {
            const AnimationEvaluationDefinition* definition = service_bindings_.at(pair.first).descriptor->evaluation.get();
            for (const RuntimeGraphClipAdvance& advance : advances) {
                if (definition == nullptr || advance.clip_index >= definition->clips.size()) continue;
                const RuntimeGraphClip& clip = definition->clips[advance.clip_index];
                // Compatibility/checkpoint metadata is derived from the
                // graph's shared clock; it never feeds evaluation.
                if (clip.rate != 0.0f) work.clip.time = advance.current_time / clip.rate;
                std::vector<AnimationMarkerEvent> local;
                emit_crossed_markers(work.instance, {clip.markers.data(), uint32_t(clip.markers.size())},
                                     clip.duration, clip.loop, advance.previous_time, advance.current_time, local);
                for (const AnimationMarkerEvent& event : local)
                    emitted.push_back({work.instance, advance.node_index, advance.clip_index, event});
            }
        } else {
            // Direct fixed-work registration is a deliberately narrow test/tool
            // seam.  Service-bound animators never use this descriptor clock.
            const float previous = work.clip.time;
            const float current = previous + work.clip.rate * static_cast<float>(fixed_delta);
            std::vector<AnimationMarkerEvent> local;
            emit_runtime_markers(work.instance, work.clip.markers, work.clip.duration, work.clip.loop, previous, current, local);
            for (const auto& event : local) emitted.push_back({work.instance, work.clip.node_index, work.clip.clip_index, event});
            work.clip.time = current;
        }
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
        if (service_bindings_.find(pair.first) != service_bindings_.end()) {
            if (!evaluator_.fixed_root_motion(work.instance, motion)) continue;
        } else {
            motion.delta = runtime_root_delta(work.root_previous, work.root_current);
            motion.valid = true;
        }
        (void)publish_desired_root_motion(work.instance, motion, tick);
    }
    trace(AnimationScheduleEvent::FixedEmitMarkers, fixed_delta);
}

void AnimationSystems::run_pre_physics(flecs::world& world, double fixed_delta) {
    const uint64_t tick = world.get<ecs::AnimationFixedState>().current_tick;
    for (const auto& pair : fixed_work_) {
        DesiredRootMotion motion{};
        if (!consume_desired_root_motion(pair.second.instance, tick, motion)) continue;
        // Root motion has a single authority: this pre-physics boundary.  It
        // writes the authored root transform before physics pushes kinematic
        // bodies, so there is no test-only side channel or second consumer.
        if (pair.second.root_entity != 0) {
            flecs::entity root = world.entity(pair.second.root_entity);
            if (ecs::LocalTransform* local = root.try_get_mut<ecs::LocalTransform>()) {
                local->translation.x += motion.delta.translation.x;
                local->translation.y += motion.delta.translation.y;
                local->translation.z += motion.delta.translation.z;
                local->rotation = multiply_quaternion(motion.delta.rotation, local->rotation);
                root.modified<ecs::LocalTransform>();
                root.add<ecs::TransformDirty>();
            }
        }
        consumed_root_motion_.push_back(motion);
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
    // Controller execution is deterministic by (declared priority, animator
    // handle, declaration order). Each controller produces a complete write
    // batch; a rejected batch leaves all target state unchanged.
    struct ControllerJob { int32_t priority; uint64_t key; uint16_t order; };
    std::vector<ControllerJob> jobs;
    for(const auto& pair:service_bindings_) if(pair.second.descriptor) for(uint16_t i=0;i<pair.second.descriptor->controllers.size();++i) jobs.push_back({pair.second.descriptor->controllers[i].priority,pair.first,i});
    std::stable_sort(jobs.begin(),jobs.end(),[](const ControllerJob&a,const ControllerJob&b){if(a.priority!=b.priority)return a.priority<b.priority;if(a.key!=b.key)return a.key<b.key;return a.order<b.order;});
    // Controllers receive only this deterministic, per-tick broker.  Their
    // sorted execution order is the broker admission order; overflow is an
    // explicit no-hit rather than an unsafe direct query bypass.
    ControllerQueryBroker controller_queries(world_queries_, world_query_overflow_count_);
    for(const ControllerJob& job:jobs) {
        auto& runtime=target_runtime_[job.key]; const auto& binding=service_bindings_.at(job.key); const auto& spec=binding.descriptor->controllers[job.order];
        if(job.order>=runtime.controllers.size() || !runtime.controllers[job.order]) continue;
        NativeControllerContext context{}; context.fixed_delta_seconds=fixed_delta; context.world_queries=&controller_queries;
        const auto work=fixed_work_.find(job.key);
        if (work != fixed_work_.end() && work->second.root_entity != 0) {
            const ecs::WorldTransform* transform=world.entity(work->second.root_entity).try_get<ecs::WorldTransform>();
            if (transform != nullptr) {
                context.entity_world_origin={transform->matrix.m[3],transform->matrix.m[7],transform->matrix.m[11]};
                context.entity_world_rotation=matrix_rotation(transform->matrix);
            }
        }
        // Snapshot only this controller's declared fixed controls before it
        // executes.  This both fixes ordering and prevents controllers from
        // observing unrelated/frame-rate state through the runtime bridge.
        bool inputs_valid = true;
        context.inputs.reserve(spec.inputs.size());
        for (const auto& input : spec.inputs) {
            if (input.cadence != EvaluationCadence::Fixed ||
                input.input_index >= binding.fixed_current.size() ||
                binding.fixed_current[input.input_index].type != input.type) {
                inputs_valid = false;
                break;
            }
            AnimationValue value{};
            if (!controller_input_value(binding.fixed_current[input.input_index], value) || value.type != input.type) {
                inputs_valid = false;
                break;
            }
            context.inputs.push_back(std::move(value));
        }
        if (!inputs_valid) continue;
        if(!runtime.controllers[job.order]->fixed_update(context)) continue;
        std::vector<AnimationTargetState> candidate=runtime.targets; bool valid=true;
        for(const ControllerTargetWrite& write:context.writes) {
            if(write.target_index>=candidate.size() || std::find(spec.target_indices.begin(),spec.target_indices.end(),write.target_index)==spec.target_indices.end() ||
               !std::isfinite(write.weight) || write.weight<0||write.weight>1 || !finite_transform(write.transform)) {valid=false;break;}
            candidate[write.target_index].desired=write.transform; candidate[write.target_index].desired_weight=write.weight; candidate[write.target_index].enabled=true;
        }
        if(valid) {
            if (runtime.desired_world.size() != candidate.size()) continue;
            for(const ControllerTargetWrite& write:context.writes) runtime.desired_world[write.target_index]=write.transform;
            runtime.targets=std::move(candidate);
        }
    }
    trace(AnimationScheduleEvent::FixedWorldQueries, fixed_delta);
    std::vector<AnimationWorldQueryRequest> queries;
    for (const auto& pair : fixed_work_)
        queries.insert(queries.end(), pair.second.queries.begin(), pair.second.queries.end());
    (void)execute_fixed_world_queries(std::move(queries));
    trace(AnimationScheduleEvent::FixedSmoothTargets, fixed_delta);
    for (auto& pair : fixed_work_) {
        AnimationFixedWork& work = pair.second;
        if (work.root_entity == 0 || !work.target_enabled) continue;
        const flecs::entity root = world.entity(work.root_entity);
        const ecs::WorldTransform* transform = root.try_get<ecs::WorldTransform>();
        if (transform == nullptr || !resolve_world_target(transform->matrix, work.desired_target_world,
                                                           work.evaluated_target_root_relative)) continue;
    }
    for(const auto& pair:service_bindings_) {
        if (!pair.second.descriptor || !pair.second.descriptor->evaluation ||
            !has_targets_for_cadence(pair.second.descriptor->targets, EvaluationCadence::Fixed)) continue;
        if (!apply_targets(world,pair.second.instance,EvaluationCadence::Fixed,fixed_delta)) continue;
        const auto runtime=target_runtime_.find(pair.first); if(runtime==target_runtime_.end() || runtime->second.targets.empty()) continue;
        const ecs::AnimationFrameState frame=world.get<ecs::AnimationFrameState>();
        const auto phase_targets=targets_for_cadence(pair.second.descriptor->targets,runtime->second.targets,EvaluationCadence::Fixed);
        if(evaluator_.solve_targets(pair.second.instance,*pair.second.descriptor->evaluation,pair.second.descriptor->targets,phase_targets,frame.frame_serial)) {
            const AnimationPoseSnapshot pose=evaluator_.snapshot(pair.second.instance); if(pose.instance.valid()) (void)pose_snapshots_.publish(pose);
        }
    }
    // Preserve the complete solved result of each fixed step before any frame
    // presentation work.  The two stores form the interpolation source and
    // are intentionally not part of renderer publication/checkpoint state.
    for (const auto& pair : service_bindings_) {
        const AnimationPoseSnapshot solved=evaluator_.snapshot(pair.second.instance);
        if (!solved.instance.valid()) continue;
        const AnimationPoseSnapshot prior=fixed_pose_snapshots_.latest(pair.second.instance);
        if (prior.instance.valid()) (void)previous_fixed_pose_snapshots_.publish(prior);
        else (void)previous_fixed_pose_snapshots_.publish(solved);
        (void)fixed_pose_snapshots_.publish(solved);
    }
    trace(AnimationScheduleEvent::FixedPublishSnapshot, fixed_delta);
}

void AnimationSystems::run_frame(flecs::world& world, double frame_delta) {
    ecs::AnimationFrameState state = world.get<ecs::AnimationFrameState>();
    ++state.frame_serial;
    state.interpolation_alpha = interpolation_alpha_;
    world.set<ecs::AnimationFrameState>(state);
    if (std::isfinite(frame_delta) && frame_delta > 0.0)
        presentation_time_seconds_ += frame_delta;
    if (service_ != nullptr) (void)service_->sample_frame_controls();
    trace(AnimationScheduleEvent::FrameSampleApiWrites, frame_delta);
    trace(AnimationScheduleEvent::FrameInterpolateFixedState, frame_delta);
    trace(AnimationScheduleEvent::FrameEvaluatePresentationGraph, frame_delta);
    // Presentation is an owned copy of the already solved fixed state.  It
    // never samples the graph again on wall-frame time: doing so would both
    // overwrite fixed controller/IK output and advance evaluator history.
    std::map<uint64_t, PoseLodDecision> pose_lod_decisions;
    for (const auto& pair : service_bindings_) {
        if (!pair.second.descriptor || !pair.second.descriptor->evaluation) continue;
        AnimationPresentationObservation observation{pair.second.instance, true, 0.0f, 0};
        const auto observed = completed_visibility_.find(pair.first);
        if (observed != completed_visibility_.end()) observation = observed->second;
        const PoseLodDecision decision = pose_lod_scheduler_.schedule(
            {pair.first, observation.visible, observation.distance,
             observation.explicit_priority, presentation_time_seconds_,
             state.frame_serial});
        pose_lod_decisions.emplace(pair.first, decision);
        if (!decision.evaluate_now) {
            AnimationPoseSnapshot fallback =
                last_complete_presentation_pose_snapshots_.latest(pair.second.instance);
            if (!fallback.instance.valid())
                fallback = fixed_pose_snapshots_.latest(pair.second.instance);
            if (fallback.instance.valid()) {
                fallback.frame_serial = state.frame_serial;
                (void)pose_snapshots_.publish(fallback);
                ++presentation_budget_stats_.last_complete_fallback_count;
            } else {
                ++presentation_budget_stats_.bind_pose_fallback_count;
            }
            ++presentation_budget_stats_.frozen_pose_count;
            continue;
        }
        if (decision.resample_current_graph_time)
            ++presentation_budget_stats_.resampled_pose_count;
        const AnimationPoseSnapshot fixed = fixed_pose_snapshots_.latest(pair.second.instance);
        const AnimationPoseSnapshot previous = previous_fixed_pose_snapshots_.latest(pair.second.instance);
        float fixed_previous_time=0.0f, fixed_current_time=0.0f;
        if (!fixed.instance.valid() || !previous.instance.valid() ||
            !evaluator_.fixed_clock(pair.second.instance,fixed_previous_time,fixed_current_time) ||
            !presentation_evaluator_.seed_presentation_clock(pair.second.instance,*pair.second.descriptor->evaluation,
                                                               fixed.fixed_tick,fixed_previous_time,fixed_current_time)) continue;
        std::vector<AnimationValue> previous_controls, current_controls, frame_controls;
        const auto append_controls=[](const std::vector<AnimationRuntimeBindingLease::Value>& source,
                                      std::vector<AnimationValue>& out) {
            out.reserve(source.size());
            for (const auto& value : source) switch (value.type) {
                case AnimationValueType::Bool: out.emplace_back(value.boolean); break;
                case AnimationValueType::Number: out.emplace_back(value.number); break;
                case AnimationValueType::Float3: out.emplace_back(value.float3); break;
                case AnimationValueType::Quaternion: out.emplace_back(value.quaternion); break;
                case AnimationValueType::Transform: out.emplace_back(value.transform); break;
                case AnimationValueType::Symbol: { AnimationValue symbol{}; symbol.type=AnimationValueType::Symbol; symbol.symbol=std::to_string(value.symbol); out.push_back(std::move(symbol)); break; }
            }
        };
        append_controls(pair.second.fixed_previous,previous_controls);
        append_controls(pair.second.fixed_current,current_controls);
        append_controls(pair.second.frame_controls,frame_controls);
        AnimationEvaluationRequest request{};
        request.instance=pair.second.instance; request.definition=pair.second.descriptor->evaluation.get();
        request.fixed_previous={previous_controls.data(),uint32_t(previous_controls.size())};
        request.fixed_current={current_controls.data(),uint32_t(current_controls.size())};
        request.frame_controls={frame_controls.data(),uint32_t(frame_controls.size())};
        request.fixed_tick=fixed.fixed_tick; request.frame_serial=state.frame_serial;
        request.accumulator_alpha=static_cast<float>(interpolation_alpha_);
        const auto fixed_work=fixed_work_.find(pair.first);
        request.root_lock=fixed_work!=fixed_work_.end() && fixed_work->second.root_entity!=0;
        if (!presentation_evaluator_.evaluate({request})) continue;
        // Frame graph evaluation never advances the fixed controller.  It does
        // recompute the graph at the interpolated fixed controls, then layers
        // the already sampled fixed target state before any frame-only IK.
        const auto fixed_targets=targets_for_cadence(pair.second.descriptor->targets,
                                                      target_runtime_[pair.first].targets,EvaluationCadence::Fixed);
        if (!presentation_evaluator_.solve_targets(pair.second.instance,*pair.second.descriptor->evaluation,
                                                    pair.second.descriptor->targets,fixed_targets,state.frame_serial)) continue;
        ++presentation_budget_stats_.evaluated_presentation_pose_count;
    }
    trace(AnimationScheduleEvent::FrameSolveTargetsAndIk, frame_delta);
    for(const auto& pair:service_bindings_) {
        if (!pair.second.descriptor || !pair.second.descriptor->evaluation) continue;
        const auto decision = pose_lod_decisions.find(pair.first);
        if (decision == pose_lod_decisions.end() || !decision->second.evaluate_now) continue;
        const bool has_frame_targets=has_targets_for_cadence(pair.second.descriptor->targets, EvaluationCadence::Frame);
        if (!has_frame_targets) {
            const AnimationPoseSnapshot pose=presentation_evaluator_.snapshot(pair.second.instance);
            if(pose.instance.valid()) {
                (void)last_complete_presentation_pose_snapshots_.publish(pose);
                (void)pose_snapshots_.publish(pose);
            }
            continue;
        }
        if (has_frame_targets && !apply_targets(world,pair.second.instance,EvaluationCadence::Frame,frame_delta)) continue;
        const auto runtime=target_runtime_.find(pair.first); if(runtime==target_runtime_.end() || runtime->second.targets.empty()) continue;
        const auto phase_targets=targets_for_cadence(pair.second.descriptor->targets,runtime->second.targets,EvaluationCadence::Frame);
        if(presentation_evaluator_.solve_targets(pair.second.instance,*pair.second.descriptor->evaluation,pair.second.descriptor->targets,phase_targets,state.frame_serial)) {
            const AnimationPoseSnapshot pose=presentation_evaluator_.snapshot(pair.second.instance);
            if(pose.instance.valid()) {
                (void)last_complete_presentation_pose_snapshots_.publish(pose);
                (void)pose_snapshots_.publish(pose);
            }
        }
    }
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
