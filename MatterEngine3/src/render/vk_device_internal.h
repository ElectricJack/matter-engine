#pragma once

// MatterEngine3/src/render/vk_device_internal.h
//
// Private plumbing between VulkanDevice (MatterEngine3/src/render/vk_context.cpp)
// and the files that allocate Vulkan objects on its device —
// vk_resources.cpp (buffers, images, acceleration structures) and
// vk_pipeline.cpp (compute pipelines). NOT a public header: nothing outside
// MatterEngine3/src/render includes it, and matter/vulkan_device.h only
// forward-declares the types here.
//
// THE PROBLEM IT SOLVES. GPU allocations outlive individual frames and are
// held by shared_ptr all over the renderer, so at shutdown some of them are
// still alive when the VkDevice is destroyed. Two things follow:
//   1. Their vkDestroy* calls must happen BEFORE vkDestroyDevice.
//   2. If teardown deliberately leaked the device instead (see cleanup() in
//      vk_context.cpp — completion could not be proven), those calls must
//      NOT happen at all.
// DeviceAccessToken + DeviceLifetimeControl give both: every allocation
// registers itself on the device's token, cleanup() walks the registry and
// releases them while the device is valid, and invalidate() makes every
// surviving control see VK_NULL_HANDLE and skip its destroy calls.
//
// The *Access classes are the second half: friend-shims that let those files
// reach VulkanDevice::Impl (the token, the retention list, the immediate
// submit) without exposing any of it in the public header.
//
// THREADING. None of this is synchronized. The registry is an intrusive
// doubly-linked list mutated by construction/destruction of controls, so
// allocations and their release must stay on the thread that owns the device.

#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace matter {

class VulkanDevice;

namespace detail {

class DeviceLifetimeControl;

// The shared "is this VkDevice still usable?" handle, plus the head of the
// intrusive list of controls registered against it.
//
// Created once by VulkanDevice::Impl after vkCreateDevice and held by
// shared_ptr; every DeviceLifetimeControl keeps a reference, so the token
// outlives the VulkanDevice whenever an allocation does. Once
// invalidate() has been called, device() returns VK_NULL_HANDLE forever and
// all registered controls become no-ops — that is how a deliberately leaked
// device stops its children from issuing vkDestroy* calls against it.
class DeviceAccessToken {
public:
    explicit DeviceAccessToken(VkDevice device) noexcept : device_(device) {}

    VkDevice device() const noexcept { return device_; }
    // Releases every registered control's Vulkan objects, in registration
    // order, while the device is still valid. Called once from
    // VulkanDevice::Impl::cleanup(); each control must tolerate a second
    // release from its own destructor.
    void destroy_registered_resources() noexcept;
    // Permanently marks the device unusable. Not a destroy: it makes every
    // control skip its vkDestroy* calls, which is what turns an unproven
    // teardown into an intentional leak instead of a use-after-free.
    void invalidate() noexcept { device_ = VK_NULL_HANDLE; }

private:
    friend class DeviceLifetimeControl;
    void register_control(DeviceLifetimeControl& control) noexcept;
    void unregister_control(DeviceLifetimeControl& control) noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    DeviceLifetimeControl* controls_ = nullptr;
};

// Base class for anything holding Vulkan objects that must not outlive the
// device. Derive, implement release_device_objects(), and call it from your
// own destructor too — teardown can come from either end:
//
//   - the owner drops the last shared_ptr, the destructor runs, the control
//     unregisters itself; or
//   - the device tears down first and destroy_registered_resources() releases
//     it in place, after which the destructor's second call must be harmless.
//
// So release_device_objects() must be idempotent and must null the handles it
// destroys. Always fetch the device through live_device() rather than caching
// it: it returns VK_NULL_HANDLE once the token was invalidated, which is the
// signal to release nothing at all. Non-copyable, non-movable (the list links
// are raw pointers into `this`).
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

// A resource parked on the device until the device itself is destroyed, for
// the case where completion could never be proven — e.g. a command pool whose
// immediate submission may still be running (see submit_and_wait_for_phase's
// `completion_proven`). Deriving types put the un-destroyable handles in the
// subclass and let the destructor run at cleanup() time.
//
// `next` is the intrusive singly-linked list link; the device owns the list
// and deletes every node in cleanup().
class DeviceRetainedResource {
public:
    virtual ~DeviceRetainedResource() = default;

    DeviceRetainedResource* next = nullptr;
};

// Friend-shim: hands ownership of `resource` to the device's retention list.
// Takes the unique_ptr by value — the resource is released into a raw
// intrusive list and freed only when the device is destroyed.
class DeviceRetentionAccess {
public:
    static void retain(
        VulkanDevice& device,
        std::unique_ptr<DeviceRetainedResource> resource) noexcept;
};

// Friend-shim exposing the device's access token, so a newly created
// allocation can register a DeviceLifetimeControl against it. The
// test_destroy_call_count hooks exist only under
// MATTER_VK_TEST_FAULT_INJECTION, where the smoke suite asserts on how many
// live_device() lookups (i.e. real destroy attempts) a teardown made.
class DeviceLifetimeAccess {
public:
    static std::shared_ptr<DeviceAccessToken> token(VulkanDevice& device);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    static void reset_test_destroy_call_count();
    static uint32_t test_destroy_call_count();
#endif
};

// Friend-shim onto VulkanDevice::submit_and_wait_for_phase(), for the resource
// paths that need a one-shot synchronous submit (staging copies, acceleration
// structure builds). `completion_proven` false on return means the caller must
// retain the command pool rather than destroy it; `fault_phase` only matters
// to the fault-injection fixtures.
class DeviceSubmitAccess {
public:
    static bool submit_and_wait(VulkanDevice& device,
                                VkCommandBuffer command_buffer, VkFence fence,
                                bool& completion_proven,
                                const char* fault_phase, std::string& error);
};

}  // namespace detail
}  // namespace matter
