#include "animation/animation_budget.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace matter::animation {

bool AnimationBudgetConfig::valid() const noexcept {
    return max_assets > 0 && max_assets <= kHardMaxAssets &&
           max_runtime_instances > 0 && max_runtime_instances <= kHardMaxRuntimeInstances &&
           max_joints_per_asset > 0 && max_joints_per_asset <= kHardMaxJointsPerAsset &&
           max_graph_nodes > 0 && max_graph_nodes <= kHardMaxGraphNodes &&
           max_controller_nodes <= kHardMaxControllerNodes &&
           max_skin_work_items > 0 && max_skin_work_items <= kHardMaxSkinWorkItems &&
           max_skinned_vertices > 0 && max_skinned_vertices <= kHardMaxSkinnedVertices &&
           max_mutable_bytes > 0 && max_mutable_bytes <= kHardMaxMutableBytes &&
           std::isfinite(near_distance) && std::isfinite(mid_distance) &&
           std::isfinite(frozen_distance) && std::isfinite(distance_hysteresis) &&
           near_distance >= 0.0f && mid_distance > near_distance &&
           frozen_distance > mid_distance && distance_hysteresis >= 0.0f;
}

void AnimationBudgetRuntimeStats::record_fallback(AnimationFallbackReason reason) noexcept {
    if (reason == AnimationFallbackReason::None ||
        reason >= AnimationFallbackReason::Count) return;
    ++fallback_count;
    ++fallbacks[static_cast<size_t>(reason)];
}

void AnimationBudgetRuntimeStats::merge(
    const AnimationBudgetRuntimeStats& other) noexcept {
    evaluated_pose_count += other.evaluated_pose_count;
    evaluated_joint_count += other.evaluated_joint_count;
    submitted_skin_work_items += other.submitted_skin_work_items;
    submitted_skinned_vertices += other.submitted_skinned_vertices;
    evaluated_presentation_pose_count += other.evaluated_presentation_pose_count;
    frozen_pose_count += other.frozen_pose_count;
    resampled_pose_count += other.resampled_pose_count;
    last_complete_fallback_count += other.last_complete_fallback_count;
    bind_pose_fallback_count += other.bind_pose_fallback_count;
    fallback_count += other.fallback_count;
    for (size_t i = 0; i < fallbacks.size(); ++i) fallbacks[i] += other.fallbacks[i];
}

PoseLodScheduler::PoseLodScheduler(AnimationBudgetConfig config)
    : config_(config.valid() ? config : AnimationBudgetConfig{}) {}

double PoseLodScheduler::interval(AnimationPoseLodTier tier) noexcept {
    switch (tier) {
        case AnimationPoseLodTier::Hz60: return 1.0 / 60.0;
        case AnimationPoseLodTier::Hz30: return 1.0 / 30.0;
        case AnimationPoseLodTier::Hz15: return 1.0 / 15.0;
        case AnimationPoseLodTier::Frozen: return std::numeric_limits<double>::infinity();
    }
    return std::numeric_limits<double>::infinity();
}

AnimationPoseLodTier PoseLodScheduler::select_tier(AnimationPoseLodTier previous,
                                                    float distance) const noexcept {
    if (!std::isfinite(distance) || distance < 0.0f) return AnimationPoseLodTier::Frozen;
    const float h = config_.distance_hysteresis;
    switch (previous) {
        case AnimationPoseLodTier::Hz60:
            if (distance <= config_.near_distance + h) return AnimationPoseLodTier::Hz60;
            return distance <= config_.mid_distance ? AnimationPoseLodTier::Hz30 :
                   (distance <= config_.frozen_distance ? AnimationPoseLodTier::Hz15 : AnimationPoseLodTier::Frozen);
        case AnimationPoseLodTier::Hz30:
            if (distance < config_.near_distance - h) return AnimationPoseLodTier::Hz60;
            if (distance <= config_.mid_distance + h) return AnimationPoseLodTier::Hz30;
            return distance <= config_.frozen_distance ? AnimationPoseLodTier::Hz15 : AnimationPoseLodTier::Frozen;
        case AnimationPoseLodTier::Hz15:
            if (distance < config_.near_distance - h) return AnimationPoseLodTier::Hz60;
            if (distance < config_.mid_distance - h) return AnimationPoseLodTier::Hz30;
            if (distance <= config_.frozen_distance + h) return AnimationPoseLodTier::Hz15;
            return AnimationPoseLodTier::Frozen;
        case AnimationPoseLodTier::Frozen:
            if (distance >= config_.frozen_distance - h) return AnimationPoseLodTier::Frozen;
            if (distance < config_.near_distance) return AnimationPoseLodTier::Hz60;
            return distance < config_.mid_distance ? AnimationPoseLodTier::Hz30 : AnimationPoseLodTier::Hz15;
    }
    return AnimationPoseLodTier::Frozen;
}

PoseLodDecision PoseLodScheduler::schedule(const PoseLodRequest& request) {
    PoseLodDecision result{};
    if (request.instance_key == 0 || !std::isfinite(request.presentation_seconds) ||
        request.presentation_seconds < 0.0) return result;
    State& state = states_[request.instance_key];
    if (state.last_frame_serial == request.frame_serial) return state.last_decision;
    state.last_frame_serial = request.frame_serial;
    if (!request.visible) {
        state.visible = false;
        state.visible_grace_frames = 0;
        state.tier = AnimationPoseLodTier::Frozen;
        state.next_due_seconds = request.presentation_seconds;
        state.last_decision = result;
        return result;
    }
    const bool became_visible = !state.visible;
    if (became_visible) {
        state.visible = true;
        state.visible_grace_frames = 2;
        state.tier = AnimationPoseLodTier::Hz60;
        state.next_due_seconds = request.presentation_seconds;
    }
    AnimationPoseLodTier selected = AnimationPoseLodTier::Hz60;
    if (state.visible_grace_frames == 0) {
        selected = state.has_post_grace_tier
            ? state.post_grace_tier : select_tier(state.tier, request.distance);
        state.has_post_grace_tier = false;
    }
    result.resample_current_graph_time =
        became_visible ||
        static_cast<uint8_t>(selected) < static_cast<uint8_t>(state.tier);
    state.tier = selected;
    result.tier = selected;
    result.newly_visible = state.visible_grace_frames > 0;
    result.evaluate_now = selected != AnimationPoseLodTier::Frozen &&
                          request.presentation_seconds + 1e-9 >= state.next_due_seconds;
    if (result.evaluate_now) {
        state.next_due_seconds = request.presentation_seconds + interval(selected);
    }
    if (state.visible_grace_frames > 0 && --state.visible_grace_frames == 0) {
        // The forced 60 Hz grace is not an actual distance-band decision.
        // Seed the next frame from the raw bands so a newly visible far item
        // does not inherit 60 Hz hysteresis indefinitely.
        state.post_grace_tier = select_tier(AnimationPoseLodTier::Frozen, request.distance);
        state.has_post_grace_tier = true;
    }
    state.last_decision = result;
    return result;
}

void PoseLodScheduler::forget(uint64_t instance_key) noexcept { states_.erase(instance_key); }

AnimationPoseSource select_pose_fallback(AnimationPoseAvailability availability) noexcept {
    return availability.current_complete ? AnimationPoseSource::Current :
           (availability.last_complete ? AnimationPoseSource::LastComplete : AnimationPoseSource::BindPose);
}

const SkinnedRtBuildContract& skinned_rt_build_contract() noexcept {
    static const SkinnedRtBuildContract contract{};
    return contract;
}

}  // namespace matter::animation
