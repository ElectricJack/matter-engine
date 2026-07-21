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

#include "matter/math_types.h"
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

// Froxel grid dimensions.
static constexpr uint32_t kVolGridW = 160;
static constexpr uint32_t kVolGridH = 90;
static constexpr uint32_t kVolGridD = 128;
static constexpr uint32_t kVolMaxEmitters = 256;
static constexpr float    kVolFarRange = 300.0f;
static constexpr uint32_t kVolNoiseSize = 32;

class VkVolumetrics {
public:
    VkVolumetrics();
    ~VkVolumetrics();

    VkVolumetrics(const VkVolumetrics&) = delete;
    VkVolumetrics& operator=(const VkVolumetrics&) = delete;

    // Create all GPU resources (images, buffers, pipelines, descriptors).
    // Returns false and populates |error| on any Vulkan failure.
    bool init(matter::VulkanDevice& vulkan, std::string& error);

    // Latch the per-frame settings from the UI / world definition.
    void update_settings(const matter::VulkanVolumetricsSettings& vol,
                         const matter::FogSettings& fog);

    // Upload the gathered emitter list for the current frame.
    void update_emitters(matter::VulkanDevice& vulkan,
                         const std::vector<GpuVolumeEmitter>& emitters);

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
    matter::VkImageResource& vol_integrated() { return vol_integrated_; }
    const matter::VkImageResource& vol_integrated() const { return vol_integrated_; }

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
    bool create_volume_images(matter::VulkanDevice& vulkan, std::string& error);
    bool create_emitter_buffer(matter::VulkanDevice& vulkan, std::string& error);
    bool create_samplers(matter::VulkanDevice& vulkan, std::string& error);
    bool create_density_pipeline(matter::VulkanDevice& vulkan, std::string& error);
    bool create_scatter_pipeline(matter::VulkanDevice& vulkan, std::string& error);
    bool create_integrate_pipeline(matter::VulkanDevice& vulkan, std::string& error);

    // Persistent volume images.
    matter::VkImageResource vol_media_;
    matter::VkImageResource vol_scatter_[2];    // ping-pong for temporal
    matter::VkImageResource vol_integrated_;
    matter::VkImageResource noise_texture_;

    // Emitter SSBO: uint32 count at offset 0, then GpuVolumeEmitter[256]
    // starting at offset 16 (std430 alignment).
    matter::VkBufferResource emitter_ssbo_;

    // Samplers.
    VkSampler linear_clamp_sampler_ = VK_NULL_HANDLE;
    VkSampler linear_repeat_sampler_ = VK_NULL_HANDLE;

    // Density pass resources.
    VkDescriptorSetLayout density_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout density_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline density_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool density_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet density_set_ = VK_NULL_HANDLE;

    // Scatter pass resources (2 descriptor sets for ping-pong).
    VkDescriptorSetLayout scatter_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout scatter_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline scatter_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool scatter_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet scatter_sets_[2] = {};   // [ping_index] is write target

    // Integrate pass resources.
    VkDescriptorSetLayout integrate_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout integrate_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline integrate_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool integrate_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet integrate_sets_[2] = {};  // one per scatter output

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
    float fog_density_mul_ = 1.0f;
    float fog_floor_offset_ = 0.0f;
    float fog_falloff_mul_ = 1.0f;
    float fog_color_mul_[3] = {1.0f, 1.0f, 1.0f};
    float fog_wind_mul_[3] = {1.0f, 1.0f, 1.0f};

    // Lighting state (set externally before record).
    float sun_direction_[3] = {-0.45f, -0.80f, -0.35f};
    float sun_intensity_ = 1.0f;
    float sun_color_[3] = {2.2f, 2.05f, 1.8f};
    float sky_color_[3] = {0.38f, 0.43f, 0.52f};

public:
    // Set lighting state that the scatter pass needs.
    void set_lighting(const VkSceneLighting& lighting);
};

}  // namespace viewer
