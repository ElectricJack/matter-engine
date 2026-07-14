#include "check.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>

#include "matter/vulkan_device.h"

int main() {
#ifdef MATTER_VK_TEST_LAYER_PATH
    // MSYS2 installs validation-layer manifests outside the Windows registry.
    // Point this standalone test at that installed development package and let
    // Windows resolve the layer's dependent DLLs from the same directory.
    SetDllDirectoryA(MATTER_VK_TEST_LAYER_PATH);
    SetEnvironmentVariableA("VK_LAYER_PATH", MATTER_VK_TEST_LAYER_PATH);
#endif
    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "FAIL: glfwInit failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window =
        glfwCreateWindow(320, 200, "vk-smoke", nullptr, nullptr);
    CHECK(window != nullptr, "create hidden GLFW window");

    std::string error;
    auto vulkan =
        window ? matter::VulkanDevice::create(window, true, error) : nullptr;
    CHECK(vulkan != nullptr, error.empty() ? "create Vulkan device" : error.c_str());

    if (vulkan) {
        for (int i = 0; i < 3; ++i) {
            matter::VulkanFrame frame{};
            const bool began = vulkan->begin_frame(frame, error);
            CHECK(began, error.empty() ? "begin frame" : error.c_str());
            if (!began) break;

            const bool ended = vulkan->end_frame(frame, error);
            CHECK(ended, error.empty() ? "end frame" : error.c_str());
            if (!ended) break;
        }
        CHECK(vulkan->validation_error_count() == 0,
              "no Vulkan validation errors");
        std::printf("validation errors: %u\n",
                    vulkan->validation_error_count());
        vulkan->wait_idle();
        vulkan.reset();
    }

    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return check_summary();
}
