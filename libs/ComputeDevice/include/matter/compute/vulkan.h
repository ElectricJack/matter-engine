#pragma once
#include <matter/compute/device.h>
#include <vulkan/vulkan.h>
#include <mutex>

namespace matter::compute {
// The owner keeps the physical/logical device alive. All users of this queue
// MUST use the same mutex; a busy queue rejects admission instead of blocking.
// No window, surface, graphics scene, swapchain or frame loop is involved.
struct VulkanBinding {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    std::shared_ptr<void> owner;
    std::shared_ptr<std::mutex> queue_mutex;
};
// The queue family must support compute and belong to the supplied device.
// Binding native handles is a trusted host operation; Vulkan cannot prove their
// provenance. Request identities are checked on every execute call.
std::shared_ptr<Device> bind_vulkan(VulkanBinding, data::ValidationResult &);
// Optional convenience for standalone native consumers. Validation, when
// requested, is required (missing layer is an explicit capability failure).
std::shared_ptr<Device> create_vulkan(bool validation, data::ValidationResult &);
// Counts validation errors, including synchronization validation, separately
// from loader diagnostics (e.g. unrelated stale implicit-layer registrations).
// Available for create_vulkan(true); injected bindings own their diagnostics.
uint32_t validation_errors(const std::shared_ptr<Device> &) noexcept;
std::vector<std::string> validation_messages(const std::shared_ptr<Device> &);
} // namespace matter::compute
