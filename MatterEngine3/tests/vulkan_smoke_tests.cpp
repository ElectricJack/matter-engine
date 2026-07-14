#include "check.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "matter/vulkan_device.h"
#include "render/gpu_matrix_pack.h"
#include "render/matrix_math.h"
#include "render/vk_pipeline.h"

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
        const char* smoke_mode = std::getenv("MATTER_VK_SMOKE_MODE");
        if (smoke_mode && std::string(smoke_mode) == "retention-fault") {
            _putenv_s("MATTER_VK_TEST_FORCE_IMMEDIATE_WAIT_AMBIGUOUS", "1");
            matter::Float4 output{};
            const bool probe_ran = matter::run_transform_probe(
                *vulkan, viewer::pack_glsl_mat4(viewer::mat4_identity()),
                {1.0f, 2.0f, 3.0f, 1.0f}, output);
            _putenv_s("MATTER_VK_TEST_FORCE_IMMEDIATE_WAIT_AMBIGUOUS", "");
            CHECK(!probe_ran,
                  "fault injection makes immediate completion ambiguous");
            CHECK(vulkan->validation_error_count() == 0,
                  "ambiguous submit retains all referenced resources");
            vulkan.reset();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "transform") {
            const matter::Mat4f matrix = viewer::mat4_mul(
                viewer::mat4_translation({3.0f, 4.0f, 5.0f}),
                viewer::mat4_rotation_y(0.5f));
            const matter::Float4 input{1.0f, 2.0f, 3.0f, 1.0f};
            matter::Float4 output{};
            const matter::Float4 expected = viewer::transform(matrix, input);
            const auto close4 = [](matter::Float4 a, matter::Float4 b,
                                   float epsilon) {
                return std::fabs(a.x - b.x) <= epsilon &&
                       std::fabs(a.y - b.y) <= epsilon &&
                       std::fabs(a.z - b.z) <= epsilon &&
                       std::fabs(a.w - b.w) <= epsilon;
            };
            const bool probe_ran = matter::run_transform_probe(
                *vulkan, viewer::pack_glsl_mat4(matrix), input, output);
            CHECK(probe_ran, "run Vulkan transform probe");
            CHECK(close4(output, expected, 1e-5f),
                  "CPU GPU transform parity");
            std::printf("transform CPU: %.8f %.8f %.8f %.8f\n", expected.x,
                        expected.y, expected.z, expected.w);
            std::printf("transform GPU: %.8f %.8f %.8f %.8f\n", output.x,
                        output.y, output.z, output.w);
            CHECK(vulkan->validation_error_count() == 0,
                  "no Vulkan validation errors");
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            vulkan.reset();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }

        for (int i = 0; i < 3; ++i) {
            matter::VulkanFrame frame{};
            const bool began = vulkan->begin_frame(frame, error);
            CHECK(began, error.empty() ? "begin frame" : error.c_str());
            if (!began) break;

            const bool ended = vulkan->end_frame(frame, error);
            CHECK(ended, error.empty() ? "end frame" : error.c_str());
            if (!ended) break;
        }

        int original_width = 0;
        int original_height = 0;
        glfwGetFramebufferSize(window, &original_width, &original_height);
        glfwSetWindowSize(window, 480, 270);
        int resized_width = 0;
        int resized_height = 0;
        bool framebuffer_changed = false;
        for (int i = 0; i < 200; ++i) {
            glfwPollEvents();
            glfwGetFramebufferSize(window, &resized_width, &resized_height);
            framebuffer_changed = resized_width != original_width ||
                                  resized_height != original_height;
            if (framebuffer_changed) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(framebuffer_changed,
              "framebuffer size changes before resize recreation assertion");
        if (framebuffer_changed) {
            matter::VulkanFrame resized{};
            const bool began_resized = vulkan->begin_frame(resized, error);
            CHECK(began_resized,
                  error.empty() ? "begin resized frame" : error.c_str());
            if (began_resized) {
                CHECK(resized.swapchain_recreated,
                      "resize recreates the swapchain once");
                const bool ended_resized = vulkan->end_frame(resized, error);
                CHECK(ended_resized,
                      error.empty() ? "end resized frame" : error.c_str());

                if (ended_resized) {
                    matter::VulkanFrame stable{};
                    const bool began_stable = vulkan->begin_frame(stable, error);
                    CHECK(
                        began_stable,
                        error.empty() ? "begin stable resized frame"
                                      : error.c_str());
                    if (began_stable) {
                        CHECK(!stable.swapchain_recreated,
                              "stable framebuffer does not recreate perpetually");
                        CHECK(vulkan->end_frame(stable, error),
                              error.empty() ? "end stable resized frame"
                                            : error.c_str());
                    }
                }
            }
        }

        glfwShowWindow(window);
        glfwIconifyWindow(window);
        int minimized_width = -1;
        int minimized_height = -1;
        for (int i = 0; i < 50; ++i) {
            glfwPollEvents();
            glfwGetFramebufferSize(window, &minimized_width, &minimized_height);
            if (minimized_width == 0 || minimized_height == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (minimized_width == 0 || minimized_height == 0) {
            matter::VulkanFrame minimized{};
            const auto start = std::chrono::steady_clock::now();
            const bool began_minimized = vulkan->begin_frame(minimized, error);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            CHECK(!began_minimized,
                  "zero-sized framebuffer skips frame acquisition");
            CHECK(error.find("zero-sized") != std::string::npos,
                  "zero-sized framebuffer reports a recoverable result");
            CHECK(elapsed < std::chrono::milliseconds(250),
                  "zero-sized framebuffer returns promptly");
        } else {
            std::printf("SKIP: platform did not expose a zero-sized minimized "
                        "framebuffer\n");
        }
        glfwRestoreWindow(window);
        glfwHideWindow(window);
        int restored_width = 0;
        int restored_height = 0;
        for (int i = 0; i < 50; ++i) {
            glfwPollEvents();
            glfwGetFramebufferSize(window, &restored_width, &restored_height);
            if (restored_width > 0 && restored_height > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (restored_width > 0 && restored_height > 0) {
            matter::VulkanFrame restored{};
            const bool began_restored = vulkan->begin_frame(restored, error);
            CHECK(began_restored,
                  error.empty() ? "begin restored frame" : error.c_str());
            if (began_restored) {
                CHECK(restored.swapchain_recreated,
                      "restored framebuffer recreates after becoming nonzero");
                CHECK(vulkan->end_frame(restored, error),
                      error.empty() ? "end restored frame" : error.c_str());
            }
        } else {
            std::printf("SKIP: minimized window did not restore a nonzero "
                        "framebuffer\n");
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
