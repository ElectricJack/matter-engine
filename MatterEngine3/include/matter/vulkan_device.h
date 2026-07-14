#pragma once

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

struct GLFWwindow;

namespace matter {

namespace detail {
class DeviceRetentionAccess;
class DeviceSubmitAccess;
}  // namespace detail

struct VulkanFrame {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    uint32_t image_index = 0;
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
    bool submit_and_wait(VkCommandBuffer command_buffer, VkFence fence,
                         bool& completion_proven, std::string& error);
    void wait_idle();

    VkInstance instance() const;
    VkPhysicalDevice physical_device() const;
    VkDevice device() const;
    VkQueue graphics_queue() const;
    uint32_t graphics_queue_family() const;
    uint32_t validation_error_count() const;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    static uint32_t test_validation_error_total();
#endif

private:
    VulkanDevice();
    friend class detail::DeviceRetentionAccess;
    friend class detail::DeviceSubmitAccess;
    bool submit_and_wait_for_phase(VkCommandBuffer command_buffer,
                                   VkFence fence, bool& completion_proven,
                                   const char* fault_phase,
                                   std::string& error);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace matter
