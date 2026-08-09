#include "vk_cloud_shadows.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <utility>

namespace viewer {
namespace {

struct ClearImagesRecord {
    const std::vector<matter::VkImageResource*>* images = nullptr;
};

struct ReadTauRecord {
    matter::VkImageResource* image = nullptr;
    VkBuffer destination = VK_NULL_HANDLE;
};

void record_clear_images(VkCommandBuffer command_buffer, void* user_data) {
    const auto& record = *static_cast<ClearImagesRecord*>(user_data);
    const VkClearColorValue clear_tau{{0.0f, 0.0f, 0.0f, 0.0f}};
    const VkImageSubresourceRange range{
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (matter::VkImageResource* image : *record.images) {
        matter::record_image_transition(
            command_buffer, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(command_buffer, image->image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear_tau, 1, &range);
        matter::record_image_transition(
            command_buffer, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

void record_read_tau(VkCommandBuffer command_buffer, void* user_data) {
    const auto& record = *static_cast<ReadTauRecord*>(user_data);
    matter::record_image_transition(
        command_buffer, *record.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(command_buffer, record.image->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           record.destination, 1, &region);
    matter::record_image_transition(
        command_buffer, *record.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

bool same_desc(const matter::CloudShadowLevelDesc& a,
               const matter::CloudShadowLevelDesc& b) {
    return a.width == b.width && a.height == b.height &&
           a.depth == b.depth && a.coverage_m == b.coverage_m;
}

float frame_angle_degrees(const matter::CloudShadowFrame& a,
                          const matter::CloudShadowFrame& b) {
    const float dot = std::fmax(-1.0f, std::fmin(
        matter::detail::cloud_shadow_dot(a.incoming_light_axis,
                                         b.incoming_light_axis),
        1.0f));
    return std::acos(dot) * (180.0f / 3.14159265358979323846f);
}

}  // namespace

VkCloudShadows::~VkCloudShadows() { destroy(); }

bool VkCloudShadows::init(matter::VulkanDevice& vulkan, std::string& error) {
    error.clear();
    if (initialized_) return true;
    vulkan_ = &vulkan;
    device_ = vulkan.device();
    requested_settings_.enabled = false;
    requested_levels_ = matter::resolve_cloud_shadow_levels(requested_settings_);
    if (!create_emergency_images(error)) {
        destroy();
        return false;
    }
    initialized_ = true;
    return true;
}

void VkCloudShadows::request_settings(
    const matter::CloudShadowSettings& settings) {
    matter::CloudShadowSettings clean = settings;
    const auto resolved = matter::resolve_cloud_shadow_levels(settings);
    clean.near_coverage_m = resolved[0].coverage_m;
    clean.far_coverage_m = resolved[1].coverage_m;
    if (!std::isfinite(clean.filter_scale) || clean.filter_scale < 0.0f)
        clean.filter_scale = 1.0f;
    if (!std::isfinite(clean.update_fraction)) clean.update_fraction = 0.25f;
    clean.update_fraction = std::fmax(0.0f,
                                      std::fmin(clean.update_fraction, 1.0f));
    const bool changed = clean.enabled != requested_settings_.enabled ||
        !same_desc(resolved[0], requested_levels_[0]) ||
        !same_desc(resolved[1], requested_levels_[1]) ||
        clean.filter_scale != requested_settings_.filter_scale ||
        clean.update_fraction != requested_settings_.update_fraction;
    requested_settings_ = clean;
    requested_levels_ = resolved;
    if (changed) {
        request_failed_ = false;
        allocation_error_.clear();
    }
}

bool VkCloudShadows::create_emergency_images(std::string& error) {
    std::vector<matter::VkImageResource*> images;
    std::vector<std::shared_ptr<void>> lifetimes;
    for (auto& image : emergency_) {
        if (!matter::create_image(
                *vulkan_, VK_IMAGE_TYPE_3D, VK_FORMAT_R16_SFLOAT, {1, 1, 1},
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                image, error)) {
            for (auto& partial : emergency_) partial.reset();
            return false;
        }
        images.push_back(&image);
        lifetimes.push_back(image.lifetime);
    }
    return clear_images(images, error);
}

bool VkCloudShadows::clear_images(
    const std::vector<matter::VkImageResource*>& images, std::string& error) {
    ClearImagesRecord record{&images};
    std::vector<std::shared_ptr<void>> lifetimes;
    lifetimes.reserve(images.size());
    for (const auto* image : images) lifetimes.push_back(image->lifetime);
    return matter::submit_immediate(
        *vulkan_, record_clear_images, &record, error,
        matter::ImmediateSubmitPhase::staging_upload, std::move(lifetimes));
}

bool VkCloudShadows::create_level_pair(
    const std::array<matter::CloudShadowLevelDesc, 2>& descs,
    LevelPair& pair, std::string& error) {
    failed_candidate_lifetimes_.clear();
    uint32_t allocation_count = 0;
    const auto fail = [&]() {
        for (auto& level : pair) {
            if (level.density.lifetime)
                failed_candidate_lifetimes_.push_back(level.density.lifetime);
            for (auto& image : level.cumulative)
                if (image.lifetime)
                    failed_candidate_lifetimes_.push_back(image.lifetime);
        }
        destroy_level_pair(pair);
        return false;
    };
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    std::vector<matter::VkImageResource*> images;
    for (uint32_t level_index = 0; level_index < 2; ++level_index) {
        auto& level = pair[level_index];
        level.desc = descs[level_index];
        const VkExtent3D extent{level.desc.width, level.desc.height,
                                level.desc.depth};
        if (!matter::create_image(
                *vulkan_, VK_IMAGE_TYPE_3D, VK_FORMAT_R16_SFLOAT, extent,
                usage, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, level.density, error))
            return fail();
        images.push_back(&level.density);
        ++allocation_count;
        for (auto& cumulative : level.cumulative) {
            if (!matter::create_image(
                    *vulkan_, VK_IMAGE_TYPE_3D, VK_FORMAT_R16_SFLOAT, extent,
                    usage, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cumulative, error))
                return fail();
            images.push_back(&cumulative);
            ++allocation_count;
            if (fail_next_bundle_creation_for_test_ && allocation_count == 2) {
                fail_next_bundle_creation_for_test_ = false;
                error = "injected cloud-shadow bundle allocation failure after partial candidate creation";
                return fail();
            }
        }
    }
    if (!clear_images(images, error)) return fail();
    return true;
}

void VkCloudShadows::destroy_level_pair(LevelPair& pair) {
    for (auto& level : pair) {
        level.density.reset();
        level.cumulative[0].reset();
        level.cumulative[1].reset();
        level = {};
    }
}

void VkCloudShadows::retire_active(uint32_t completed_frame_slot) {
    if (!active_) return;
    retired_bundles_.push_back(
        {std::move(active_levels_), completed_frame_slot ^ 1u});
    active_levels_ = {};
    active_ = false;
}

void VkCloudShadows::collect_retired(uint32_t completed_frame_slot) {
    for (auto it = retired_bundles_.begin(); it != retired_bundles_.end();) {
        if (it->protected_slot == completed_frame_slot) {
            destroy_level_pair(it->levels);
            it = retired_bundles_.erase(it);
        } else {
            ++it;
        }
    }
}

bool VkCloudShadows::requested_layout_matches_active() const {
    return active_ && same_desc(active_levels_[0].desc, requested_levels_[0]) &&
           same_desc(active_levels_[1].desc, requested_levels_[1]);
}

std::string VkCloudShadows::allocation_diagnostic(
    const std::string& detail) const {
    char diagnostic[384]{};
    const double mib = static_cast<double>(
        3ull * 2ull *
        (static_cast<uint64_t>(requested_levels_[0].width) *
             requested_levels_[0].height * requested_levels_[0].depth +
         static_cast<uint64_t>(requested_levels_[1].width) *
             requested_levels_[1].height * requested_levels_[1].depth)) /
        (1024.0 * 1024.0);
    std::snprintf(
        diagnostic, sizeof(diagnostic),
        "cloud-shadow allocation failed for near %ux%ux%u and far %ux%ux%u (%.2f MiB): %s",
        requested_levels_[0].width, requested_levels_[0].height,
        requested_levels_[0].depth, requested_levels_[1].width,
        requested_levels_[1].height, requested_levels_[1].depth, mib,
        detail.c_str());
    return diagnostic;
}

bool VkCloudShadows::prepare_frame(
    uint32_t frame_slot, const matter::Float3& camera,
    const matter::Float3& sun_direction, float sun_angular_diameter_deg,
    std::string& error) {
    error.clear();
    if (!initialized_) {
        error = "cloud-shadow resources are not initialized";
        return false;
    }
    if (frame_slot >= 2) {
        error = "cloud-shadow descriptor frame slot is out of range";
        return false;
    }
    collect_retired(frame_slot);
    sun_angular_radius_ = std::isfinite(sun_angular_diameter_deg)
        ? std::fmax(0.0f, sun_angular_diameter_deg) *
              (3.14159265358979323846f / 360.0f)
        : 0.0f;
    if (!requested_settings_.enabled) {
        retire_active(frame_slot);
        request_failed_ = false;
        allocation_error_.clear();
        return true;
    }
    if (request_failed_) return true;

    if (!requested_layout_matches_active()) {
        LevelPair candidate{};
        std::string detail;
        if (!create_level_pair(requested_levels_, candidate, detail)) {
            retire_active(frame_slot);
            request_failed_ = true;
            allocation_error_ = allocation_diagnostic(detail);
            return true;
        }
        retire_active(frame_slot);
        active_levels_ = std::move(candidate);
        active_ = true;
    }

    for (uint32_t index = 0; index < 2; ++index) {
        auto& level = active_levels_[index];
        const matter::CloudShadowFrame next = matter::make_cloud_shadow_frame(
            level.desc, camera, sun_direction);
        if (!matter::detail::cloud_shadow_frame_is_valid(next)) {
            retire_active(frame_slot);
            request_failed_ = true;
            allocation_error_ = allocation_diagnostic(
                "non-finite sun-space coordinate frame");
            return true;
        }
        const float sun_delta = level.current_frame.valid
            ? frame_angle_degrees(level.current_frame, next) : 180.0f;
        level.previous_frame = level.current_frame;
        level.current_frame = next;
        level.history_valid = level.history_valid &&
            !matter::cloud_shadow_requires_full_invalidation(
                level.previous_frame, level.current_frame, sun_delta);
    }
    return true;
}

bool VkCloudShadows::record(VkCommandBuffer, float, std::string& error) {
    error.clear();
    // Task 10 owns only initialized clear resources. Density evaluation and
    // cumulative prefix generation begin in Task 11.
    return initialized_;
}

const CloudShadowLevelBundle& VkCloudShadows::level(uint32_t index) const {
    static const CloudShadowLevelBundle empty{};
    return index < active_levels_.size() ? active_levels_[index] : empty;
}

uint64_t VkCloudShadows::persistent_bytes() const {
    if (!active_) return 0;
    uint64_t voxels = 0;
    for (const auto& level : active_levels_) {
        voxels += static_cast<uint64_t>(level.desc.width) *
                  level.desc.height * level.desc.depth;
    }
    return voxels * 3ull * 2ull;
}

const matter::VkImageResource& VkCloudShadows::environment_image(
    uint32_t index) const {
    if (active_ && index < 4) {
        const uint32_t level_index = index / 2;
        const uint32_t ping = index % 2;
        return active_levels_[level_index].cumulative[ping];
    }
    return emergency_[std::min(index, 3u)];
}

std::array<float, 40> VkCloudShadows::environment_block() const {
    std::array<float, 40> block{};
    for (uint32_t matrix = 0; matrix < 2; ++matrix) {
        for (uint32_t i = 0; i < 4; ++i)
            block[matrix * 16 + i * 5] = 1.0f;
    }
    if (!active_) return block;
    for (uint32_t matrix = 0; matrix < 2; ++matrix) {
        const auto& source = active_levels_[matrix].current_frame.world_to_uvw;
        for (uint32_t row = 0; row < 4; ++row)
            for (uint32_t column = 0; column < 4; ++column)
                block[matrix * 16 + column * 4 + row] =
                    source.m[row * 4 + column];
    }
    block[32] = 1.0f;
    block[33] = static_cast<float>(active_levels_[0].active_index);
    block[34] = static_cast<float>(active_levels_[1].active_index);
    block[36] = active_levels_[0].current_frame.voxel_xy_m;
    block[37] = active_levels_[1].current_frame.voxel_xy_m;
    block[38] = sun_angular_radius_;
    block[39] = requested_settings_.filter_scale;
    return block;
}

bool VkCloudShadows::failed_candidate_destroyed_for_test() const {
    return !failed_candidate_lifetimes_.empty() &&
        std::all_of(failed_candidate_lifetimes_.begin(),
                    failed_candidate_lifetimes_.end(),
                    [](const std::weak_ptr<void>& lifetime) {
                        return lifetime.expired();
                    });
}

bool VkCloudShadows::environment_image_is_clear_for_test(
    uint32_t index, std::string& error) {
    error.clear();
    if (!vulkan_ || index >= 4) {
        error = "cloud-shadow clear readback index is invalid";
        return false;
    }
    matter::VkImageResource& image = active_ ?
        active_levels_[index / 2].cumulative[index % 2] : emergency_[index];
    matter::VkBufferResource readback;
    if (!matter::create_buffer(
            *vulkan_, sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback, error) ||
        !matter::map_buffer(readback, error)) return false;
    ReadTauRecord request{&image, readback.buffer};
    if (!matter::submit_immediate(
            *vulkan_, record_read_tau, &request, error,
            matter::ImmediateSubmitPhase::staging_readback,
            {image.lifetime, readback.lifetime}) ||
        !matter::invalidate_buffer(readback, 0, sizeof(uint16_t), error))
        return false;
    uint16_t bits = 1;
    std::memcpy(&bits, readback.mapped, sizeof(bits));
    return bits == 0;
}

void VkCloudShadows::destroy() {
    if (!vulkan_ && device_ == VK_NULL_HANDLE) return;
    destroy_level_pair(active_levels_);
    for (auto& retired : retired_bundles_) destroy_level_pair(retired.levels);
    retired_bundles_.clear();
    for (auto& image : emergency_) image.reset();
    failed_candidate_lifetimes_.clear();
    vulkan_ = nullptr;
    device_ = VK_NULL_HANDLE;
    active_ = false;
    initialized_ = false;
    request_failed_ = false;
    allocation_error_.clear();
}

}  // namespace viewer
