#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "matter/cloud_shadow_settings.h"
#include "vk_resources.h"

namespace viewer {

inline bool cloud_shadow_uvw_inside(const matter::Float3& uvw) {
    return std::isfinite(uvw.x) && std::isfinite(uvw.y) &&
           std::isfinite(uvw.z) && uvw.x >= 0.0f && uvw.x <= 1.0f &&
           uvw.y >= 0.0f && uvw.y <= 1.0f && uvw.z >= 0.0f && uvw.z <= 1.0f;
}

inline float cloud_shadow_outer_edge_fade(const matter::Float3& uvw) {
    if (!cloud_shadow_uvw_inside(uvw)) return 0.0f;
    const float edge = std::fmin(std::fmin(uvw.x, uvw.y),
                                 std::fmin(1.0f - uvw.x, 1.0f - uvw.y));
    const float t = std::fmax(0.0f, std::fmin(edge / 0.08f, 1.0f));
    return t * t * (3.0f - 2.0f * t);
}

inline float cloud_shadow_filter_radius_texels(float sun_angular_radius,
                                                float receiver_distance_m,
                                                float voxel_size_m,
                                                float filter_scale) {
    if (!std::isfinite(sun_angular_radius) ||
        !std::isfinite(receiver_distance_m) ||
        !std::isfinite(voxel_size_m) || !std::isfinite(filter_scale) ||
        sun_angular_radius <= 0.0f || receiver_distance_m <= 0.0f ||
        voxel_size_m <= 0.0f || filter_scale <= 0.0f) return 0.0f;
    return std::fmin(sun_angular_radius * receiver_distance_m /
                         voxel_size_m * filter_scale,
                     4.0f);
}

inline float cloud_shadow_reference_tau(
    const matter::Float3& uvw, const std::array<float, 5>& samples) {
    if (!cloud_shadow_uvw_inside(uvw)) return 0.0f;
    float sum = 0.0f;
    for (float sample : samples) {
        if (!std::isfinite(sample)) continue;
        sum += std::fmax(0.0f, std::fmin(sample, 80.0f));
    }
    const float filtered = sum * 0.2f;
    return std::fmax(0.0f, std::fmin(
        filtered * cloud_shadow_outer_edge_fade(uvw), 80.0f));
}

inline float cloud_shadow_reference_transmittance(
    const matter::Float3& uvw, const std::array<float, 5>& samples) {
    if (!cloud_shadow_uvw_inside(uvw)) return 1.0f;
    const float transmittance = std::exp(-cloud_shadow_reference_tau(uvw, samples));
    return std::isfinite(transmittance)
        ? std::fmax(0.0f, std::fmin(transmittance, 1.0f)) : 1.0f;
}

inline float cloud_shadow_reference_blended_transmittance(
    const matter::Float3& near_uvw, const std::array<float, 5>& near_samples,
    const matter::Float3& far_uvw, const std::array<float, 5>& far_samples) {
    const float far_tau = cloud_shadow_reference_tau(far_uvw, far_samples);
    if (!cloud_shadow_uvw_inside(near_uvw)) return std::exp(-far_tau);
    const float near_weight = cloud_shadow_outer_edge_fade(near_uvw);
    const float near_tau = cloud_shadow_reference_tau(near_uvw, near_samples);
    const float tau = far_tau + (near_tau - far_tau) * near_weight;
    const float transmittance = std::exp(-std::fmax(0.0f, std::fmin(tau, 80.0f)));
    return std::isfinite(transmittance)
        ? std::fmax(0.0f, std::fmin(transmittance, 1.0f)) : 1.0f;
}

struct CloudShadowLevelBundle {
    matter::CloudShadowLevelDesc desc{};
    matter::VkImageResource density;
    matter::VkImageResource cumulative[2];
    uint32_t active_index = 0;
    matter::CloudShadowFrame current_frame{};
    matter::CloudShadowFrame previous_frame{};
    bool history_valid = false;
};

class VkCloudShadows {
public:
    VkCloudShadows() = default;
    ~VkCloudShadows();
    VkCloudShadows(const VkCloudShadows&) = delete;
    VkCloudShadows& operator=(const VkCloudShadows&) = delete;

    bool init(matter::VulkanDevice& vulkan, std::string& error);
    void request_settings(const matter::CloudShadowSettings& settings);
    bool prepare_frame(uint32_t frame_slot, const matter::Float3& camera,
                       const matter::Float3& sun_direction,
                       float sun_angular_diameter_deg, std::string& error);
    bool record(VkCommandBuffer command_buffer, float frame_time,
                std::string& error);
    const CloudShadowLevelBundle& level(uint32_t index) const;
    uint64_t persistent_bytes() const;
    bool active() const { return active_; }
    void destroy();

    const matter::VkImageResource& environment_image(uint32_t index) const;
    std::array<float, 40> environment_block() const;
    const std::string& allocation_error() const { return allocation_error_; }
    void set_fail_next_bundle_creation_for_test(bool enabled) {
        fail_next_bundle_creation_for_test_ = enabled;
    }
    bool failed_candidate_destroyed_for_test() const;
    bool environment_image_is_clear_for_test(uint32_t index,
                                             std::string& error);
    size_t retired_bundle_count_for_test() const {
        return retired_bundles_.size();
    }

private:
    using LevelPair = std::array<CloudShadowLevelBundle, 2>;
    struct RetiredPair {
        LevelPair levels;
        uint32_t protected_slot = 0;
    };

    bool create_emergency_images(std::string& error);
    bool create_level_pair(const std::array<matter::CloudShadowLevelDesc, 2>& descs,
                           LevelPair& pair, std::string& error);
    bool clear_images(const std::vector<matter::VkImageResource*>& images,
                      std::string& error);
    void destroy_level_pair(LevelPair& pair);
    void retire_active(uint32_t completed_frame_slot);
    void collect_retired(uint32_t completed_frame_slot);
    bool requested_layout_matches_active() const;
    std::string allocation_diagnostic(const std::string& detail) const;

    matter::VulkanDevice* vulkan_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    matter::VkImageResource emergency_[4];
    LevelPair active_levels_{};
    std::vector<RetiredPair> retired_bundles_;
    matter::CloudShadowSettings requested_settings_{};
    std::array<matter::CloudShadowLevelDesc, 2> requested_levels_{};
    float sun_angular_radius_ = 0.0f;
    bool active_ = false;
    bool initialized_ = false;
    bool request_failed_ = false;
    bool fail_next_bundle_creation_for_test_ = false;
    std::string allocation_error_;
    std::vector<std::weak_ptr<void>> failed_candidate_lifetimes_;
};

}  // namespace viewer
