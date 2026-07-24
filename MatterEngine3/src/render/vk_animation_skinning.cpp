#include "vk_animation_skinning.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace viewer {
namespace {

bool less_submission(const VkSkinSubmission& left,
                     const VkSkinSubmission& right) noexcept {
    if (left.render_priority != right.render_priority)
        return left.render_priority > right.render_priority;
    if (left.distance_bucket != right.distance_bucket)
        return left.distance_bucket < right.distance_bucket;
    if (left.instance_slot != right.instance_slot)
        return left.instance_slot < right.instance_slot;
    return left.lod < right.lod;
}

bool checked_add(uint32_t base, uint32_t count, uint32_t& result) noexcept {
    if (count > std::numeric_limits<uint32_t>::max() - base) return false;
    result = base + count;
    return true;
}

}  // namespace

VkAnimationSkinning::VkAnimationSkinning(uint32_t frame_slots)
    : frames_(frame_slots == 0 ? 1 : frame_slots) {}

bool VkAnimationSkinning::identical(const std::vector<VkSkinInfluence>& a,
                                    const std::vector<VkSkinInfluence>& b) noexcept {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(VkSkinInfluence)) == 0);
}

bool VkAnimationSkinning::register_asset(
    uint64_t asset_key, const std::vector<VkSkinInfluence>& influences) {
    if (asset_key == 0 || influences.empty()) return false;
    const auto existing = assets_.find(asset_key);
    if (existing != assets_.end()) return identical(existing->second.values, influences);
    assets_.emplace(asset_key, AssetInfluences{influences});
    return true;
}

bool VkAnimationSkinning::begin_frame(uint32_t frame_slot,
                                      uint64_t completed_fence) {
    if (frame_slot >= frames_.size()) return false;
    VkSkinFrameArenas& target = frames_[frame_slot];
    if (target.in_flight && target.submitted_fence > completed_fence) return false;
    target = {};
    return true;
}

bool VkAnimationSkinning::submit_visible(
    uint32_t frame_slot, const std::vector<VkSkinSubmission>& visible) {
    if (frame_slot >= frames_.size()) return false;
    VkSkinFrameArenas& target = frames_[frame_slot];
    if (target.in_flight) return false;

    std::vector<VkSkinSubmission> sorted = visible;
    std::sort(sorted.begin(), sorted.end(), less_submission);
    const auto publish_fallbacks = [&target, &sorted]() {
        VkSkinFrameArenas fallback{};
        fallback.fallbacks.reserve(sorted.size());
        for (const VkSkinSubmission& value : sorted)
            fallback.fallbacks.push_back({value.instance_slot,
                                          VkSkinFallbackMode::BindPoseOrLastPose});
        target = std::move(fallback);
    };
    if (sorted.size() > kVkMaxSkinWorkItems) {
        publish_fallbacks();
        ++fallback_count_;
        return false;
    }

    uint32_t output_count = 0;
    uint32_t palette_count = 0;
    for (const VkSkinSubmission& value : sorted) {
        const auto asset = assets_.find(value.asset_key);
        uint32_t source_end = 0;
        uint32_t output_end = 0;
        if (asset == assets_.end() || value.vertex_count == 0 ||
            value.pose.current.empty() || value.pose.previous.size() != value.pose.current.size() ||
            !checked_add(value.source_vertex, value.vertex_count, source_end) ||
            source_end > asset->second.values.size() ||
            !checked_add(output_count, value.vertex_count, output_end) ||
            output_end > kVkMaxSkinnedOutputVertices ||
            !checked_add(palette_count, static_cast<uint32_t>(value.pose.current.size()), palette_count)) {
            publish_fallbacks();
            ++fallback_count_;
            return false;
        }
        output_count = output_end;
    }

    // Nothing below this point may fail: the complete queue has passed hard
    // caps and source bounds, so frame state publishes atomically.
    VkSkinFrameArenas staged{};
    staged.palette_current.reserve(palette_count);
    staged.palette_previous.reserve(palette_count);
    staged.work_items.reserve(sorted.size());
    staged.current_output.reserve(sorted.size());
    staged.previous_output.reserve(sorted.size());
    uint32_t output_offset = 0;
    uint32_t palette_offset = 0;
    for (const VkSkinSubmission& value : sorted) {
        const uint32_t joint_count = static_cast<uint32_t>(value.pose.current.size());
        staged.palette_current.insert(staged.palette_current.end(),
                                      value.pose.current.begin(), value.pose.current.end());
        if (value.history_valid) {
            staged.palette_previous.insert(staged.palette_previous.end(),
                                           value.pose.previous.begin(), value.pose.previous.end());
        } else {
            staged.palette_previous.insert(staged.palette_previous.end(),
                                           value.pose.current.begin(), value.pose.current.end());
        }
        VkSkinWorkItem item{};
        item.source_vertex = value.source_vertex;
        item.influence = value.source_vertex;
        item.vertex_count = value.vertex_count;
        item.palette = palette_offset;
        item.output_current = output_offset;
        item.output_previous = output_offset;
        item.instance_slot = value.instance_slot;
        item.flags = value.history_valid ? 0u : kVkSkinHistoryInvalid;
        staged.work_items.push_back(item);
        staged.current_output.push_back({output_offset, value.vertex_count});
        staged.previous_output.push_back({output_offset, value.vertex_count});
        output_offset += value.vertex_count;
        palette_offset += joint_count;
    }
    staged.current_output_vertices = output_count;
    staged.previous_output_vertices = output_count;
    target = std::move(staged);
    return true;
}

bool VkAnimationSkinning::mark_submitted(uint32_t frame_slot, uint64_t fence) {
    if (frame_slot >= frames_.size()) return false;
    VkSkinFrameArenas& target = frames_[frame_slot];
    // A slot can be associated with only one submitted fence until it has
    // been observed complete and begin_frame has reset it.  Fence values are
    // a single monotonically increasing timeline across all slots.
    if (target.in_flight ||
        (has_submitted_fence_ && fence <= last_submitted_fence_)) {
        return false;
    }
    target.submitted_fence = fence;
    target.in_flight = true;
    last_submitted_fence_ = fence;
    has_submitted_fence_ = true;
    return true;
}

const VkSkinFrameArenas& VkAnimationSkinning::frame(uint32_t frame_slot) const {
    static const VkSkinFrameArenas empty{};
    return frame_slot < frames_.size() ? frames_[frame_slot] : empty;
}

}  // namespace viewer
