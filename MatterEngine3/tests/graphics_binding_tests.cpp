// VulkanDevice -> MatterGraphics VulkanDeviceBinding: the binding must hold
// the real device alive past ~VulkanDevice and serialize on the same queue
// mutex the engine takes. Needs a Vulkan device (lavapipe is enough), the
// Khronos validation layer and a display: the vendored GLFW has no null platform.
#include "matter/windows_compat.h"
#define GLFW_INCLUDE_NONE
#include "component_adapters/graphics/vulkan_device_binding.h"
#include "matter/log.h"
#include "matter/vulkan_device.h"
#include <GLFW/glfw3.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace mg = matter::graphics;

static unsigned failures = 0;
static void check(bool ok, const char* message) {
    if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
}

static std::atomic<unsigned> preserve_warnings{0};
static std::atomic<unsigned> validation_errors{0};
static void count_preserve(matter::log::Level, const char*, const char* message, void*) {
    if (std::strstr(message, "a graphics binding still holds them")) ++preserve_warnings;
    // Counted here rather than through validation_error_count() so errors raised
    // after ~VulkanDevice, while only the binding holds the device, still show.
    if (std::strstr(message, "Vulkan validation ERROR")) ++validation_errors;
}

struct Window {
    GLFWwindow* handle = nullptr;
    Window() {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        handle = glfwCreateWindow(64, 64, "Graphics binding", nullptr, nullptr);
    }
    ~Window() { if (handle) glfwDestroyWindow(handle); }
};

static std::unique_ptr<matter::VulkanDevice> make_device(Window& window) {
    std::string error;
    auto vk = window.handle ? matter::VulkanDevice::create(window.handle, true, error) : nullptr;
    check(bool(vk), error.c_str());
    return vk;
}

static bool bind(matter::VulkanDevice& vk, mg::VulkanDeviceBinding& binding) {
    std::string error;
    const bool ok = matter::vulkan_device_binding(vk, binding, error);
    check(ok, error.c_str());
    return ok;
}

static std::shared_ptr<const std::vector<uint8_t>> bytes(std::vector<uint8_t> values) {
    return std::make_shared<const std::vector<uint8_t>>(std::move(values));
}

int main() {
#ifdef MATTER_VK_TEST_LAYER_PATH
    SetDllDirectoryA(MATTER_VK_TEST_LAYER_PATH);
    SetEnvironmentVariableA("VK_LAYER_PATH", MATTER_VK_TEST_LAYER_PATH);
#endif
    if (glfwInit() != GLFW_TRUE) {
        const char* reason = nullptr;
        glfwGetError(&reason);
        std::printf("FAIL: GLFW initializes: %s\n", reason ? reason : "unknown");
        return 1;
    }
    matter::log::add_sink(count_preserve);

    // A binding taken and released before teardown leaves the ordinary
    // destroy path in place.
    {
        Window window;
        auto vk = make_device(window);
        if (vk) {
            mg::VulkanDeviceBinding binding;
            if (bind(*vk, binding)) {
                check(binding.device == vk->device() && binding.queue == vk->graphics_queue() &&
                      binding.physical_device == vk->physical_device(),
                      "binding carries the device's own handles");
                check(binding.queue_mutex.get() == &vk->queue_mutex(),
                      "binding shares the device's queue mutex");
                mg::VulkanDeviceBinding again;
                bind(*vk, again);
                check(again.lifetime == binding.lifetime, "bindings share one lease");
            }
            binding = {};
            vk.reset();
            check(preserve_warnings == 0, "released lease does not preserve the device");
        }
    }

    // Engine waits block on a queue mutex held through the binding.
    {
        Window window;
        auto vk = make_device(window);
        mg::VulkanDeviceBinding binding;
        if (vk && bind(*vk, binding)) {
            std::atomic<bool> waited{false};
            std::thread waiter;
            {
                std::lock_guard<std::mutex> lock(*binding.queue_mutex);
                waiter = std::thread([&] { vk->wait_idle(); waited = true; });
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                check(!waited, "wait_idle waits for the binding's queue mutex");
            }
            waiter.join();
            check(waited, "wait_idle proceeds once the mutex is released");
            binding = {};
        }
        vk.reset();
        check(preserve_warnings == 0, "no preserve without an outstanding lease");
    }

    // A buffer outliving the VulkanDevice keeps a usable device behind it.
    {
        Window window;
        auto vk = make_device(window);
        mg::VulkanDeviceBinding binding;
        if (vk && bind(*vk, binding)) {
            auto device = std::make_shared<mg::VulkanResourceDevice>(binding);
            binding = {};
            std::shared_ptr<mg::DeviceBuffer> buffer;
            const auto input = bytes({1, 2, 3, 4, 5, 6, 7, 8});
            check(device->upload(mg::BufferUsage::vertex, input, buffer).ok(), "upload through binding");
            std::vector<uint8_t> output;
            check(device->readback(buffer, output).ok() && output == *input, "readback through binding");
            vk.reset();
            check(preserve_warnings == 1, "teardown under a live binding preserves the device");
            output.clear();
            check(device->readback(buffer, output).ok() && output == *input,
                  "binding stays usable after the VulkanDevice is gone");
            std::shared_ptr<mg::DeviceBuffer> late;
            check(device->upload(mg::BufferUsage::index, bytes({9, 9, 9, 9}), late).ok(),
                  "upload after the VulkanDevice is gone");
            device->drain();
        }
    }

    check(validation_errors == 0, "no validation errors");
    matter::log::remove_sink(count_preserve);
    glfwTerminate();
    if (failures) { std::printf("%u FAILURES\n", failures); return 1; }
    std::printf("ALL PASS\n");
    return 0;
}
