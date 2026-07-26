#include "vk_animation_skinning.h"

#include <algorithm>
#include <cmath>
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

void transform_point(const VkSkinMatrix& matrix, const float in[4],
                     float out[4]) noexcept {
    for (uint32_t row = 0; row != 4; ++row)
        out[row] = matrix.elements[row] * in[0] +
                   matrix.elements[4 + row] * in[1] +
                   matrix.elements[8 + row] * in[2] +
                   matrix.elements[12 + row] * in[3];
}

void transform_direction(const VkSkinMatrix& matrix, const float in[4],
                         float out[4]) noexcept {
    float direction[4]{in[0], in[1], in[2], 0.0f};
    transform_point(matrix, direction, out);
}

bool finite3(const float value[4]) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

bool finite_matrix(const VkSkinMatrix& value) noexcept {
    for (float element : value.elements)
        if (!std::isfinite(element)) return false;
    return true;
}

bool finite_joint(const VkSkinJoint& value) noexcept {
    return finite_matrix(value.position) && finite_matrix(value.normal);
}

bool valid_influence(const VkSkinInfluence& influence,
                     uint32_t palette_count) noexcept {
    uint32_t total_weight = 0;
    for (uint32_t lane = 0; lane != 4; ++lane) {
        const uint16_t weight = influence.weight[lane];
        if (weight == 0) continue;
        if (influence.joint[lane] >= palette_count) return false;
        total_weight += weight;
    }
    return total_weight != 0;
}

}  // namespace

VkAnimationSkinning::VkAnimationSkinning(
    uint32_t frame_slots, matter::animation::AnimationBudgetConfig budget)
    : frames_(frame_slots == 0 ? 1 : frame_slots),
      source_dependency_fence_(frames_.size(), 0),
      pending_source_users_(frames_.size(), 0),
      pending_source_dependencies_(frames_.size()),
      budget_(budget.valid() ? budget : matter::animation::AnimationBudgetConfig{}) {}

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
    if (assets_.size() >= budget_.max_assets) {
        stats_.record_fallback(matter::animation::AnimationFallbackReason::AssetLimit);
        return false;
    }
    if (influences.size() > std::numeric_limits<uint32_t>::max() -
                                influence_arena_.size()) return false;
    const uint32_t offset = static_cast<uint32_t>(influence_arena_.size());
    influence_arena_.insert(influence_arena_.end(), influences.begin(), influences.end());
    assets_.emplace(asset_key, AssetInfluences{influences, offset});
    return true;
}

bool VkAnimationSkinning::begin_frame(uint32_t frame_slot,
                                      uint64_t completed_fence) {
    if (frame_slot >= frames_.size()) return false;
    VkSkinFrameArenas& target = frames_[frame_slot];
    if ((target.in_flight && target.submitted_fence > completed_fence) ||
        source_dependency_fence_[frame_slot] > completed_fence ||
        pending_source_users_[frame_slot] != 0) return false;
    // A retained slice is valid only while its fence-owned frame resource is
    // sealed. Reusing that slot first retires every reference to its buffers.
    for (auto retained = retained_outputs_.begin();
         retained != retained_outputs_.end();) {
        if (retained->second.output_frame_slot == frame_slot)
            retained = retained_outputs_.erase(retained);
        else
            ++retained;
    }
    source_dependency_fence_[frame_slot] = 0;
    release_pending_dependencies(frame_slot);
    target = {};
    return true;
}

bool VkAnimationSkinning::submit_visible(
    uint32_t frame_slot, const std::vector<VkSkinSubmission>& visible) {
    if (frame_slot >= frames_.size()) return false;
    VkSkinFrameArenas& target = frames_[frame_slot];
    if (target.in_flight) return false;

    // `visible` is already a current-frame conservative animated
    // bounds/frustum/LOD result. Compact before validation, sorting, budget
    // admission, or output allocation: a current off-frustum instance has no
    // skin work, output slice, or custom raster draw.
    std::vector<VkSkinSubmission> sorted;
    sorted.reserve(visible.size());
    for (const VkSkinSubmission& value : visible)
        if (value.current_frustum_visible) sorted.push_back(value);
    std::sort(sorted.begin(), sorted.end(), less_submission);
    const auto concrete_fallback =
        [this](const VkSkinSubmission& value,
               matter::animation::AnimationFallbackReason reason,
               VkSkinRasterDraw* retained_draw) {
        const auto retained =
            retained_outputs_.find(instance_key(value.instance_slot,
                                                value.instance_generation));
        const bool compatible =
            retained != retained_outputs_.end() &&
            retained->second.asset_key == value.asset_key &&
            retained->second.lod == value.lod &&
            retained->second.source_vertex == value.source_vertex &&
            retained->second.vertex_count == value.vertex_count &&
            retained->second.first_index == value.first_index &&
            retained->second.index_count == value.index_count &&
            retained->second.cluster == value.cluster &&
            value.index_count != 0 && value.index_count % 3u == 0u;
        if (compatible && retained_draw) {
            *retained_draw = {
                retained->second.asset_key,
                retained->second.first_index,
                retained->second.index_count,
                retained->second.source_vertex,
                retained->second.output_vertex,
                retained->second.vertex_count,
                value.instance_slot,
                value.instance_generation,
                retained->second.output_frame_slot,
                retained->second.lod,
                retained->second.cluster,
                0};
        }
        return VkSkinFallback{
            value.instance_slot, value.instance_generation,
            compatible ? VkSkinFallbackMode::LastCompletePose
                       : VkSkinFallbackMode::BindPose,
            reason};
    };
    const auto publish_invalid = [this, &target, &sorted, &concrete_fallback](
                                     matter::animation::AnimationFallbackReason reason) {
        VkSkinFrameArenas fallback{};
        fallback.fallbacks.reserve(sorted.size());
        for (const VkSkinSubmission& value : sorted) {
            VkSkinRasterDraw retained_draw{};
            const VkSkinFallback item =
                concrete_fallback(value, reason, &retained_draw);
            fallback.fallbacks.push_back(item);
            if (item.mode == VkSkinFallbackMode::LastCompletePose)
                fallback.raster_draws.push_back(retained_draw);
            if (item.mode == VkSkinFallbackMode::LastCompletePose)
                ++stats_.last_complete_fallback_count;
            else
                ++stats_.bind_pose_fallback_count;
            stats_.record_fallback(reason);
        }
        target = std::move(fallback);
    };
    const auto capture_dependencies = [this, frame_slot, &target]() {
        release_pending_dependencies(frame_slot);
        std::vector<uint32_t>& dependencies =
            pending_source_dependencies_[frame_slot];
        for (const VkSkinRasterDraw& draw : target.raster_draws) {
            if (draw.output_frame_slot == frame_slot ||
                draw.output_frame_slot >= frames_.size()) continue;
            if (std::find(dependencies.begin(), dependencies.end(),
                          draw.output_frame_slot) == dependencies.end()) {
                dependencies.push_back(draw.output_frame_slot);
                ++pending_source_users_[draw.output_frame_slot];
            }
        }
    };

    std::vector<VkSkinSubmission> accepted;
    std::vector<VkSkinFallback> overflow;
    std::vector<VkSkinRasterDraw> retained_fallback_draws;
    accepted.reserve(std::min<size_t>(sorted.size(), budget_.max_skin_work_items));
    uint32_t output_count = 0;
    uint32_t palette_count = 0;
    bool tail_overflow = false;
    matter::animation::AnimationFallbackReason tail_reason =
        matter::animation::AnimationFallbackReason::SkinWorkBudget;
    for (const VkSkinSubmission& value : sorted) {
        const auto asset = assets_.find(value.asset_key);
        uint32_t source_end = 0;
        uint32_t output_end = 0;
        if (asset == assets_.end() || value.vertex_count == 0 ||
            value.lod > kVkSkinLodMax || value.cluster > kVkSkinClusterMax ||
            value.pose.current.empty() || value.pose.previous.size() != value.pose.current.size() ||
            value.pose.current.size() > kVkSkinPaletteCountMax ||
            !checked_add(value.source_vertex, value.vertex_count, source_end) ||
            !checked_add(value.influence_vertex, value.vertex_count, source_end) ||
            source_end > asset->second.values.size() ||
            !checked_add(output_count, value.vertex_count, output_end)) {
            publish_invalid(matter::animation::AnimationFallbackReason::InvalidSkinSubmission);
            capture_dependencies();
            fallback_count_ += static_cast<uint32_t>(sorted.size());
            return false;
        }
        const uint32_t joint_count = static_cast<uint32_t>(value.pose.current.size());
        for (const VkSkinJoint& joint : value.pose.current) {
            if (!finite_joint(joint)) {
                publish_invalid(matter::animation::AnimationFallbackReason::InvalidSkinSubmission);
                capture_dependencies();
                fallback_count_ += static_cast<uint32_t>(sorted.size());
                return false;
            }
        }
        if (value.history_valid) {
            for (const VkSkinJoint& joint : value.pose.previous) {
                if (!finite_joint(joint)) {
                    publish_invalid(matter::animation::AnimationFallbackReason::InvalidSkinSubmission);
                    capture_dependencies();
                    fallback_count_ += static_cast<uint32_t>(sorted.size());
                    return false;
                }
            }
        }
        for (uint32_t vertex = value.influence_vertex;
             vertex != value.influence_vertex + value.vertex_count; ++vertex) {
            if (!valid_influence(asset->second.values[vertex], joint_count)) {
                publish_invalid(matter::animation::AnimationFallbackReason::InvalidSkinSubmission);
                capture_dependencies();
                fallback_count_ += static_cast<uint32_t>(sorted.size());
                return false;
            }
        }
        if (!tail_overflow && accepted.size() >= budget_.max_skin_work_items) {
            tail_overflow = true;
            tail_reason = matter::animation::AnimationFallbackReason::SkinWorkBudget;
        }
        if (!tail_overflow && output_end > budget_.max_skinned_vertices) {
            tail_overflow = true;
            tail_reason = matter::animation::AnimationFallbackReason::SkinVertexBudget;
        }
        if (tail_overflow) {
            VkSkinRasterDraw retained_draw{};
            const VkSkinFallback fallback =
                concrete_fallback(value, tail_reason, &retained_draw);
            overflow.push_back(fallback);
            if (fallback.mode == VkSkinFallbackMode::LastCompletePose)
                retained_fallback_draws.push_back(retained_draw);
            stats_.record_fallback(tail_reason);
            continue;
        }
        accepted.push_back(value);
        output_count = output_end;
        if (!checked_add(palette_count, static_cast<uint32_t>(value.pose.current.size()), palette_count)) {
            publish_invalid(matter::animation::AnimationFallbackReason::InvalidSkinSubmission);
            capture_dependencies();
            fallback_count_ += static_cast<uint32_t>(sorted.size());
            return false;
        }
    }

    // Nothing below this point may fail: the complete queue has passed hard
    // caps and source bounds, so frame state publishes atomically.
    VkSkinFrameArenas staged{};
    staged.palette_current.reserve(palette_count);
    staged.palette_previous.reserve(palette_count);
    staged.work_items.reserve(accepted.size());
    staged.work_instance_generations.reserve(accepted.size());
    staged.current_output.reserve(accepted.size());
    staged.previous_output.reserve(accepted.size());
    staged.raster_draws.reserve(accepted.size());
    staged.fallbacks = std::move(overflow);
    staged.raster_draws = std::move(retained_fallback_draws);
    for (const VkSkinFallback& fallback : staged.fallbacks) {
        if (fallback.mode == VkSkinFallbackMode::LastCompletePose)
            ++stats_.last_complete_fallback_count;
        else
            ++stats_.bind_pose_fallback_count;
    }
    uint32_t output_offset = 0;
    uint32_t palette_offset = 0;
    for (const VkSkinSubmission& value : accepted) {
        const auto asset = assets_.find(value.asset_key);
        // The validation pass above proves this lookup and its source range.
        if (asset == assets_.end()) return false;
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
        item.influence = asset->second.offset + value.influence_vertex;
        item.vertex_count = value.vertex_count;
        item.palette = palette_offset;
        item.output_current = output_offset;
        item.output_previous = output_offset;
        item.instance_slot = value.instance_slot;
        item.flags = (value.history_valid ? 0u : kVkSkinHistoryInvalid) |
                     vk_skin_pack_cull_identity(value.lod, value.cluster) |
                     (joint_count << kVkSkinPaletteCountShift);
        staged.work_items.push_back(item);
        staged.work_instance_generations.push_back(value.instance_generation);
        staged.current_output.push_back({output_offset, value.vertex_count});
        staged.previous_output.push_back({output_offset, value.vertex_count});
        // Indexed raster consumption is opt-in for the C1-compatible queue:
        // a caller which did not provide a visible indexed range remains
        // compute-only and the renderer leaves its static/last-good draw in
        // place.  Never infer a range from a vertex count.
        if (value.index_count != 0 && value.index_count % 3u == 0u) {
            staged.raster_draws.push_back(
                {value.asset_key, value.first_index, value.index_count,
                 value.source_vertex, output_offset, value.vertex_count,
                 value.instance_slot, value.instance_generation, frame_slot,
                 value.lod, value.cluster, item.flags});
        }
        output_offset += value.vertex_count;
        palette_offset += joint_count;
    }
    staged.current_output_vertices = output_count;
    staged.previous_output_vertices = output_count;
    target = std::move(staged);
    capture_dependencies();
    fallback_count_ += static_cast<uint32_t>(target.fallbacks.size());
    stats_.submitted_skin_work_items += accepted.size();
    stats_.submitted_skinned_vertices += output_count;
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
    for (uint32_t source : pending_source_dependencies_[frame_slot]) {
        source_dependency_fence_[source] =
            std::max(source_dependency_fence_[source], fence);
        --pending_source_users_[source];
    }
    pending_source_dependencies_[frame_slot].clear();
    for (const VkSkinRasterDraw& draw : target.raster_draws) {
        if (draw.output_frame_slot != frame_slot) continue;
        retained_outputs_[instance_key(draw.instance_slot,
                                       draw.instance_generation)] =
            {draw.asset_key, draw.lod, draw.source_vertex, draw.vertex_count,
             draw.first_index, draw.index_count, draw.cluster, draw.output_vertex,
             draw.output_frame_slot};
    }
    last_submitted_fence_ = fence;
    has_submitted_fence_ = true;
    return true;
}

bool VkAnimationSkinning::reject_gpu_frame(
    uint32_t frame_slot, VkSkinGpuFailureReason reason) {
    if (frame_slot >= frames_.size() || reason == VkSkinGpuFailureReason::None)
        return false;
    VkSkinFrameArenas& target = frames_[frame_slot];
    if (target.in_flight) return false;

    // Retained fallback draws already published for budget pressure remain
    // valid. Current-frame draws are never retained: their output allocation
    // or upload just failed and must not reach raster or cull exclusion.
    VkSkinFrameArenas fallback{};
    fallback.gpu_failure = reason;
    for (const VkSkinRasterDraw& draw : target.raster_draws) {
        if (draw.output_frame_slot != frame_slot)
            fallback.raster_draws.push_back(draw);
    }
    fallback.fallbacks = target.fallbacks;
    for (size_t work_index = 0; work_index != target.work_items.size();
         ++work_index) {
        const VkSkinWorkItem& work = target.work_items[work_index];
        const uint32_t lod = vk_skin_work_lod(work.flags);
        const uint32_t cluster = vk_skin_work_cluster(work.flags);
        const auto active_draw = std::find_if(
            target.raster_draws.begin(), target.raster_draws.end(),
            [&work, lod, cluster, frame_slot](const VkSkinRasterDraw& draw) {
                return draw.output_frame_slot == frame_slot &&
                       draw.instance_slot == work.instance_slot &&
                       draw.lod == lod && draw.cluster == cluster;
            });
        VkSkinFallback item{};
        item.instance_slot = work.instance_slot;
        item.instance_generation = work_index < target.work_instance_generations.size()
            ? target.work_instance_generations[work_index]
            : (active_draw != target.raster_draws.end()
                   ? active_draw->instance_generation : 0);
        item.reason = matter::animation::AnimationFallbackReason::InvalidSkinSubmission;
        if (active_draw != target.raster_draws.end()) {
            const auto retained = retained_outputs_.find(instance_key(
                active_draw->instance_slot, active_draw->instance_generation));
            if (retained != retained_outputs_.end() &&
                retained->second.asset_key == active_draw->asset_key &&
                retained->second.lod == active_draw->lod &&
                retained->second.cluster == active_draw->cluster &&
                retained->second.source_vertex == active_draw->source_vertex &&
                retained->second.vertex_count == active_draw->vertex_count &&
                retained->second.first_index == active_draw->first_index &&
                retained->second.index_count == active_draw->index_count) {
                item.mode = VkSkinFallbackMode::LastCompletePose;
                fallback.raster_draws.push_back(
                    {retained->second.asset_key, retained->second.first_index,
                     retained->second.index_count, retained->second.source_vertex,
                     retained->second.output_vertex, retained->second.vertex_count,
                     active_draw->instance_slot, active_draw->instance_generation,
                     retained->second.output_frame_slot, retained->second.lod,
                     retained->second.cluster, 0});
                ++stats_.last_complete_fallback_count;
            } else {
                item.mode = VkSkinFallbackMode::BindPose;
                ++stats_.bind_pose_fallback_count;
            }
        } else {
            item.mode = VkSkinFallbackMode::BindPose;
            ++stats_.bind_pose_fallback_count;
        }
        fallback.fallbacks.push_back(item);
        stats_.record_fallback(item.reason);
    }
    release_pending_dependencies(frame_slot);
    for (const VkSkinRasterDraw& draw : fallback.raster_draws) {
        if (draw.output_frame_slot == frame_slot ||
            draw.output_frame_slot >= frames_.size()) continue;
        std::vector<uint32_t>& dependencies =
            pending_source_dependencies_[frame_slot];
        if (std::find(dependencies.begin(), dependencies.end(),
                      draw.output_frame_slot) == dependencies.end()) {
            dependencies.push_back(draw.output_frame_slot);
            ++pending_source_users_[draw.output_frame_slot];
        }
    }
    fallback_count_ += static_cast<uint32_t>(target.work_items.size());
    ++gpu_failure_count_;
    if (reason == VkSkinGpuFailureReason::Allocation)
        ++gpu_allocation_failure_count_;
    else
        ++gpu_upload_failure_count_;
    target = std::move(fallback);
    return true;
}

uint64_t VkAnimationSkinning::instance_key(uint32_t slot,
                                           uint32_t generation) noexcept {
    return (uint64_t(slot) << 32u) | generation;
}

void VkAnimationSkinning::release_pending_dependencies(
    uint32_t frame_slot) noexcept {
    for (uint32_t source : pending_source_dependencies_[frame_slot]) {
        if (pending_source_users_[source] != 0)
            --pending_source_users_[source];
    }
    pending_source_dependencies_[frame_slot].clear();
}

const VkSkinFrameArenas& VkAnimationSkinning::frame(uint32_t frame_slot) const {
    static const VkSkinFrameArenas empty{};
    return frame_slot < frames_.size() ? frames_[frame_slot] : empty;
}

bool vk_skin_vertex_cpu(const VkSkinSourceVertex& source,
                        const VkSkinInfluence& influence,
                        const VkSkinJoint* current_palette,
                        const VkSkinJoint* previous_palette,
                        uint32_t palette_count,
                        VkSkinVertex& output) noexcept {
    if (!current_palette || !previous_palette || palette_count == 0)
        return false;
    float current[4]{};
    float previous[4]{};
    float normal[4]{};
    const float source_position[4]{source.position[0], source.position[1],
                                   source.position[2], 1.0f};
    float weight_sum = 0.0f;
    for (uint32_t lane = 0; lane != 4; ++lane) {
        const float weight = vk_skin_decode_weight(influence.weight[lane]);
        if (weight == 0.0f) continue;
        if (influence.joint[lane] >= palette_count) return false;
        float transformed[4]{};
        transform_point(current_palette[influence.joint[lane]].position,
                        source_position, transformed);
        for (uint32_t component = 0; component != 4; ++component)
            current[component] += transformed[component] * weight;
        transform_point(previous_palette[influence.joint[lane]].position,
                        source_position, transformed);
        for (uint32_t component = 0; component != 4; ++component)
            previous[component] += transformed[component] * weight;
        transform_direction(current_palette[influence.joint[lane]].normal,
                            source.normal, transformed);
        for (uint32_t component = 0; component != 3; ++component)
            normal[component] += transformed[component] * weight;
        weight_sum += weight;
    }
    if (weight_sum == 0.0f || !finite3(current) || !finite3(previous) ||
        !finite3(normal)) return false;
    const float length = std::sqrt(normal[0] * normal[0] +
                                   normal[1] * normal[1] +
                                   normal[2] * normal[2]);
    if (!(length > 0.0f) || !std::isfinite(length)) return false;
    for (uint32_t component = 0; component != 3; ++component)
        normal[component] /= length;
    output = {};
    std::memcpy(output.position, current, sizeof(current));
    std::memcpy(output.previous_position, previous, sizeof(previous));
    std::memcpy(output.normal, normal, sizeof(normal));
    std::memcpy(output.tint, source.tint, sizeof(output.tint));
    std::memcpy(output.surface, source.surface, sizeof(output.surface));
    output.material_index = source.material_index;
    return true;
}

}  // namespace viewer
