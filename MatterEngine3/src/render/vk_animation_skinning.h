#pragma once

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
    uint32_t source_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t instance_slot = 0;
    int32_t render_priority = 0;
    uint32_t distance_bucket = 0;
    uint32_t lod = 0;
    bool history_valid = false;
    VkSkinPose pose;
};

struct VkSkinArenaSlice {
    uint32_t offset = 0;
    uint32_t count = 0;
};

// C1 deliberately leaves the actual raster-instance fallback choice to C2;
// this record makes rejection visible and deterministic without emitting a
// partially valid compute work item.
enum class VkSkinFallbackMode : uint8_t { BindPoseOrLastPose };
struct VkSkinFallback {
    uint32_t instance_slot = 0;
    VkSkinFallbackMode mode = VkSkinFallbackMode::BindPoseOrLastPose;
};

struct VkSkinFrameArenas {
    std::vector<VkSkinJoint> palette_current;
    std::vector<VkSkinJoint> palette_previous;
    std::vector<VkSkinWorkItem> work_items;
    std::vector<VkSkinArenaSlice> current_output;
    std::vector<VkSkinArenaSlice> previous_output;
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
    explicit VkAnimationSkinning(uint32_t frame_slots = 3);

    bool register_asset(uint64_t asset_key,
                        const std::vector<VkSkinInfluence>& influences);
    bool begin_frame(uint32_t frame_slot, uint64_t completed_fence);
    bool submit_visible(uint32_t frame_slot,
                        const std::vector<VkSkinSubmission>& visible);
    void mark_submitted(uint32_t frame_slot, uint64_t fence);

    const VkSkinFrameArenas& frame(uint32_t frame_slot) const;
    uint32_t fallback_count() const noexcept { return fallback_count_; }

private:
    struct AssetInfluences {
        std::vector<VkSkinInfluence> values;
    };
    std::map<uint64_t, AssetInfluences> assets_;
    std::vector<VkSkinFrameArenas> frames_;
    uint32_t fallback_count_ = 0;

    static bool identical(const std::vector<VkSkinInfluence>& a,
                          const std::vector<VkSkinInfluence>& b) noexcept;
};

}  // namespace viewer
