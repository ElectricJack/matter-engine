// libs/MatterGraphics/src/native/vk_device_internal.cpp
//
// Definitions for vk_device_internal.h: the device-lifetime registry, the
// friend-shims, and the VulkanDevice accessors the resource helpers need. They
// read only the detail::DeviceCore base of VulkanDevice::Impl, so MatterGraphics
// links them without the host's window/swapchain device code. Device creation,
// teardown and the immediate submit itself stay in
// MatterEngine3/src/render/vk_context.cpp.

#include "vk_device_internal.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "matter/vulkan_device.h"

namespace matter {
namespace {

#ifdef MATTER_VK_TEST_FAULT_INJECTION
std::atomic<uint32_t> g_test_resource_destroy_call_total{0};
#endif

}  // namespace

detail::DeviceLifetimeControl::DeviceLifetimeControl(
    std::shared_ptr<DeviceAccessToken> device_access) noexcept
    : device_access_(std::move(device_access)) {
    if (device_access_) device_access_->register_control(*this);
}

detail::DeviceLifetimeControl::~DeviceLifetimeControl() {
    if (device_access_) device_access_->unregister_control(*this);
}

VkDevice detail::DeviceLifetimeControl::live_device() const noexcept {
    const VkDevice device =
        device_access_ ? device_access_->device() : VK_NULL_HANDLE;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (device != VK_NULL_HANDLE) {
        g_test_resource_destroy_call_total.fetch_add(1,
                                                     std::memory_order_relaxed);
    }
#endif
    return device;
}

void detail::DeviceAccessToken::register_control(
    DeviceLifetimeControl& control) noexcept {
    control.next_ = controls_;
    if (controls_) controls_->previous_ = &control;
    controls_ = &control;
}

void detail::DeviceAccessToken::unregister_control(
    DeviceLifetimeControl& control) noexcept {
    if (control.previous_) {
        control.previous_->next_ = control.next_;
    } else if (controls_ == &control) {
        controls_ = control.next_;
    }
    if (control.next_) control.next_->previous_ = control.previous_;
    control.previous_ = nullptr;
    control.next_ = nullptr;
}

// Walks every live DeviceLifetimeControl and releases its Vulkan objects while
// the device is still valid. Called from cleanup() just before vkDestroyDevice;
// each control's release_device_objects() must therefore be idempotent, since
// its own destructor will run it again later.
void detail::DeviceAccessToken::destroy_registered_resources() noexcept {
    for (DeviceLifetimeControl* control = controls_; control;
         control = control->next_) {
        control->release_device_objects();
    }
}

void detail::DeviceRetentionAccess::retain(
    VulkanDevice& owner,
    std::unique_ptr<DeviceRetainedResource> resource) noexcept {
    if (!resource) return;
    resource->next = owner.core_->retained_resources;
    owner.core_->retained_resources = resource.release();
}

std::shared_ptr<detail::DeviceAccessToken>
detail::DeviceLifetimeAccess::token(VulkanDevice& owner) {
    return owner.core_->device_lifetime;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
void detail::DeviceLifetimeAccess::reset_test_destroy_call_count() {
    g_test_resource_destroy_call_total.store(0, std::memory_order_relaxed);
}

uint32_t detail::DeviceLifetimeAccess::test_destroy_call_count() {
    return g_test_resource_destroy_call_total.load(std::memory_order_relaxed);
}
#endif

bool detail::DeviceSubmitAccess::submit_and_wait(
    VulkanDevice& owner, VkCommandBuffer command_buffer, VkFence fence,
    bool& completion_proven, const char* fault_phase, std::string& error) {
    return owner.core_->submit_and_wait_for_phase(command_buffer, fence,
                                                  completion_proven,
                                                  fault_phase, error);
}

VkPhysicalDevice VulkanDevice::physical_device() const {
    return core_->physical_device;
}
VkDevice VulkanDevice::device() const { return core_->device; }
uint32_t VulkanDevice::graphics_queue_family() const {
    return core_->graphics_queue_family;
}

bool VulkanDevice::ray_tracing_available() const {
    return core_->ray_tracing_enabled;
}

const std::string& VulkanDevice::ray_tracing_unavailable_reason() const {
    return core_->ray_tracing_reason;
}

}  // namespace matter
