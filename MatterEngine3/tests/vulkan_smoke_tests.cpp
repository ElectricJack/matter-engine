#include "check.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "matter/vulkan_device.h"
#include "render/gpu_matrix_pack.h"
#include "render/matrix_math.h"
#include "render/vk_device_internal.h"
#include "render/vk_pipeline.h"
#include "render/vk_resources.h"

namespace {

void finish_vulkan_test(std::unique_ptr<matter::VulkanDevice>& vulkan) {
    CHECK(vulkan->validation_error_count() == 0,
          "no Vulkan validation errors before device teardown");
    vulkan.reset();
    CHECK(matter::VulkanDevice::test_validation_error_total() == 0,
          "no Vulkan validation errors through retained device teardown");
}

bool run_retention_fault(matter::VulkanDevice& vulkan,
                         const std::string& phase, std::string& error) {
    _putenv_s("MATTER_VK_TEST_FORCE_IMMEDIATE_WAIT_AMBIGUOUS", phase.c_str());
    bool result = false;
    if (phase == "staging-upload") {
        matter::VkBufferResource buffer;
        const uint32_t value = 0x12345678u;
        result = matter::create_buffer(
                     vulkan, sizeof(value),
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, buffer, error) &&
                 matter::upload_buffer(vulkan, buffer, &value, sizeof(value), 0,
                                       error);
    } else if (phase == "staging-readback") {
        matter::VkBufferResource buffer;
        uint32_t value = 0;
        result = matter::create_buffer(
                     vulkan, sizeof(value),
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, buffer, error) &&
                 matter::readback_buffer(vulkan, buffer, &value, sizeof(value),
                                         0, error);
    } else if (phase == "image-transition") {
        matter::VkImageResource image;
        result = matter::create_image(
                     vulkan, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                     {1, 1, 1},
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, error) &&
                 matter::transition_image(
                     vulkan, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, error);
    } else if (phase == "dispatch-moved-buffer") {
        matter::VkBufferResource buffer;
        matter::VkComputePipelineResource pipeline;
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        if (matter::create_buffer(
                vulkan, 96,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer, error) &&
            matter::create_compute_pipeline(vulkan,
                                            "transform_probe.comp.spv",
                                            {binding}, pipeline, error)) {
            matter::write_storage_buffer_descriptor(pipeline, 0, buffer, 0, 96);
            std::vector<matter::VkBufferResource> relocated;
            relocated.push_back(std::move(buffer));
            const auto* original_address = &relocated.front();
            relocated.reserve(relocated.capacity() + 1);
            CHECK(&relocated.front() != original_address,
                  "bound buffer owner relocates after descriptor write");
            _putenv_s("MATTER_VK_TEST_FORCE_IMMEDIATE_WAIT_AMBIGUOUS",
                      "staging-upload");
            CHECK(matter::dispatch_compute(vulkan, pipeline, 1, 1, 1, error),
                  "fault injection ignores a nonmatching submit phase");
            _putenv_s("MATTER_VK_TEST_FORCE_IMMEDIATE_WAIT_AMBIGUOUS",
                      phase.c_str());
            result = matter::dispatch_compute(vulkan, pipeline, 1, 1, 1, error);
        }
    }
    _putenv_s("MATTER_VK_TEST_FORCE_IMMEDIATE_WAIT_AMBIGUOUS", "");
    return result;
}

void run_outlive_resources(std::unique_ptr<matter::VulkanDevice>& vulkan,
                           std::string& error, bool force_unproven_cleanup) {
    matter::VkBufferResource buffer;
    matter::VkImageResource image;
    matter::VkComputePipelineResource pipeline;
    CHECK(matter::create_buffer(
              *vulkan, 64,
              VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer, error),
          error.empty() ? "create outliving buffer" : error.c_str());
    CHECK(matter::create_image(
              *vulkan, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
              {1, 1, 1},
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
              image, error),
          error.empty() ? "create outliving image" : error.c_str());
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    CHECK(matter::create_compute_pipeline(
              *vulkan, "transform_probe.comp.spv", {binding}, pipeline, error),
          error.empty() ? "create outliving pipeline" : error.c_str());

    matter::VkBufferResource moved_buffer(std::move(buffer));
    matter::VkImageResource moved_image(std::move(image));
    matter::VkComputePipelineResource moved_pipeline(std::move(pipeline));
    CHECK(buffer.buffer == VK_NULL_HANDLE && !buffer.lifetime,
          "moved-from buffer releases lifetime control");
    CHECK(image.image == VK_NULL_HANDLE && !image.lifetime,
          "moved-from image releases lifetime control");
    CHECK(pipeline.pipeline == VK_NULL_HANDLE && !pipeline.lifetime,
          "moved-from pipeline releases lifetime control");

    CHECK(vulkan->validation_error_count() == 0,
          "no validation errors before outlive device teardown");
    if (force_unproven_cleanup) {
        matter::detail::DeviceLifetimeAccess::reset_test_destroy_call_count();
        _putenv_s("MATTER_VK_TEST_FORCE_CLEANUP_UNPROVEN", "1");
    }
    vulkan.reset();
    _putenv_s("MATTER_VK_TEST_FORCE_CLEANUP_UNPROVEN", "");
    moved_buffer.reset();
    // The moved image and pipeline intentionally use their destructors after
    // their VulkanDevice owner has already been destroyed.
}

}  // namespace

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
        if (smoke_mode &&
            (std::string(smoke_mode) == "outlive-resources" ||
             std::string(smoke_mode) == "outlive-unproven")) {
            const bool force_unproven =
                std::string(smoke_mode) == "outlive-unproven";
            run_outlive_resources(vulkan, error, force_unproven);
            if (force_unproven) {
                CHECK(matter::detail::DeviceLifetimeAccess::
                          test_destroy_call_count() == 0,
                      "unproven cleanup blocks late child destruction");
            }
            CHECK(matter::VulkanDevice::test_validation_error_total() == 0,
                  "outliving resource teardown has no validation errors");
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode &&
            std::string(smoke_mode).rfind("retention-fault-", 0) == 0) {
            const std::string phase = std::string(smoke_mode).substr(16);
            const bool completed = run_retention_fault(*vulkan, phase, error);
            CHECK(!completed, "selected submit phase becomes ambiguous");
            CHECK(error.find("forced ambiguous") != std::string::npos,
                  "fault injection reached the selected submit phase");
            finish_vulkan_test(vulkan);
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
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
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

        std::printf("validation errors: %u\n",
                    vulkan->validation_error_count());
        vulkan->wait_idle();
        finish_vulkan_test(vulkan);
    }

    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return check_summary();
}
