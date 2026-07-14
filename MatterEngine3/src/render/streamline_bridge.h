#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace matter {

// Keeps the proprietary Streamline SDK at one optional boundary.  The default
// build intentionally has no Streamline headers, libraries, or runtime files.
class StreamlineBridge {
public:
    static StreamlineBridge initialize_before_vulkan();

    bool initialized() const { return initialized_; }
    bool dlss_requested() const { return dlss_requested_; }
    bool dlss_available() const { return dlss_available_; }
    const std::string& dlss_unavailable_reason() const {
        return dlss_unavailable_reason_;
    }
    bool proxy_dispatch_used() const { return proxy_dispatch_used_; }

    // Merges the second sequence after the first, preserving first-seen order.
    static std::vector<const char*> merge_extensions(
        const std::vector<const char*>& first,
        const std::vector<const char*>& second);

    // Applies requirements returned by slGetFeatureRequirements before Vulkan
    // availability checks and device creation.  The existing 1.2/1.3 feature
    // structs remain in the caller's pNext chain; only their feature bits grow.
    void append_requirements(std::vector<const char*>& instance_extensions,
                             std::vector<const char*>& device_extensions,
                             VkPhysicalDeviceVulkan12Features& features12,
                             VkPhysicalDeviceVulkan13Features& features13,
                             uint32_t& graphics_queue_count,
                             uint32_t& compute_queue_count) const;
    bool validate_requirements(
        const VkPhysicalDeviceVulkan12Features& supported_features12,
        const VkPhysicalDeviceVulkan13Features& supported_features13,
        std::string& error) const;

    bool set_vulkan_info(VkInstance instance, VkPhysicalDevice physical_device,
                         VkDevice device, uint32_t graphics_queue_family,
                         uint32_t graphics_queue_index,
                         uint32_t compute_queue_family,
                         uint32_t compute_queue_index);
    // Disabling Streamline is always fail-open: the caller continues through
    // the native Vulkan path with no residual feature or queue requirements.
    void disable(std::string reason);
    void shutdown();

    // All Vulkan calls pass through these wrappers.  They use native Vulkan
    // unless a future presentation path explicitly opts into SL proxies.
    VkResult create_instance(const VkInstanceCreateInfo* create,
                             const VkAllocationCallbacks* allocator,
                             VkInstance* instance);
    VkResult create_device(VkPhysicalDevice physical_device,
                           const VkDeviceCreateInfo* create,
                           const VkAllocationCallbacks* allocator,
                           VkDevice* device);
    VkResult queue_present(VkQueue queue, const VkPresentInfoKHR* present);
    VkResult create_swapchain(VkDevice device,
                              const VkSwapchainCreateInfoKHR* create,
                              const VkAllocationCallbacks* allocator,
                              VkSwapchainKHR* swapchain);
    void destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
                           const VkAllocationCallbacks* allocator);
    VkResult acquire_next_image(VkDevice device, VkSwapchainKHR swapchain,
                                uint64_t timeout, VkSemaphore semaphore,
                                VkFence fence, uint32_t* image_index);
    VkResult device_wait_idle(VkDevice device);

private:
    bool initialized_ = false;
    bool dlss_requested_ = false;
    bool dlss_available_ = false;
    bool proxy_dispatch_used_ = false;
    bool use_proxy_dispatch_ = false;
    bool streamline_initialized_ = false;
    std::string dlss_unavailable_reason_;
    std::vector<const char*> instance_extensions_;
    std::vector<const char*> device_extensions_;
    VkPhysicalDeviceVulkan12Features required_features12_{};
    VkPhysicalDeviceVulkan13Features required_features13_{};
    uint32_t additional_graphics_queues_ = 0;
    uint32_t additional_compute_queues_ = 0;
    void* streamline_module_ = nullptr;
    void* sl_init_ = nullptr;
    void* sl_get_feature_requirements_ = nullptr;
    void* sl_set_vulkan_info_ = nullptr;
    void* sl_shutdown_ = nullptr;
    PFN_vkCreateInstance create_instance_proxy_ = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr_proxy_ = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr_proxy_ = nullptr;
    PFN_vkCreateDevice create_device_proxy_ = nullptr;
    PFN_vkQueuePresentKHR queue_present_proxy_ = nullptr;
    PFN_vkCreateSwapchainKHR create_swapchain_proxy_ = nullptr;
    PFN_vkDestroySwapchainKHR destroy_swapchain_proxy_ = nullptr;
    PFN_vkAcquireNextImageKHR acquire_next_image_proxy_ = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle_proxy_ = nullptr;

    bool populate_instance_proxies(VkInstance instance);
    bool populate_device_proxies(VkDevice device);
};

}  // namespace matter
