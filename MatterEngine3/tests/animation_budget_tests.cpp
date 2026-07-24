#include "animation/animation_budget.h"
#include "check.h"

#include <vector>

using namespace matter::animation;

int main() {
    AnimationBudgetConfig defaults;
    CHECK(defaults.valid(), "default animation budget is internally valid");
    CHECK(defaults.max_skin_work_items <= AnimationBudgetConfig::kHardMaxSkinWorkItems,
          "default work limit is bounded by the hard cap");
    CHECK(defaults.max_skinned_vertices <= AnimationBudgetConfig::kHardMaxSkinnedVertices,
          "default vertex limit is bounded by the hard cap");

    AnimationBudgetConfig oversized = defaults;
    oversized.max_skin_work_items = AnimationBudgetConfig::kHardMaxSkinWorkItems + 1;
    CHECK(!oversized.valid(), "runtime work budget above hard cap is rejected");
    oversized = defaults;
    oversized.max_joints_per_asset = AnimationBudgetConfig::kHardMaxJointsPerAsset + 1;
    CHECK(!oversized.valid(), "asset joint budget above hard cap is rejected");

    AnimationBudgetRuntimeStats stats;
    stats.record_fallback(AnimationFallbackReason::SkinVertexBudget);
    stats.record_fallback(AnimationFallbackReason::SkinVertexBudget);
    CHECK(stats.fallback_count == 2 &&
              stats.fallbacks[static_cast<size_t>(AnimationFallbackReason::SkinVertexBudget)] == 2,
          "fallback counters preserve their reason");

    AnimationBudgetConfig lod_config = defaults;
    lod_config.near_distance = 10.0f;
    lod_config.mid_distance = 30.0f;
    lod_config.frozen_distance = 100.0f;
    lod_config.distance_hysteresis = 2.0f;
    PoseLodScheduler scheduler(lod_config);
    PoseLodRequest request{};
    request.instance_key = 7;
    request.visible = true;
    request.distance = 12.0f;
    request.presentation_seconds = 0.0;
    request.frame_serial = 1;
    PoseLodDecision first = scheduler.schedule(request);
    CHECK(first.tier == AnimationPoseLodTier::Hz60 && first.evaluate_now && first.newly_visible,
          "newly visible instances begin at 60 Hz immediately");
    request.presentation_seconds = 1.0 / 60.0;
    request.frame_serial = 2;
    PoseLodDecision second = scheduler.schedule(request);
    CHECK(second.tier == AnimationPoseLodTier::Hz60 && second.newly_visible,
          "newly visible instances retain 60 Hz for the second frame");
    request.presentation_seconds = 2.0 / 60.0;
    request.frame_serial = 3;
    PoseLodDecision third = scheduler.schedule(request);
    CHECK(third.tier == AnimationPoseLodTier::Hz30 && !third.newly_visible,
          "after the visibility grace period distance selects the cosmetic tier");

    request.distance = 11.0f;
    request.presentation_seconds += 1.0 / 30.0;
    ++request.frame_serial;
    CHECK(scheduler.schedule(request).tier == AnimationPoseLodTier::Hz30,
          "hysteresis prevents a boundary oscillation");
    request.distance = 7.0f;
    request.presentation_seconds += 1.0 / 30.0;
    ++request.frame_serial;
    PoseLodDecision faster = scheduler.schedule(request);
    CHECK(faster.tier == AnimationPoseLodTier::Hz60 && faster.resample_current_graph_time,
          "returning to a faster tier resamples instead of replaying skipped poses");

    request.visible = false;
    request.presentation_seconds += 1.0;
    ++request.frame_serial;
    CHECK(scheduler.schedule(request).tier == AnimationPoseLodTier::Frozen,
          "invisible instances freeze cosmetic presentation");

    CHECK(select_pose_fallback({true, true}) == AnimationPoseSource::Current,
          "complete current pose wins fallback selection");
    CHECK(select_pose_fallback({false, true}) == AnimationPoseSource::LastComplete,
          "last completed pose is retained after a rejected current pose");
    CHECK(select_pose_fallback({false, false}) == AnimationPoseSource::BindPose,
          "first-frame rejection falls back to immutable bind pose");

    CHECK(skinned_rt_build_contract().build_once && !skinned_rt_build_contract().allow_update &&
              !skinned_rt_build_contract().allow_refit,
          "deforming raster meshes retain build-once bind-pose RT BLAS");
    return check_summary();
}
