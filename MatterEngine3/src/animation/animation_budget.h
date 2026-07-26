// Central, renderer-neutral animation budgets and cosmetic presentation LOD.
// Fixed simulation is intentionally absent: fixed graph clocks, controllers,
// markers, and root motion always run at the simulation cadence.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

namespace matter::animation {

enum class AnimationFallbackReason : uint8_t {
    None,
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

enum class AnimationPoseLodTier : uint8_t { Hz60, Hz30, Hz15, Frozen };
enum class AnimationPoseSource : uint8_t { Current, LastComplete, BindPose };

struct AnimationPoseAvailability {
    bool current_complete = false;
    bool last_complete = false;
};

// This is deliberately a build-once policy rather than a Vulkan flag type.
// It prevents the animation layer from ever widening the RT contract toward
// BLAS update/refit while raster deformation remains exact.
struct SkinnedRtBuildContract {
    bool build_once = true;
    bool allow_update = false;
    bool allow_refit = false;
};

struct AnimationBudgetConfig {
    static constexpr uint32_t kHardMaxAssets = 4096;
    static constexpr uint32_t kHardMaxRuntimeInstances = 4096;
    static constexpr uint32_t kHardMaxJointsPerAsset = 256;
    static constexpr uint32_t kHardMaxGraphNodes = 128;
    static constexpr uint32_t kHardMaxControllerNodes = 128;
    static constexpr uint32_t kHardMaxSkinWorkItems = 256;
    static constexpr uint32_t kHardMaxSkinnedVertices = 2'000'000;
    static constexpr uint32_t kHardMaxEvaluatedJointsPerFrame = 1'000'000;
    static constexpr uint32_t kHardMaxWorldQueriesPerFixedTick = 2048;
    static constexpr size_t kHardMaxMutableBytes = 64u * 1024u * 1024u;

    uint32_t max_assets = 1024;
    uint32_t max_runtime_instances = 4096;
    uint32_t max_joints_per_asset = 256;
    uint32_t max_graph_nodes = 128;
    uint32_t max_controller_nodes = 128;
    uint32_t max_skin_work_items = 256;
    uint32_t max_skinned_vertices = 2'000'000;
    uint32_t max_evaluated_joints_per_frame = 65'536;
    uint32_t max_world_queries_per_fixed_tick = 2'048;
    size_t max_mutable_bytes = 64u * 1024u * 1024u;

    // Distance bands select 60/30/15/frozen, in that order. A stateful
    // selector applies distance_hysteresis on both sides of every boundary.
    float near_distance = 12.0f;
    float mid_distance = 35.0f;
    float frozen_distance = 120.0f;
    float distance_hysteresis = 2.0f;

    bool valid() const noexcept;
};

// The public AnimationService already exposes a compact allocation snapshot as
// `matter::AnimationRuntimeStats`. This detailed private/runtime view augments
// it with evaluator and renderer counters without widening that stable API.
struct AnimationBudgetRuntimeStats {
    uint64_t evaluated_pose_count = 0;
    uint64_t evaluated_joint_count = 0;
    uint64_t submitted_skin_work_items = 0;
    uint64_t submitted_skinned_vertices = 0;
    uint64_t evaluated_presentation_pose_count = 0;
    uint64_t frozen_pose_count = 0;
    uint64_t resampled_pose_count = 0;
    uint64_t last_complete_fallback_count = 0;
    uint64_t bind_pose_fallback_count = 0;
    uint64_t fallback_count = 0;
    std::array<uint64_t, static_cast<size_t>(AnimationFallbackReason::Count)> fallbacks{};

    void record_fallback(AnimationFallbackReason reason) noexcept;
    void merge(const AnimationBudgetRuntimeStats& other) noexcept;
};

struct PoseLodRequest {
    uint64_t instance_key = 0;
    bool visible = false;
    float distance = 0.0f;
    int32_t explicit_priority = 0;
    // Monotonic presentation time. This deliberately is not a render-frame
    // divisor, so a 30 Hz render loop and a 144 Hz render loop agree.
    double presentation_seconds = 0.0;
    uint64_t frame_serial = 0;
};

struct PoseLodDecision {
    AnimationPoseLodTier tier = AnimationPoseLodTier::Frozen;
    bool evaluate_now = false;
    bool newly_visible = false;
    bool resample_current_graph_time = false;
};

class PoseLodScheduler {
public:
    explicit PoseLodScheduler(AnimationBudgetConfig config = {});
    PoseLodDecision schedule(const PoseLodRequest& request);
    void forget(uint64_t instance_key) noexcept;

private:
    struct State {
        AnimationPoseLodTier tier = AnimationPoseLodTier::Frozen;
        AnimationPoseLodTier post_grace_tier = AnimationPoseLodTier::Frozen;
        bool visible = false;
        bool has_post_grace_tier = false;
        uint8_t visible_grace_frames = 0;
        double next_due_seconds = 0.0;
        uint64_t last_frame_serial = UINT64_MAX;
        PoseLodDecision last_decision{};
    };

    AnimationPoseLodTier select_tier(AnimationPoseLodTier previous,
                                     float distance) const noexcept;
    static double interval(AnimationPoseLodTier tier) noexcept;

    AnimationBudgetConfig config_;
    std::map<uint64_t, State> states_;
};

AnimationPoseSource select_pose_fallback(AnimationPoseAvailability availability) noexcept;
const SkinnedRtBuildContract& skinned_rt_build_contract() noexcept;

}  // namespace matter::animation
