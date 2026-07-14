#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace matter {

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
    uint32_t validation_error_count() const;
    // External API work may outlive every completion primitive we can safely
    // query. In that terminal case, preserve the logical device and children.
    void preserve_after_unproven_external_work() noexcept;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    static uint32_t test_validation_error_total();
#endif

private:
    VulkanDevice();
    friend class detail::DeviceRetentionAccess;
    friend class detail::DeviceLifetimeAccess;
    friend class detail::DeviceSubmitAccess;
    bool submit_and_wait_for_phase(VkCommandBuffer command_buffer,
                                   VkFence fence, bool& completion_proven,
                                   const char* fault_phase,
                                   std::string& error);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace matter
