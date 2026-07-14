#pragma once

#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace matter {

class VulkanDevice;

namespace detail {

class DeviceRetainedResource {
public:
    virtual ~DeviceRetainedResource() = default;

    DeviceRetainedResource* next = nullptr;
};

class DeviceRetentionAccess {
public:
    static void retain(
        VulkanDevice& device,
        std::unique_ptr<DeviceRetainedResource> resource) noexcept;
};

class DeviceSubmitAccess {
public:
    static bool submit_and_wait(VulkanDevice& device,
                                VkCommandBuffer command_buffer, VkFence fence,
                                bool& completion_proven,
                                const char* fault_phase, std::string& error);
};

}  // namespace detail
}  // namespace matter
