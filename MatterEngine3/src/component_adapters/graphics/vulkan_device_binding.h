#pragma once

// Thin conversion from the engine's VulkanDevice to a MatterGraphics
// VulkanDeviceBinding, for consumers that build with both include trees.
// Ownership and serialization are the device's own: see
// detail::DeviceQueueLease in src/render/vk_device_internal.h.

#include <matter/graphics/vulkan_resources.h>

#include <string>

#include "../../render/vk_device_internal.h"

namespace matter {

inline bool vulkan_device_binding(VulkanDevice& device,
                                  graphics::VulkanDeviceBinding& binding,
                                  std::string& error) {
    detail::DeviceQueueLease lease;
    if (!detail::DeviceGraphicsBindingAccess::lease(device, lease, error))
        return false;
    binding = graphics::VulkanDeviceBinding{};
    binding.physical_device = lease.physical_device;
    binding.device = lease.device;
    binding.queue = lease.queue;
    binding.queue_family = lease.queue_family;
    binding.lifetime = std::move(lease.lifetime);
    binding.queue_mutex = std::move(lease.queue_mutex);
    return true;
}

}  // namespace matter
