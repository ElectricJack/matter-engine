// MatterEngine3/src/animation/animation_budget.cpp
//
// Implementation of the animation budget policy (see animation_budget.h for
// the config and the hard caps). Three unrelated pieces live here:
//
//  - `AnimationBudgetConfig::valid()` — the admission check for a config.
//  - `AnimationBudgetRuntimeStats` — plain counters, plus `merge` so
//    per-worker copies can be folded into a frame total.
//  - `PoseLodScheduler` — the stateful part, and the only interesting one.
//
// PoseLodScheduler
// ---------------------------------------------------------------------------
// Decides, per animated instance per frame, which cosmetic pose rate applies
// (60/30/15 Hz or Frozen) and whether a pose evaluation is due *now*. This is
// presentation only: fixed-cadence graph clocks, controllers, markers and
// root motion are unaffected, by design.
//
// Tier selection is a distance-band lookup with hysteresis, and it is
// STATEFUL — `select_tier` takes the previous tier and applies
// `distance_hysteresis` on both sides of every boundary, so an instance
// hovering on a band edge does not oscillate. A newly visible instance gets
// a forced two-frame 60 Hz grace period, after which the next tier is seeded
// from the raw bands (not from the grace tier) so a distant newcomer does not
// inherit near-field hysteresis.
//
// Timing uses `presentation_seconds`, a monotonic wall-ish clock, never a
// frame counter, so the behaviour is identical at 30 Hz and 144 Hz render
// rates. Deadlines are carried forward from the previous deadline rather than
// rebased on "now" — see the long comment inside `schedule`, which records the
// halved-refresh-rate bug that motivated it.
//
// Lifetime and threading: state is one `State` per `instance_key` in a
// `std::map` that grows until `forget()` is called, so the owner MUST call
// `forget` when an instance goes away; nothing here evicts by age. There is
// no internal locking — the scheduler is expected to be driven from one
// thread. Calling `schedule` twice with the same `frame_serial` returns the
// memoized decision without advancing any state, so it is safe to query more
// than once per frame.
#include "animation/animation_budget.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace matter::animation {

// Admission check: every limit must be within (0, hard cap] and the distance
// bands must be finite and strictly increasing
// (near < mid < frozen, hysteresis >= 0).
//
// One deliberate asymmetry: `max_controller_nodes` is only bounded above, so
// zero is accepted there while it is rejected for every other limit.
bool AnimationBudgetConfig::valid() const noexcept {
    return max_assets > 0 && max_assets <= kHardMaxAssets &&
           max_runtime_instances > 0 && max_runtime_instances <= kHardMaxRuntimeInstances &&
           max_joints_per_asset > 0 && max_joints_per_asset <= kHardMaxJointsPerAsset &&
           max_graph_nodes > 0 && max_graph_nodes <= kHardMaxGraphNodes &&
           max_controller_nodes <= kHardMaxControllerNodes &&
           max_skin_work_items > 0 && max_skin_work_items <= kHardMaxSkinWorkItems &&
           max_skinned_vertices > 0 && max_skinned_vertices <= kHardMaxSkinnedVertices &&
           max_evaluated_joints_per_frame > 0 && max_evaluated_joints_per_frame <= kHardMaxEvaluatedJointsPerFrame &&
           max_world_queries_per_fixed_tick > 0 && max_world_queries_per_fixed_tick <= kHardMaxWorldQueriesPerFixedTick &&
           max_mutable_bytes > 0 && max_mutable_bytes <= kHardMaxMutableBytes &&
           std::isfinite(near_distance) && std::isfinite(mid_distance) &&
           std::isfinite(frozen_distance) && std::isfinite(distance_hysteresis) &&
           near_distance >= 0.0f && mid_distance > near_distance &&
           frozen_distance > mid_distance && distance_hysteresis >= 0.0f;
}

// Count one fallback, both in the total and in the per-reason histogram.
// `None` and any out-of-range value are silently ignored, so a caller may
// pass an unconditional "reason" without branching.
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

// An invalid config is silently replaced with the defaults rather than
// rejected — validate with `AnimationBudgetConfig::valid()` first if you need
// to know that your values were taken.
PoseLodScheduler::PoseLodScheduler(AnimationBudgetConfig config)
    : config_(config.valid() ? config : AnimationBudgetConfig{}) {}

size_t PoseLodScheduler::state_mutable_bytes() noexcept { return sizeof(State); }

// Seconds between evaluations for a tier. `Frozen` (and any unknown value)
// returns +infinity, which is what makes the `next_due_seconds` arithmetic
// in `schedule` naturally never come due.
double PoseLodScheduler::interval(AnimationPoseLodTier tier) noexcept {
    switch (tier) {
        case AnimationPoseLodTier::Hz60: return 1.0 / 60.0;
        case AnimationPoseLodTier::Hz30: return 1.0 / 30.0;
        case AnimationPoseLodTier::Hz15: return 1.0 / 15.0;
        case AnimationPoseLodTier::Frozen: return std::numeric_limits<double>::infinity();
    }
    return std::numeric_limits<double>::infinity();
}

// Distance -> tier, with `distance_hysteresis` widening whichever band the
// instance is already in, so `previous` genuinely changes the answer near a
// boundary. `distance` is in world units (metres); a non-finite or negative
// distance is treated as "unknown" and frozen. Pass `Frozen` as `previous` to
// get the raw, hysteresis-free band.
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

// The per-frame decision for one instance. Mutates the scheduler: it creates
// per-instance state on first use and advances tier and deadline, so it is
// not a query.
//
// Behaviour worth knowing:
//  - A malformed request (zero `instance_key`, non-finite or negative
//    `presentation_seconds`) returns a default decision — Frozen, no
//    evaluation — and creates no state.
//  - Repeating the same `frame_serial` returns the memoized decision and
//    changes nothing, so multiple queries per frame are safe.
//  - Invisible: forced to Frozen and the deadline is reset to now, so the
//    instance evaluates immediately when it reappears rather than banking a
//    backlog.
//  - Newly visible: two frames forced to 60 Hz (`newly_visible` set) before
//    distance bands apply.
//  - `resample_current_graph_time` is set when the instance just became
//    visible or moved to a FINER tier, telling the caller its interpolation
//    base is stale.
//  - `evaluate_now` is false for Frozen and whenever the deadline has not
//    arrived; the deadline advance is deliberately carried forward, not
//    rebased (see the inline comment).
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
        // Carry the deadline forward from the PREVIOUS deadline, not from
        // "now": rebasing on now rounds every deadline up to the next frame,
        // so a render loop running at almost exactly the tier rate (60 Hz
        // frames against the Hz60 tier) arrives a hair early every other
        // frame and the effective refresh rate is halved -- the pose holds
        // for two frames, then jumps two fixed steps. Snap back to now only
        // when more than one interval behind (stall, tier change, long
        // invisibility) so no burst of catch-up evaluations is banked.
        const double step = interval(selected);
        const double carried = state.next_due_seconds + step;
        state.next_due_seconds = carried < request.presentation_seconds
            ? request.presentation_seconds + step : carried;
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

// Drop an instance's state. Nothing evicts automatically, so this must be
// called when an instance is destroyed or the map grows for the lifetime of
// the scheduler. Re-scheduling a forgotten key restarts it as newly visible.
void PoseLodScheduler::forget(uint64_t instance_key) noexcept { states_.erase(instance_key); }

// Pick the best pose the caller can actually draw: this frame's pose if it
// completed, otherwise the last complete one, otherwise the bind pose. The
// two degraded outcomes are normal (a skipped or in-flight evaluation), not
// errors — the caller decides whether to also count a fallback.
AnimationPoseSource select_pose_fallback(AnimationPoseAvailability availability) noexcept {
    return availability.current_complete ? AnimationPoseSource::Current :
           (availability.last_complete ? AnimationPoseSource::LastComplete : AnimationPoseSource::BindPose);
}

const SkinnedRtBuildContract& skinned_rt_build_contract() noexcept {
    static const SkinnedRtBuildContract contract{};
    return contract;
}

}  // namespace matter::animation
