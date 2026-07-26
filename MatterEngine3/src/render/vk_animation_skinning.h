#pragma once

#include "animation/animation_budget.h"
#include "vk_animation_types.h"

#include <cstdint>
#include <map>
#include <vector>

namespace viewer {

// CPU-side presentation of one immutable evaluator snapshot.  C2 changes the
// upload implementation, not this lifetime rule: both palette streams are
// copied from this single snapshot before the frame queue is published.
struct VkSkinPose {
    std::vector<VkSkinJoint> current;
    std::vector<VkSkinJoint> previous;
};

struct VkSkinSubmission {
    uint64_t asset_key = 0;
    // Immutable asset-local influence offset.  Source vertices are global to
    // the renderer arena, so they must not be conflated once parts are
    // compacted or multiple assets are resident.
    uint32_t influence_vertex = 0;
    uint32_t source_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t instance_slot = 0;
    uint32_t instance_generation = 0;
    int32_t render_priority = 0;
    uint32_t distance_bucket = 0;
    uint32_t lod = 0;
    // Global indexed-raster range selected by the visibility producer.  A
    // zero count deliberately means "compute only": it keeps C1 callers
    // source-compatible, but it must never manufacture a raster draw.
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    bool history_valid = false;
    VkSkinPose pose;
};

// One accepted work item has exactly one immutable raster mapping.  The
// output offsets are vertex indices (not bytes); record_raster converts them
// to VkSkinVertex binding offsets.  Keeping this beside the work queue makes
// sorting and fallback selection transactional.
struct VkSkinRasterDraw {
    uint64_t asset_key = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    uint32_t source_vertex = 0;
    uint32_t output_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t instance_slot = 0;
    uint32_t instance_generation = 0;
    // Selects the fence-owned frame output buffer. Current work points at the
    // frame being built; retained fallbacks may point at an older, still
    // sealed frame slot.
    uint32_t output_frame_slot = 0;
    uint32_t lod = 0;
    uint32_t flags = 0;
};

// Static indirect commands are identified by their immutable global indexed
// range.  A skinned draw may replace only an exact range match: overlap or a
// stale partial range leaves static rendering intact.
bool vk_skin_replaces_static_command(const std::vector<VkSkinRasterDraw>& draws,
                                     uint32_t first_index,
                                     uint32_t index_count) noexcept;

struct VkSkinArenaSlice {
    uint32_t offset = 0;
    uint32_t count = 0;
};

// C1 deliberately leaves the actual raster-instance fallback choice to C2;
// this record makes rejection visible and deterministic without emitting a
// partially valid compute work item.
enum class VkSkinFallbackMode : uint8_t { LastCompletePose, BindPose };
struct VkSkinFallback {
    uint32_t instance_slot = 0;
    uint32_t instance_generation = 0;
    VkSkinFallbackMode mode = VkSkinFallbackMode::BindPose;
    matter::animation::AnimationFallbackReason reason =
        matter::animation::AnimationFallbackReason::None;
};

struct VkSkinFrameArenas {
    std::vector<VkSkinJoint> palette_current;
    std::vector<VkSkinJoint> palette_previous;
    std::vector<VkSkinWorkItem> work_items;
    std::vector<VkSkinArenaSlice> current_output;
    std::vector<VkSkinArenaSlice> previous_output;
    std::vector<VkSkinRasterDraw> raster_draws;
    std::vector<VkSkinFallback> fallbacks;
    uint32_t current_output_vertices = 0;
    uint32_t previous_output_vertices = 0;
    uint64_t submitted_fence = 0;
    bool in_flight = false;
};

// Owns no Vulkan resources in C1. It establishes the allocation/lifetime
// transaction that C2 maps to device-local buffers. Frame slots may only be
// reset after their submitted fence has completed.
class VkAnimationSkinning {
public:
    explicit VkAnimationSkinning(
        uint32_t frame_slots = 3,
        matter::animation::AnimationBudgetConfig budget = {});

    bool register_asset(uint64_t asset_key,
                        const std::vector<VkSkinInfluence>& influences);
    bool begin_frame(uint32_t frame_slot, uint64_t completed_fence);
    bool submit_visible(uint32_t frame_slot,
                        const std::vector<VkSkinSubmission>& visible);
    // Frame slots are sealed exactly once.  A rejected seal leaves the
    // in-flight fence untouched, so a stale caller cannot make an active
    // arena appear reusable.
    bool mark_submitted(uint32_t frame_slot, uint64_t fence);

    const VkSkinFrameArenas& frame(uint32_t frame_slot) const;
    // Immutable packed influence arena. WorkItem::influence indexes this
    // buffer directly, so the renderer can upload it once per asset revision.
    const std::vector<VkSkinInfluence>& influences() const noexcept {
        return influence_arena_;
    }
    uint32_t fallback_count() const noexcept { return fallback_count_; }
    const matter::animation::AnimationBudgetRuntimeStats& stats() const noexcept {
        return stats_;
    }
    const matter::animation::AnimationBudgetConfig& budget_config() const noexcept {
        return budget_;
    }

private:
    struct AssetInfluences {
        std::vector<VkSkinInfluence> values;
        uint32_t offset = 0;
    };
    std::map<uint64_t, AssetInfluences> assets_;
    struct RetainedOutput {
        uint64_t asset_key = 0;
        uint32_t lod = 0;
        uint32_t source_vertex = 0;
        uint32_t vertex_count = 0;
        uint32_t first_index = 0;
        uint32_t index_count = 0;
        uint32_t output_vertex = 0;
        uint32_t output_frame_slot = 0;
    };
    std::map<uint64_t, RetainedOutput> retained_outputs_;
    std::vector<VkSkinInfluence> influence_arena_;
    std::vector<VkSkinFrameArenas> frames_;
    uint32_t fallback_count_ = 0;
    matter::animation::AnimationBudgetConfig budget_;
    matter::animation::AnimationBudgetRuntimeStats stats_;
    uint64_t last_submitted_fence_ = 0;
    bool has_submitted_fence_ = false;

    static bool identical(const std::vector<VkSkinInfluence>& a,
                          const std::vector<VkSkinInfluence>& b) noexcept;
    static uint64_t instance_key(uint32_t slot, uint32_t generation) noexcept;
};

}  // namespace viewer
