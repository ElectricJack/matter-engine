#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace matter {

struct VulkanRayTracingCapabilities {
    bool acceleration_structure_extension = false;
    bool ray_tracing_pipeline_extension = false;
    bool deferred_host_operations_extension = false;
    bool spirv_1_4_extension = false;
    bool shader_float_controls_extension = false;
    bool buffer_device_address = false;
    bool acceleration_structure = false;
    bool ray_tracing_pipeline = false;
    bool storage_image_r8 = false;
    bool shader_storage_image_extended_formats = false;
};

struct VulkanRayTracingProperties {
    uint32_t shader_group_handle_size = 0;
    uint32_t shader_group_handle_alignment = 0;
    uint32_t shader_group_base_alignment = 0;
    uint32_t max_ray_recursion_depth = 0;
    uint32_t min_acceleration_structure_scratch_offset_alignment = 0;
    uint32_t max_shader_group_stride = 0;
    uint32_t max_ray_dispatch_invocation_count = 0;
};

struct VulkanRayTracingSettings {
    bool enabled = false;
    float max_distance = 10000.0f;
    float bias = 0.001f;
    uint32_t samples = 1;
    bool debug_view = false;
};

bool supports_native_ray_tracing(const VulkanRayTracingCapabilities& capabilities,
                                 std::string& reason);

namespace detail {
class DeviceRetentionAccess;
class DeviceLifetimeAccess;
class DeviceSubmitAccess;
}  // namespace detail

struct VulkanFrame {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkImage swapchain_image = VK_NULL_HANDLE;
    VkImageView swapchain_image_view = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    uint32_t image_index = 0;
    uint32_t image_count = 0;
    uint32_t frame_slot = 0;
    uint32_t frame_slot_count = 0;
    VkExtent2D extent{};
    uint64_t serial = 0;
    bool swapchain_recreated = false;
};

class VulkanDevice {
public:
    static std::unique_ptr<VulkanDevice> create(GLFWwindow* window,
                                                 bool enable_validation,
                                                 std::string& error);

    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    bool begin_frame(VulkanFrame& frame, std::string& error);
    bool end_frame(const VulkanFrame& frame, std::string& error);
    bool end_frame(const VulkanFrame& frame, bool& presented,
                   std::string& error);
    bool retain_for_frame(const VulkanFrame& frame,
                          std::vector<std::shared_ptr<void>> resources,
                          std::string& error);
    // Records a copy of the fully composed swapchain image. The destination is
    // populated by end_frame after GPU completion and normalized to RGBA8.
    bool readback_swapchain_rgba8(const VulkanFrame& frame,
                                  std::vector<uint8_t>& rgba,
                                  std::string& error);
    bool submit_and_wait(VkCommandBuffer command_buffer, VkFence fence,
                         bool& completion_proven, std::string& error);
    void wait_idle();

    VkInstance instance() const;
    VkPhysicalDevice physical_device() const;
    VkDevice device() const;
    VkQueue graphics_queue() const;
    uint32_t graphics_queue_family() const;
    VkFormat swapchain_format() const;
    uint32_t swapchain_image_count() const;
    bool draw_indirect_first_instance_enabled() const;
    bool multi_draw_indirect_enabled() const;
    bool dlss_available() const;
    const std::string& dlss_unavailable_reason() const;
    bool ray_tracing_available() const;
    const std::string& ray_tracing_unavailable_reason() const;
    const VulkanRayTracingProperties& ray_tracing_properties() const;
    uint32_t validation_error_count() const;
    // External API work may outlive every completion primitive we can safely
    // query. In that terminal case, preserve the logical device and children.
    void preserve_after_unproven_external_work() noexcept;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    static bool test_present_result_was_presented(VkResult result);
    static uint32_t test_validation_error_total();
    const std::vector<std::string>& test_presentation_events() const;
    void test_clear_presentation_events();
    uint64_t test_last_present_common_serial() const;
#endif

private:
    struct Impl;
    explicit VulkanDevice(std::unique_ptr<Impl> impl);
    friend class detail::DeviceRetentionAccess;
    friend class detail::DeviceLifetimeAccess;
    friend class detail::DeviceSubmitAccess;
    bool submit_and_wait_for_phase(VkCommandBuffer command_buffer,
                                   VkFence fence, bool& completion_proven,
                                   const char* fault_phase,
                                   std::string& error);
    std::unique_ptr<Impl> impl_;
};

}  // namespace matter
