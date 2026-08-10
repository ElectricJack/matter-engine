#pragma once
// vk_volumetrics.h -- Vulkan host module for froxel-based volumetric fog.
//
// Manages three compute passes per frame:
//   1. Density  -- injects height fog + emitters into a 3D media texture
//   2. Scatter  -- evaluates in-scattering with shadow rays (ray query) and
//                  temporal reprojection
//   3. Integrate -- front-to-back marches each column for the composite shader
//
// The final vol_integrated 3D texture is sampled by the composite fragment
// shader to blend volumetric fog into the HDR image.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "matter/cloud_layers.h"
#include "matter/math_types.h"
#include "matter/volumetric_quality.h"
#include "vk_resources.h"

namespace matter {
class VulkanDevice;
}  // namespace matter

namespace viewer {

struct GpuVolumeEmitter;
struct FrameMatrices;
struct VkSceneLighting;
}  // namespace viewer

namespace matter {

struct FogSettings;
struct CloudShadowSettings;
struct VulkanVolumetricsSettings;

}  // namespace matter

namespace viewer {

static constexpr uint32_t kVolMaxEmitters = 256;
static constexpr float    kVolFroxelFarRange = 3000.0f;
static constexpr float    kVolShadowFarRange = 300.0f;
static constexpr uint32_t kVolNoiseSize = 32;

struct FroxelDispatchGrid {
    uint32_t density_x = 0;
    uint32_t density_y = 0;
    uint32_t integrate_x = 0;
    uint32_t integrate_y = 0;
};

struct FroxelCameraReference {
    matter::Float3 eye{};
    matter::Float3 forward{0.0f, 0.0f, -1.0f};
    matter::Float3 right{1.0f, 0.0f, 0.0f};
    matter::Float3 up{0.0f, 1.0f, 0.0f};
    float tan_half_fov = 1.0f;
    float aspect_ratio = 1.0f;
    float near_plane = 0.1f;
};

inline matter::Float3 volumetric_camera_eye(
    const matter::Mat4f& world_to_view) {
    const auto& m = world_to_view.m;
    return {
        -(m[0] * m[3] + m[4] * m[7] + m[8] * m[11]),
        -(m[1] * m[3] + m[5] * m[7] + m[9] * m[11]),
        -(m[2] * m[3] + m[6] * m[7] + m[10] * m[11])};
}

inline bool world_to_froxel_reference(
    const FroxelCameraReference& camera, const matter::Float3& world,
    matter::Float3& uvw) {
    constexpr float froxel_near = 0.1f;
    constexpr float froxel_far = 3000.0f;
    const matter::Float3 relative{world.x - camera.eye.x,
                                  world.y - camera.eye.y,
                                  world.z - camera.eye.z};
    const auto dot = [](const matter::Float3& a, const matter::Float3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    const float depth = dot(relative, camera.forward);
    const float tan_y = camera.tan_half_fov;
    const float tan_x = tan_y * camera.aspect_ratio;
    if (!std::isfinite(depth) || depth + 1.0e-5f < camera.near_plane ||
        depth > froxel_far || !(tan_x > 0.0f) || !(tan_y > 0.0f))
        return false;
    const float ndc_x = dot(relative, camera.right) / (depth * tan_x);
    const float ndc_y = dot(relative, camera.up) / (depth * tan_y);
    const float u = ndc_x * 0.5f + 0.5f;
    const float v = 0.5f - ndc_y * 0.5f;
    if (!std::isfinite(u) || !std::isfinite(v) || u < -1.0e-5f ||
        u > 1.0f + 1.0e-5f || v < -1.0e-5f || v > 1.0f + 1.0e-5f)
        return false;
    const float clamped_depth = std::clamp(depth, froxel_near, froxel_far);
    uvw = {std::clamp(u, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f),
           std::log(clamped_depth / froxel_near) /
               std::log(froxel_far / froxel_near)};
    return true;
}

inline matter::Float3 froxel_to_world_reference(
    const FroxelCameraReference& camera, const matter::Float3& uvw) {
    constexpr float froxel_near = 0.1f;
    constexpr float froxel_far = 3000.0f;
    const float depth = froxel_near *
        std::pow(froxel_far / froxel_near, std::clamp(uvw.z, 0.0f, 1.0f));
    const float view_x = (uvw.x * 2.0f - 1.0f) * depth *
                         camera.tan_half_fov * camera.aspect_ratio;
    const float view_y = (1.0f - uvw.y * 2.0f) * depth *
                         camera.tan_half_fov;
    return {camera.eye.x + camera.forward.x * depth +
                camera.right.x * view_x + camera.up.x * view_y,
            camera.eye.y + camera.forward.y * depth +
                camera.right.y * view_x + camera.up.y * view_y,
            camera.eye.z + camera.forward.z * depth +
                camera.right.z * view_x + camera.up.z * view_y};
}

inline float froxel_ray_exit_distance_reference(
    const FroxelCameraReference& camera, const matter::Float3& world,
    const matter::Float3& direction) {
    constexpr float froxel_far = 3000.0f;
    const matter::Float3 relative{world.x - camera.eye.x,
                                  world.y - camera.eye.y,
                                  world.z - camera.eye.z};
    const auto dot = [](const matter::Float3& a, const matter::Float3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    const float px = dot(relative, camera.right);
    const float py = dot(relative, camera.up);
    const float pz = dot(relative, camera.forward);
    const float dx = dot(direction, camera.right);
    const float dy = dot(direction, camera.up);
    const float dz = dot(direction, camera.forward);
    const float tan_y = std::max(camera.tan_half_fov, 1.0e-6f);
    const float tan_x = std::max(tan_y * camera.aspect_ratio, 1.0e-6f);
    const float a[6]{pz - camera.near_plane, froxel_far - pz,
                     pz * tan_x - px, pz * tan_x + px,
                     pz * tan_y - py, pz * tan_y + py};
    const float b[6]{dz, -dz, dz * tan_x - dx, dz * tan_x + dx,
                     dz * tan_y - dy, dz * tan_y + dy};
    float exit_distance = std::numeric_limits<float>::max();
    for (size_t plane = 0; plane < 6; ++plane) {
        if (a[plane] < -1.0e-4f) return 0.0f;
        if (b[plane] < -1.0e-7f)
            exit_distance = std::min(exit_distance,
                                     std::max(a[plane], 0.0f) / -b[plane]);
    }
    return std::isfinite(exit_distance) ? std::max(exit_distance, 0.0f) : 0.0f;
}

// Independent CPU references for the numerical Task 12 gates. They use
// literal coefficients from the approved shader contract and deliberately
// accept both start/end coarse tau so the no-overlap test can catch choosing
// the wrong cumulative sample.
struct CloudSelfShadowReference {
    float marched_distance_m = 0.0f;
    float tau_local_full = 0.0f;
    float tau_remaining_coarse = 0.0f;
    float tau_total = 0.0f;
    float transmittance = 1.0f;
    uint32_t samples_taken = 0;
    bool stopped_at_froxel_exit = false;
};

inline CloudSelfShadowReference cloud_self_shadow_constant_slab_reference(
    float sigma, float requested_distance_m, float froxel_exit_distance_m,
    uint32_t steps, float coarse_start_tau, float coarse_end_tau) {
    CloudSelfShadowReference result{};
    (void)coarse_start_tau;  // The start prefix is exactly the overlap to omit.
    if (!std::isfinite(sigma) || sigma < 0.0f) sigma = 0.0f;
    if (!std::isfinite(requested_distance_m) || requested_distance_m < 0.0f)
        requested_distance_m = 0.0f;
    if (!std::isfinite(froxel_exit_distance_m) ||
        froxel_exit_distance_m < 0.0f)
        froxel_exit_distance_m = 0.0f;
    steps = std::min(steps, 32u);
    if (steps > 0 && requested_distance_m > 0.0f &&
        froxel_exit_distance_m > 0.0f) {
        result.marched_distance_m =
            std::min(requested_distance_m, froxel_exit_distance_m);
        const float step_m = result.marched_distance_m /
                             static_cast<float>(steps);
        for (uint32_t i = 0; i < steps; ++i) {
            result.tau_local_full += sigma * step_m;
            ++result.samples_taken;
        }
        result.stopped_at_froxel_exit =
            froxel_exit_distance_m < requested_distance_m;
    }
    result.tau_remaining_coarse =
        std::isfinite(coarse_end_tau) ? std::clamp(coarse_end_tau, 0.0f, 80.0f)
                                      : 0.0f;
    result.tau_total = std::clamp(
        result.tau_local_full + result.tau_remaining_coarse, 0.0f, 80.0f);
    result.transmittance = std::exp(-result.tau_total);
    return result;
}

struct CloudLightingReference {
    float cloud_radiance = 0.0f;
    float fog_radiance = 0.0f;
    float normalized_order_energy = 0.0f;
};

inline float cloud_hg_phase_reference(float mu, float g) {
    constexpr float pi = 3.14159265358979323846f;
    const float g2 = g * g;
    const float denominator = std::max(
        1.0f + g2 - 2.0f * g * std::clamp(mu, -1.0f, 1.0f), 1.0e-6f);
    return (1.0f - g2) /
           (4.0f * pi * denominator * std::sqrt(denominator));
}

inline CloudLightingReference cloud_lighting_reference(
    float cloud_extinction, float fog_scattering, float tau_total,
    float tau_local_full, float mu, int orders, float strength,
    float powder_strength, float direct, float ambient, float fog_phase_g) {
    CloudLightingReference result{};
    cloud_extinction = std::isfinite(cloud_extinction)
        ? std::max(cloud_extinction, 0.0f) : 0.0f;
    fog_scattering = std::isfinite(fog_scattering)
        ? std::max(fog_scattering, 0.0f) : 0.0f;
    tau_total = std::isfinite(tau_total)
        ? std::clamp(tau_total, 0.0f, 80.0f) : 0.0f;
    tau_local_full = std::isfinite(tau_local_full)
        ? std::clamp(tau_local_full, 0.0f, 80.0f) : 0.0f;
    orders = std::clamp(orders, 1, 4);
    strength = std::isfinite(strength)
        ? std::clamp(strength, 0.0f, 1.0f) : 0.0f;
    powder_strength = std::isfinite(powder_strength)
        ? std::clamp(powder_strength, 0.0f, 1.0f) : 0.0f;
    float weighted_orders = 0.0f;
    float order_energy = 0.0f;
    for (int order = 0; order < orders; ++order) {
        const float extinction_scale = std::pow(0.55f, float(order));
        const float anisotropy_scale = std::pow(0.5f, float(order));
        const float energy = order == 0
            ? 1.0f : strength * std::pow(0.5f, float(order - 1));
        const float phase =
            0.8f * cloud_hg_phase_reference(mu, 0.85f * anisotropy_scale) +
            0.2f * cloud_hg_phase_reference(mu, -0.30f * anisotropy_scale);
        weighted_orders += energy * std::exp(-tau_total * extinction_scale) *
                           phase;
        order_energy += energy;
    }
    const float maximum_energy = 1.0f + 2.0f * strength;
    if (order_energy > maximum_energy && order_energy > 0.0f)
        weighted_orders *= maximum_energy / order_energy;
    result.normalized_order_energy = std::min(order_energy, maximum_energy);
    const float powder = 1.0f + powder_strength *
        (std::min(2.0f, 1.0f + (1.0f - std::exp(-2.0f * tau_local_full))) -
         1.0f);
    result.cloud_radiance = 0.99f * cloud_extinction *
        (std::max(direct, 0.0f) * weighted_orders +
         std::max(ambient, 0.0f)) * powder;
    result.fog_radiance = fog_scattering *
        (std::max(direct, 0.0f) *
             cloud_hg_phase_reference(mu, std::clamp(fog_phase_g, -0.99f, 0.99f)) +
         std::max(ambient, 0.0f));
    return result;
}

class VkVolumetrics {
public:
    VkVolumetrics();
    ~VkVolumetrics();

    VkVolumetrics(const VkVolumetrics&) = delete;
    VkVolumetrics& operator=(const VkVolumetrics&) = delete;

    // Create all GPU resources (images, buffers, pipelines, descriptors).
    // Returns false and populates |error| on any Vulkan failure.
    bool init(matter::VulkanDevice& vulkan,
              VkDescriptorSetLayout environment_layout, std::string& error);

    // Latch the per-frame settings from the UI / world definition.
    void update_settings(const matter::VulkanVolumetricsSettings& vol,
                         const matter::FogSettings& fog,
                         const matter::CloudShadowSettings& shadows);

    // Upload the gathered emitter list for the current frame.
    void update_emitters(matter::VulkanDevice& vulkan,
                         const std::vector<GpuVolumeEmitter>& emitters);

    // Called while the renderer has acquired this frame slot but before any
    // scene descriptor is bound. A successful swap is therefore visible to
    // this frame's composite descriptor as well as the compute passes.
    bool prepare_froxel_bundle(uint32_t frame_slot, std::string& error);

    // Record the three compute dispatches into |cmd|.  No-ops when volumetrics
    // is disabled or ray query is unavailable.  Previous-frame matrices for
    // temporal reprojection are stored internally from the prior call.
    bool record(VkCommandBuffer cmd,
                uint32_t frame_slot,
                matter::VkImageResource& depth_image,
                VkAccelerationStructureKHR tlas,
                const FrameMatrices& matrices,
                float frame_time,
                std::string& error);

    // The integration output -- sampled by the composite fragment shader.
    matter::VkImageResource& vol_integrated() { return active_bundle_.integrated; }
    const matter::VkImageResource& vol_integrated() const { return active_bundle_.integrated; }
    const matter::VkImageResource& cloud_density_or_dummy() const {
        return active_bundle_.enhanced_clouds ? active_bundle_.cloud_density : cloud_density_dummy_;
    }
    matter::VkImageResource& cloud_density_or_dummy() {
        return active_bundle_.enhanced_clouds ? active_bundle_.cloud_density : cloud_density_dummy_;
    }
    matter::FroxelGridDimensions dimensions() const { return active_bundle_.dimensions; }
    matter::FroxelXyScale effective_xy_scale() const;
    matter::FroxelDepthSlices effective_depth_slices() const;
    FroxelDispatchGrid last_dispatch_grid() const { return last_dispatch_grid_; }
    bool last_scatter_history_was_valid_for_test() const {
        return last_scatter_history_was_valid_;
    }
    uint64_t resource_generation() const { return resource_generation_; }
    bool allocation_rejected() const { return allocation_rejected_; }
    const std::string& allocation_error() const { return allocation_error_; }
    void set_fail_next_bundle_creation_for_test(bool enabled) {
        fail_next_bundle_creation_for_test_ = enabled;
    }
    void set_fail_next_bundle_descriptor_allocation_for_test(bool enabled) {
        fail_next_bundle_descriptor_allocation_for_test_ = enabled;
    }
    // Real-device Task 9 assertions.  These report the active production
    // bundle; the 1^3 stable descriptor dummy is intentionally excluded from
    // grid accounting.
    uint32_t grid_rgba16f_volume_count_for_test() const;
    bool cloud_density_allocated_for_test() const;
    matter::FroxelGridDimensions cloud_density_dimensions_for_test() const;
    uint64_t grid_bytes_for_test() const;
    bool readback_density_voxel_for_test(uint32_t x, uint32_t y, uint32_t z,
                                         matter::Float4& media,
                                         float& cloud_density,
                                         std::string& error);
    bool readback_integrated_voxel_for_test(uint32_t x, uint32_t y,
                                            uint32_t z,
                                            matter::Float4& integrated,
                                            std::string& error);
    bool readback_scatter_voxel_for_test(uint32_t x, uint32_t y, uint32_t z,
                                         matter::Float4& scatter,
                                         std::string& error);
    // Whether volumetrics is currently active (enabled + ray query available).
    bool active() const { return enabled_ && ray_query_available_; }

    // Release all Vulkan resources.
    void destroy();

private:
    // Push-constant structs matching the GLSL shaders exactly.
    struct DensityConstants {
        float clip_to_world[16];    // mat4 (column-major for GLSL)
        float camera_pos[3];
        float frame_time;
        float fog_density;
        float fog_floor;
        float fog_falloff;
        float camera_near;
        float fog_color[3];
        float camera_far;
        float fog_wind[3];
        float pad2;
    };
    static_assert(sizeof(DensityConstants) == 128);

    struct ScatterConstants {
        float clip_to_world[16];        // mat4
        float prev_world_to_clip[16];   // mat4
        float camera_pos[3];
        uint32_t frame_index;
        float sun_dir[3];
        uint32_t local_sun_march_steps;
        float phase_g;
        float temporal_blend;
        uint32_t history_valid;
        float camera_near;
        float camera_far;
        uint32_t multiple_scattering_orders;
        float multiple_scattering_strength;
        float powder_strength;
        float camera_fwd[3];
        float tan_half_fov;
        float camera_right[3];
        float aspect_ratio;
        float camera_up[3];
        float local_march_distance_m;
    };
    static_assert(sizeof(ScatterConstants) == 240);
    static_assert(offsetof(ScatterConstants, camera_pos) == 128);
    static_assert(offsetof(ScatterConstants, local_sun_march_steps) == 156);
    static_assert(offsetof(ScatterConstants, camera_fwd) == 192);
    static_assert(offsetof(ScatterConstants, local_march_distance_m) == 236);

    bool create_noise_texture(matter::VulkanDevice& vulkan, std::string& error);
    struct FroxelBundle {
        matter::FroxelGridDimensions dimensions{};
        bool enhanced_clouds = false;  // reserved for Task 9; false in Task 8.
        matter::VkImageResource media;
        matter::VkImageResource scatter[2];
        matter::VkImageResource integrated;
        matter::VkImageResource cloud_density;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet density_set = VK_NULL_HANDLE;
        // The TLAS/depth bindings change for each recycled renderer frame
        // slot. Keep them separate from temporal ping-pong so record() never
        // updates a descriptor already bound by the other slot.
        VkDescriptorSet scatter_sets[2][2] = {};  // [frame_slot][ping]
        VkDescriptorSet integrate_sets[2] = {};
        uint32_t ping_index = 0;
    };
    bool create_froxel_bundle(matter::VulkanDevice& vulkan,
                              matter::FroxelGridDimensions dimensions,
                              FroxelBundle& bundle, std::string& error);
    void destroy_froxel_bundle(FroxelBundle& bundle);
    bool replace_froxel_bundle(uint32_t completed_frame_slot, std::string& error);
    bool create_bundle_descriptors(FroxelBundle& bundle, std::string& error);
    bool create_emitter_buffer(matter::VulkanDevice& vulkan, std::string& error);
    bool create_cloud_buffer(matter::VulkanDevice& vulkan, std::string& error);
    bool create_samplers(matter::VulkanDevice& vulkan, std::string& error);
    bool create_density_pipeline(matter::VulkanDevice& vulkan, std::string& error);
    bool create_scatter_pipeline(matter::VulkanDevice& vulkan, std::string& error);
    bool create_integrate_pipeline(matter::VulkanDevice& vulkan, std::string& error);

    // Runtime-sized resources are replaced only at a completed frame slot.
    FroxelBundle active_bundle_{};
    struct RetiredBundle { FroxelBundle bundle; uint32_t protected_slot = 0; };
    std::vector<RetiredBundle> retired_bundles_;
    matter::FroxelGridDimensions requested_dimensions_{160, 90, 128};
    uint64_t resource_generation_ = 0;
    uint32_t prepared_frame_slot_ = UINT32_MAX;
    bool allocation_rejected_ = false;
    std::string allocation_error_;
    bool fail_next_bundle_creation_for_test_ = false;
    bool fail_next_bundle_descriptor_allocation_for_test_ = false;
    FroxelDispatchGrid last_dispatch_grid_{};
    bool last_scatter_history_was_valid_ = false;
    matter::VulkanDevice* vulkan_ = nullptr;
    matter::VkImageResource noise_texture_;
    // Stable binding-4 backing for Current cost; excluded from grid accounting.
    matter::VkImageResource cloud_density_dummy_;
    bool enhanced_clouds_requested_ = false;

    // Emitter SSBO: uint32 count at offset 0, then GpuVolumeEmitter[256]
    // starting at offset 16 (std430 alignment).
    matter::VkBufferResource emitter_ssbo_;

    // Cloud-layer SSBO: GpuCloudLayer[kMaxCloudLayers], always bound. The
    // enabled count is a specialization constant, not a field in here — see
    // density_pipelines_ below.
    matter::VkBufferResource cloud_ssbo_;

    // Samplers.
    VkSampler linear_clamp_sampler_ = VK_NULL_HANDLE;
    VkSampler linear_border_sampler_ = VK_NULL_HANDLE;
    VkSampler linear_repeat_sampler_ = VK_NULL_HANDLE;

    // Density pass resources.
    VkDescriptorSetLayout density_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout density_pipeline_layout_ = VK_NULL_HANDLE;
    // One pipeline per enabled-cloud-layer count, all from the same SPIR-V
    // module with a different value baked into vol_density.comp's
    // `constant_id = 0`. Index IS the layer count, so record() indexes
    // straight by it. Built up front in create_density_pipeline: five compute
    // compiles at startup beats a driver compile in the frame a layer is
    // switched on, and it means every permutation is validated on every run.
    VkPipeline density_pipelines_[matter::kMaxCloudLayers + 1][2] = {};

    // Scatter pass resources (2 descriptor sets for ping-pong).
    VkDescriptorSetLayout scatter_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout environment_set_layout_ = VK_NULL_HANDLE;  // borrowed
    VkPipelineLayout scatter_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline scatter_pipelines_[2] = {};
    VkDescriptorSet environment_descriptor_set_ = VK_NULL_HANDLE;

    // Integrate pass resources.
    VkDescriptorSetLayout integrate_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout integrate_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline integrate_pipeline_ = VK_NULL_HANDLE;

    // State.
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t ping_index_ = 0;
    uint32_t frame_index_ = 0;
    bool ray_query_available_ = false;
    bool enabled_ = false;
    bool initialized_ = false;

    // Temporal reprojection: previous frame's world→clip matrix.
    matter::Mat4f prev_world_to_clip_{};
    bool has_prev_matrices_ = false;

    // Latched settings.
    float temporal_blend_ = 0.85f;
    float phase_g_ = 0.3f;
    float fog_density_ = 0.0f;
    float fog_floor_ = 0.0f;
    float fog_falloff_ = 30.0f;
    float fog_color_[3] = {0.9f, 0.92f, 0.95f};
    float fog_wind_[3] = {0.0f, 0.0f, 0.0f};
    uint32_t local_sun_march_steps_ = 8;
    float local_march_distance_m_ = 250.0f;
    uint32_t multiple_scattering_orders_ = 2;
    float multiple_scattering_strength_ = 0.55f;
    float powder_strength_ = 0.25f;
    bool settings_initialized_ = false;
    // Live cloud decks. cloud_count_ is the number of leading entries that
    // are enabled AND well formed — the same number that selects the density
    // pipeline, so the shader can never read past what was uploaded.
    matter::CloudLayer cloud_layers_[matter::kMaxCloudLayers]{};
    int cloud_count_ = 0;
    bool cloud_overflow_warned_ = false;

    // Lighting state (set externally before record).
    float sun_direction_[3] = {-0.45f, -0.80f, -0.35f};

public:
    // Set lighting state that the scatter pass needs.
    void set_lighting(const VkSceneLighting& lighting);
    void set_environment_descriptor(VkDescriptorSet set) {
        environment_descriptor_set_ = set;
    }
    void invalidate_history() { has_prev_matrices_ = false; }
};

}  // namespace viewer
