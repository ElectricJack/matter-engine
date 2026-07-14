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
#include "render/vk_scene_renderer.h"

namespace {

struct FixedCullScene {
    viewer::FrameMatrices frame{};
    matter::Float3 eye{};
    std::vector<viewer::VkScenePart> parts;
    std::vector<viewer::VkSceneInstance> instances;
};

struct CullResult {
    viewer::VkCullStats stats{};
    std::vector<viewer::DrawCommand> commands;
};

matter::Mat4f identity_matrix() {
    matter::Mat4f result{};
    result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
    return result;
}

viewer::VkScenePart fixed_part(uint64_t hash, matter::Float3 minimum,
                               matter::Float3 maximum, uint32_t first_vertex) {
    viewer::VkSceneCluster cluster{};
    cluster.aabb_min = minimum;
    cluster.aabb_max = maximum;
    const float dx = maximum.x - minimum.x;
    const float dy = maximum.y - minimum.y;
    const float dz = maximum.z - minimum.z;
    cluster.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    cluster.lods.push_back({first_vertex, 3, 0.0f});
    return {hash, {cluster}};
}

FixedCullScene make_fixed_cull_scene() {
    FixedCullScene scene{};
    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    std::string error;
    CHECK(viewer::build_frame_matrices(camera, 320, 320, scene.frame, error),
          error.empty() ? "build fixed cull matrices" : error.c_str());
    scene.eye = camera.position;

    scene.parts.push_back(fixed_part(1, {-0.5f, -0.5f, -2.5f},
                                     {0.5f, 0.5f, -1.5f}, 0));
    scene.parts.push_back(fixed_part(2, {-0.5f, -0.5f, 1.5f},
                                     {0.5f, 0.5f, 2.5f}, 3));
    scene.parts.push_back(fixed_part(3, {-0.2f, -0.2f, -0.2f},
                                     {0.2f, 0.2f, 0.05f}, 6));
    scene.parts.push_back(fixed_part(4, {-0.5f, -0.5f, -12.5f},
                                     {0.5f, 0.5f, -11.5f}, 9));
    scene.parts.push_back(fixed_part(5, {-0.25f, -0.25f, -0.25f},
                                     {0.25f, 0.25f, 0.25f}, 12));
    for (uint64_t hash = 1; hash <= 5; ++hash) {
        viewer::VkSceneInstance instance{};
        instance.part_hash = hash;
        instance.object_to_world = identity_matrix();
        if (hash == 5) {
            instance.object_to_world =
                viewer::mat4_translation({1.0f, 0.0f, -3.0f});
        }
        scene.instances.push_back(instance);
    }
    return scene;
}

bool clip_aabb_visible(const FixedCullScene& scene,
                       const viewer::VkScenePart& part,
                       const viewer::VkSceneInstance& instance) {
    const auto& cluster = part.clusters.front();
    matter::Mat4f object_to_clip = viewer::mat4_mul(
        scene.frame.world_to_clip, instance.object_to_world);
    matter::Float4 clip[8]{};
    for (int i = 0; i < 8; ++i) {
        const matter::Float4 point{
            (i & 4) ? cluster.aabb_max.x : cluster.aabb_min.x,
            (i & 2) ? cluster.aabb_max.y : cluster.aabb_min.y,
            (i & 1) ? cluster.aabb_max.z : cluster.aabb_min.z, 1.0f};
        clip[i] = viewer::transform(object_to_clip, point);
    }
    for (int plane = 0; plane < 6; ++plane) {
        bool all_outside = true;
        for (const auto& c : clip) {
            const bool inside = plane == 0 ? c.x >= -c.w
                                : plane == 1 ? c.x <= c.w
                                : plane == 2 ? c.y >= -c.w
                                : plane == 3 ? c.y <= c.w
                                : plane == 4 ? c.z >= 0.0f
                                             : c.z <= c.w;
            if (inside) {
                all_outside = false;
                break;
            }
        }
        if (all_outside) return false;
    }
    return true;
}

CullResult run_cpu_cull(const FixedCullScene& scene) {
    CullResult result{};
    result.commands.resize(scene.parts.size() * viewer::kVkMaxLod);
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        auto& command = result.commands[i * viewer::kVkMaxLod];
        command.vertex_count = scene.parts[i].clusters[0].lods[0].vertex_count;
        command.first_vertex = scene.parts[i].clusters[0].lods[0].first_vertex;
        command.first_instance = static_cast<uint32_t>(i);
        if (clip_aabb_visible(scene, scene.parts[i], scene.instances[i])) {
            command.instance_count = 1;
            ++result.stats.emitted;
        } else {
            ++result.stats.frustum_culled;
        }
    }
    return result;
}

CullResult run_vk_cull(matter::VulkanDevice& vulkan,
                       const FixedCullScene& scene) {
    CullResult result{};
    viewer::VkSceneRenderer renderer(vulkan);
    std::string error;
    CHECK(renderer.init(error), error.empty() ? "init Vulkan scene renderer"
                                              : error.c_str());
    for (const auto& part : scene.parts) {
        CHECK(renderer.ensure_part(part, error) >= 0,
              error.empty() ? "ensure Vulkan scene part" : error.c_str());
    }
    CHECK(renderer.update_instances(scene.instances, error),
          error.empty() ? "upload Vulkan scene instances" : error.c_str());
    CHECK(renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error),
          error.empty() ? "dispatch Vulkan scene culling" : error.c_str());
    CHECK(renderer.cull_stats(result.stats, error),
          error.empty() ? "read Vulkan cull stats" : error.c_str());
    CHECK(renderer.readback_commands(result.commands, error),
          error.empty() ? "read Vulkan draw commands" : error.c_str());
    return result;
}

void run_cull_parity(matter::VulkanDevice& vulkan) {
    const FixedCullScene scene = make_fixed_cull_scene();
    const CullResult cpu = run_cpu_cull(scene);
    const CullResult gpu = run_vk_cull(vulkan, scene);
    CHECK(gpu.stats.emitted == cpu.stats.emitted, "emitted parity");
    CHECK(gpu.stats.frustum_culled == cpu.stats.frustum_culled,
          "culled parity");
    CHECK(gpu.commands == cpu.commands, "command parity");
    CHECK(gpu.stats.emitted == 3, "front near-intersection and translated visible");
    CHECK(gpu.stats.frustum_culled == 2, "behind and far rejected");
    const char* case_names[] = {"front", "behind", "near-intersection", "far",
                                "translated"};
    const uint32_t expected_instances[] = {1, 0, 1, 0, 1};
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        const uint32_t cpu_instances =
            cpu.commands[i * viewer::kVkMaxLod].instance_count;
        const uint32_t gpu_instances =
            gpu.commands[i * viewer::kVkMaxLod].instance_count;
        CHECK(gpu_instances == expected_instances[i], case_names[i]);
        std::printf("cull case %-17s CPU=%u GPU=%u\n", case_names[i],
                    cpu_instances, gpu_instances);
    }
    std::printf("cull CPU: emitted=%u frustum_culled=%u\n", cpu.stats.emitted,
                cpu.stats.frustum_culled);
    std::printf("cull GPU: emitted=%u frustum_culled=%u\n", gpu.stats.emitted,
                gpu.stats.frustum_culled);
}

void run_cull_region_and_lifecycle_tests(matter::VulkanDevice& vulkan) {
    viewer::VkSceneCluster cluster{};
    cluster.aabb_min = {-0.25f, -0.25f, -0.25f};
    cluster.aabb_max = {0.25f, 0.25f, 0.25f};
    cluster.radius = 0.5f;
    cluster.lods = {{0, 3, 0.2f}, {3, 3, 0.0f}};
    const viewer::VkScenePart part{77, {cluster}};
    viewer::VkSceneInstance near_instance{77, viewer::mat4_translation(
                                                  {0.0f, 0.0f, -2.0f})};
    viewer::VkSceneInstance far_instance{77, viewer::mat4_translation(
                                                 {0.0f, 0.0f, -5.0f})};

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices frame{};
    std::string error;
    CHECK(viewer::build_frame_matrices(camera, 320, 320, frame, error),
          error.empty() ? "build multi-LOD matrices" : error.c_str());

    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error), error.empty() ? "init multi-LOD renderer"
                                              : error.c_str());
    CHECK(renderer.ensure_part(part, error) >= 0,
          error.empty() ? "ensure multi-LOD part" : error.c_str());
    CHECK(renderer.update_instances({near_instance, far_instance}, error),
          error.empty() ? "upload multi-LOD instances" : error.c_str());
    CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error),
          error.empty() ? "dispatch multi-LOD culling" : error.c_str());
    std::vector<viewer::DrawCommand> commands;
    std::vector<viewer::GpuMat4> transforms;
    CHECK(renderer.readback_commands(commands, error),
          error.empty() ? "read multi-LOD commands" : error.c_str());
    CHECK(renderer.readback_draw_transforms(transforms, error),
          error.empty() ? "read multi-LOD transforms" : error.c_str());
    CHECK(commands[0].instance_count == 1, "near instance selects fine LOD");
    CHECK(commands[1].instance_count == 1, "far instance selects coarse LOD");
    CHECK(commands[0].first_instance != commands[1].first_instance,
          "multi-LOD transform regions do not overlap");
    CHECK(std::fabs(transforms[commands[0].first_instance].elements[14] + 2.0f) <
              1e-5f,
          "fine LOD transform retained");
    CHECK(std::fabs(transforms[commands[1].first_instance].elements[14] + 5.0f) <
              1e-5f,
          "coarse LOD transform retained");

    renderer.release_part(77);
    std::vector<viewer::VkSceneRenderer::RtInstance> rt_instances;
    CHECK(renderer.fill_rt_instances(rt_instances) == 0,
          "release removes RT instances");
    renderer.reset();
    CHECK(renderer.draw_command_count() == 0, "reset reclaims command tombstones");
    CHECK(renderer.ensure_part(part, error) >= 0,
          error.empty() ? "re-add part after reset" : error.c_str());
    CHECK(renderer.draw_command_count() == viewer::kVkMaxLod,
          "re-add after reset uses bounded command storage");
}

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
        if (smoke_mode && std::string(smoke_mode) == "cull") {
            run_cull_parity(*vulkan);
            run_cull_region_and_lifecycle_tests(*vulkan);
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
