#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "matter/cloud_layers.h"
#include "matter/cloud_shadow_settings.h"
#include "vk_resources.h"

namespace matter { struct FogSettings; }

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

template <size_t N>
inline std::array<float, N> cloud_shadow_prefix_integrate(
    const std::array<float, N>& density, float voxel_depth_m) {
    std::array<float, N> cumulative{};
    if (!std::isfinite(voxel_depth_m) || voxel_depth_m <= 0.0f)
        return cumulative;
    float tau = 0.0f;
    for (size_t z = 0; z < N; ++z) {
        float sigma = density[z];
        if (!std::isfinite(sigma) || sigma < 0.0f) sigma = 0.0f;
        tau = std::fmin(tau + sigma * voxel_depth_m, 80.0f);
        cumulative[z] = std::isfinite(tau) ? tau : 0.0f;
    }
    return cumulative;
}

inline matter::Float3 cloud_shadow_previous_uvw_for_voxel(
    const matter::CloudShadowFrame& current,
    const matter::CloudShadowFrame& previous,
    const matter::CloudShadowLevelDesc& level,
    uint32_t x, uint32_t y, uint32_t z) {
    if (level.width == 0 || level.height == 0 || level.depth == 0)
        return {-1.0f, -1.0f, -1.0f};
    const matter::Float3 uvw{
        (static_cast<float>(x) + 0.5f) / static_cast<float>(level.width),
        (static_cast<float>(y) + 0.5f) / static_cast<float>(level.height),
        (static_cast<float>(z) + 0.5f) / static_cast<float>(level.depth)};
    const auto& c = current.uvw_to_world.m;
    const matter::Float3 world{
        c[0] * uvw.x + c[1] * uvw.y + c[2] * uvw.z + c[3],
        c[4] * uvw.x + c[5] * uvw.y + c[6] * uvw.z + c[7],
        c[8] * uvw.x + c[9] * uvw.y + c[10] * uvw.z + c[11]};
    const auto& p = previous.world_to_uvw.m;
    return {p[0] * world.x + p[1] * world.y + p[2] * world.z + p[3],
            p[4] * world.x + p[5] * world.y + p[6] * world.z + p[7],
            p[8] * world.x + p[9] * world.y + p[10] * world.z + p[11]};
}

inline uint32_t cloud_shadow_phase_count(float update_fraction) {
    if (!std::isfinite(update_fraction) || update_fraction <= 0.0f)
        update_fraction = 0.0625f;
    update_fraction = std::fmax(0.0625f, std::fmin(update_fraction, 1.0f));
    const float rounded = std::round(1.0f / update_fraction);
    return static_cast<uint32_t>(std::fmax(1.0f, std::fmin(rounded, 16.0f)));
}

inline uint32_t cloud_shadow_tile_hash(uint32_t x, uint32_t y,
                                       uint32_t level) {
    uint32_t hash = (x / 8u) * 0x8da6b343u ^
                    (y / 8u) * 0xd8163841u ^ level * 0xcb1ab31fu;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    return hash;
}

inline bool cloud_shadow_column_selected(
    bool history_valid, bool previous_out_of_bounds,
    const std::array<uint32_t, 2>& column, uint32_t level,
    uint32_t frame_index, float update_fraction) {
    if (!history_valid || previous_out_of_bounds) return true;
    const uint32_t phases = cloud_shadow_phase_count(update_fraction);
    return cloud_shadow_tile_hash(column[0], column[1], level) % phases ==
           frame_index % phases;
}

inline bool cloud_shadow_direct_sun_visible(
    const matter::Float3& stored_incoming_from_sun) {
    return std::isfinite(stored_incoming_from_sun.x) &&
           std::isfinite(stored_incoming_from_sun.y) &&
           std::isfinite(stored_incoming_from_sun.z) &&
           -stored_incoming_from_sun.y > 0.0f;
}

struct CloudShadowLevelBundle {
    matter::CloudShadowLevelDesc desc{};
    matter::VkImageResource density;
    matter::VkImageResource cumulative[2];
    uint32_t active_index = 0;
    matter::CloudShadowFrame current_frame{};
    matter::CloudShadowFrame previous_frame{};
    bool history_valid = false;
    matter::VkBufferResource generation_constants[2];
    VkDescriptorPool generation_descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet generation_sets[2][2]{};
};

class VkCloudShadows {
public:
    VkCloudShadows() = default;
    ~VkCloudShadows();
    VkCloudShadows(const VkCloudShadows&) = delete;
    VkCloudShadows& operator=(const VkCloudShadows&) = delete;

    bool init(matter::VulkanDevice& vulkan, std::string& error);
    void request_settings(const matter::CloudShadowSettings& settings);
    void request_cloud_layers(const matter::FogSettings& fog);
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
    void set_density_override_for_test(float even_sigma, int32_t nan_slice,
                                       float odd_sigma,
                                       bool invalidate_history);
    bool generate_for_test(uint32_t frame_slot,
                           const matter::Float3& camera,
                           const matter::Float3& sun_direction,
                           float frame_time, std::string& error);
    bool readback_cumulative_voxel_for_test(
        uint32_t level, uint32_t ping, uint32_t x, uint32_t y, uint32_t z,
        float& value, uint16_t& raw, std::string& error);
    uint32_t last_generation_dispatch_count_for_test() const {
        return last_generation_dispatch_count_;
    }
    void append_frame_lifetimes(
        uint32_t frame_slot,
        std::vector<std::shared_ptr<void>>& lifetimes) const;

private:
    using LevelPair = std::array<CloudShadowLevelBundle, 2>;
    struct RetiredPair {
        LevelPair levels;
        uint32_t protected_slot = 0;
    };

    bool create_emergency_images(std::string& error);
    bool create_generation_resources(std::string& error);
    bool create_level_descriptors(CloudShadowLevelBundle& level,
                                  std::string& error);
    bool create_level_pair(const std::array<matter::CloudShadowLevelDesc, 2>& descs,
                           LevelPair& pair, std::string& error);
    bool clear_images(const std::vector<matter::VkImageResource*>& images,
                      std::string& error);
    void destroy_level_pair(LevelPair& pair);
    void retire_active(uint32_t completed_frame_slot);
    void collect_retired(uint32_t completed_frame_slot);
    bool requested_layout_matches_active() const;
    std::string allocation_diagnostic(const std::string& detail) const;
    bool readback_voxel(matter::VkImageResource& image,
                        uint32_t x, uint32_t y, uint32_t z,
                        float& value, uint16_t& raw, std::string& error);

    matter::VulkanDevice* vulkan_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    matter::VkImageResource emergency_[4];
    matter::VkBufferResource cloud_layer_ssbo_[2];
    VkSampler generation_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout generation_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout generation_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline reproject_pipeline_ = VK_NULL_HANDLE;
    VkPipeline density_pipeline_ = VK_NULL_HANDLE;
    VkPipeline integrate_pipeline_ = VK_NULL_HANDLE;
    LevelPair active_levels_{};
    std::vector<RetiredPair> retired_bundles_;
    matter::CloudShadowSettings requested_settings_{};
    std::array<matter::CloudShadowLevelDesc, 2> requested_levels_{};
    float sun_angular_radius_ = 0.0f;
    bool active_ = false;
    bool initialized_ = false;
    bool request_failed_ = false;
    bool fail_next_bundle_creation_for_test_ = false;
    bool force_history_invalidation_ = true;
    bool direct_sun_visible_ = false;
    uint32_t prepared_frame_slot_ = 0;
    uint32_t frame_index_ = 0;
    uint32_t last_generation_dispatch_count_ = 0;
    uint32_t cloud_layer_count_ = 0;
    std::array<matter::GpuCloudLayer, matter::kMaxCloudLayers>
        packed_cloud_layers_{};
    int32_t density_override_mode_ = 0;
    int32_t density_override_nan_slice_ = -1;
    float density_override_even_sigma_ = 0.0f;
    float density_override_odd_sigma_ = 0.0f;
    std::string allocation_error_;
    std::vector<std::weak_ptr<void>> failed_candidate_lifetimes_;
};

}  // namespace viewer
