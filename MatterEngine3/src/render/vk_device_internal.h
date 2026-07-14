#pragma once

#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace matter {

class VulkanDevice;

namespace detail {

class DeviceLifetimeControl;

class DeviceAccessToken {
public:
    explicit DeviceAccessToken(VkDevice device) noexcept : device_(device) {}

    VkDevice device() const noexcept { return device_; }
    void destroy_registered_resources() noexcept;
    void invalidate() noexcept { device_ = VK_NULL_HANDLE; }

private:
    friend class DeviceLifetimeControl;
    void register_control(DeviceLifetimeControl& control) noexcept;
    void unregister_control(DeviceLifetimeControl& control) noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    DeviceLifetimeControl* controls_ = nullptr;
};

class DeviceLifetimeControl {
public:
    explicit DeviceLifetimeControl(
        std::shared_ptr<DeviceAccessToken> device_access) noexcept;
    virtual ~DeviceLifetimeControl();

    DeviceLifetimeControl(const DeviceLifetimeControl&) = delete;
    DeviceLifetimeControl& operator=(const DeviceLifetimeControl&) = delete;

protected:
    VkDevice live_device() const noexcept;
    virtual void release_device_objects() noexcept = 0;

private:
    friend class DeviceAccessToken;
    std::shared_ptr<DeviceAccessToken> device_access_;
    DeviceLifetimeControl* previous_ = nullptr;
    DeviceLifetimeControl* next_ = nullptr;
};

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

class DeviceLifetimeAccess {
public:
    static std::shared_ptr<DeviceAccessToken> token(VulkanDevice& device);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    static void reset_test_destroy_call_count();
    static uint32_t test_destroy_call_count();
#endif
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
