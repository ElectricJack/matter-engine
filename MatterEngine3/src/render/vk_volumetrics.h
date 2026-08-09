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

#include <cstdint>
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
                         const matter::FogSettings& fog);

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
        float sun_intensity;
        float sun_color[3];
        float phase_g;
        float sky_color[3];
        float temporal_blend;
        uint32_t history_valid;
        float camera_near;
        float camera_far;
        float pad2;
    };
    static_assert(sizeof(ScatterConstants) == 208);

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
    VkPipeline scatter_pipeline_ = VK_NULL_HANDLE;
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
    // Live cloud decks. cloud_count_ is the number of leading entries that
    // are enabled AND well formed — the same number that selects the density
    // pipeline, so the shader can never read past what was uploaded.
    matter::CloudLayer cloud_layers_[matter::kMaxCloudLayers]{};
    int cloud_count_ = 0;
    bool cloud_overflow_warned_ = false;

    // Lighting state (set externally before record).
    float sun_direction_[3] = {-0.45f, -0.80f, -0.35f};
    float sun_intensity_ = 1.0f;
    float sun_color_[3] = {2.2f, 2.05f, 1.8f};
    float sky_color_[3] = {0.38f, 0.43f, 0.52f};

public:
    // Set lighting state that the scatter pass needs.
    void set_lighting(const VkSceneLighting& lighting);
    void set_environment_descriptor(VkDescriptorSet set) {
        environment_descriptor_set_ = set;
    }
    void invalidate_history() { has_prev_matrices_ = false; }
};

}  // namespace viewer
