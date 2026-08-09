#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

#include "matter/atmosphere.h"
#include "matter/math_types.h"
#include "vk_resources.h"

namespace matter {
class VulkanDevice;
}

namespace viewer {

// Owns the physical atmosphere lookup textures.  Task 6 deliberately stops at
// producing these immutable resources; production lighting consumers bind them
// in Task 7.
class VkAtmosphere {
public:
    bool init(matter::VulkanDevice&, std::string& error);
    void request_settings(const matter::AtmosphereSettings&);
    bool record(VkCommandBuffer, float camera_world_y,
                const matter::Float3& to_sun, std::string& error);
    const matter::VkImageResource& sky_view() const;
    const matter::VkImageResource& irradiance_sh() const;
    matter::Float3 direct_sun_transmittance(float camera_world_y,
                                             const matter::Float3& to_sun) const;
    bool readback_transmittance_for_test(matter::VulkanDevice&,
                                         uint32_t x, uint32_t y,
                                         matter::Float3& out,
                                         std::string& error) const;
    uint64_t generation_serial() const { return generation_serial_; }
    bool generated_this_frame() const { return generated_this_frame_; }
    void destroy();

private:
    static constexpr uint32_t kTransmittanceWidth = 256;
    static constexpr uint32_t kTransmittanceHeight = 64;
    static constexpr uint32_t kMultiscatterSize = 32;
    static constexpr uint32_t kSkyViewWidth = 192;
    static constexpr uint32_t kSkyViewHeight = 108;
    static constexpr uint32_t kIrradianceSize = 3;

    bool create_images(matter::VulkanDevice&, std::string& error);
    bool create_pipelines(matter::VulkanDevice&, std::string& error);
    bool initialize_emergency(matter::VulkanDevice&, std::string& error);
    bool coefficient_change_pending() const;
    bool view_change_pending(float camera_world_y, const matter::Float3& to_sun) const;
    bool record_dispatches(VkCommandBuffer, bool coefficients_dirty, float camera_world_y,
                           const matter::Float3& to_sun, std::string& error);

    matter::VulkanDevice* vulkan_ = nullptr;
    matter::VkImageResource transmittance_;
    matter::VkImageResource multiscatter_;
    matter::VkImageResource sky_view_;
    matter::VkImageResource irradiance_sh_;
    matter::VkImageResource emergency_transmittance_;
    matter::VkImageResource emergency_multiscatter_;
    matter::VkImageResource emergency_sky_view_;
    matter::VkImageResource emergency_irradiance_sh_;
    struct ComputePass {
        VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    };
    ComputePass transmittance_pass_;
    ComputePass multiscatter_pass_;
    ComputePass sky_view_pass_;
    ComputePass irradiance_pass_;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;

    matter::AtmosphereSettings requested_settings_{};
    matter::AtmosphereSettings committed_settings_{};
    matter::Float3 committed_to_sun_{0.0f, 1.0f, 0.0f};
    float committed_camera_world_y_ = 0.0f;
    uint64_t generation_serial_ = 0;
    bool initialized_ = false;
    bool has_committed_settings_ = false;
    bool physical_selected_ = false;
    bool generated_this_frame_ = false;
};

}  // namespace viewer
