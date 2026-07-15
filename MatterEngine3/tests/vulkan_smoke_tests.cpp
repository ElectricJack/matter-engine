#include "check.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "matter/vulkan_device.h"
#include "render/gpu_matrix_pack.h"
#include "render/matrix_math.h"
#include "render/raster_mesh.h"
#include "render/streamline_bridge.h"
#include "render/vk_temporal.h"
#include "render/vk_gi_math.h"
#include "render/vk_cuda_interop.h"
#include "render/vk_device_internal.h"
#include "render/vk_instance_cache.h"
#include "render/vk_pipeline.h"
#include "render/vk_resources.h"
#include "render/vk_scene_renderer.h"
#include "provider/sector_resolver.h"

namespace {

bool close4(matter::Float4 actual, matter::Float4 expected, float epsilon);

void run_vulkan_gi_math_tests() {
    const matter::VulkanGiSettings defaults{};
    CHECK(defaults.enabled && defaults.max_bounces == 1u &&
              defaults.samples_per_pixel == 1u && defaults.trace_scale == 1.0f &&
              defaults.diffuse_multiplier == 1.0f &&
              defaults.reflection_multiplier == 1.0f &&
              defaults.max_reflection_roughness == 1.0f &&
              defaults.transmission_multiplier == 1.0f &&
              defaults.scattering_multiplier == 1.0f,
          "Vulkan GI defaults enable one full-resolution diffuse bounce");

    const matter::Float3 normal{0.26726124f, 0.53452248f, 0.80178373f};
    const viewer::VulkanCosineSample sample =
        viewer::vulkan_cosine_sample(normal, 0.25f, 0.75f);
    const float direction_length = std::sqrt(
        sample.direction.x * sample.direction.x +
        sample.direction.y * sample.direction.y +
        sample.direction.z * sample.direction.z);
    const float cosine = sample.direction.x * normal.x +
                         sample.direction.y * normal.y +
                         sample.direction.z * normal.z;
    CHECK(std::fabs(direction_length - 1.0f) < 1e-5f && cosine > 0.0f &&
              std::fabs(sample.pdf - cosine / 3.14159265358979323846f) < 1e-6f,
          "cosine sampler produces an orthonormal upper-hemisphere direction");

    const uint32_t first = viewer::vulkan_gi_pcg_hash(0x12345678u);
    CHECK(first == viewer::vulkan_gi_pcg_hash(0x12345678u) &&
              first != viewer::vulkan_gi_pcg_hash(0x12345679u),
          "GI PCG hash is fixed-seed deterministic and input-sensitive");
    const uint32_t retry_seed = viewer::vulkan_gi_seed(17, 23, 9, 1);
    CHECK(retry_seed == viewer::vulkan_gi_seed(17, 23, 9, 1) &&
              retry_seed != viewer::vulkan_gi_seed(17, 23, 9, 0) &&
              retry_seed != viewer::vulkan_gi_seed(17, 23, 10, 1),
          "GI seed uses committed frame identity and explicit bounce component");
    const auto source_uv =
        viewer::vulkan_gi_source_uv(10, 5, 80, 40, 160, 80);
    CHECK(std::fabs(source_uv.x - 21.5f / 160.0f) < 1e-7f &&
              std::fabs(source_uv.y - 11.5f / 80.0f) < 1e-7f,
          "scaled GI reconstruction uses the selected source texel center");

    const matter::Float3 f0{0.04f, 0.1f, 0.8f};
    const matter::Float3 normal_fresnel =
        viewer::vulkan_schlick_fresnel(f0, 1.0f);
    const matter::Float3 grazing_fresnel =
        viewer::vulkan_schlick_fresnel(f0, 0.0f);
    CHECK(std::fabs(normal_fresnel.x - f0.x) < 1e-6f &&
              std::fabs(normal_fresnel.y - f0.y) < 1e-6f &&
              std::fabs(normal_fresnel.z - f0.z) < 1e-6f &&
              std::fabs(grazing_fresnel.x - 1.0f) < 1e-6f &&
              std::fabs(grazing_fresnel.y - 1.0f) < 1e-6f &&
              std::fabs(grazing_fresnel.z - 1.0f) < 1e-6f,
          "Schlick Fresnel equals F0 at normal incidence and one at grazing");
    for (const float roughness : {0.02f, 0.1f, 0.5f, 1.0f}) {
        const float pdf = viewer::vulkan_ggx_reflection_pdf(
            0.8f, 0.65f, roughness);
        CHECK(std::isfinite(pdf) && pdf >= 0.0f,
              "GGX reflection PDF remains finite across authored roughness");
    }
    CHECK(viewer::vulkan_clearcoat_selection_probability(0.0f) == 0.0f &&
              std::fabs(viewer::vulkan_clearcoat_selection_probability(1.0f) -
                        0.5f) < 1e-6f,
          "zero clearcoat launches no coat samples and full coat normalizes lobe selection");
}

void run_raster_mesh_material_contract_tests() {
    Tri triangle{};
    triangle.vertex0 = make_float3(-1.0f, 0.0f, 0.0f);
    triangle.vertex1 = make_float3(1.0f, 0.0f, 0.0f);
    triangle.vertex2 = make_float3(0.0f, 1.0f, 0.0f);
    TriEx surface{};
    surface.uv0 = make_float2(0.1f, 0.2f);
    surface.uv1 = make_float2(0.3f, 0.4f);
    surface.uv2 = make_float2(0.5f, 0.6f);
    surface.N0 = surface.N1 = surface.N2 = make_float3(0.0f, 0.0f, 1.0f);
    surface.materialId = 7;
    surface.tint = make_float4(0.2f, 0.4f, 0.6f, 0.75f);
    surface.ao0 = 0.2f;
    surface.ao1 = 0.5f;
    surface.ao2 = 0.8f;

    const viewer::RasterMeshData mesh =
        viewer::build_raster_mesh_data(&triangle, &surface, 1);
    CHECK(mesh.surface_uvs ==
              std::vector<float>({0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f}),
          "raster mesh retains Vulkan source UVs");
    CHECK(mesh.material_ids == std::vector<uint32_t>({7u, 7u, 7u}),
          "raster mesh retains exact Vulkan material ids");
    CHECK(mesh.baked_ao == std::vector<float>({0.2f, 0.5f, 0.8f}),
          "raster mesh retains baked AO source values");

    const viewer::RasterMeshData fallback =
        viewer::build_raster_mesh_data(&triangle, nullptr, 1);
    CHECK(fallback.surface_uvs == std::vector<float>(6, 0.0f) &&
              fallback.material_ids ==
                  std::vector<uint32_t>(3, 0xffffffffu) &&
              fallback.baked_ao == std::vector<float>(3, 1.0f),
          "raster mesh supplies neutral Vulkan sources without TriEx");
}

void run_ray_tracing_capability_contract_tests() {
    CHECK(viewer::vk_scene_detail::scene_binding_stage_flags(5) ==
              (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
          "material binding is visible only to raster shader stages");
    CHECK(viewer::vk_scene_detail::scene_storage_limits_supported(5, 6) &&
              !viewer::vk_scene_detail::scene_storage_limits_supported(4, 6) &&
              !viewer::vk_scene_detail::scene_storage_limits_supported(5, 5),
          "scene capability accounting requires five compute and six set buffers");
    matter::VulkanRayTracingCapabilities unsupported{};
    unsupported.buffer_device_address = true;
    std::string reason;
    CHECK(!matter::supports_native_ray_tracing(unsupported, reason) &&
              reason.find(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) !=
                  std::string::npos,
          "unsupported fake device cleanly disables native ray tracing");

    matter::VulkanRayTracingCapabilities complete{};
    complete.acceleration_structure_extension = true;
    complete.ray_tracing_pipeline_extension = true;
    complete.deferred_host_operations_extension = true;
    complete.spirv_1_4_extension = true;
    complete.shader_float_controls_extension = true;
    complete.buffer_device_address = true;
    complete.acceleration_structure = true;
    complete.ray_tracing_pipeline = true;
    complete.storage_image_r8 = true;
    complete.shader_storage_image_extended_formats = true;
    CHECK(matter::supports_native_ray_tracing(complete, reason) &&
              reason.empty(),
          "complete fake RTX capability set enables native ray tracing");
    complete.storage_image_r8 = false;
    CHECK(!matter::supports_native_ray_tracing(complete, reason) &&
              reason.find("R8_UNORM") != std::string::npos,
          "missing R8 storage support cleanly disables native ray tracing");
    CHECK((viewer::vk_scene_detail::ray_depth_destination_stages(true) &
           VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR) != 0,
          "depth barrier includes ray tracing shader reads");
    CHECK((viewer::vk_scene_detail::ray_depth_destination_stages(false) &
           VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR) == 0,
          "fallback depth barrier excludes unavailable RT stages");
}

struct RetainProbe {
    uint32_t* destroyed = nullptr;
    ~RetainProbe() { ++*destroyed; }
};

void run_streamline_bridge_fallback_tests() {
    const matter::StreamlineBridge bridge =
        matter::StreamlineBridge::initialize_before_vulkan();
    CHECK(bridge.initialized(),
          "Streamline bridge fallback initialization succeeds without SDK");
    CHECK(!bridge.dlss_requested(),
          "Streamline bridge does not request DLSS when the SDK is absent");
    CHECK(bridge.dlss_unavailable_reason().find("not found") !=
              std::string::npos,
          "Streamline bridge reports that the SDK was not found");
    CHECK(!bridge.proxy_dispatch_used(),
          "Streamline fallback never dispatches through a proxy");
    CHECK(!matter::StreamlineBridge::requires_explicit_vulkan_info(true) &&
              matter::StreamlineBridge::requires_explicit_vulkan_info(false),
          "proxy-created Vulkan devices are not registered with Streamline twice");
    CHECK(matter::StreamlineBridge::test_missing_proxy("instance")
                  .native_retry_required() &&
              matter::StreamlineBridge::test_missing_proxy("device")
                  .native_retry_required(),
          "missing Streamline proxy acquisition preserves native retry intent");

    const std::vector<const char*> merged =
        matter::StreamlineBridge::merge_extensions({"A", "B"},
                                                    {"B", "C"});
    CHECK(merged.size() == 3 && std::string(merged[0]) == "A" &&
              std::string(merged[1]) == "B" && std::string(merged[2]) == "C",
          "Streamline extension merge preserves first-seen order");
}

void run_dlss_bridge_contract_tests() {
    const auto image = [](uintptr_t value) {
        return reinterpret_cast<VkImage>(value);
    };
    const auto view = [](uintptr_t value) {
        return reinterpret_cast<VkImageView>(value);
    };
    const auto memory = [](uintptr_t value) {
        return reinterpret_cast<VkDeviceMemory>(value);
    };
    matter::DlssConstants constants{};
    for (uint32_t index = 0; index < 16; ++index) {
        constants.camera_view_to_clip[index] = static_cast<float>(index + 1);
        constants.clip_to_camera_view[index] = static_cast<float>(index + 17);
        constants.clip_to_prev_clip[index] = static_cast<float>(index + 33);
        constants.prev_clip_to_clip[index] = static_cast<float>(index + 49);
    }
    constants.jitter_offset = {0.25f, -0.125f};
    constants.motion_vector_scale = {1.0f / 1280.0f, 1.0f / 720.0f};
    constants.motion_vectors_jittered = true;
    constants.reset = true;
    constants.internal_extent = {1280, 720};
    constants.output_extent = {1920, 1080};

    matter::DlssResources resources{};
    resources.hdr = {image(1), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    resources.depth = {image(2), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    resources.velocity = {image(3), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    resources.output = {image(4), VK_IMAGE_LAYOUT_GENERAL};
    resources.hdr.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    resources.hdr.extent = {1280, 720};
    resources.hdr.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    resources.hdr.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    resources.depth.format = VK_FORMAT_D32_SFLOAT;
    resources.depth.extent = {1280, 720};
    resources.depth.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    resources.depth.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    resources.velocity.format = VK_FORMAT_R16G16_SFLOAT;
    resources.velocity.extent = {1280, 720};
    resources.velocity.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    resources.velocity.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    resources.output.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    resources.output.extent = {1920, 1080};
    resources.output.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    resources.output.access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    resources.hdr.view = view(11);
    resources.hdr.memory = memory(21);
    resources.hdr.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    resources.hdr.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    resources.depth.view = view(12);
    resources.depth.memory = memory(22);
    resources.depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT;
    resources.depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    resources.velocity.view = view(13);
    resources.velocity.memory = memory(23);
    resources.velocity.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    resources.velocity.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    resources.output.view = view(14);
    resources.output.memory = memory(24);
    resources.output.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    resources.output.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    const matter::DlssOptions options{matter::DlssMode::Quality,
                                      {1920, 1080}, true, true};

    matter::StreamlineBridge native = matter::StreamlineBridge::native_fallback(
        "test native fallback");
    std::string error;
    matter::DlssConstants native_constants = constants;
    native_constants.output_extent = native_constants.internal_extent;
    matter::DlssEvaluationOutput evaluation_output{};
    CHECK(native.evaluate_dlss(VK_NULL_HANDLE, 1,
                               {matter::DlssMode::Native, {1280, 720}, true,
                                false},
                               native_constants, resources,
                               evaluation_output, error) &&
              native_constants.internal_extent.width ==
                  native_constants.output_extent.width &&
              native_constants.internal_extent.height ==
                  native_constants.output_extent.height &&
              native.test_dlss_evaluation_count() == 0,
          "Native mode keeps equal extents and never evaluates Streamline");

    bool received = false;
    std::vector<matter::DlssMode> mode_transitions;
    matter::StreamlineBridge fake = matter::StreamlineBridge::test_fake_dlss(
        [&](VkCommandBuffer command_buffer, uint64_t token,
            const matter::DlssOptions& captured_options,
            const matter::DlssConstants& captured,
            const matter::DlssResources& tagged,
            matter::DlssEvaluationOutput& output, std::string&) {
            mode_transitions.push_back(captured_options.mode);
            if (captured_options.mode == matter::DlssMode::Native) return true;
            received = command_buffer == VK_NULL_HANDLE &&
                       (token == 77 || token == 79) &&
                       captured_options.mode == matter::DlssMode::Quality &&
                       captured_options.output_extent.width == 1920 &&
                       captured_options.output_extent.height == 1080 &&
                       captured_options.color_buffers_hdr &&
                       captured_options.use_auto_exposure &&
                       captured.camera_view_to_clip[0] == 1.0f &&
                       captured.camera_view_to_clip[15] == 16.0f &&
                       captured.motion_vector_scale.x == 1.0f / 1280.0f &&
                       captured.motion_vector_scale.y == 1.0f / 720.0f &&
                       captured.motion_vectors_jittered && captured.reset &&
                       tagged.hdr.image != tagged.depth.image &&
                       tagged.hdr.image != tagged.velocity.image &&
                       tagged.hdr.image != tagged.output.image &&
                       tagged.hdr.layout ==
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                       tagged.depth.layout ==
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                       tagged.velocity.layout ==
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                       tagged.output.layout == VK_IMAGE_LAYOUT_GENERAL &&
                       tagged.depth.stage ==
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                       tagged.depth.access ==
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT &&
                       tagged.velocity.stage ==
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                       tagged.velocity.access ==
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT &&
                       tagged.output.access ==
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT &&
                       tagged.hdr.view == view(11) &&
                       tagged.hdr.memory == memory(21) &&
                       tagged.hdr.aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
                       tagged.depth.view == view(12) &&
                       tagged.depth.memory == memory(22) &&
                       tagged.depth.aspect == VK_IMAGE_ASPECT_DEPTH_BIT &&
                       tagged.velocity.view == view(13) &&
                       tagged.velocity.memory == memory(23) &&
                       tagged.output.view == view(14) &&
                       tagged.output.memory == memory(24) &&
                       (tagged.output.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
            output = {true, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
            return true;
        },
        [](const matter::DlssOptions& queried,
           matter::DlssOptimalSettings& optimal, std::string&) {
            if (queried.mode != matter::DlssMode::Quality ||
                queried.output_extent.width != 1920 ||
                queried.output_extent.height != 1080)
                return false;
            optimal = {{1280, 720}, 0.0f};
            return true;
        });
    std::vector<const char*> instance_extensions;
    std::vector<const char*> device_extensions;
    VkPhysicalDeviceVulkan12Features required12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features required13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    uint32_t graphics_queues = 1;
    uint32_t compute_queues = 0;
    fake.append_requirements(instance_extensions, device_extensions, required12,
                             required13, graphics_queues, compute_queues);
    CHECK(required13.privateData == VK_TRUE,
          "active Streamline bridge explicitly enables Vulkan 1.3 privateData");
    matter::DlssOptimalSettings optimal{};
    CHECK(fake.query_dlss_optimal_settings(options, optimal, error) &&
              optimal.render_extent.width == 1280 &&
              optimal.render_extent.height == 720 &&
              optimal.sharpness == 0.0f,
          "fake Quality returns exact optimal settings for requested output");
    CHECK(fake.evaluate_dlss(VK_NULL_HANDLE, 77, options, constants,
                             resources, evaluation_output, error) && received &&
              evaluation_output.output_written &&
              fake.test_dlss_evaluation_count() == 1 &&
              fake.active_dlss_mode() == matter::DlssMode::Quality,
          "fake Quality receives exact constants, distinct tagged resources, and output");
    matter::DlssOptions native_options{matter::DlssMode::Native,
                                       {1920, 1080}, true, true};
    CHECK(fake.evaluate_dlss(VK_NULL_HANDLE, 78, native_options, constants,
                             resources, evaluation_output, error) &&
              fake.active_dlss_mode() == matter::DlssMode::Native &&
              fake.consume_dlss_history_reset() &&
              !fake.consume_dlss_history_reset(),
          "Quality to Native sends eOff and requests exactly one history reset");
    received = false;
    const std::vector<matter::DlssMode> expected_mode_transitions{
        matter::DlssMode::Quality, matter::DlssMode::Native,
        matter::DlssMode::Quality};
    CHECK(fake.evaluate_dlss(VK_NULL_HANDLE, 79, options, constants, resources,
                             evaluation_output, error) && received &&
              fake.active_dlss_mode() == matter::DlssMode::Quality &&
              mode_transitions == expected_mode_transitions,
          "fake DLSS bridge observes the complete Quality Native Quality transition");

    matter::StreamlineBridge failing = matter::StreamlineBridge::test_fake_dlss(
        [](VkCommandBuffer, uint64_t, const matter::DlssOptions&,
           const matter::DlssConstants&, const matter::DlssResources&,
           matter::DlssEvaluationOutput&, std::string& evaluation_error) {
            evaluation_error = "injected DLSS evaluation failure";
            return false;
        });
    CHECK(!failing.evaluate_dlss(VK_NULL_HANDLE, 78, options, constants,
                                 resources, evaluation_output, error) &&
              failing.active_dlss_mode() == matter::DlssMode::Native &&
              failing.consume_dlss_history_reset() &&
              !failing.consume_dlss_history_reset() &&
              failing.dlss_unavailable_reason().find("injected") !=
                  std::string::npos,
          "evaluation error selects Native and resets exactly the following history");
}

void run_streamline_presentation_funnel_tests(matter::VulkanDevice& vulkan) {
    CHECK(matter::VulkanDevice::test_present_result_was_presented(VK_SUCCESS) &&
              matter::VulkanDevice::test_present_result_was_presented(VK_SUBOPTIMAL_KHR) &&
              !matter::VulkanDevice::test_present_result_was_presented(
                  VK_ERROR_OUT_OF_DATE_KHR) &&
              !matter::VulkanDevice::test_present_result_was_presented(
                  VK_ERROR_SURFACE_LOST_KHR),
          "end-frame outcome distinguishes actual presentation from recreation");
    std::string error;
    const auto has_common_present = [&]() {
        const auto& events = vulkan.test_presentation_events();
        return std::find(events.begin(), events.end(), "present_common") !=
               events.end();
    };

    matter::VulkanFrame record_failure{};
    vulkan.test_clear_presentation_events();
    CHECK(vulkan.begin_frame(record_failure, error),
          error.empty() ? "begin record-failure presentation frame" : error.c_str());
    _putenv_s("MATTER_VK_TEST_END_FRAME_FAULT", "record");
    CHECK(!vulkan.end_frame(record_failure, error),
          "record failure aborts presentation before the common plugin handoff");
    _putenv_s("MATTER_VK_TEST_END_FRAME_FAULT", "");
    CHECK(!has_common_present(),
          "record failure has no common-present handoff");

    matter::VulkanFrame submit_failure{};
    vulkan.test_clear_presentation_events();
    CHECK(vulkan.begin_frame(submit_failure, error),
          error.empty() ? "begin submit-failure presentation frame" : error.c_str());
    _putenv_s("MATTER_VK_TEST_END_FRAME_FAULT", "submit");
    CHECK(!vulkan.end_frame(submit_failure, error),
          "submit failure aborts presentation before the common plugin handoff");
    _putenv_s("MATTER_VK_TEST_END_FRAME_FAULT", "");
    CHECK(!has_common_present(),
          "submit failure has no common-present handoff");

    matter::VulkanFrame submitted{};
    vulkan.test_clear_presentation_events();
    CHECK(vulkan.begin_frame(submitted, error),
          error.empty() ? "begin successful presentation frame" : error.c_str());
    bool actually_presented = false;
    CHECK(vulkan.end_frame(submitted, actually_presented, error) &&
              actually_presented,
          error.empty() ? "end successful presentation frame" : error.c_str());
    const auto& successful_events = vulkan.test_presentation_events();
    const auto acquire = std::find(successful_events.begin(),
                                   successful_events.end(), "acquire");
    const auto common_present = std::find(
        acquire, successful_events.end(), "present_common");
    const auto event_after_common =
        common_present == successful_events.end()
            ? successful_events.end()
            : std::next(common_present);
    CHECK(acquire != successful_events.end() &&
              common_present != successful_events.end() &&
              event_after_common != successful_events.end() &&
              *event_after_common == "present" &&
              std::count(successful_events.begin(), successful_events.end(),
                         "present_common") == 1 &&
              std::count(successful_events.begin(), successful_events.end(),
                         "present") == 1,
          "VulkanDevice funnels acquire before adjacent sole common-present and present");
    CHECK(vulkan.test_last_present_common_serial() == submitted.serial,
          "common-present handoff exposes the submitted frame serial");
}

void run_cuda_vulkan_interop(matter::VulkanDevice& vulkan) {
    std::string error;
    auto interop = matter::CudaVulkanInterop::create(vulkan, error);
    CHECK(interop != nullptr,
          error.empty() ? "create CUDA Vulkan interop" : error.c_str());
    if (!interop) return;
    CHECK(matter::cuda_vulkan_application_export_handle_count() == 0,
          "CUDA interop creation closes every application-owned export handle");

    CHECK(!matter::cuda_vulkan_device_ids_match_for_test(
              interop->vulkan_uuid(), interop->cuda_uuid(), true),
          "UUID mismatch test seam rejects different adapters");
    const std::array<uint8_t, VK_LUID_SIZE> vk_luid{{1,2,3,4,5,6,7,8}};
    auto cuda_luid = vk_luid;
    CHECK(matter::cuda_vulkan_luid_matches_for_test(
              true, vk_luid, 1, true, cuda_luid, 1),
          "matching Vulkan/CUDA LUID and node mask select the adapter");
    cuda_luid[0] ^= 1;
    CHECK(!matter::cuda_vulkan_luid_matches_for_test(
              true, vk_luid, 1, true, cuda_luid, 1) &&
              !matter::cuda_vulkan_luid_matches_for_test(
                  true, vk_luid, 1, true, vk_luid, 2),
          "LUID or node-mask mismatch rejects the CUDA adapter");
    CHECK(!matter::cuda_vulkan_luid_matches_for_test(
              true, vk_luid, 1, false, vk_luid, 1),
          "valid Vulkan LUID requires successful cuDeviceGetLuid");
    const std::string luid_error =
        matter::cuda_vulkan_luid_failure_diagnostic_for_test(
            "vk-name", interop->vulkan_uuid(), vk_luid, 3, "cuda-name",
            interop->cuda_uuid(), 999);
    CHECK(luid_error.find("CUresult 999") != std::string::npos &&
              luid_error.find("vk-name") != std::string::npos &&
              luid_error.find("cuda-name") != std::string::npos &&
              luid_error.find("nodeMask=3") != std::string::npos &&
              luid_error.find("luid=<unavailable>") != std::string::npos,
          "cuDeviceGetLuid failure reports result, names, IDs, and masks");

    const char* cycles_text = std::getenv("MATTER_VK_SMOKE_RESIZES");
    const unsigned long parsed = cycles_text ? std::strtoul(cycles_text, nullptr, 10) : 100;
    const uint32_t cycles = parsed > 0 && parsed <= 10000
                                ? static_cast<uint32_t>(parsed)
                                : 100;
    matter::CudaVulkanInteropPixel pixel{};
    for (uint32_t warmup = 0; warmup < 2; ++warmup) {
        const VkExtent2D extent = warmup == 0 ? VkExtent2D{64, 64}
                                              : VkExtent2D{96, 80};
        CHECK(interop->round_trip(extent, warmup + 1, pixel, error),
              error.empty() ? "CUDA Vulkan interop size-class warm-up"
                            : error.c_str());
        CHECK(matter::cuda_vulkan_application_export_handle_count() == 0,
              "size-class import closes its application-owned export handle");
    }
    CHECK(!interop->round_trip({64, 64}, 2, pixel, error) &&
              error.find("strictly increasing") != std::string::npos,
          "interop rejects repeated timeline serials");
    error.clear();
    const uint32_t baseline_handles = matter::win32_process_handle_count();
    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        const VkExtent2D extent = (cycle & 1u) == 0 ? VkExtent2D{64, 64}
                                                    : VkExtent2D{96, 80};
        CHECK(interop->round_trip(extent, cycle + 3, pixel, error),
              error.empty() ? "CUDA Vulkan interop round trip" : error.c_str());
        CHECK(close4({pixel.r, pixel.g, pixel.b, pixel.a},
                     {0.0f, 1.0f, 1.0f, 1.0f}, 1e-3f),
              "CUDA imported surface contains exact cyan pixel");
        if (!error.empty()) break;
    }
    const uint32_t steady_handles = matter::win32_process_handle_count();
    CHECK(steady_handles <= baseline_handles + 2,
          "100 interop operations keep steady-state handle count within two");
    interop.reset();
    CHECK(matter::cuda_vulkan_application_export_handle_count() == 0,
          "interop reset owns no exported Win32 handles");
    const uint32_t final_handles = matter::win32_process_handle_count();
    std::printf("interop cycles: %u\n", cycles);
    std::printf("interop pixel: %.6f %.6f %.6f %.6f\n", pixel.r, pixel.g,
                pixel.b, pixel.a);
    std::printf("interop handles steady: %u -> %u; after teardown: %u\n",
                baseline_handles, steady_handles, final_handles);
}

bool run_cuda_vulkan_interop_fault(matter::VulkanDevice& vulkan,
                                   const char* fault) {
    std::string error;
    matter::cuda_vulkan_reset_test_destroy_call_count();
    const uintptr_t caller_context =
        matter::cuda_vulkan_create_caller_context_for_test(error);
    CHECK(caller_context != 0,
          error.empty() ? "create non-null caller CUDA context" : error.c_str());
    std::string query_error;
    CHECK(matter::cuda_vulkan_current_context_for_test(query_error) == caller_context,
          query_error.empty() ? "caller CUDA context is current"
                              : query_error.c_str());
    auto interop = matter::CudaVulkanInterop::create(vulkan, error);
    CHECK(interop != nullptr,
          error.empty() ? "create fault-test CUDA Vulkan interop" : error.c_str());
    CHECK(matter::cuda_vulkan_current_context_for_test(query_error) == caller_context,
          query_error.empty() ? "interop create preserves caller CUDA current context"
                              : query_error.c_str());
    if (!interop) {
        matter::cuda_vulkan_destroy_caller_context_for_test(caller_context);
        return false;
    }
    _putenv_s("MATTER_VK_INTEROP_FAULT", fault);
    matter::CudaVulkanInteropPixel pixel{};
    const bool completed = interop->round_trip({64, 64}, 1, pixel, error);
    _putenv_s("MATTER_VK_INTEROP_FAULT", "");
    CHECK(!completed, "selected CUDA Vulkan interop fault aborts the round trip");
    const std::string fault_name = fault;
    std::string expected;
    if (fault_name == "after-kernel-before-signal") expected = "after kernel launch";
    else if (fault_name == "signal-enqueue-failure") expected = "cuSignalExternalSemaphoresAsync";
    else if (fault_name == "after-signal-before-vk-wait") expected = "after CUDA signal";
    else if (fault_name == "cuda-async-unproven") expected = "asynchronous CUDA";
    else if (fault_name == "vk-wait-failure") expected = "Vulkan fence wait";
    else if (fault_name == "vk-recovery-unproven") expected = "Vulkan timeline";
    CHECK(!expected.empty() && error.find(expected) != std::string::npos,
          "fault diagnostic identifies the selected ownership phase");
    CHECK(interop->poisoned(), "post-launch failure poisons CUDA Vulkan interop");
    const std::string poison = error;
    CHECK(!interop->round_trip({64, 64}, 2, pixel, error) && error == poison,
          "poisoned CUDA Vulkan interop fails closed with a stable diagnostic");
    CHECK(matter::cuda_vulkan_current_context_for_test(query_error) == caller_context,
          query_error.empty() ? "interop failure preserves caller CUDA current context"
                              : query_error.c_str());
    CHECK(matter::cuda_vulkan_application_export_handle_count() == 0,
          "failure path owns no exported Win32 handles");
    interop.reset();
    CHECK(matter::cuda_vulkan_current_context_for_test(query_error) == caller_context,
          query_error.empty() ? "interop reset preserves caller CUDA current context"
                              : query_error.c_str());
    const bool unproven = fault_name == "cuda-async-unproven" ||
                          fault_name == "vk-recovery-unproven";
    if (unproven) {
        CHECK(matter::cuda_vulkan_test_destroy_call_count() == 0,
              "unproven completion preserves all interop resources");
    }
    matter::cuda_vulkan_destroy_caller_context_for_test(caller_context);
    return unproven;
}

void run_vulkan_only_handle_diagnostic(matter::VulkanDevice& vulkan) {
    std::string error;
    const auto trace = [](const char* label) {
        std::printf("HANDLE_DIAG %-38s count=%u result=0 sync=n/a\n", label,
                    matter::win32_process_handle_count());
    };
    trace("Vulkan-only baseline");
    matter::VkImageResource image;
    CHECK(matter::create_image(
              vulkan, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT,
              {64, 64, 1},
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
              image, error),
          error.empty() ? "Vulkan-only diagnostic image" : error.c_str());
    trace("Vulkan-only create image+memory");
    CHECK(matter::transition_image(
              vulkan, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
              VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
              error),
          error.empty() ? "Vulkan-only diagnostic submit" : error.c_str());
    trace("Vulkan-only submit+fence wait");
    image.reset();
    trace("Vulkan-only image destroy");
}

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

void run_vulkan_temporal_tests() {
    viewer::TemporalState temporal;
    viewer::FrameMatrices camera{};
    camera.world_to_view = identity_matrix();
    camera.view_to_clip = identity_matrix();
    camera.world_to_clip = identity_matrix();
    camera.clip_to_world = identity_matrix();
    const viewer::TemporalInstance still{7, identity_matrix()};

    viewer::TemporalFrame first = temporal.begin(
        camera, {100, 80}, {100, 80}, {still}, {});
    CHECK(first.reset && !first.instances[0].history_valid,
          "first temporal frame resets invalid history");
    CHECK(first.presented_frame_index == 0,
          "first candidate seeds from zero successfully presented frames");
    CHECK(temporal.commit_presented(first.attempt_token),
          "successful presentation commits temporal candidate");

    viewer::TemporalFrame static_frame = temporal.begin(
        camera, {100, 80}, {100, 80}, {still}, {});
    const matter::Float3 static_velocity =
        viewer::temporal_velocity_pixels(static_frame, 7, {0.0f, 0.0f, 0.0f});
    CHECK(!static_frame.reset && static_frame.instances[0].history_valid &&
              std::fabs(static_velocity.x + 0.25f) < 1e-6f &&
              std::fabs(static_velocity.y + 1.0f / 3.0f) < 1e-6f,
          "static camera and rigid instance preserve the known Halton delta");
    CHECK(std::fabs(static_frame.previous_jittered.world_to_clip.m[3] -
                    first.current_jittered.world_to_clip.m[3]) < 1e-6f &&
              std::fabs(static_frame.previous_jittered.world_to_clip.m[7] -
                        first.current_jittered.world_to_clip.m[7]) < 1e-6f &&
              std::fabs(static_frame.previous_jittered.jitter_pixels[0] -
                        first.jitter_pixels[0]) < 1e-6f &&
              std::fabs(static_frame.previous_jittered.jitter_pixels[1] -
                        first.jitter_pixels[1]) < 1e-6f,
          "previous projection retains the last actually presented jitter");
    CHECK(temporal.commit_presented(static_frame.attempt_token),
          "second successful presentation advances temporal history");

    viewer::FrameMatrices moved_camera = camera;
    moved_camera.view_to_clip = viewer::mat4_translation({-0.2f, 0.0f, 0.0f});
    moved_camera.world_to_clip = viewer::mat4_translation({-0.2f, 0.0f, 0.0f});
    moved_camera.clip_to_world = viewer::mat4_translation({0.2f, 0.0f, 0.0f});
    viewer::TemporalFrame camera_motion = temporal.begin(
        moved_camera, {100, 80}, {100, 80}, {still}, {});
    const matter::Float3 camera_velocity = viewer::temporal_velocity_pixels(
        camera_motion, 7, {0.0f, 0.0f, 0.0f});
    CHECK(std::fabs(camera_velocity.x + 9.5f) < 1e-5f &&
              std::fabs(camera_velocity.y - 5.0f / 9.0f) < 1e-5f,
          "known camera translation includes the presented Halton delta");
    CHECK(temporal.commit_presented(camera_motion.attempt_token),
          "camera-motion candidate commits");

    const viewer::TemporalInstance moved_object{
        7, viewer::mat4_translation({0.4f, 0.0f, 0.0f})};
    viewer::TemporalFrame object_motion = temporal.begin(
        moved_camera, {100, 80}, {100, 80}, {moved_object}, {});
    const matter::Float3 object_velocity = viewer::temporal_velocity_pixels(
        object_motion, 7, {0.0f, 0.0f, 0.0f});
    CHECK(std::fabs(object_velocity.x - 19.375f) < 1e-5f &&
              std::fabs(object_velocity.y + 1.0f / 3.0f) < 1e-5f,
          "known rigid-instance translation includes the presented Halton delta");
    CHECK(temporal.commit_presented(object_motion.attempt_token),
          "object-motion candidate commits");

    const auto expect_one_reset = [&](VkExtent2D internal,
                                      viewer::TemporalInvalidation invalidation,
                                      std::vector<viewer::TemporalInstance> instances,
                                      const char* label) {
        viewer::TemporalFrame reset = temporal.begin(
            moved_camera, internal, {100, 80}, instances, invalidation);
        CHECK(reset.reset, label);
        CHECK(temporal.commit_presented(reset.attempt_token), label);
        viewer::TemporalFrame stable = temporal.begin(
            moved_camera, internal, {100, 80}, instances, {});
        CHECK(!stable.reset, "temporal invalidation resets exactly one frame");
        CHECK(temporal.commit_presented(stable.attempt_token),
              "post-reset candidate commits");
    };
    expect_one_reset({120, 80}, {}, {moved_object}, "resize resets temporal history");
    expect_one_reset({120, 80}, {.camera_cut = true}, {moved_object},
                     "camera cut resets temporal history");
    expect_one_reset({120, 80}, {.world_reload = true}, {moved_object},
                     "world reload resets temporal history");
    expect_one_reset({120, 80}, {.renderer_reset = true}, {moved_object},
                     "renderer recovery resets temporal history");
    expect_one_reset({120, 80}, {}, {{99, identity_matrix()}},
                     "missing previous rigid instance resets temporal history");

    viewer::TemporalFrame failed = temporal.begin(
        moved_camera, {120, 80}, {100, 80}, {{99, identity_matrix()}}, {});
    const uint64_t failed_token = failed.attempt_token;
    CHECK(temporal.discard_failed_attempt(failed_token),
          "failed presentation discards uncommitted candidate");
    viewer::TemporalFrame after_failure = temporal.begin(
        moved_camera, {120, 80}, {100, 80}, {{99, identity_matrix()}}, {});
    CHECK(after_failure.reset && after_failure.attempt_token > failed_token &&
              std::fabs(after_failure.jitter_pixels[0] -
                        failed.jitter_pixels[0]) < 1e-6f &&
              std::fabs(after_failure.jitter_pixels[1] -
                        failed.jitter_pixels[1]) < 1e-6f,
          "failed presentation forces reset without advancing presented jitter");
    CHECK(!temporal.commit_presented(failed_token),
          "stale failed attempt token cannot commit temporal history");
    CHECK(after_failure.presented_frame_index == failed.presented_frame_index,
          "retry retains committed frame identity despite a new attempt token");

    viewer::TemporalState stable_ids;
    const uint64_t a_id = viewer::temporal_instance_id(41, 1001, 0);
    const uint64_t b_id = viewer::temporal_instance_id(42, 1002, 0);
    viewer::TemporalFrame two = stable_ids.begin(
        camera, {100, 80}, {100, 80},
        {{a_id, identity_matrix()}, {b_id, identity_matrix()}}, {});
    CHECK(stable_ids.commit_presented(two.attempt_token),
          "two-instance temporal baseline commits");
    viewer::TemporalFrame only_b = stable_ids.begin(
        camera, {100, 80}, {100, 80}, {{b_id, identity_matrix()}}, {});
    CHECK(!only_b.reset && only_b.instances.size() == 1 &&
              only_b.instances[0].instance_id == b_id &&
              only_b.instances[0].history_valid,
          "stable rigid identity survives [A,B] to [B] compaction");
    CHECK(stable_ids.commit_presented(only_b.attempt_token),
          "single surviving instance commits");
    viewer::TemporalFrame empty = stable_ids.begin(
        camera, {100, 80}, {100, 80}, {}, {});
    CHECK(stable_ids.commit_presented(empty.attempt_token),
          "presented clear frame advances empty temporal history");
    viewer::TemporalFrame returning_b = stable_ids.begin(
        camera, {100, 80}, {100, 80}, {{b_id, identity_matrix()}}, {});
    CHECK(returning_b.reset && !returning_b.instances[0].history_valid,
          "instance returning after presented clear frame resets history");

    CHECK(viewer::vk_scene_detail::frame_constants_size_for_test() == 288,
          "C++ FrameConstants matches final std140 uvec4 padding and size");
}

void run_vulkan_gi_temporal_sequence_tests() {
    viewer::GiTemporalState history;
    const viewer::GiTemporalSurface stable{
        {1.0f, 0.25f, 0.125f}, 0.5f, {0.0f, 0.0f, 1.0f}, 7u, 41u};

    auto present = [&](viewer::GiTemporalSurface surface,
                       matter::Float3 velocity, bool reset,
                       uint64_t attempt) {
        const viewer::GiTemporalResult result = history.accumulate(
            surface, velocity, {4, 4}, {2, 2}, reset, attempt);
        CHECK(history.commit_presented(attempt),
              "presented GI candidate commits its ping-pong history");
        return result;
    };

    const auto first = present(stable, {}, true, 1);
    const auto second = present(stable, {}, false, 2);
    const auto third = present(stable, {}, false, 3);
    CHECK(first.history_length == 1u && second.history_length == 2u &&
              third.history_length == 3u && third.rejection_bits == 0u,
          "static GI pixel reaches history length three");

    history.seed_presented_for_test({4, 4}, {1, 2}, stable, 3u);
    const auto translated = history.accumulate(
        stable, {1.0f, 0.0f, 0.0f}, {4, 4}, {2, 2}, false, 4);
    CHECK(translated.previous_pixel.x == 1 &&
              translated.previous_pixel.y == 2 &&
              translated.history_length == 4u,
          "current-to-previous pixel velocity samples current minus velocity");
    CHECK(history.commit_presented(4), "translated GI candidate commits");

    const auto expect_rejection = [&](viewer::GiTemporalSurface changed,
                                      uint32_t expected_bit,
                                      uint64_t attempt,
                                      const char* label) {
        history.seed_presented_for_test({4, 4}, {2, 2}, stable, 3u);
        const auto rejected = history.accumulate(
            changed, {}, {4, 4}, {2, 2}, false, attempt);
        CHECK(rejected.history_length == 1u &&
                  rejected.rejection_bits == expected_bit,
              label);
        CHECK(history.commit_presented(attempt), label);
    };
    auto changed = stable;
    changed.depth += 0.2f;
    expect_rejection(changed, viewer::kGiRejectDepth, 5,
                     "depth discontinuity has unique GI rejection bit");
    changed = stable;
    changed.normal = {1.0f, 0.0f, 0.0f};
    expect_rejection(changed, viewer::kGiRejectNormal, 6,
                     "normal discontinuity has unique GI rejection bit");
    changed = stable;
    changed.material_index++;
    expect_rejection(changed, viewer::kGiRejectMaterial, 7,
                     "material discontinuity has unique GI rejection bit");
    changed = stable;
    changed.instance_token++;
    expect_rejection(changed, viewer::kGiRejectInstance, 8,
                     "instance discontinuity has unique GI rejection bit");

    history.seed_presented_for_test({4, 4}, {2, 2}, stable, 3u);
    const auto failed = history.accumulate(stable, {}, {4, 4}, {2, 2}, false, 9);
    CHECK(failed.history_length == 4u && history.discard_failed_attempt(9),
          "failed GI attempt discards candidate history");
    const auto retry = history.accumulate(stable, {}, {4, 4}, {2, 2}, false, 10);
    CHECK(retry.history_length == 4u,
          "failed presentation cannot become future GI history");
    CHECK(history.commit_presented(10), "retried GI candidate commits");

    const auto reset_once = [&](VkExtent2D extent, uint64_t first_attempt,
                                const char* label) {
        const auto reset = history.accumulate(
            stable, {}, extent, {1, 1}, true, first_attempt);
        CHECK(reset.history_length == 1u &&
                  reset.rejection_bits == viewer::kGiRejectReset,
              label);
        CHECK(history.commit_presented(first_attempt), label);
        const auto stable_again = history.accumulate(
            stable, {}, extent, {1, 1}, false, first_attempt + 1);
        CHECK(stable_again.history_length == 2u &&
                  stable_again.rejection_bits == 0u,
              "GI invalidation resets exactly one presented frame");
        CHECK(history.commit_presented(first_attempt + 1), label);
    };
    reset_once({8, 8}, 11, "resize resets GI history once");
    reset_once({8, 8}, 13, "camera cut resets GI history once");
    reset_once({8, 8}, 15, "world reload resets GI history once");
    reset_once({8, 8}, 17, "Native/DLSS mode change resets GI history once");
}

void run_vulkan_instance_cache_tests() {
    viewer::ResolvedInstance a{};
    a.part_hash = 11;
    a.stable_id = 41;
    a.segment = 0;
    a.transform[0] = a.transform[5] = a.transform[10] = a.transform[15] = 1.0f;
    viewer::ResolvedInstance b = a;
    b.part_hash = 12;
    b.stable_id = 42;
    std::vector<viewer::ResolvedInstance> roots{a, b};

    viewer::VulkanInstanceCache cache;
    CHECK(!cache.matches(roots), "empty Vulkan instance cache misses");
    std::vector<viewer::VkSceneInstance> expanded(2);
    expanded[0].part_hash = 21;
    expanded[1].part_hash = 22;
    cache.store(roots, std::move(expanded));
    CHECK(cache.matches(roots), "unchanged resolved roots hit Vulkan cache");
    CHECK(cache.instances().size() == 2 && cache.expansion_count() == 1,
          "Vulkan cache retains expanded instances and counts one expansion");

    roots[1].lod_level = 3;
    CHECK(cache.matches(roots), "LOD change preserves Vulkan cache hit");
    roots[1].segment = 1;
    CHECK(!cache.matches(roots), "segment change invalidates Vulkan cache");
    roots[1].segment = 0;
    roots[1].transform[3] = 1.0f;
    CHECK(!cache.matches(roots), "transform change invalidates Vulkan cache");
    cache.invalidate();
    CHECK(cache.instances().empty(), "cache invalidation releases expansion");
}

bool gpu_matrix_equal(const viewer::GpuMat4& actual,
                      const matter::Mat4f& expected) {
    const viewer::GpuMat4 packed = viewer::pack_glsl_mat4(expected);
    for (size_t i = 0; i < 16; ++i) {
        if (!(std::fabs(actual.elements[i] - packed.elements[i]) < 1e-5f))
            return false;
    }
    return true;
}

bool rt_matrix_equal(const float actual[16], const matter::Mat4f& expected) {
    for (size_t i = 0; i < 16; ++i) {
        if (!(std::fabs(actual[i] - expected.m[i]) < 1e-5f)) return false;
    }
    return true;
}

bool close4(matter::Float4 actual, matter::Float4 expected, float epsilon) {
    return std::fabs(actual.x - expected.x) <= epsilon &&
           std::fabs(actual.y - expected.y) <= epsilon &&
           std::fabs(actual.z - expected.z) <= epsilon &&
           std::fabs(actual.w - expected.w) <= epsilon;
}

bool close3(matter::Float3 actual, matter::Float3 expected, float epsilon) {
    return std::fabs(actual.x - expected.x) <= epsilon &&
           std::fabs(actual.y - expected.y) <= epsilon &&
           std::fabs(actual.z - expected.z) <= epsilon;
}

viewer::VkScenePart fixed_part(uint64_t hash, matter::Float3 minimum,
                               matter::Float3 maximum,
                               uint32_t first_vertex);

viewer::VkScenePart known_raster_triangle(uint64_t hash,
                                          uint32_t material_index = 7u) {
    viewer::VkScenePart part = fixed_part(
        hash, {-0.75f, -0.75f, -2.0f}, {0.75f, 1.5f, -2.0f}, 0);
    const matter::Float3 normal{0.0f, 1.0f, 0.0f};
    const matter::Float4 tint{0.9f, 0.1f, 0.3f, 0.0f};
    part.vertices = {
        {{-0.75f, -0.75f, -2.0f}, normal, tint,
         {0.1f, 0.2f, 0.2f, 1.0f}, material_index, {}},
        {{0.75f, -0.75f, -2.0f}, normal, tint,
         {0.3f, 0.4f, 0.5f, 1.0f}, material_index, {}},
        {{0.0f, 1.5f, -2.0f}, normal, tint,
         {0.5f, 0.6f, 0.8f, 1.0f}, material_index, {}},
    };
    return part;
}

void run_raster_path(matter::VulkanDevice& vulkan) {
    constexpr uint32_t width = 160;
    constexpr uint32_t height = 160;
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    matter::VulkanGiSettings scaled_gi{};
    scaled_gi.samples_per_pixel = 16;
    scaled_gi.trace_scale = 0.5f;
    renderer.set_gi_settings(scaled_gi);
    std::vector<MaterialGpuRecord> materials(9);
    materials[7].base_roughness[0] = 0.25f;
    materials[7].base_roughness[1] = 0.5f;
    materials[7].base_roughness[2] = 0.75f;
    materials[7].base_roughness[3] = 0.2f;
    materials[7].metal_opacity_spec_coat[0] = 0.7f;
    materials[7].metal_opacity_spec_coat[1] = 1.0f;
    materials[7].scattering_shape[3] = 1.0f;
    materials[7].emission_strength[3] = 5.0f;
    materials[8] = materials[7];
    materials[8].base_roughness[0] = 0.8f;
    CHECK(renderer.update_materials(materials, 1, 1, error),
          error.empty() ? "stage shared raster materials" : error.c_str());
    const auto half_roundtrip = [](float value) {
        if (value == 0.0f) return 0.0f;
        int exponent = 0;
        const float mantissa = std::frexp(value, &exponent);
        return std::ldexp(std::round(std::ldexp(mantissa, 11)),
                          exponent - 11);
    };
    const auto decoded_emission = [&](float emission) {
        const float encoded = half_roundtrip(
            viewer::vulkan_encode_emission(emission));
        return std::exp2(std::fmin(encoded,
                                   viewer::kVkMaxEncodedEmission)) - 1.0f;
    };
    const float decoded_five = decoded_emission(5.0f);
    const float decoded_thousand = decoded_emission(1000.0f);
    const float decoded_max =
        decoded_emission(std::numeric_limits<float>::max());
    CHECK(std::fabs(decoded_five - 5.0f) < 0.02f,
          "emission 5 survives CPU half-float quantization");
    CHECK(std::fabs(decoded_thousand - 1000.0f) < 4.0f,
          "emission 1000 survives CPU half-float quantization");
    CHECK(std::isfinite(decoded_max) && decoded_max > decoded_thousand,
          "FLT_MAX emission half roundtrip saturates finite and monotonic");
    CHECK(viewer::vulkan_material_uses_unsupported_texture(2.0f) &&
              !viewer::vulkan_material_uses_unsupported_texture(-1.0f) &&
              !viewer::vulkan_material_uses_unsupported_texture(
                  std::numeric_limits<float>::quiet_NaN()),
          "packed runtime texture override triggers Vulkan warning path");

    // The first part reserves transform slot zero.  The known triangle then
    // draws with firstInstance=1, catching any raster shader that incorrectly
    // adds gl_BaseInstance to gl_InstanceIndex a second time.
    const viewer::VkScenePart dummy = fixed_part(
        900, {-0.1f, -0.1f, -2.1f}, {0.1f, 0.1f, -1.9f}, 0);
    const viewer::VkScenePart triangle = known_raster_triangle(901);
    const viewer::VkScenePart unaffected = known_raster_triangle(902, 8u);
    CHECK(renderer.ensure_part(dummy, error) >= 0,
          error.empty() ? "ensure raster dummy part" : error.c_str());
    CHECK(renderer.ensure_part(triangle, error) >= 0,
          error.empty() ? "ensure known raster triangle" : error.c_str());
    CHECK(renderer.ensure_part(unaffected, error) >= 0,
          error.empty() ? "ensure unaffected material triangle"
                        : error.c_str());

    const matter::Mat4f identity = identity_matrix();
    CHECK(renderer.update_instances({{900, identity, 111},
                                     {901, identity, 222}}, error),
          error.empty() ? "upload raster instances" : error.c_str());

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices frame{};
    CHECK(viewer::build_frame_matrices(camera, width, height, frame, error),
          error.empty() ? "build raster frame matrices" : error.c_str());
    CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error),
          error.empty() ? "dispatch raster culling" : error.c_str());
    std::vector<viewer::DrawCommand> raster_commands;
    CHECK(renderer.readback_commands(raster_commands, error),
          error.empty() ? "read raster indirect commands" : error.c_str());
    CHECK(renderer.raster_draw_command_count() == 2 &&
              raster_commands.size() > 2 * viewer::kVkMaxLod &&
              raster_commands[2 * viewer::kVkMaxLod].instance_count == 0,
          "visible cull-only parts cannot issue raster indirect draws");
    CHECK(raster_commands.size() > viewer::kVkMaxLod &&
              raster_commands[viewer::kVkMaxLod].first_instance == 1,
          "known triangle uses nonzero firstInstance transform region");
    CHECK(renderer.render_gbuffer_and_composite(width, height, error),
          error.empty() ? "render G-buffer and composite" : error.c_str());

    const viewer::VkRasterAttachments attachments =
        renderer.raster_attachments();
    CHECK(attachments.albedo.format == VK_FORMAT_R8G8B8A8_UNORM,
          "albedo attachment format");
    CHECK(attachments.normal.format == VK_FORMAT_R16G16B16A16_SFLOAT,
          "normal attachment format");
    CHECK(attachments.orm.format == VK_FORMAT_R8G8B8A8_UNORM,
          "ORM attachment format");
    CHECK(attachments.velocity.format == VK_FORMAT_R16G16_SFLOAT,
          "sampled velocity attachment format");
    CHECK(attachments.material_instance.format == VK_FORMAT_R32G32_UINT,
          "integer material and instance attachment format");
    CHECK(attachments.depth.format == VK_FORMAT_D32_SFLOAT,
          "depth attachment format");
    CHECK(attachments.hdr.format == VK_FORMAT_R16G16B16A16_SFLOAT,
          "HDR attachment format");
    CHECK(renderer.test_raw_diffuse_format() ==
              VK_FORMAT_R16G16B16A16_SFLOAT,
          "raw diffuse GI attachment format");
    CHECK(renderer.test_gi_samples_per_pixel() == 1u &&
              renderer.test_raw_diffuse_extent().width == width / 2 &&
              renderer.test_raw_diffuse_extent().height == height / 2,
          "GI enforces one continuation sample and allocates at trace scale");
    CHECK(attachments.extent.width == width &&
              attachments.extent.height == height,
          "raster attachment extent");

    viewer::VkRasterPixel center{};
    viewer::VkRasterPixel lower_right_inside{};
    viewer::VkRasterPixel background{};
    CHECK(renderer.readback_raster_pixel(width / 2, height / 2, center,
                                         error),
          error.empty() ? "read raster center" : error.c_str());
    CHECK(renderer.readback_raster_pixel(103, 100, lower_right_inside, error),
          error.empty() ? "read asymmetric raster structural pixel"
                        : error.c_str());
    CHECK(renderer.readback_raster_pixel(4, 4, background, error),
          error.empty() ? "read raster background" : error.c_str());
    CHECK(close4(center.albedo, {0.25f, 0.5f, 0.75f, 1.0f}, 6e-3f),
          "known center albedo");
    CHECK(close4(center.normal,
                 {0.0f, 1.0f, 0.0f,
                  viewer::vulkan_encode_emission(5.0f)},
                 4e-3f),
          "known center normal xyz and half-float emission payload");
    CHECK(close4(center.orm,
                 {0.2f, 0.7f, 0.5f, 1.0f},
                 6e-3f),
          "known center ORM retains interpolated baked AO");
    CHECK(center.material_index == 7u,
          "G-buffer retains exact material id");
    CHECK(center.instance_token != 0u,
          "draw writes stable instance history token");
    CHECK(center.instance_token == viewer::vulkan_history_token(222),
          "draw writes token derived from stable instance identity");
    CHECK(std::fabs(center.orm.z - 0.5f) < 0.01f,
          "baked AO survives interpolation");
    CHECK(std::isfinite(center.depth) && center.depth >= 0.0f &&
              center.depth <= 1.0f,
          "known center Vulkan depth range");
    CHECK(std::fabs(center.depth - 0.959596f) <= 2e-3f,
          "known center projected depth");
    CHECK(lower_right_inside.albedo.w > 0.99f,
          "negative-height viewport preserves top-left framebuffer convention");
    CHECK(background.albedo.w < 0.01f && background.depth >= 0.999f,
          "background color and depth remain clear");
    CHECK(background.material_index == UINT32_MAX &&
              background.instance_token == UINT32_MAX,
          "background material and instance channels clear to invalid");
    CHECK(close4(center.raw_diffuse, {0.0f, 0.0f, 0.0f, 0.0f}, 1e-6f),
          "disabled RT produces zero raw diffuse GI");
    CHECK(std::fabs(center.velocity.x) < 1e-6f &&
              std::fabs(center.velocity.y) < 1e-6f &&
              std::fabs(background.velocity.x) < 1e-6f &&
              std::fabs(background.velocity.y) < 1e-6f,
          "invalid first-frame history and background write zero velocity");
    CHECK(std::isfinite(background.hdr.x) &&
              std::isfinite(background.hdr.y) &&
              std::isfinite(background.hdr.z) &&
              std::isfinite(background.hdr.w),
          "cleared background produces finite HDR");
    CHECK(close4(background.hdr, {0.0f, 0.0f, 0.0f, 1.0f}, 2e-3f),
          "cleared background produces deterministic black HDR");
    CHECK(close3(center.visibility, {1.0f, 1.0f, 1.0f}, 1e-6f) &&
              close3(background.visibility, {1.0f, 1.0f, 1.0f}, 1e-6f),
          "disabled RT GPU clear writes full visibility");
    CHECK(center.hdr.x > background.hdr.x &&
              center.hdr.y > background.hdr.y &&
              center.hdr.z > background.hdr.z,
          "composite samples G-buffer into HDR output");

    const viewer::VkSceneUploadCounters before_material_update =
        renderer.upload_counters();
    materials[7].absorption_pad[0] = 0.875f;
    CHECK(renderer.update_materials(materials, 2, 1, error),
          error.empty() ? "update shading-only material revision"
                        : error.c_str());
    CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error),
          error.empty() ? "upload shading-only material revision"
                        : error.c_str());
    std::vector<MaterialGpuRecord> uploaded_materials;
    CHECK(renderer.readback_materials(uploaded_materials, error) &&
              uploaded_materials.size() == materials.size() &&
              uploaded_materials[7].absorption_pad[0] == 0.875f,
          error.empty() ? "shared material buffer changes in place"
                        : error.c_str());
    CHECK(renderer.upload_counters().vertex_uploads ==
              before_material_update.vertex_uploads,
          "shading-only material update skips part geometry upload");
    CHECK(renderer.consume_gi_history_reset(),
          "shading-only material update requests one GI history reset");
    CHECK(!renderer.consume_gi_history_reset(),
          "GI history reset request is one-shot");

    materials[7].flags_misc[0] |= MATERIAL_ALPHA_TESTED;
    CHECK(renderer.update_materials(materials, 2, 2, error),
          error.empty() ? "update geometry material revision"
                        : error.c_str());
    CHECK(renderer.rt_geometry_classification_dirty(901),
          "classification-changing material revision dirties affected RT part");
    CHECK(!renderer.rt_geometry_classification_dirty(902),
          "classification-changing revision leaves other material parts clean");
    CHECK(renderer.consume_gi_history_reset(),
          "geometry material revision requests GI history reset");
    materials[8].transmission[0] = 0.5f;
    CHECK(renderer.update_materials(materials, 3, 2, error),
          error.empty() ? "update shading revision across RT classification"
                        : error.c_str());
    CHECK(renderer.rt_geometry_classification_dirty(902),
          "shading revision defensively dirties a classification crossing");
    renderer.release_part(902);

    viewer::TemporalFrame rigid_motion{};
    rigid_motion.current_jittered = frame;
    rigid_motion.previous_jittered = frame;
    rigid_motion.internal_extent = {width, height};
    rigid_motion.reset = false;
    rigid_motion.attempt_token = 1;
    rigid_motion.instances = {
        {1, identity, identity, true},
        {2, viewer::mat4_translation({0.2f, 0.2f, 0.0f}), identity, true}};
    renderer.set_temporal_frame(rigid_motion);
    const bool rigid_updated = renderer.update_instances(
        {{900, identity, 1},
         {901, viewer::mat4_translation({0.2f, 0.2f, 0.0f}), 2}},
        error);
    const bool rigid_dispatched =
        rigid_updated &&
        renderer.dispatch_culling(frame, camera.position, 1.0f, error);
    CHECK(rigid_dispatched &&
              renderer.render_gbuffer_and_composite(width, height, error),
          error.empty() ? "render exact rigid velocity" : error.c_str());
    viewer::VkRasterPixel moving_center{};
    CHECK(renderer.readback_raster_pixel(width / 2, height / 2,
                                         moving_center, error),
          error.empty() ? "read exact rigid velocity" : error.c_str());
    const matter::Float3 expected_velocity =
        viewer::temporal_velocity_pixels(rigid_motion, 2, {0.0f, 0.0f, -2.0f});
    CHECK(std::fabs(expected_velocity.x - 8.0f) < 1e-5f &&
              std::fabs(expected_velocity.y + 8.0f) < 1e-5f &&
              std::fabs(moving_center.velocity.x - expected_velocity.x) < 0.02f &&
              std::fabs(moving_center.velocity.y - expected_velocity.y) < 0.002f,
          "velocity attachment stores exact current-to-previous input pixels");

    const VkPipelineStageFlags2 rt_compute_fragment =
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    CHECK(viewer::vk_scene_detail::gbuffer_sampled_stages_for_test(1, true) ==
              rt_compute_fragment &&
              viewer::vk_scene_detail::gbuffer_sampled_stages_for_test(4, true) ==
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          "normal and identity producers synchronize with temporal compute and RT consumers");

    viewer::TemporalFrame restored_temporal = rigid_motion;
    restored_temporal.instances = {{1, identity, identity, true},
                                   {2, identity, identity, true}};
    renderer.set_temporal_frame(restored_temporal);
    CHECK(renderer.update_instances({{900, identity, 1}, {901, identity, 2}},
                                    error) &&
              renderer.dispatch_culling(frame, camera.position, 1.0f, error),
          error.empty() ? "restore static temporal raster scene"
                        : error.c_str());

    viewer::VkSceneLighting dark{};
    dark.sun_intensity = 0.0f;
    dark.sky_color = {0.0f, 0.0f, 0.0f};
    renderer.set_lighting(dark);
    CHECK(renderer.render_gbuffer_and_composite(width, height, error),
          error.empty() ? "render authored dark lighting" : error.c_str());
    viewer::VkRasterPixel dark_center{};
    CHECK(renderer.readback_raster_pixel(width / 2, height / 2, dark_center,
                                         error),
          error.empty() ? "read authored dark lighting" : error.c_str());
    CHECK(std::isfinite(dark_center.hdr.x) &&
              std::fabs(dark_center.hdr.x - 1.25f) < 0.04f &&
              std::fabs(dark_center.hdr.y - 2.50f) < 0.05f &&
              std::fabs(dark_center.hdr.z - 3.75f) < 0.06f,
          "material emission 5 survives UNORM G-buffer and HDR composite");

    materials[7].emission_strength[3] = 1000.0f;
    CHECK(renderer.update_materials(materials, 4, 2, error) &&
              renderer.dispatch_culling(frame, camera.position, 1.0f, error) &&
              renderer.render_gbuffer_and_composite(width, height, error),
          error.empty() ? "render emission 1000" : error.c_str());
    viewer::VkRasterPixel thousand_center{};
    CHECK(renderer.readback_raster_pixel(width / 2, height / 2,
                                         thousand_center, error),
          error.empty() ? "read emission 1000" : error.c_str());
    CHECK(std::isfinite(thousand_center.hdr.x) &&
              thousand_center.hdr.x > dark_center.hdr.x * 100.0f,
          "GPU composite keeps emission 1000 finite and above emission 5");

    materials[7].emission_strength[3] =
        std::numeric_limits<float>::max();
    CHECK(renderer.update_materials(materials, 5, 2, error) &&
              renderer.dispatch_culling(frame, camera.position, 1.0f, error) &&
              renderer.render_gbuffer_and_composite(width, height, error),
          error.empty() ? "render FLT_MAX emission" : error.c_str());
    viewer::VkRasterPixel max_center{};
    CHECK(renderer.readback_raster_pixel(width / 2, height / 2, max_center,
                                         error),
          error.empty() ? "read FLT_MAX emission" : error.c_str());
    CHECK(std::isfinite(max_center.hdr.x) &&
              max_center.hdr.x > thousand_center.hdr.x &&
              max_center.hdr.x > 14000.0f &&
              max_center.hdr.x < 16000.0f,
          "GPU composite saturates FLT_MAX emission finite, strictly "
          "monotonic, and in the encoded saturation band");
    std::printf("emission HDR: five=%.5f thousand=%.5f max=%.5f\n",
                dark_center.hdr.x, thousand_center.hdr.x, max_center.hdr.x);

    materials[7].emission_strength[3] = 5.0f;
    CHECK(renderer.update_materials(materials, 6, 2, error) &&
              renderer.dispatch_culling(frame, camera.position, 1.0f, error),
          error.empty() ? "restore emission 5 before authored bright sky"
                        : error.c_str());
    viewer::VkSceneLighting bright = dark;
    bright.sky_color = {2.0f, 2.0f, 2.0f};
    renderer.set_lighting(bright);
    CHECK(renderer.render_gbuffer_and_composite(width, height, error),
          error.empty() ? "render authored bright sky" : error.c_str());
    viewer::VkRasterPixel bright_center{};
    CHECK(renderer.readback_raster_pixel(width / 2, height / 2, bright_center,
                                         error),
          error.empty() ? "read authored bright sky" : error.c_str());
    CHECK(close4(bright_center.albedo, dark_center.albedo, 1e-5f) &&
              close4(bright_center.normal, dark_center.normal, 1e-5f) &&
              close4(bright_center.orm, dark_center.orm, 1e-5f) &&
              std::fabs(bright_center.depth - dark_center.depth) < 1e-6f,
          "dark and bright sky samples keep identical G-buffer inputs");
    CHECK(bright_center.hdr.x > dark_center.hdr.x &&
              bright_center.hdr.y > dark_center.hdr.y &&
              bright_center.hdr.z > dark_center.hdr.z,
          "authored world sky lighting changes raster pixels");

    const VkImage old_albedo = attachments.albedo.image;
    const VkDeviceSize initial_vertex_capacity =
        renderer.raster_vertex_buffer_size();
    renderer.release_part(901);
    CHECK(renderer.raster_vertex_count() == 0,
          "releasing a raster part reclaims its vertices");
    CHECK(renderer.uploaded_raster_draw_command_count() == 1,
          "staging release preserves the last uploaded raster mask");
    for (uint64_t hash = 902; hash <= 906; ++hash) {
        CHECK(renderer.ensure_part(known_raster_triangle(hash), error) >= 0 &&
                  renderer.raster_vertex_count() == 3,
              error.empty()
                  ? "re-adding raster part reuses compact vertex storage"
                  : error.c_str());
        CHECK(renderer.update_instances({{900, identity}, {hash, identity}},
                                        error) &&
                  renderer.dispatch_culling(frame, camera.position, 1.0f,
                                             error),
              error.empty() ? "dispatch re-added raster part"
                            : error.c_str());
        CHECK(renderer.raster_vertex_buffer_size() == initial_vertex_capacity,
              "streaming eviction/reload keeps raster vertex residency bounded");
        if (hash != 906) {
            renderer.release_part(hash);
            CHECK(renderer.raster_vertex_count() == 0,
                  "streaming eviction releases raster vertex residency");
        }
    }
    CHECK(renderer.render_gbuffer_and_composite(96, 64, error),
          error.empty() ? "recreate resized raster attachments"
                        : error.c_str());
    const viewer::VkRasterAttachments resized = renderer.raster_attachments();
    CHECK(resized.extent.width == 96 && resized.extent.height == 64,
          "raster attachments resize");
    CHECK(resized.albedo.image != VK_NULL_HANDLE &&
              resized.albedo.image != old_albedo,
          "raster resize recreates attachments");
    viewer::VkRasterPixel resized_center{};
    CHECK(renderer.readback_raster_pixel(48, 32, resized_center, error) &&
              resized_center.albedo.w > 0.99f,
          error.empty() ? "resized raster attachment contains geometry"
                        : error.c_str());

    std::printf(
        "raster center: albedo=%.5f %.5f %.5f normal=%.5f %.5f %.5f "
        "orm=%.5f %.5f %.5f depth=%.6f hdr=%.5f %.5f %.5f\n",
        center.albedo.x, center.albedo.y, center.albedo.z, center.normal.x,
        center.normal.y, center.normal.z, center.orm.x, center.orm.y,
        center.orm.z, center.depth, center.hdr.x, center.hdr.y, center.hdr.z);
    std::printf("raster background: albedo=%.5f %.5f %.5f depth=%.6f "
                "hdr=%.5f %.5f %.5f\n",
                background.albedo.x, background.albedo.y,
                background.albedo.z, background.depth, background.hdr.x,
                background.hdr.y, background.hdr.z);
    matter::VulkanRayTracingSettings disabled_rt{};
    disabled_rt.enabled = false;
    renderer.set_ray_tracing_settings(disabled_rt);
    matter::VulkanFrame acquired{};
    CHECK(vulkan.begin_frame(acquired, error),
          error.empty() ? "begin RT-disabled production frame" : error.c_str());
    if (acquired.command_buffer != VK_NULL_HANDLE) {
        CHECK(renderer.prepare_frame(acquired, frame, camera.position, 1.0f,
                                     error) &&
                  renderer.record_cull_and_render(
                      acquired, frame, camera.position, 1.0f, error) &&
                  renderer.record_composite_to_swapchain(acquired, error),
              error.empty() ? "record RT-disabled production frame"
                            : error.c_str());
        CHECK(!renderer.rt_effective_observed() &&
                  renderer.rt_trace_dispatches_observed() == 0 &&
                  renderer.rt_fallback_reason_observed() ==
                      "disabled by render options",
              "RT-disabled production frame observes no dispatch and its reason");
        CHECK(vulkan.end_frame(acquired, error),
              error.empty() ? "submit RT-disabled production frame"
                            : error.c_str());
    }
}

void run_native_ray_tracing_path(matter::VulkanDevice& vulkan) {
    CHECK(vulkan.ray_tracing_available(),
          vulkan.ray_tracing_unavailable_reason().empty()
              ? "native ray tracing available"
              : vulkan.ray_tracing_unavailable_reason().c_str());
    if (!vulkan.ray_tracing_available()) return;
    const auto& properties = vulkan.ray_tracing_properties();
    CHECK(properties.shader_group_handle_alignment != 0 &&
              properties.shader_group_base_alignment != 0 &&
              properties.shader_group_handle_size != 0 &&
              properties.shader_group_base_alignment >=
                  properties.shader_group_handle_alignment &&
              properties.max_shader_group_stride != 0 &&
              properties.max_ray_dispatch_invocation_count >= 320u * 200u,
          "queried SBT handle and base alignments are retained");

    std::string error;
    {
        viewer::VkSceneRenderer surface_query(vulkan);
        viewer::VkScenePart first = known_raster_triangle(912);
        viewer::VkScenePart second = fixed_part(
            913, {-1.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0);
        const matter::Float3 local_normal{0.70710678f, 0.0f, 0.70710678f};
        second.vertices = {
            {{-1.0f, 0.0f, 1.0f}, local_normal, {0.2f, 0.4f, 0.6f, 1.0f},
             {0.0f, 0.0f, 0.2f, 1.0f}, 7},
            {{1.0f, 0.0f, -1.0f}, local_normal, {0.4f, 0.6f, 0.8f, 1.0f},
             {1.0f, 0.0f, 0.5f, 1.0f}, 7},
            {{0.0f, 1.0f, 0.0f}, local_normal, {0.6f, 0.8f, 1.0f, 1.0f},
             {0.5f, 1.0f, 0.8f, 1.0f}, 7}};
        matter::Mat4f first_transform =
            viewer::mat4_translation({-2.0f, 0.0f, -3.0f});
        matter::Mat4f second_transform = identity_matrix();
        second_transform.m[0] = 2.0f;
        second_transform.m[10] = 0.5f;
        second_transform.m[3] = 2.0f;
        second_transform.m[11] = -3.0f;
        const int slot0 = surface_query.ensure_part(first, error);
        const int slot1 = surface_query.ensure_part(second, error);
        CHECK(slot0 >= 0 && slot1 >= 0 &&
                  surface_query.update_instances(
                      {{912, first_transform}, {913, second_transform}}, error),
              error.empty() ? "prepare secondary surface-query fixture"
                            : error.c_str());
        const matter::Float3 expected_world_normal{
            0.24253563f, 0.0f, 0.97014250f};
        matter::CameraDesc query_camera{};
        query_camera.position = {0.0f, 0.5f, 1.0f};
        query_camera.target = {0.0f, 0.5f, -3.0f};
        query_camera.up = {0.0f, 1.0f, 0.0f};
        query_camera.vertical_fov_radians = 1.0f;
        query_camera.near_plane = 0.1f;
        query_camera.far_plane = 20.0f;
        viewer::FrameMatrices query_matrices{};
        CHECK(viewer::build_frame_matrices(query_camera, 64, 64,
                                           query_matrices, error),
              error.empty() ? "build surface-query matrices" : error.c_str());
        matter::VulkanRayTracingSettings query_settings{};
        query_settings.enabled = true;
        query_settings.max_distance = 100.0f;
        surface_query.set_ray_tracing_settings(query_settings);
        matter::VulkanGiSettings disabled_query_gi{};
        disabled_query_gi.enabled = 0;
        surface_query.set_gi_settings(disabled_query_gi);
        const auto trace_surface = [&](matter::Float3 origin,
                                       matter::Float3 direction,
                                       uint32_t invalid_part_slot,
                                       viewer::RtSurfaceHit& hit,
                                       uint32_t& invalid_count) {
            matter::VulkanFrame query_frame{};
            if (!vulkan.begin_frame(query_frame, error)) return false;
            const bool recorded = surface_query.prepare_frame(
                                      query_frame, query_matrices,
                                      query_camera.position, 1.0f, error) &&
                                  surface_query.record_cull_and_render(
                                      query_frame, query_matrices,
                                      query_camera.position, 1.0f, error) &&
                                  surface_query.record_composite_to_swapchain(
                                      query_frame, error) &&
                                  surface_query.record_test_surface_ray(
                                      query_frame, origin, direction,
                                      invalid_part_slot, error);
            const bool submitted = recorded && vulkan.end_frame(query_frame, error);
            surface_query.finish_ray_tracing_frame(query_frame.serial,
                                                    submitted);
            if (!submitted) return false;
            vulkan.wait_idle();
            return surface_query.readback_test_surface_hit(
                query_frame.frame_slot, hit, invalid_count, error);
        };
        viewer::RtSurfaceHit hit0{};
        viewer::RtSurfaceHit hit1{};
        uint32_t invalid_count0 = UINT32_MAX;
        uint32_t invalid_count1 = UINT32_MAX;
        CHECK(trace_surface({-2.0f, 0.25f, 0.0f}, {0.0f, 0.0f, -1.0f},
                            UINT32_MAX, hit0, invalid_count0) &&
                  trace_surface(
                      {2.0f + expected_world_normal.x * 3.0f,
                       1.0f / 3.0f,
                       -3.0f + expected_world_normal.z * 3.0f},
                      {-expected_world_normal.x, 0.0f,
                       -expected_world_normal.z},
                      UINT32_MAX, hit1, invalid_count1),
              error.empty() ? "trace GPU secondary surface-query rays"
                            : error.c_str());
        CHECK(surface_query.test_rt_miss_region_size() ==
                      2 * surface_query.test_rt_sbt_stride() &&
                  surface_query.test_rt_hit_region_size() ==
                      2 * surface_query.test_rt_sbt_stride() &&
                  surface_query.test_rt_sbt_address() %
                          properties.shader_group_base_alignment ==
                      0 &&
                  surface_query.test_rt_test_raygen_address() %
                          properties.shader_group_base_alignment ==
                      0 &&
                  surface_query.test_rt_miss_address() %
                          properties.shader_group_base_alignment ==
                      0 &&
                  surface_query.test_rt_hit_address() %
                          properties.shader_group_base_alignment ==
                      0,
              "shadow and radiance SBT records occupy aligned category regions");
        CHECK(surface_query.test_surface_trace_dispatches() == 2,
              "test raygen dispatches radiance miss and surface hit index one");
        CHECK(hit0.valid && hit0.part_slot == static_cast<uint32_t>(slot0) &&
                  hit0.primitive == 0,
              "first ray identifies part and primitive");
        CHECK(hit1.valid && hit1.material_index == 7,
              "second ray fetches material from pinned geometry");
        CHECK(std::fabs(hit1.normal.x - expected_world_normal.x) < 1e-4f &&
                  std::fabs(hit1.normal.y - expected_world_normal.y) < 1e-4f &&
                  std::fabs(hit1.normal.z - expected_world_normal.z) < 1e-4f,
              "inverse-transpose normal is correct");
        CHECK(std::fabs(hit1.baked_ao - 0.5f) < 1e-4f,
              "secondary barycentric AO matches raster data");
        CHECK(close4(hit1.tint, {0.4f, 0.6f, 0.8f, 1.0f}, 1e-4f) &&
                  std::fabs(hit1.uv[0] - 0.5f) < 1e-4f &&
                  std::fabs(hit1.uv[1] - 1.0f / 3.0f) < 1e-4f &&
                  (hit1.flags & viewer::kRtSurfaceFrontFace) != 0 &&
                  invalid_count0 == 0 && invalid_count1 == 0,
              "GPU surface query returns tint UV front-face and clean counter");
        viewer::RtSurfaceHit miss_hit{};
        uint32_t miss_invalid_count = UINT32_MAX;
        CHECK(trace_surface({0.0f, 10.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                            UINT32_MAX, miss_hit, miss_invalid_count) &&
                  !miss_hit.valid && miss_hit.part_slot == UINT32_MAX &&
                  miss_invalid_count == 0 &&
                  close4(miss_hit.tint, {1.0f, 0.0f, 1.0f, 1.0f}, 1e-6f),
              error.empty() ? "radiance miss index one returns invalid surface"
                            : error.c_str());
        viewer::RtSurfaceHit invalid_hit{};
        uint32_t invalid_count = 0;
        CHECK(trace_surface(
                  {2.0f + expected_world_normal.x * 3.0f, 1.0f / 3.0f,
                   -3.0f + expected_world_normal.z * 3.0f},
                  {-expected_world_normal.x, 0.0f,
                   -expected_world_normal.z},
                  static_cast<uint32_t>(slot1), invalid_hit, invalid_count) &&
                  !invalid_hit.valid && invalid_count == 1 &&
                  surface_query.test_surface_trace_dispatches() == 4 &&
                  close4(invalid_hit.tint, {1.0f, 0.0f, 1.0f, 1.0f}, 1e-6f),
              error.empty() ? "invalid GPU part record reports debug surface"
                            : error.c_str());
        surface_query.release_part(912);
        CHECK(surface_query.ensure_part(second, error) == slot1,
              "live RT part slot remains stable while an earlier slot retires");
    }
    {
        viewer::VkSceneRenderer pinning(vulkan);
        const viewer::VkScenePart receiver = known_raster_triangle(910);
        CHECK(pinning.ensure_part(receiver, error) >= 0,
              error.empty() ? "build receiver BLAS" : error.c_str());
        const VkDeviceAddress pinned = pinning.test_rt_geometry_address(910);
        viewer::VkScenePart growth = known_raster_triangle(911);
        growth.vertices.reserve(3 * 4096);
        for (uint32_t i = 1; i < 4096; ++i) {
            const auto triangle = known_raster_triangle(911 + i).vertices;
            growth.vertices.insert(growth.vertices.end(), triangle.begin(),
                                   triangle.end());
        }
        CHECK(pinning.ensure_part(growth, error) >= 0 && pinned != 0 &&
                  pinning.test_rt_geometry_address(910) == pinned,
              error.empty()
                  ? "BLAS input geometry stays pinned across raster growth"
                  : error.c_str());
    }

    {
        const auto aligned_triangle = [](uint64_t hash, float z,
                                         uint32_t material_index) {
            viewer::VkScenePart part = known_raster_triangle(hash,
                                                              material_index);
            for (auto& vertex : part.vertices) vertex.position.z = z;
            return part;
        };
        struct VisibilityResult {
            matter::Float3 visibility{-1.0f, -1.0f, -1.0f};
            matter::Float3 composite{-1.0f, -1.0f, -1.0f};
            viewer::RtTraceCounters counters{};
            VkFormat format = VK_FORMAT_UNDEFINED;
        };
        const auto trace_visibility = [&](uint64_t hash_base,
                                          MaterialGpuRecord blocker,
                                          uint32_t layer_count,
                                          bool test_reclassification) {
            VisibilityResult result{};
            viewer::VkSceneRenderer visibility(vulkan);
            std::vector<MaterialGpuRecord> materials(2);
            materials[0].metal_opacity_spec_coat[1] = 1.0f;
            materials[0].scattering_shape[3] = 1.0f;
            materials[1] = blocker;
            if (!visibility.update_materials(materials, 1, 1, error) ||
                visibility.ensure_part(
                    aligned_triangle(hash_base, -2.0f, 0), error) < 0)
                return result;
            std::vector<viewer::VkSceneInstance> instances{
                {hash_base, identity_matrix()}};
            for (uint32_t layer = 0; layer < layer_count; ++layer) {
                const uint64_t hash = hash_base + layer + 1;
                if (visibility.ensure_part(aligned_triangle(
                        hash, -3.0f - static_cast<float>(layer), 1),
                        error) < 0)
                    return result;
                instances.push_back({hash, identity_matrix()});
            }
            if (!visibility.update_instances(instances, error)) return result;

            matter::VulkanRayTracingSettings settings{};
            settings.enabled = true;
            settings.max_distance = 100.0f;
            settings.bias = 0.001f;
            settings.debug_view = true;
            visibility.set_ray_tracing_settings(settings);
            viewer::VkSceneLighting lighting{};
            lighting.sun_direction = {0.0f, 0.0f, 1.0f};
            visibility.set_lighting(lighting);
            matter::CameraDesc camera{};
            camera.position = {0.0f, 0.0f, 0.0f};
            camera.target = {0.0f, 0.0f, -1.0f};
            camera.up = {0.0f, 1.0f, 0.0f};
            camera.vertical_fov_radians = 1.0f;
            camera.near_plane = 0.1f;
            camera.far_plane = 20.0f;
            viewer::FrameMatrices matrices{};
            if (!viewer::build_frame_matrices(camera, 320, 200, matrices,
                                               error))
                return result;
            matter::VulkanFrame frame{};
            const bool began = vulkan.begin_frame(frame, error);
            const bool recorded =
                began && visibility.prepare_frame(frame, matrices,
                                                   camera.position, 1.0f,
                                                   error) &&
                visibility.record_cull_and_render(
                    frame, matrices, camera.position, 1.0f, error) &&
                visibility.record_composite_to_swapchain(frame, error);
            const bool submitted = recorded && vulkan.end_frame(frame, error);
            visibility.finish_ray_tracing_frame(frame.serial, submitted);
            if (!submitted) return result;
            viewer::VkRasterPixel center{};
            if (!visibility.readback_raster_pixel(160, 100, center, error))
                return result;
            result.visibility = center.visibility;
            result.composite = {center.hdr.x, center.hdr.y, center.hdr.z};
            result.format = visibility.test_visibility_format();
            if (!visibility.readback_rt_trace_counters(
                    frame.frame_slot, result.counters, error))
                return VisibilityResult{};
            if (test_reclassification) {
                const uint32_t original_slot = frame.frame_slot;
                const std::weak_ptr<void> old_blas =
                    visibility.test_rt_blas_lifetime(hash_base + 1);
                materials[1].metal_opacity_spec_coat[1] = 0.25f;
                materials[1].scattering_shape[2] = 0.5f;
                materials[1].flags_misc[0] = MATERIAL_ALPHA_TESTED;
                if (!visibility.update_materials(materials, 1, 2, error))
                    return VisibilityResult{};
                CHECK(visibility.rt_geometry_classification_dirty(
                          hash_base + 1) &&
                          !visibility.rt_geometry_classification_dirty(
                              hash_base),
                      "geometry revision dirties only the reclassified BLAS");
                vulkan.test_clear_presentation_events();
                const uint64_t immediate_before =
                    matter::immediate_submit_count();
                matter::VulkanFrame rebuild_frame{};
                const bool rebuild_began =
                    vulkan.begin_frame(rebuild_frame, error);
                const bool rebuild_recorded =
                    rebuild_began &&
                    visibility.prepare_frame(rebuild_frame, matrices,
                                             camera.position, 1.0f, error) &&
                    visibility.record_cull_and_render(
                        rebuild_frame, matrices, camera.position, 1.0f,
                        error) &&
                    visibility.record_composite_to_swapchain(rebuild_frame,
                                                             error);
                const bool rebuild_submitted =
                    rebuild_recorded && vulkan.end_frame(rebuild_frame, error);
                visibility.finish_ray_tracing_frame(rebuild_frame.serial,
                                                    rebuild_submitted);
                if (!rebuild_submitted) return VisibilityResult{};
                const auto& replacement_events =
                    vulkan.test_presentation_events();
                CHECK(std::find(replacement_events.begin(),
                                replacement_events.end(),
                                "device_wait_idle") ==
                          replacement_events.end() &&
                          matter::immediate_submit_count() == immediate_before,
                      "BLAS replacement records without global wait or immediate submit");
                CHECK(!old_blas.expired(),
                      "published replacement retains old BLAS until its frame slot completes");
                viewer::VkRasterPixel rebuilt_center{};
                CHECK(visibility.readback_raster_pixel(
                          160, 100, rebuilt_center, error) &&
                          close3(rebuilt_center.visibility,
                                 {1.0f, 1.0f, 1.0f}, 1e-6f) &&
                          !visibility.rt_geometry_classification_dirty(
                              hash_base + 1),
                      "classification rebuild routes the affected BLAS through any-hit");
                matter::VulkanFrame recycle{};
                do {
                    CHECK(vulkan.begin_frame(recycle, error),
                          error.empty() ? "begin BLAS lifetime recycle frame"
                                        : error.c_str());
                    if (recycle.command_buffer == VK_NULL_HANDLE)
                        return VisibilityResult{};
                    if (recycle.frame_slot == original_slot)
                        CHECK(old_blas.expired(),
                              "completed frame slot releases replaced BLAS lifetime");
                    CHECK(visibility.prepare_frame(
                              recycle, matrices, camera.position, 1.0f,
                              error) &&
                              visibility.record_cull_and_render(
                                  recycle, matrices, camera.position, 1.0f,
                                  error) &&
                              visibility.record_composite_to_swapchain(recycle,
                                                                     error) &&
                              vulkan.end_frame(recycle, error),
                          error.empty() ? "submit BLAS lifetime recycle frame"
                                        : error.c_str());
                    visibility.finish_ray_tracing_frame(recycle.serial, true);
                } while (recycle.frame_slot != original_slot);
            }
            return result;
        };

        MaterialGpuRecord opaque{};
        opaque.metal_opacity_spec_coat[1] = 1.0f;
        opaque.scattering_shape[3] = 1.0f;
        MaterialGpuRecord cutout = opaque;
        cutout.metal_opacity_spec_coat[1] = 0.25f;
        cutout.scattering_shape[2] = 0.5f;
        cutout.flags_misc[0] = MATERIAL_ALPHA_TESTED;
        MaterialGpuRecord glass = opaque;
        glass.base_roughness[0] = 1.0f;
        glass.base_roughness[1] = 1.0f;
        glass.base_roughness[2] = 1.0f;
        glass.transmission[0] = 0.5f;
        MaterialGpuRecord colored_glass = opaque;
        colored_glass.base_roughness[0] = 0.8f;
        colored_glass.base_roughness[1] = 0.4f;
        colored_glass.base_roughness[2] = 0.2f;
        colored_glass.transmission[0] = 1.0f;
        MaterialGpuRecord cap_glass = opaque;
        cap_glass.base_roughness[0] = 1.0f;
        cap_glass.base_roughness[1] = 1.0f;
        cap_glass.base_roughness[2] = 1.0f;
        cap_glass.transmission[0] = 1.0f;
        const VisibilityResult opaque_visibility =
            trace_visibility(940, opaque, 1, true);
        const VisibilityResult cutout_visibility =
            trace_visibility(950, cutout, 1, false);
        const VisibilityResult glass_visibility =
            trace_visibility(960, glass, 2, false);
        const VisibilityResult colored_visibility =
            trace_visibility(970, colored_glass, 1, false);
        const VisibilityResult capped_visibility =
            trace_visibility(980, cap_glass, 32, false);
        std::printf("aligned visibility: opaque=%.5f cutout=%.5f "
                    "glass=%.5f colored=%.5f/%.5f/%.5f\n",
                    opaque_visibility.visibility.x,
                    cutout_visibility.visibility.x,
                    glass_visibility.visibility.x,
                    colored_visibility.visibility.x,
                    colored_visibility.visibility.y,
                    colored_visibility.visibility.z);
        CHECK(close3(opaque_visibility.visibility, {0.0f, 0.0f, 0.0f},
                     1e-6f) &&
                  opaque_visibility.counters.any_hit_invocations == 0,
              "opaque aligned layer terminates visibility");
        CHECK(close3(cutout_visibility.visibility, {1.0f, 1.0f, 1.0f},
                     1e-6f) &&
                  cutout_visibility.counters.any_hit_invocations > 0,
              "alpha-cutout layer below cutoff preserves visibility");
        CHECK(close3(glass_visibility.visibility, {0.25f, 0.25f, 0.25f},
                     0.02f) &&
                  glass_visibility.counters.any_hit_layers > 0,
              "two half-shadow glass layers retain quarter visibility");
        CHECK(close3(colored_visibility.visibility, {0.8f, 0.4f, 0.2f},
                     0.01f) &&
                  close3(colored_visibility.composite,
                         {0.8f, 0.4f, 0.2f}, 0.01f),
              "colored transmission preserves unequal RGB visibility");
        CHECK(capped_visibility.counters.capped_rays > 0 &&
                  capped_visibility.counters.any_hit_layers >= 32,
              "pathological transparent stack terminates at 32 layers");
        CHECK(opaque_visibility.format ==
                  VK_FORMAT_R16G16B16A16_SFLOAT,
              "visibility target preserves RGB in a float format");
    }

    viewer::VkSceneRenderer renderer(vulkan);
    const auto horizontal = [](uint64_t hash, float y, float radius,
                               matter::Float3 normal, uint32_t material_index,
                               float baked_ao) {
        const float front_z = radius > 5.0f ? 10.0f : -1.0f;
        const float back_z = radius > 5.0f ? -30.0f : -3.5f;
        viewer::VkScenePart part = fixed_part(
            hash, {-radius, y - 0.01f, back_z},
            {radius, y + 0.01f, front_z}, 0);
        const matter::Float4 albedo{1.0f, 1.0f, 1.0f, 0.0f};
        const matter::Float4 orm{0.5f, 0.0f, baked_ao, 1.0f};
        part.vertices = {{{-radius, y, front_z}, normal, albedo, orm,
                          material_index, {}},
                         {{radius, y, front_z}, normal, albedo, orm,
                          material_index, {}},
                         {{0.0f, y, back_z}, normal, albedo, orm,
                          material_index, {}}};
        return part;
    };
    std::vector<MaterialGpuRecord> gi_materials(2);
    gi_materials[0].base_roughness[0] = 1.0f;
    gi_materials[0].base_roughness[1] = 0.02f;
    gi_materials[0].base_roughness[2] = 0.02f;
    gi_materials[0].metal_opacity_spec_coat[1] = 1.0f;
    gi_materials[0].scattering_shape[3] = 1.0f;
    gi_materials[1].base_roughness[0] = 1.0f;
    gi_materials[1].base_roughness[1] = 1.0f;
    gi_materials[1].base_roughness[2] = 1.0f;
    gi_materials[1].base_roughness[3] = 0.02f;
    gi_materials[1].metal_opacity_spec_coat[0] = 0.0f;
    gi_materials[1].metal_opacity_spec_coat[1] = 1.0f;
    gi_materials[1].metal_opacity_spec_coat[2] = 1.0f;
    gi_materials[1].specular_tint_coat_roughness[0] = 1.0f;
    gi_materials[1].specular_tint_coat_roughness[1] = 1.0f;
    gi_materials[1].specular_tint_coat_roughness[2] = 1.0f;
    gi_materials[1].specular_tint_coat_roughness[3] = 0.08f;
    gi_materials[1].scattering_shape[3] = 1.0f;
    CHECK(renderer.update_materials(gi_materials, 1, 1, error) &&
              renderer.ensure_part(horizontal(920, -1.0f, 20.0f,
                                              {0.0f, 1.0f, 0.0f}, 0, 1.0f),
                                   error) >= 0 &&
              renderer.ensure_part(horizontal(921, 0.0f, 0.55f,
                                              {0.0f, -1.0f, 0.0f}, 1, 1.0f),
                                   error) >= 0 &&
              renderer.update_instances(
                  {{920, identity_matrix()}, {921, identity_matrix()}},
                  error),
          error.empty() ? "prepare native RT two-triangle fixture"
                        : error.c_str());

    matter::VulkanRayTracingSettings disabled{};
    disabled.enabled = false;
    renderer.set_ray_tracing_settings(disabled);
    CHECK(renderer.test_shadow_visibility_for_ray(false) == 1.0f &&
              renderer.test_shadow_visibility_for_ray(true) == 1.0f,
          "disabled ray tracing deterministically produces full visibility");
    matter::VulkanRayTracingSettings enabled{};
    enabled.enabled = true;
    enabled.max_distance = 100.0f;
    enabled.bias = 0.001f;
    enabled.samples = 4;
    enabled.debug_view = true;
    renderer.set_ray_tracing_settings(enabled);
    matter::VulkanGiSettings gi{};
    gi.samples_per_pixel = 16;
    gi.trace_scale = 0.5f;
    renderer.set_gi_settings(gi);
    CHECK(renderer.test_gi_samples_per_pixel() == 1u,
          "RT-active GI clamps authored sample count to one continuation");
    viewer::VkSceneLighting lighting{};
    lighting.sun_direction = {0.0f, -1.0f, 0.0f};
    renderer.set_lighting(lighting);
    const float open = renderer.test_shadow_visibility_for_ray(false);
    const float blocked = renderer.test_shadow_visibility_for_ray(true);
    CHECK(open == 1.0f && std::isfinite(blocked) && blocked < 1.0f,
          "two-triangle shadow contract is deterministic for open and blocked rays");

    matter::CameraDesc camera{};
    camera.position = {0.0f, 1.5f, 1.0f};
    camera.target = {0.0f, -0.75f, -2.2f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices matrices{};
    CHECK(viewer::build_frame_matrices(camera, 320, 200, matrices, error),
          error.empty() ? "build native RT frame matrices" : error.c_str());
    viewer::TemporalFrame gi_temporal{};
    gi_temporal.current_unjittered = matrices;
    gi_temporal.previous_unjittered = matrices;
    gi_temporal.current_jittered = matrices;
    gi_temporal.previous_jittered = matrices;
    gi_temporal.internal_extent = {320, 200};
    gi_temporal.output_extent = {320, 200};
    gi_temporal.attempt_token = 101;
    gi_temporal.presented_frame_index = 7;
    renderer.set_temporal_frame(gi_temporal);
    const uint64_t immediate_before = matter::immediate_submit_count();
    matter::VulkanFrame frame{};
    CHECK(vulkan.begin_frame(frame, error),
          error.empty() ? "begin native RT frame" : error.c_str());
    if (frame.command_buffer != VK_NULL_HANDLE) {
        CHECK(renderer.prepare_frame(frame, matrices, camera.position, 1.0f,
                                     error) &&
                  renderer.record_cull_and_render(
                      frame, matrices, camera.position, 1.0f, error) &&
                  renderer.record_composite_to_swapchain(frame, error),
              error.empty() ? "record BLAS TLAS native shadow trace"
                            : error.c_str());
        CHECK(!renderer.test_rt_blas_built(920) &&
                  renderer.test_rt_blas_candidate_serial(920) == frame.serial,
              "BLAS build stays candidate-only until frame success");
        CHECK(renderer.test_rt_sbt_address() %
                      properties.shader_group_base_alignment == 0 &&
                  renderer.test_rt_sbt_stride() <=
                      properties.max_shader_group_stride,
              "SBT device address and stride obey queried limits");
        CHECK(renderer.test_rt_scratch_address(frame.frame_slot) %
                      properties
                          .min_acceleration_structure_scratch_offset_alignment ==
                  0,
              "AS scratch address obeys queried minimum alignment");
        CHECK(renderer.test_gi_presented_history_index() == 0u &&
                  renderer.test_gi_candidate_history_index() == 1u,
              "GI temporal dispatch records into the non-presented ping-pong set");
        CHECK(renderer.test_last_rt_samples() == 4 &&
                  renderer.test_last_rt_debug_view(),
              "ray generation records sample and debug settings");
        CHECK(renderer.rt_available_observed() &&
                  renderer.rt_effective_observed() &&
                  renderer.rt_trace_dispatches_observed() == 2 &&
                  renderer.rt_fallback_reason_observed().empty(),
              "native RT frame observes direct-shadow and diffuse-GI dispatches");
        CHECK(renderer.test_composite_uses_gi_temporal(),
              "same-frame composite descriptor samples accumulated GI output");
        CHECK(matter::immediate_submit_count() == immediate_before,
              "native RT frame records without immediate submit");
        CHECK(vulkan.end_frame(frame, error),
              error.empty() ? "submit native RT frame" : error.c_str());
        vulkan.wait_idle();
        bool failed_receiver_seen = false;
        uint32_t retry_x = 0;
        uint32_t retry_y = 0;
        matter::Float4 failed_raw{};
        for (uint32_t y = 20; y < 200 && !failed_receiver_seen; y += 20) {
            for (uint32_t x = 20; x < 320; x += 20) {
                viewer::VkRasterPixel pixel{};
                if (renderer.readback_raster_pixel(x, y, pixel, error) &&
                    pixel.material_index == 1u) {
                    failed_receiver_seen = true;
                    retry_x = x;
                    retry_y = y;
                    failed_raw = pixel.raw_diffuse;
                    break;
                }
            }
        }
        renderer.finish_ray_tracing_frame(frame.serial, false);
        CHECK(renderer.test_gi_presented_history_index() == 0u,
              "failed presentation does not publish candidate GI history");
        CHECK(!renderer.test_rt_blas_built(920) &&
                  renderer.test_rt_blas_candidate_serial(920) == 0,
              "failed frame rolls back candidate BLAS state");
        gi_temporal.attempt_token = 202;
        renderer.set_temporal_frame(gi_temporal);
        CHECK(vulkan.begin_frame(frame, error) &&
                  renderer.prepare_frame(frame, matrices, camera.position,
                                         1.0f, error) &&
                  renderer.record_cull_and_render(
                      frame, matrices, camera.position, 1.0f, error) &&
                  renderer.record_composite_to_swapchain(frame, error) &&
                  vulkan.end_frame(frame, error),
              error.empty() ? "retry rolled-back native RT frame"
                            : error.c_str());
        renderer.finish_ray_tracing_frame(frame.serial, true);
        CHECK(renderer.test_gi_presented_history_index() == 1u,
              "successful presentation publishes candidate GI history");
        CHECK(renderer.test_gi_history_reset_count() == 1u,
              "first successfully presented temporal GI frame resets once");
        viewer::VkRasterPixel retry_pixel{};
        CHECK(failed_receiver_seen &&
                  renderer.readback_raster_pixel(retry_x, retry_y, retry_pixel,
                                                 error) &&
                  retry_pixel.material_index == 1u &&
                  close4(retry_pixel.raw_diffuse, failed_raw, 1e-6f),
              "failed-attempt retry keeps GPU GI deterministic from committed frame identity");
        viewer::GiTemporalGpuFixture temporal_fixture{};
        const float fixture_luminance =
            0.2126f * temporal_fixture.raw.x +
            0.7152f * temporal_fixture.raw.y +
            0.0722f * temporal_fixture.raw.z;
        temporal_fixture.previous_moments =
            {fixture_luminance, fixture_luminance * fixture_luminance, 0.0f};
        temporal_fixture.velocity = {1.0f, 1.0f, 0.0f};
        temporal_fixture.history_patch_pixel = {2, 2};
        viewer::GiTemporalGpuResult temporal_result{};
        CHECK(renderer.test_dispatch_gi_temporal_fixture(
                  temporal_fixture, temporal_result, error) &&
                  temporal_result.history_length == 4u &&
                  temporal_result.rejection_bits == 0u &&
                  std::fabs(temporal_result.moments.x - fixture_luminance) <
                      0.002f,
              error.empty()
                  ? "GPU temporal shader reprojects X and top-left Y and accumulates moments"
                  : error.c_str());

        const auto gpu_rejection = [&](viewer::GiTemporalGpuFixture changed,
                                       uint32_t expected,
                                       const char* label) {
            viewer::GiTemporalGpuResult rejected{};
            CHECK(renderer.test_dispatch_gi_temporal_fixture(
                      changed, rejected, error) &&
                      rejected.history_length == 1u &&
                      rejected.rejection_bits == expected,
                  error.empty() ? label : error.c_str());
        };
        temporal_fixture.velocity = {20.0f, 20.0f, 0.0f};
        gpu_rejection(temporal_fixture, viewer::kGiRejectBounds,
                      "GPU temporal shader emits bounds rejection");
        temporal_fixture.velocity = {};
        temporal_fixture.history_patch_pixel = temporal_fixture.output_pixel;
        auto changed_temporal = temporal_fixture;
        changed_temporal.depth = 0.8f;
        gpu_rejection(changed_temporal, viewer::kGiRejectDepth,
                      "GPU temporal shader emits depth rejection");
        changed_temporal = temporal_fixture;
        changed_temporal.normal = {1.0f, 0.0f, 0.0f, 0.0f};
        gpu_rejection(changed_temporal, viewer::kGiRejectNormal,
                      "GPU temporal shader emits normal rejection");
        changed_temporal = temporal_fixture;
        changed_temporal.material_index++;
        gpu_rejection(changed_temporal, viewer::kGiRejectMaterial,
                      "GPU temporal shader emits material rejection");
        changed_temporal = temporal_fixture;
        changed_temporal.instance_token++;
        gpu_rejection(changed_temporal, viewer::kGiRejectInstance,
                      "GPU temporal shader emits instance rejection");
        changed_temporal = temporal_fixture;
        changed_temporal.reset = true;
        gpu_rejection(changed_temporal, viewer::kGiRejectReset,
                      "GPU temporal shader emits reset rejection");
        changed_temporal = temporal_fixture;
        changed_temporal.previous_radiance = {100.0f, 50.0f, 25.0f, 1.0f};
        CHECK(renderer.test_dispatch_gi_temporal_fixture(
                  changed_temporal, temporal_result, error) &&
                  close4(temporal_result.radiance, changed_temporal.raw,
                         0.003f),
              error.empty() ? "GPU temporal 3x3 clip rejects radiance outlier"
                            : error.c_str());

        viewer::GiAtrousGpuFixture atrous_fixture{};
        constexpr uint32_t atrous_width = 65;
        constexpr uint32_t atrous_height = 9;
        constexpr uint32_t atrous_boundary = 32;
        constexpr size_t atrous_pixels = atrous_width * atrous_height;
        atrous_fixture.extent = {atrous_width, atrous_height};
        atrous_fixture.signal.resize(atrous_pixels);
        atrous_fixture.moments.resize(atrous_pixels);
        atrous_fixture.depth.resize(atrous_pixels);
        atrous_fixture.normal.resize(atrous_pixels);
        atrous_fixture.material_index.resize(atrous_pixels);
        atrous_fixture.history_length.resize(atrous_pixels, 8u);
        for (uint32_t y = 0; y < atrous_height; ++y) {
            for (uint32_t x = 0; x < atrous_width; ++x) {
                const size_t i = y * atrous_width + x;
                const bool left = x < atrous_boundary;
                const float noise = (((x / 16u) + y) & 1u)
                    ? 0.35f : -0.35f;
                const float value = std::max(0.0f, (left ? 1.0f : 0.15f) + noise);
                atrous_fixture.signal[i] = {value, value, value, 1.0f};
                atrous_fixture.moments[i] =
                    {value, value * value + 0.16f, 0.0f};
                atrous_fixture.depth[i] = left ? 0.25f : 0.75f;
                atrous_fixture.normal[i] = left
                    ? matter::Float4{0.0f, 1.0f, 0.0f, 0.0f}
                    : matter::Float4{1.0f, 0.0f, 0.0f, 0.0f};
                atrous_fixture.material_index[i] = left ? 3u : 7u;
            }
        }
        atrous_fixture.history_length[0] = 0u;
        atrous_fixture.material_index[atrous_pixels - 1] = UINT32_MAX;
        viewer::GiAtrousGpuResult atrous_result{};
        CHECK(renderer.test_dispatch_gi_atrous_fixture(
                  atrous_fixture, atrous_result, error),
              error.empty() ? "dispatch real-GPU 9x9 A-trous fixture"
                            : error.c_str());
        const auto region_variance = [](const std::vector<matter::Float4>& values,
                                        uint32_t begin_x, uint32_t end_x) {
            double sum = 0.0, sum2 = 0.0;
            uint32_t count = 0;
            for (uint32_t y = 0; y < atrous_height; ++y)
                for (uint32_t x = begin_x; x < end_x; ++x) {
                    const float value = values[y * atrous_width + x].x;
                    sum += value;
                    sum2 += value * value;
                    ++count;
                }
            const double mean = sum / count;
            return sum2 / count - mean * mean;
        };
        const std::array<uint32_t, 5> expected_atrous_steps{
            1u, 2u, 4u, 8u, 16u};
        const double left_variance_before = region_variance(
            atrous_fixture.signal, 0, atrous_boundary);
        const double left_variance_after = region_variance(
            atrous_result.filtered, 0, atrous_boundary);
        const double right_variance_before = region_variance(
            atrous_fixture.signal, atrous_boundary, atrous_width);
        const double right_variance_after = region_variance(
            atrous_result.filtered, atrous_boundary, atrous_width);
        CHECK(atrous_result.gpu_step_widths == expected_atrous_steps &&
                  left_variance_after < left_variance_before * 0.75 &&
                  right_variance_after < right_variance_before * 0.75,
              "GPU-observed five-pass sequence meaningfully reduces variance in both regions");
        float pass_five_delta = 0.0f;
        for (size_t i = 0; i < atrous_pixels; ++i)
            pass_five_delta = std::max(
                pass_five_delta,
                std::fabs(atrous_result.filtered[i].x -
                          atrous_result.penultimate[i].x));
        std::printf("A-trous GPU: variance %.6f->%.6f %.6f->%.6f pass5=%.6f\n",
                    left_variance_before, left_variance_after,
                    right_variance_before, right_variance_after,
                    pass_five_delta);
        CHECK(pass_five_delta > 0.01f,
              "width-16 GPU pass measurably changes the width-65 fixture");
        for (uint32_t y = 0; y < atrous_height; ++y)
            for (uint32_t x = 0; x < atrous_width; ++x) {
                const size_t i = y * atrous_width + x;
                CHECK(std::isfinite(atrous_result.filtered[i].x) &&
                          std::isfinite(atrous_result.filtered[i].y) &&
                          std::isfinite(atrous_result.filtered[i].z),
                      "A-trous readback contains only finite values");
            }
        CHECK(close4(atrous_result.filtered[0], atrous_fixture.signal[0],
                     0.001f) &&
                  close4(atrous_result.filtered[atrous_pixels - 1],
                         atrous_fixture.signal[atrous_pixels - 1],
                         0.001f),
              "invalid history and background pixels pass through unchanged");

        viewer::GiAtrousGpuFixture boundary_fixture = atrous_fixture;
        for (uint32_t y = 0; y < atrous_height; ++y)
            for (uint32_t x = 0; x < atrous_width; ++x) {
                const size_t i = y * atrous_width + x;
                const float value = x < atrous_boundary ? 1.0f : 0.0f;
                boundary_fixture.signal[i] = {value, value, value, 1.0f};
                boundary_fixture.moments[i] =
                    {value, value * value + 0.16f, 0.0f};
            }
        viewer::GiAtrousGpuResult boundary_result{};
        CHECK(renderer.test_dispatch_gi_atrous_fixture(
                  boundary_fixture, boundary_result, error),
              error.empty() ? "dispatch isolated A-trous boundary fixture"
                            : error.c_str());
        double boundary_leak = 0.0;
        const double source_energy =
            static_cast<double>(atrous_boundary) * atrous_height;
        for (uint32_t y = 0; y < atrous_height; ++y)
            for (uint32_t x = atrous_boundary; x < atrous_width; ++x)
                boundary_leak +=
                    std::max(0.0f,
                             boundary_result.filtered[y * atrous_width + x].x);
        CHECK(boundary_leak < source_energy * 0.02,
              "depth normal and exact material weights keep boundary crossing below 2 percent");

        viewer::GiAtrousGpuFixture constant_fixture = atrous_fixture;
        std::fill(constant_fixture.signal.begin(), constant_fixture.signal.end(),
                  matter::Float4{0.375f, 0.25f, 0.125f, 1.0f});
        std::fill(constant_fixture.moments.begin(), constant_fixture.moments.end(),
                  matter::Float3{0.285125f, 0.0812963f, 0.0f});
        std::fill(constant_fixture.depth.begin(), constant_fixture.depth.end(), 0.5f);
        std::fill(constant_fixture.normal.begin(), constant_fixture.normal.end(),
                  matter::Float4{0.0f, 1.0f, 0.0f, 0.0f});
        std::fill(constant_fixture.material_index.begin(),
                  constant_fixture.material_index.end(), 11u);
        CHECK(renderer.test_dispatch_gi_atrous_fixture(
                  constant_fixture, atrous_result, error),
              error.empty() ? "dispatch constant-color A-trous fixture"
                            : error.c_str());
        bool constant_identity =
            atrous_result.filtered.size() == atrous_pixels;
        for (const auto& value : atrous_result.filtered)
            constant_identity = constant_identity &&
                close4(value, {0.375f, 0.25f, 0.125f, 1.0f}, 0.001f);
        CHECK(constant_identity,
              "constant-color A-trous input is an identity operation");
        const auto render_temporal_control = [&](uint64_t attempt_token,
                                                 bool reset = false) {
            gi_temporal.attempt_token = attempt_token;
            gi_temporal.reset = reset;
            renderer.set_temporal_frame(gi_temporal);
            matter::VulkanFrame control{};
            const bool rendered = vulkan.begin_frame(control, error) &&
                renderer.prepare_frame(control, matrices, camera.position,
                                       1.0f, error) &&
                renderer.record_cull_and_render(
                    control, matrices, camera.position, 1.0f, error) &&
                renderer.record_composite_to_swapchain(control, error) &&
                vulkan.end_frame(control, error);
            renderer.finish_ray_tracing_frame(control.serial, rendered);
            return rendered;
        };
        matter::VulkanRayTracingSettings rt_disabled = enabled;
        rt_disabled.enabled = false;
        renderer.set_ray_tracing_settings(rt_disabled);
        CHECK(render_temporal_control(240) &&
                  renderer.test_gi_history_reset_count() == 1u,
              error.empty() ? "RT disable preserves pending stale-history invalidation"
                            : error.c_str());
        renderer.set_ray_tracing_settings(enabled);
        CHECK(render_temporal_control(241) &&
                  renderer.test_gi_history_reset_count() == 2u,
              error.empty() ? "RT re-enable resets stale GI history once"
                            : error.c_str());
        CHECK(render_temporal_control(242) &&
                  renderer.test_gi_history_reset_count() == 2u,
              error.empty() ? "stable RT frame does not repeat re-enable reset"
                            : error.c_str());
        renderer.set_test_dlss_bridge(matter::StreamlineBridge::test_fake_dlss(
            [](VkCommandBuffer, uint64_t, const matter::DlssOptions&,
               const matter::DlssConstants&, const matter::DlssResources&,
               matter::DlssEvaluationOutput& output, std::string&) {
                output.output_written = true;
                output.layout = VK_IMAGE_LAYOUT_GENERAL;
                output.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                output.access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                return true;
            }));
        renderer.set_dlss_mode(matter::DlssMode::Quality);
        CHECK(render_temporal_control(243) &&
                  renderer.test_gi_history_reset_count() == 3u &&
                  !renderer.consume_dlss_history_reset(),
              error.empty() ? "Quality mode transition applies one GI reset"
                            : error.c_str());
        CHECK(render_temporal_control(244) &&
                  renderer.test_gi_history_reset_count() == 3u &&
                  !renderer.consume_dlss_history_reset(),
              error.empty() ? "stable Quality mode does not repeat GI reset"
                            : error.c_str());
        renderer.set_dlss_mode(matter::DlssMode::Native);
        const bool native_transition_rendered = render_temporal_control(245);
        const uint64_t native_transition_reset_count =
            renderer.test_gi_history_reset_count();
        const bool native_transition_invalidated =
            renderer.consume_dlss_history_reset();
        CHECK(native_transition_rendered &&
                  native_transition_reset_count == 4u &&
                  !native_transition_invalidated,
              error.empty() ? "Native mode transition applies one GI reset"
                            : error.c_str());
        CHECK(render_temporal_control(246) &&
                  renderer.test_gi_history_reset_count() == 4u &&
                  !renderer.consume_dlss_history_reset(),
              error.empty() ? "stable Native mode does not repeat GI reset"
                            : error.c_str());
        matter::VulkanRayTracingSettings non_debug_rt = enabled;
        non_debug_rt.debug_view = false;
        renderer.set_ray_tracing_settings(non_debug_rt);
        CHECK(render_temporal_control(247),
              error.empty() ? "render accumulated-GI composite proof frame"
                            : error.c_str());
        viewer::VkRasterPixel composite_pixel{};
        CHECK(renderer.readback_raster_pixel(retry_x, retry_y,
                                             composite_pixel, error),
              error.empty() ? "read accumulated-GI composite proof pixel"
                            : error.c_str());
        const matter::Float3 to_sun{0.0f, 1.0f, 0.0f};
        const float direct = std::max(
            0.0f, composite_pixel.normal.x * to_sun.x +
                      composite_pixel.normal.y * to_sun.y +
                      composite_pixel.normal.z * to_sun.z);
        const float diffuse_scale = 1.0f - composite_pixel.orm.y;
        const float sun_base = direct * lighting.sun_intensity *
            (1.0f + (0.65f - 1.0f) * composite_pixel.orm.x);
        const float emission_strength =
            std::exp2(std::min(composite_pixel.normal.w,
                               viewer::kVkMaxEncodedEmission)) - 1.0f;
        const matter::Float3 expected_composite{
            composite_pixel.albedo.x * diffuse_scale * lighting.sky_color.x *
                    composite_pixel.orm.z +
                composite_pixel.albedo.x * diffuse_scale * lighting.sun_color.x *
                    sun_base * composite_pixel.visibility.x +
                composite_pixel.albedo.x * emission_strength +
                composite_pixel.accumulated_diffuse.x +
                composite_pixel.accumulated_specular.x,
            composite_pixel.albedo.y * diffuse_scale * lighting.sky_color.y *
                    composite_pixel.orm.z +
                composite_pixel.albedo.y * diffuse_scale * lighting.sun_color.y *
                    sun_base * composite_pixel.visibility.y +
                composite_pixel.albedo.y * emission_strength +
                composite_pixel.accumulated_diffuse.y +
                composite_pixel.accumulated_specular.y,
            composite_pixel.albedo.z * diffuse_scale * lighting.sky_color.z *
                    composite_pixel.orm.z +
                composite_pixel.albedo.z * diffuse_scale * lighting.sun_color.z *
                    sun_base * composite_pixel.visibility.z +
                composite_pixel.albedo.z * emission_strength +
                composite_pixel.accumulated_diffuse.z +
                composite_pixel.accumulated_specular.z};
        CHECK(renderer.test_composite_uses_gi_temporal() &&
                  std::fabs(composite_pixel.hdr.x - expected_composite.x) < 0.04f &&
                  std::fabs(composite_pixel.hdr.y - expected_composite.y) < 0.04f &&
                  std::fabs(composite_pixel.hdr.z - expected_composite.z) < 0.04f,
              "GPU composite equation samples accumulated temporal radiance");
        renderer.set_ray_tracing_settings(enabled);
        const auto render_sun_probe = [&](float intensity,
                                          uint64_t attempt_token,
                                          matter::Float4& raw) {
            viewer::VkSceneLighting probe_lighting = lighting;
            probe_lighting.sun_intensity = intensity;
            renderer.set_lighting(probe_lighting);
            gi_temporal.attempt_token = attempt_token;
            renderer.set_temporal_frame(gi_temporal);
            matter::VulkanFrame probe_frame{};
            const bool rendered = vulkan.begin_frame(probe_frame, error) &&
                renderer.prepare_frame(probe_frame, matrices, camera.position,
                                       1.0f, error) &&
                renderer.record_cull_and_render(
                    probe_frame, matrices, camera.position, 1.0f, error) &&
                renderer.record_composite_to_swapchain(probe_frame, error) &&
                vulkan.end_frame(probe_frame, error);
            renderer.finish_ray_tracing_frame(probe_frame.serial, rendered);
            viewer::VkRasterPixel pixel{};
            const bool read = rendered && renderer.readback_raster_pixel(
                                              retry_x, retry_y, pixel, error);
            raw = pixel.raw_diffuse;
            return read && pixel.material_index == 1u;
        };
        const auto rgb_delta = [](matter::Float4 a, matter::Float4 b) {
            return std::max(std::fabs(a.x - b.x),
                            std::max(std::fabs(a.y - b.y),
                                     std::fabs(a.z - b.z)));
        };
        matter::Float4 unblocked_sun_zero{};
        matter::Float4 unblocked_sun_high{};
        const bool unblocked_probes_ok =
            render_sun_probe(0.0f, 299, unblocked_sun_zero) &&
            render_sun_probe(100.0f, 300, unblocked_sun_high);
        const float unblocked_sun_delta =
            rgb_delta(unblocked_sun_zero, unblocked_sun_high);
        CHECK(unblocked_probes_ok && unblocked_sun_delta > 0.05f,
              "unblocked secondary hit responds to increased sun intensity");
        viewer::VkScenePart sun_blocker = horizontal(
            924, 0.5f, 20.0f, {0.0f, -1.0f, 0.0f}, 0, 1.0f);
        sun_blocker.clusters[0].lods[0].vertex_count = 0;
        CHECK(renderer.ensure_part(sun_blocker, error) >= 0 &&
                  renderer.update_instances({{920, identity_matrix()},
                                             {921, identity_matrix()},
                                             {924, identity_matrix()}},
                                            error),
              error.empty() ? "add RT-only secondary-sun blocker"
                            : error.c_str());
        matter::Float4 blocked_sun_zero{};
        matter::Float4 blocked_sun_high{};
        const bool blocked_probes_ok =
            render_sun_probe(0.0f, 301, blocked_sun_zero) &&
            render_sun_probe(100.0f, 302, blocked_sun_high);
        const float blocked_sun_delta =
            rgb_delta(blocked_sun_zero, blocked_sun_high);
        CHECK(blocked_probes_ok && blocked_sun_delta < 2e-3f &&
                  blocked_sun_delta < unblocked_sun_delta * 0.05f,
              "secondary-hit sun is visibility tested without unshadowed leakage");
        renderer.release_part(924);
        renderer.set_lighting(lighting);
        CHECK(renderer.update_instances(
                  {{920, identity_matrix()}, {921, identity_matrix()}}, error),
              error.empty() ? "remove RT-only secondary-sun blocker"
                            : error.c_str());
        matter::Float4 restored_unblocked_raw{};
        CHECK(render_sun_probe(lighting.sun_intensity, 303,
                               restored_unblocked_raw),
              "rerender unblocked receiver before visibility comparison");
        CHECK(renderer.test_rt_blas_built(920) &&
                  renderer.test_rt_blas_candidate_serial(920) == 0,
              "successful retry publishes candidate BLAS state");
        float minimum_visibility = 1.0f;
        float maximum_visibility = 0.0f;
        bool visibility_reads_ok = true;
        bool debug_output_matches = true;
        matter::Float4 strongest_receiver_raw{};
        matter::Float4 strongest_receiver_specular{};
        bool receiver_seen = false;
        float receiver_min_visibility = 1.0f;
        float receiver_max_visibility = 0.0f;
        for (uint32_t y = 20; y < 200; y += 20) {
            for (uint32_t x = 20; x < 320; x += 20) {
                viewer::VkRasterPixel pixel{};
                if (!renderer.readback_raster_pixel(x, y, pixel, error)) {
                    visibility_reads_ok = false;
                    break;
                }
                minimum_visibility =
                    std::min(minimum_visibility, pixel.visibility.x);
                maximum_visibility =
                    std::max(maximum_visibility, pixel.visibility.x);
                debug_output_matches =
                    debug_output_matches &&
                    std::fabs(pixel.hdr.x - pixel.visibility.x) < 0.01f &&
                    std::fabs(pixel.hdr.y - pixel.visibility.y) < 0.01f &&
                    std::fabs(pixel.hdr.z - pixel.visibility.z) < 0.01f;
                if (pixel.material_index == 1u) {
                    receiver_seen = true;
                    receiver_min_visibility =
                        std::min(receiver_min_visibility, pixel.visibility.x);
                    receiver_max_visibility =
                        std::max(receiver_max_visibility, pixel.visibility.x);
                    if (pixel.raw_diffuse.x > strongest_receiver_raw.x)
                        strongest_receiver_raw = pixel.raw_diffuse;
                    if (pixel.raw_specular.x > strongest_receiver_specular.x)
                        strongest_receiver_specular = pixel.raw_specular;
                }
            }
            if (!visibility_reads_ok) break;
        }
        CHECK(visibility_reads_ok && std::isfinite(minimum_visibility) &&
                  minimum_visibility < 1.0f && maximum_visibility == 1.0f,
              error.empty()
                  ? "native two-triangle visibility contains shadowed and open pixels"
                  : error.c_str());
        CHECK(debug_output_matches,
              "RT debug view composites grayscale visibility");
        CHECK(receiver_seen &&
                  renderer.test_raw_diffuse_extent().width == 160u &&
                  renderer.test_raw_diffuse_extent().height == 100u &&
                  std::isfinite(strongest_receiver_raw.x) &&
                  std::isfinite(strongest_receiver_raw.y) &&
                  std::isfinite(strongest_receiver_raw.z) &&
                  strongest_receiver_raw.x > 0.01f &&
                  strongest_receiver_raw.x >
                      strongest_receiver_raw.y * 1.25f,
              "white receiver above red floor gains positive red indirect radiance");
        CHECK(std::isfinite(strongest_receiver_specular.x) &&
                  std::isfinite(strongest_receiver_specular.y) &&
                  std::isfinite(strongest_receiver_specular.z),
              "separate raw specular target stays finite on diffuse receiver");
        renderer.release_part(921);
        CHECK(renderer.ensure_part(horizontal(925, 0.0f, 0.55f,
                                              {0.0f, -0.3162278f, 0.9486833f},
                                              1, 1.0f), error) >= 0 &&
                  renderer.update_instances(
                      {{920, identity_matrix()}, {925, identity_matrix()}},
                      error) &&
                  render_temporal_control(304),
              error.empty() ? "render tilted mirror colored-target fixture"
                            : error.c_str());
        matter::Float4 mirror_specular{};
        for (uint32_t y = 20; y < 200; y += 10)
            for (uint32_t x = 20; x < 320; x += 10) {
                viewer::VkRasterPixel pixel{};
                if (renderer.readback_raster_pixel(x, y, pixel, error) &&
                    pixel.material_index == 1u &&
                    pixel.raw_specular.x > mirror_specular.x)
                    mirror_specular = pixel.raw_specular;
            }
        CHECK(std::isfinite(mirror_specular.x) &&
                  std::isfinite(mirror_specular.y) &&
                  std::isfinite(mirror_specular.z) &&
                  mirror_specular.x > 0.001f &&
                  mirror_specular.x > mirror_specular.y * 1.2f,
              "GGX mirror receiver reflects the colored target with finite energy");
        const auto specular_coverage = [&]() {
            uint32_t count = 0;
            matter::Float4 peak{};
            for (uint32_t sy = 20; sy < 200; sy += 10)
                for (uint32_t sx = 20; sx < 320; sx += 10) {
                    viewer::VkRasterPixel pixel{};
                    if (!renderer.readback_raster_pixel(sx, sy, pixel, error) ||
                        pixel.material_index != 1u)
                        continue;
                    const float energy = std::max(pixel.raw_specular.x,
                        std::max(pixel.raw_specular.y, pixel.raw_specular.z));
                    if (energy > 0.001f) ++count;
                    if (energy > std::max(peak.x, std::max(peak.y, peak.z)))
                        peak = pixel.raw_specular;
                }
            return std::pair<uint32_t, matter::Float4>{count, peak};
        };
        const auto mirror_stats = specular_coverage();
        gi_materials[1].metal_opacity_spec_coat[0] = 1.0f;
        gi_materials[1].base_roughness[3] = 0.65f;
        CHECK(renderer.update_materials(gi_materials, 2, 1, error) &&
                  render_temporal_control(305),
              error.empty() ? "render rough-metal broadening fixture"
                            : error.c_str());
        const auto rough_metal_stats = specular_coverage();
        CHECK(rough_metal_stats.first >= mirror_stats.first &&
                  std::isfinite(rough_metal_stats.second.x) &&
                  std::isfinite(rough_metal_stats.second.y) &&
                  std::isfinite(rough_metal_stats.second.z),
              "rough metal broadens the finite reflected signal");
        gi_materials[1].metal_opacity_spec_coat[0] = 0.0f;
        gi_materials[1].base_roughness[3] = 0.35f;
        gi_materials[1].specular_tint_coat_roughness[0] = 0.01f;
        gi_materials[1].specular_tint_coat_roughness[1] = 1.0f;
        gi_materials[1].specular_tint_coat_roughness[2] = 0.01f;
        CHECK(renderer.update_materials(gi_materials, 3, 1, error) &&
                  render_temporal_control(306),
              error.empty() ? "render tinted dielectric fixture"
                            : error.c_str());
        const auto tinted_stats = specular_coverage();
        const float mirror_red_green = mirror_stats.second.x /
            std::max(mirror_stats.second.y, 1e-6f);
        const float tinted_red_green = tinted_stats.second.x /
            std::max(tinted_stats.second.y, 1e-6f);
        CHECK(tinted_stats.first > 0u && tinted_stats.second.y > 0.0f &&
                  tinted_red_green < mirror_red_green,
              "dielectric GGX uses authored specular tint");
        gi_materials[1].specular_tint_coat_roughness[0] = 1.0f;
        gi_materials[1].specular_tint_coat_roughness[1] = 1.0f;
        gi_materials[1].specular_tint_coat_roughness[2] = 1.0f;
        gi_materials[1].base_roughness[3] = 0.8f;
        CHECK(renderer.update_materials(gi_materials, 4, 1, error) &&
                  render_temporal_control(307),
              error.empty() ? "render rough dielectric F0 fixture"
                            : error.c_str());
        const auto rough_dielectric_stats = specular_coverage();
        CHECK(rough_dielectric_stats.first > 0u &&
                  std::isfinite(rough_dielectric_stats.second.x),
              "rough dielectric retains finite nonmetal F0 response");
        gi_materials[1].metal_opacity_spec_coat[3] = 1.0f;
        gi_materials[1].specular_tint_coat_roughness[3] = 0.08f;
        CHECK(renderer.update_materials(gi_materials, 5, 1, error) &&
                  render_temporal_control(308),
              error.empty() ? "render clearcoat second-lobe fixture"
                            : error.c_str());
        const auto coat_stats = specular_coverage();
        CHECK(coat_stats.first > 0u &&
                  coat_stats.second.x > 0.0f && coat_stats.second.y > 0.0f &&
                  coat_stats.second.z > 0.0f,
              "clearcoat adds a finite untinted dielectric highlight");
        renderer.release_part(925);
        CHECK(renderer.ensure_part(horizontal(921, 0.0f, 0.55f,
                                              {0.0f, -1.0f, 0.0f}, 1, 1.0f),
                                   error) >= 0 &&
                  renderer.update_instances(
                      {{920, identity_matrix()}, {921, identity_matrix()}},
                      error),
              error.empty() ? "restore diffuse receiver after mirror fixture"
                            : error.c_str());
        renderer.release_part(921);
        CHECK(renderer.ensure_part(horizontal(922, 0.0f, 0.55f,
                                              {0.0f, -1.0f, 0.0f}, 1, 0.0f),
                                   error) >= 0 &&
                  renderer.update_instances(
                      {{920, identity_matrix()}, {922, identity_matrix()}},
                      error),
              error.empty() ? "replace GI receiver with baked-AO-zero fixture"
                            : error.c_str());
        matter::VulkanFrame ao_frame{};
        CHECK(vulkan.begin_frame(ao_frame, error) &&
                  renderer.prepare_frame(ao_frame, matrices, camera.position,
                                         1.0f, error) &&
                  renderer.record_cull_and_render(
                      ao_frame, matrices, camera.position, 1.0f, error) &&
                  renderer.record_composite_to_swapchain(ao_frame, error) &&
                  vulkan.end_frame(ao_frame, error),
              error.empty() ? "render baked-AO-zero GI fixture"
                            : error.c_str());
        renderer.finish_ray_tracing_frame(ao_frame.serial, true);
        float ao_zero_max_raw = 0.0f;
        float ao_min_visibility = 1.0f;
        float ao_max_visibility = 0.0f;
        bool ao_receiver_seen = false;
        for (uint32_t y = 20; y < 200; y += 20) {
            for (uint32_t x = 20; x < 320; x += 20) {
                viewer::VkRasterPixel pixel{};
                CHECK(renderer.readback_raster_pixel(x, y, pixel, error),
                      error.empty() ? "read baked-AO-zero GI pixel"
                                    : error.c_str());
                if (pixel.material_index == 1u) {
                    ao_receiver_seen = true;
                    ao_min_visibility =
                        std::min(ao_min_visibility, pixel.visibility.x);
                    ao_max_visibility =
                        std::max(ao_max_visibility, pixel.visibility.x);
                    ao_zero_max_raw = std::max(
                        ao_zero_max_raw,
                        std::max(pixel.raw_diffuse.x,
                                 std::max(pixel.raw_diffuse.y,
                                          pixel.raw_diffuse.z)));
                }
            }
        }
        std::printf("AO-zero GI: seen=%u raw=%.6f visibility=%.3f..%.3f\n",
                    ao_receiver_seen ? 1u : 0u, ao_zero_max_raw,
                    ao_min_visibility, ao_max_visibility);
        CHECK(ao_receiver_seen && receiver_seen && ao_zero_max_raw < 1e-5f &&
                  std::fabs(ao_min_visibility - receiver_min_visibility) <
                      1e-6f &&
                  std::fabs(ao_max_visibility - receiver_max_visibility) <
                      1e-6f,
              "baked AO zero suppresses raw indirect diffuse without changing direct visibility");
        renderer.release_part(922);
        CHECK(renderer.ensure_part(horizontal(923, 0.0f, 0.55f,
                                              {0.0f, -1.0f, 0.0f}, 1, 1.0f),
                                   error) >= 0 &&
                  renderer.update_instances(
                      {{920, identity_matrix()}, {923, identity_matrix()}},
                      error),
              error.empty() ? "restore authored-AO receiver for GI disable test"
                            : error.c_str());
        matter::VulkanGiSettings disabled_gi = gi;
        disabled_gi.enabled = 0;
        renderer.set_gi_settings(disabled_gi);
        matter::VulkanFrame disabled_gi_frame{};
        CHECK(vulkan.begin_frame(disabled_gi_frame, error) &&
                  renderer.prepare_frame(disabled_gi_frame, matrices,
                                         camera.position, 1.0f, error) &&
                  renderer.record_cull_and_render(
                      disabled_gi_frame, matrices, camera.position, 1.0f,
                      error) &&
                  renderer.record_composite_to_swapchain(disabled_gi_frame,
                                                         error) &&
                  vulkan.end_frame(disabled_gi_frame, error),
              error.empty() ? "render RT-active GI-disabled fixture"
                            : error.c_str());
        renderer.finish_ray_tracing_frame(disabled_gi_frame.serial, true);
        bool disabled_receiver_seen = false;
        float disabled_receiver_raw = 0.0f;
        float disabled_min_visibility = 1.0f;
        float disabled_max_visibility = 0.0f;
        for (uint32_t y = 20; y < 200; y += 20) {
            for (uint32_t x = 20; x < 320; x += 20) {
                viewer::VkRasterPixel pixel{};
                CHECK(renderer.readback_raster_pixel(x, y, pixel, error),
                      error.empty() ? "read RT-active GI-disabled pixel"
                                    : error.c_str());
                disabled_min_visibility =
                    std::min(disabled_min_visibility, pixel.visibility.x);
                disabled_max_visibility =
                    std::max(disabled_max_visibility, pixel.visibility.x);
                if (pixel.material_index == 1u) {
                    disabled_receiver_seen = true;
                    disabled_receiver_raw = std::max(
                        disabled_receiver_raw,
                        std::max(pixel.raw_diffuse.x,
                                 std::max(pixel.raw_diffuse.y,
                                          pixel.raw_diffuse.z)));
                }
            }
        }
        CHECK(renderer.rt_effective_observed() &&
                  renderer.rt_trace_dispatches_observed() == 1u &&
                  disabled_receiver_seen && disabled_receiver_raw < 1e-5f &&
                  disabled_min_visibility < 1.0f &&
                  disabled_max_visibility == 1.0f,
              "RT-active GI disable preserves direct visibility and clears receiver raw diffuse");
        std::printf("RT visibility range: %.3f .. %.3f\n",
                    minimum_visibility, maximum_visibility);
    }
}

void run_forced_ray_tracing_unavailable_path(matter::VulkanDevice& vulkan) {
    CHECK(!vulkan.ray_tracing_available(),
          "test fixture forces native ray tracing unavailable");
    CHECK(vulkan.ray_tracing_unavailable_reason().find("forced") !=
              std::string::npos,
          "forced native RT fallback exposes its reason");

    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.ensure_part(known_raster_triangle(930), error) >= 0 &&
              renderer.update_instances({{930, identity_matrix()}}, error),
          error.empty() ? "prepare forced-unavailable raster fixture"
                        : error.c_str());
    matter::VulkanRayTracingSettings settings{};

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;

    const VkExtent2D extents[] = {{160, 100}, {96, 64}};
    uint32_t first_frame_slot = UINT32_MAX;
    for (uint32_t index = 0; index < 2; ++index) {
        settings.enabled = index == 0;
        renderer.set_ray_tracing_settings(settings);
        viewer::FrameMatrices matrices{};
        CHECK(viewer::build_frame_matrices(camera, extents[index].width,
                                           extents[index].height, matrices,
                                           error),
              error.empty() ? "build forced-unavailable frame matrices"
                            : error.c_str());
        viewer::TemporalFrame temporal{};
        temporal.current_jittered = matrices;
        temporal.previous_jittered = matrices;
        temporal.internal_extent = extents[index];
        temporal.reset = index == 0;
        temporal.attempt_token = index + 1;
        renderer.set_temporal_frame(temporal);

        matter::VulkanFrame frame{};
        const bool began = vulkan.begin_frame(frame, error);
        CHECK(began, error.empty() ? "begin forced-unavailable frame"
                                   : error.c_str());
        if (!began) break;
        CHECK(frame.frame_slot_count >= 2,
              "fallback transition exposes at least two frames in flight");
        if (index == 0) {
            first_frame_slot = frame.frame_slot;
        } else {
            CHECK(frame.frame_slot != first_frame_slot,
                  "fallback extent/mode transition uses a second frame slot");
        }
        const bool recorded =
            renderer.prepare_frame(frame, matrices, camera.position, 1.0f,
                                   error) &&
            renderer.record_cull_and_render(frame, matrices, camera.position,
                                            1.0f, error) &&
            renderer.record_composite_to_swapchain(frame, error);
        CHECK(recorded,
              error.empty() ? "record forced-unavailable fallback frame"
                            : error.c_str());
        CHECK(recorded && vulkan.end_frame(frame, error),
              error.empty() ? "submit forced-unavailable fallback frame"
                            : error.c_str());
        CHECK(!renderer.rt_available_observed() &&
                  !renderer.rt_effective_observed() &&
                  renderer.rt_trace_dispatches_observed() == 0 &&
                  renderer.rt_fallback_reason_observed().find(
                      index == 0 ? "forced" : "disabled") !=
                      std::string::npos,
              "fallback transition observes no dispatch and its current reason");
        if (!recorded) break;
        const viewer::VkRasterAttachments attachments =
            renderer.raster_attachments();
        CHECK(attachments.extent.width == extents[index].width &&
                  attachments.extent.height == extents[index].height,
              "forced-unavailable fallback follows current internal extent");
        const VkImageUsageFlags expected_visibility_usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        CHECK(renderer.test_visibility_usage() == expected_visibility_usage,
              "forced-unavailable visibility excludes storage usage");
    }

    // Both submissions must overlap the attachment replacement above. Waiting
    // only after the transition lets validation prove the first visibility
    // image and descriptor remain alive through their submitted frame.
    vulkan.wait_idle();
    CHECK(vulkan.validation_error_count() == 0,
          "two-frame fallback extent/mode transition retains visibility");

    viewer::VkRasterPixel center{};
    CHECK(renderer.readback_raster_pixel(48, 32, center, error) &&
              close3(center.visibility, {1.0f, 1.0f, 1.0f}, 1e-6f),
          error.empty() ? "fallback resize preserves raster visibility"
                        : error.c_str());
}

void run_raster_submission_fault(matter::VulkanDevice& vulkan) {
    constexpr uint32_t width = 64;
    constexpr uint32_t height = 64;
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    const viewer::VkScenePart triangle = known_raster_triangle(950);
    const matter::Mat4f identity = identity_matrix();
    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices frame{};
    CHECK(viewer::build_frame_matrices(camera, width, height, frame, error),
          error.empty() ? "build raster fault frame matrices" : error.c_str());
    const auto prepare_scene = [&]() {
        return renderer.ensure_part(triangle, error) >= 0 &&
               renderer.update_instances({{950, identity}}, error) &&
               renderer.dispatch_culling(frame, camera.position, 1.0f, error);
    };
    CHECK(prepare_scene(),
          error.empty() ? "prepare raster submission fault scene"
                        : error.c_str());
    CHECK(renderer.render_gbuffer_and_composite(width, height, error),
          error.empty() ? "establish raster attachment baseline"
                        : error.c_str());
    CHECK(renderer.raster_attachments().hdr.image != VK_NULL_HANDLE,
          "completed raster submission exposes attachments");

    _putenv_s("MATTER_VK_TEST_FORCE_IMMEDIATE_COMPLETED_FAILURE",
              "raster-submission");
    const bool rendered =
        renderer.render_gbuffer_and_composite(width, height, error);
    _putenv_s("MATTER_VK_TEST_FORCE_IMMEDIATE_COMPLETED_FAILURE", "");
    CHECK(!rendered &&
              error.find("poisoned after partial GPU mutation") !=
                  std::string::npos &&
              error.find("forced completed immediate failure") !=
                  std::string::npos,
          "actual raster submission failure poisons renderer");
    const std::string poison_reason = error;
    const viewer::VkRasterAttachments hidden = renderer.raster_attachments();
    CHECK(hidden.albedo.image == VK_NULL_HANDLE &&
              hidden.normal.image == VK_NULL_HANDLE &&
              hidden.orm.image == VK_NULL_HANDLE &&
              hidden.velocity.image == VK_NULL_HANDLE &&
              hidden.depth.image == VK_NULL_HANDLE &&
              hidden.hdr.image == VK_NULL_HANDLE &&
              hidden.extent.width == 0 && hidden.extent.height == 0,
          "poisoned renderer exposes no raster attachments");
    viewer::VkRasterPixel pixel{};
    CHECK(!renderer.readback_raster_pixel(0, 0, pixel, error) &&
              error == poison_reason,
          "poisoned raster readback fails with stable diagnostic");

    renderer.reset();
    CHECK(renderer.raster_attachments().hdr.image == VK_NULL_HANDLE,
          "reset renderer keeps attachments hidden until re-render");
    CHECK(renderer.init(error) && prepare_scene() &&
              renderer.raster_attachments().hdr.image == VK_NULL_HANDLE &&
              renderer.render_gbuffer_and_composite(width, height, error) &&
              renderer.raster_attachments().hdr.image != VK_NULL_HANDLE,
          error.empty() ? "reset and reinit restore raster attachments only after render"
                        : error.c_str());
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
        const size_t base = i * viewer::kVkMaxLod;
        auto& command = result.commands[base];
        command.vertex_count = scene.parts[i].clusters[0].lods[0].vertex_count;
        command.first_vertex = scene.parts[i].clusters[0].lods[0].first_vertex;
        command.first_instance = static_cast<uint32_t>(i);
        for (uint32_t lod = 1; lod < viewer::kVkMaxLod; ++lod)
            result.commands[base + lod].first_instance =
                static_cast<uint32_t>(i + 1);
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

void run_frame_upload_tests(matter::VulkanDevice& vulkan) {
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    const matter::Mat4f identity = identity_matrix();
    const viewer::VkScenePart first = known_raster_triangle(970);
    const viewer::VkScenePart second = known_raster_triangle(971);
    std::vector<MaterialGpuRecord> materials(8);
    materials[7].base_roughness[0] = 0.25f;
    materials[7].metal_opacity_spec_coat[1] = 1.0f;
    CHECK(renderer.update_materials(materials, 1, 1, error),
          error.empty() ? "stage persistent frame materials" : error.c_str());
    CHECK(renderer.ensure_part(first, error) >= 0,
          error.empty() ? "ensure persistent Vulkan part" : error.c_str());
    std::vector<viewer::VkSceneInstance> instances{{970, identity},
                                                    {970, identity}};
    CHECK(renderer.update_instances(instances, error),
          error.empty() ? "upload persistent Vulkan instances" : error.c_str());

    const FixedCullScene scene = make_fixed_cull_scene();
    const auto prepare = [&](const viewer::FrameMatrices& matrices,
                             uint32_t* frame_slot = nullptr) {
        matter::VulkanFrame frame{};
        if (!vulkan.begin_frame(frame, error)) return false;
        if (frame_slot) *frame_slot = frame.frame_slot;
        const bool prepared = renderer.prepare_frame(frame, matrices, scene.eye,
                                                     1.0f, error);
        const bool ended = vulkan.end_frame(frame, error);
        return prepared && ended;
    };

    const uint64_t immediate_before_material = matter::immediate_submit_count();
    uint32_t first_material_slot = UINT32_MAX;
    CHECK(prepare(scene.frame, &first_material_slot),
          error.empty() ? "prepare initial persistent Vulkan frame"
                        : error.c_str());
    CHECK(renderer.test_material_upload_record_count(first_material_slot) == 1 &&
              (renderer.test_material_buffer_memory(first_material_slot) &
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
              (renderer.test_material_buffer_memory(first_material_slot) &
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 &&
              (renderer.test_material_staging_memory(first_material_slot) &
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
              matter::immediate_submit_count() == immediate_before_material,
          "material upload records staging copy into acquired frame");
    const viewer::VkSceneUploadCounters warm = renderer.upload_counters();

    // Warm the second slot. Reusing the first slot below must leave all
    // scene uploads unchanged for identical CPU scene data.
    materials[7].absorption_pad[0] = 0.625f;
    CHECK(renderer.update_materials(materials, 2, 1, error),
          error.empty() ? "stage second-slot material revision"
                        : error.c_str());
    uint32_t second_material_slot = UINT32_MAX;
    CHECK(renderer.update_instances(instances, error) &&
              prepare(scene.frame, &second_material_slot),
          error.empty() ? "prepare second persistent Vulkan slot"
                        : error.c_str());
    CHECK(second_material_slot != first_material_slot &&
              renderer.test_material_upload_record_count(
                  second_material_slot) == 1 &&
              matter::immediate_submit_count() == immediate_before_material,
          "material revision records independently into second in-flight slot");
    CHECK(renderer.update_instances(instances, error) && prepare(scene.frame),
          error.empty() ? "prepare stable Vulkan frame" : error.c_str());
    const viewer::VkSceneUploadCounters stable = renderer.upload_counters();
    CHECK(stable.vertex_uploads == warm.vertex_uploads,
          "stable frame does not upload vertices");
    CHECK(stable.cluster_uploads == warm.cluster_uploads,
          "stable frame does not upload clusters");
    CHECK(stable.instance_uploads == warm.instance_uploads + 1,
          "second slot uploads instances once before stable slot reuse");
    CHECK(stable.command_layout_rebuilds == warm.command_layout_rebuilds,
          "stable frame does not rebuild command layout");

    instances[1].object_to_world.m[12] = 0.25f;
    CHECK(renderer.update_instances(instances, error) && prepare(scene.frame),
          error.empty() ? "prepare transformed Vulkan frame" : error.c_str());
    const viewer::VkSceneUploadCounters transformed = renderer.upload_counters();
    CHECK(transformed.instance_uploads == stable.instance_uploads + 1,
          "changed transform uploads one instance generation");
    CHECK(transformed.vertex_uploads == stable.vertex_uploads &&
              transformed.cluster_uploads == stable.cluster_uploads &&
              transformed.command_layout_rebuilds ==
                  stable.command_layout_rebuilds,
          "changed transform leaves static scene and command layout intact");

    CHECK(renderer.ensure_part(second, error) >= 0 && prepare(scene.frame),
          error.empty() ? "prepare Vulkan frame after static scene change"
                        : error.c_str());
    const viewer::VkSceneUploadCounters static_changed =
        renderer.upload_counters();
    CHECK(static_changed.vertex_uploads == transformed.vertex_uploads + 1 &&
              static_changed.cluster_uploads == transformed.cluster_uploads + 1 &&
              static_changed.command_layout_rebuilds ==
                  transformed.command_layout_rebuilds + 1,
          "new part uploads static buffers and rebuilds command layout once");

    viewer::FrameMatrices moved_camera = scene.frame;
    moved_camera.world_to_clip.m[0] *= 0.95f;
    CHECK(prepare(moved_camera),
          error.empty() ? "prepare camera-only Vulkan frame" : error.c_str());
    const viewer::VkSceneUploadCounters camera_changed =
        renderer.upload_counters();
    CHECK(camera_changed.instance_uploads == static_changed.instance_uploads &&
              camera_changed.vertex_uploads == static_changed.vertex_uploads &&
              camera_changed.cluster_uploads == static_changed.cluster_uploads &&
              camera_changed.command_layout_rebuilds ==
                  static_changed.command_layout_rebuilds,
          "camera-only frame leaves scene uploads and command layout intact");
}

void run_frame_record_tests(matter::VulkanDevice& vulkan) {
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    const matter::Mat4f identity = identity_matrix();
    const viewer::VkScenePart first = known_raster_triangle(972);
    const viewer::VkScenePart second = known_raster_triangle(973);
    CHECK(renderer.ensure_part(first, error) >= 0 &&
              renderer.ensure_part(second, error) >= 0 &&
              renderer.update_instances(
                  {{972, identity}, {972, identity}, {973, identity},
                   {973, identity}},
                  error),
          error.empty() ? "stage two active Vulkan raster parts" : error.c_str());

    const FixedCullScene scene = make_fixed_cull_scene();
    matter::VulkanFrame frame{};
    CHECK(vulkan.begin_frame(frame, error),
          error.empty() ? "begin asynchronous Vulkan record frame"
                        : error.c_str());
    if (frame.command_buffer == VK_NULL_HANDLE) return;
    bool dlss_output_evaluated = false;
    std::vector<matter::DlssMode> dlss_mode_transitions;
    renderer.set_test_dlss_bridge(matter::StreamlineBridge::test_fake_dlss(
        [&](VkCommandBuffer command_buffer, uint64_t token,
            const matter::DlssOptions& options,
            const matter::DlssConstants& constants,
            const matter::DlssResources& resources,
            matter::DlssEvaluationOutput& output, std::string&) {
            dlss_mode_transitions.push_back(options.mode);
            if (options.mode == matter::DlssMode::Native) return true;
            dlss_output_evaluated =
                command_buffer == frame.command_buffer && token == 100 &&
                options.mode == matter::DlssMode::Quality &&
                constants.motion_vectors_jittered && constants.reset &&
                constants.internal_extent.width < constants.output_extent.width &&
                resources.hdr.image != resources.depth.image &&
                resources.hdr.image != resources.velocity.image &&
                resources.hdr.image != resources.output.image;
            const VkClearColorValue clear{{1.0f, 0.0f, 0.0f, 1.0f}};
            const VkImageSubresourceRange range{
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(command_buffer, resources.output.image,
                                 VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
            output = {true, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT};
            return true;
        },
        [](const matter::DlssOptions& options,
           matter::DlssOptimalSettings& settings, std::string&) {
            settings = {{(options.output_extent.width * 2 + 2) / 3,
                         (options.output_extent.height * 2 + 2) / 3},
                        0.0f};
            return true;
        }));
    renderer.set_dlss_mode(matter::DlssMode::Quality);
    viewer::TemporalFrame dlss_temporal{};
    dlss_temporal.current_unjittered = scene.frame;
    dlss_temporal.previous_unjittered = scene.frame;
    dlss_temporal.current_jittered = scene.frame;
    dlss_temporal.previous_jittered = scene.frame;
    dlss_temporal.internal_extent = renderer.dlss_internal_extent(frame.extent);
    dlss_temporal.output_extent = frame.extent;
    dlss_temporal.reset = true;
    dlss_temporal.attempt_token = 100;
    renderer.set_temporal_frame(dlss_temporal);
    CHECK(renderer.prepare_frame(frame, scene.frame, scene.eye, 1.0f, error),
          error.empty() ? "prepare asynchronous Vulkan record frame"
                        : error.c_str());
    const uint64_t immediate_before = matter::immediate_submit_count();
    CHECK(renderer.record_cull_and_render(frame, scene.frame, scene.eye, 1.0f,
                                          error),
          error.empty() ? "record Vulkan cull and raster" : error.c_str());
    CHECK(renderer.record_composite_to_swapchain(frame, error) &&
              dlss_output_evaluated &&
              renderer.active_dlss_mode() == matter::DlssMode::Quality,
          error.empty() ? "fake DLSS output composites before presentation"
                        : error.c_str());
    const VkImage first_dlss_output =
        renderer.test_dlss_output_image(frame.frame_slot);
    CHECK(first_dlss_output != VK_NULL_HANDLE,
          "DLSS output exists for the acquired frame slot");
    const std::weak_ptr<void> first_dlss_lifetime =
        renderer.test_dlss_output_lifetime(frame.frame_slot);
    CHECK(renderer.test_replace_dlss_output(
              frame.frame_slot,
              {frame.extent.width + 8, frame.extent.height + 8}, error) &&
              renderer.test_dlss_output_image(frame.frame_slot) !=
                  first_dlss_output &&
              !first_dlss_lifetime.expired(),
          error.empty()
              ? "replaced DLSS output stays retained while frame is pending"
              : error.c_str());
    std::vector<uint8_t> dlss_composite_rgba;
    CHECK(vulkan.readback_swapchain_rgba8(frame, dlss_composite_rgba, error),
          error.empty() ? "queue fake DLSS output readback" : error.c_str());
    CHECK(matter::immediate_submit_count() == immediate_before,
          "production Vulkan record path performs no immediate submissions");
    const auto ranges = renderer.test_recorded_draw_ranges();
    CHECK(ranges.size() == 2,
          "one grouped indirect range per active part");
    CHECK(ranges.size() == 2 && ranges[0].command_count > 1 &&
              ranges[1].command_count > 1,
          "cluster LOD commands are grouped instead of submitted individually");
    CHECK(vulkan.end_frame(frame, error),
          error.empty() ? "submit asynchronous Vulkan record frame"
                        : error.c_str());
    CHECK(dlss_composite_rgba.size() >= 4 && dlss_composite_rgba[0] > 240 &&
              dlss_composite_rgba[1] < 10 && dlss_composite_rgba[2] < 10,
          "fake evaluator writes the image actually composited to swapchain");
    CHECK(!first_dlss_lifetime.expired(),
          "completed submission keeps replaced output until slot recycle");

    (void)renderer.cached_cull_stats();
    CHECK(matter::immediate_submit_count() == immediate_before,
          "cached cull stats query performs no immediate submission");
    const uint32_t recorded_slot = frame.frame_slot;
    bool submitted_native_frame = false;
    std::vector<uint8_t> native_composite_rgba;
    do {
        CHECK(vulkan.begin_frame(frame, error),
              error.empty() ? "begin deferred cull stats frame" : error.c_str());
        if (frame.command_buffer == VK_NULL_HANDLE) return;
        renderer.set_dlss_mode(submitted_native_frame
                                   ? matter::DlssMode::Quality
                                   : matter::DlssMode::Native);
        if (frame.frame_slot == recorded_slot) {
            CHECK(first_dlss_lifetime.expired(),
                  "slot recycle releases the replaced DLSS output");
        }
        CHECK(renderer.prepare_frame(frame, scene.frame, scene.eye, 1.0f, error) &&
                  renderer.record_cull_and_render(frame, scene.frame, scene.eye,
                                                  1.0f, error) &&
                  renderer.record_composite_to_swapchain(frame, error),
              error.empty() ? "submit deferred cull stats frame" : error.c_str());
        if (!submitted_native_frame) {
            CHECK(renderer.active_dlss_mode() == matter::DlssMode::Native &&
                      renderer.test_dlss_output_image(frame.frame_slot) ==
                          VK_NULL_HANDLE &&
                      renderer.dlss_reset_count() == 1 &&
                      renderer.consume_dlss_history_reset() &&
                      !renderer.consume_dlss_history_reset(),
                  "Native transition sends eOff, resets once, and allocates no DLSS output");
            CHECK(vulkan.readback_swapchain_rgba8(frame, native_composite_rgba,
                                                  error),
                  error.empty() ? "queue Native direct composite readback"
                                : error.c_str());
        } else if (frame.frame_slot == recorded_slot) {
            CHECK(renderer.active_dlss_mode() == matter::DlssMode::Quality &&
                      renderer.test_dlss_output_image(frame.frame_slot) !=
                          VK_NULL_HANDLE,
                  "return to Quality recreates a valid per-slot DLSS output");
        }
        CHECK(vulkan.end_frame(frame, error),
              error.empty() ? "submit deferred cull stats frame" : error.c_str());
        submitted_native_frame = true;
    } while (frame.frame_slot != recorded_slot);
    const std::vector<matter::DlssMode> expected_dlss_transitions{
        matter::DlssMode::Quality, matter::DlssMode::Native,
        matter::DlssMode::Quality};
    CHECK(dlss_mode_transitions == expected_dlss_transitions,
          "renderer routes Quality Native Quality through its Streamline bridge");
    CHECK(native_composite_rgba.size() >= 4 &&
              !(native_composite_rgba[0] > 240 &&
                native_composite_rgba[1] < 10 &&
                native_composite_rgba[2] < 10),
          "Native frame composites HDR directly instead of stale DLSS output");
    const viewer::VkCullStats stats_after = renderer.cached_cull_stats();
    CHECK(stats_after.emitted == 4 && stats_after.frustum_culled == 0 &&
              stats_after.hiz_culled == 0 && stats_after.overflowed == 0,
          "completed frame publishes the known deferred culling statistics");
    CHECK(matter::immediate_submit_count() == immediate_before,
          "deferred cull stats publication remains asynchronous");

    renderer.set_test_device_limits(4096, 4096, 4096, 1024, 0);
    CHECK(vulkan.begin_frame(frame, error) &&
              renderer.prepare_frame(frame, scene.frame, scene.eye, 1.0f,
                                     error) &&
              !renderer.record_cull_and_render(frame, scene.frame, scene.eye,
                                               1.0f, error) &&
              error.find("maxDrawIndirectCount") != std::string::npos,
          "failed cull recording leaves its deferred stats unpublished");
    const uint32_t failed_slot = frame.frame_slot;
    CHECK(vulkan.end_frame(frame, error),
          error.empty() ? "submit failed-recording Vulkan frame" : error.c_str());
    renderer.clear_test_device_limits(error);
    do {
        CHECK(vulkan.begin_frame(frame, error) &&
                  renderer.prepare_frame(frame, scene.frame, scene.eye, 1.0f,
                                         error),
              error.empty() ? "reuse deferred cull stats slot" : error.c_str());
        if (frame.frame_slot == failed_slot) {
            const viewer::VkCullStats after_failed = renderer.cached_cull_stats();
            CHECK(after_failed.emitted == 4 &&
                      after_failed.frustum_culled == 0 &&
                      after_failed.hiz_culled == 0 &&
                      after_failed.overflowed == 0,
                  "failed recording does not publish zeroed culling statistics");
        }
        CHECK(renderer.record_cull_and_render(frame, scene.frame, scene.eye,
                                              1.0f, error) &&
                  vulkan.end_frame(frame, error),
              error.empty() ? "submit deferred cull stats reuse frame"
                            : error.c_str());
    } while (frame.frame_slot != failed_slot);

    renderer.set_test_device_limits(4096, 4096, 4096, 1024, 3);
    CHECK(vulkan.begin_frame(frame, error) &&
              renderer.prepare_frame(frame, scene.frame, scene.eye, 1.0f,
                                     error) &&
              renderer.record_cull_and_render(frame, scene.frame, scene.eye,
                                              1.0f, error),
          error.empty() ? "record capped grouped indirect ranges" : error.c_str());
    const auto capped_ranges = renderer.test_recorded_draw_ranges();
    bool capped_and_contiguous = !capped_ranges.empty();
    uint32_t first_offset = std::numeric_limits<uint32_t>::max();
    uint32_t second_offset = std::numeric_limits<uint32_t>::max();
    for (const auto& range : capped_ranges) {
        capped_and_contiguous = capped_and_contiguous &&
                                range.command_count <= 3;
        uint32_t& expected =
            range.part_slot == 0 ? first_offset : second_offset;
        if (expected == std::numeric_limits<uint32_t>::max())
            expected = range.first_command;
        capped_and_contiguous = capped_and_contiguous &&
                                range.first_command == expected;
        expected += range.command_count;
    }
    CHECK(capped_and_contiguous && first_offset == viewer::kVkMaxLod &&
              second_offset == 2 * viewer::kVkMaxLod,
          "capped grouped ranges cover each active part contiguously");
    CHECK(vulkan.end_frame(frame, error),
          error.empty() ? "submit capped grouped indirect ranges" : error.c_str());
    renderer.reset();
    const viewer::VkCullStats reset_stats = renderer.cached_cull_stats();
    CHECK(reset_stats.emitted == 0 && reset_stats.frustum_culled == 0 &&
              reset_stats.hiz_culled == 0 && reset_stats.overflowed == 0,
          "renderer reset clears cached culling statistics");
}

void run_frame_resource_recovery_tests(matter::VulkanDevice& vulkan) {
    std::string error;
    const FixedCullScene scene = make_fixed_cull_scene();
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.ensure_part(scene.parts[0], error) >= 0 &&
              renderer.update_instances({scene.instances[0]}, error) &&
              renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error),
          error.empty() ? "prepare one-slot legacy Vulkan resources"
                        : error.c_str());

    matter::VulkanFrame frame{};
    CHECK(vulkan.begin_frame(frame, error),
          error.empty() ? "begin multi-slot Vulkan frame" : error.c_str());
    if (frame.command_buffer == VK_NULL_HANDLE) return;
    renderer.set_test_frame_resource_failure(2);
    CHECK(!renderer.prepare_frame(frame, scene.frame, scene.eye, 1.0f, error) &&
              error.find("forced frame resource allocation failure") !=
                  std::string::npos,
          "partial frame-resource allocation fails before committing slots");
    renderer.set_test_frame_resource_failure(
        std::numeric_limits<uint32_t>::max());
    CHECK(renderer.prepare_frame(frame, scene.frame, scene.eye, 1.0f, error),
          error.empty() ? "frame-resource allocation retry succeeds"
                        : error.c_str());
    CHECK(vulkan.end_frame(frame, error),
          error.empty() ? "end recovered multi-slot Vulkan frame"
                        : error.c_str());
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
    const VkDeviceSize initial_cluster_bytes = renderer.cluster_buffer_size();
    const VkDeviceSize initial_command_bytes = renderer.command_buffer_size();
    const VkDeviceSize initial_transform_bytes =
        renderer.draw_transform_buffer_size();

    const viewer::VkSceneInstance second_near{
        77, viewer::mat4_translation({0.0f, 0.0f, -2.2f})};
    CHECK(renderer.update_instances(
              {near_instance, second_near, far_instance}, error),
          error.empty() ? "stage transform-region overflow" : error.c_str());
    CHECK(renderer.set_test_command_first_instance(1, 1, error),
          error.empty() ? "shrink first transform bucket" : error.c_str());
    CHECK(!renderer.set_test_command_first_instance(
              1, std::numeric_limits<uint32_t>::max(), error) &&
              error.find("transform region") != std::string::npos,
          "renderer rejects an invalid command transform offset");
    CHECK(!renderer.set_test_command_first_instance(2, 0, error) &&
              error.find("monotonic") != std::string::npos,
          "renderer rejects a bounded decreasing command transform offset");
    CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error),
          error.empty() ? "dispatch reduced-capacity culling" : error.c_str());
    CHECK(renderer.readback_commands(commands, error),
          error.empty() ? "read reduced-capacity commands" : error.c_str());
    CHECK(renderer.readback_draw_transforms(transforms, error),
          error.empty() ? "read reduced-capacity transforms" : error.c_str());
    viewer::VkCullStats overflow_stats{};
    CHECK(renderer.cull_stats(overflow_stats, error),
          error.empty() ? "read reduced-capacity stats" : error.c_str());
    CHECK(commands[0].instance_count == 1 && commands[1].instance_count == 1,
          "reduced command region counts only successful writes");
    CHECK(overflow_stats.emitted == 2 && overflow_stats.overflowed == 1,
          "reduced region reports deterministic overflow without spill");
    CHECK(std::fabs(transforms[commands[1].first_instance].elements[14] + 5.0f) <
              1e-5f,
          "overflow cannot overwrite adjacent bucket transform");
    renderer.release_part(77);
    std::vector<viewer::VkSceneRenderer::RtInstance> rt_instances;
    CHECK(renderer.fill_rt_instances(rt_instances) == 3,
          "release keeps the coherent uploaded RT snapshot until dispatch");
    CHECK(renderer.ensure_part(part, error) >= 0,
          error.empty() ? "re-add part without reset" : error.c_str());
    CHECK(renderer.update_instances({near_instance}, error),
          error.empty() ? "stage re-added part" : error.c_str());
    CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error),
          error.empty() ? "dispatch re-added part" : error.c_str());
    CHECK(renderer.draw_command_count() == viewer::kVkMaxLod,
          "re-add without reset reclaims command storage");

    viewer::VkScenePart mixed{88, {cluster, cluster, cluster}};
    mixed.clusters[1].lods.resize(1);
    mixed.clusters[2].lods.push_back({6, 3, -1.0f});
    CHECK(renderer.ensure_part(mixed, error) >= 0,
          error.empty() ? "ensure mixed-size part" : error.c_str());
    const viewer::VkSceneInstance mixed_instance{
        88, viewer::mat4_translation({0.0f, 0.0f, -3.0f})};
    CHECK(renderer.update_instances({near_instance, mixed_instance}, error),
          error.empty() ? "upload mixed-size instances" : error.c_str());
    CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error),
          error.empty() ? "dispatch mixed-size culling" : error.c_str());
    CHECK(renderer.cluster_count() == 4,
          "mixed uploaded scene has only active clusters");
    CHECK(renderer.draw_command_count() == 4 * viewer::kVkMaxLod,
          "mixed live scene command count is bounded by active clusters");
    viewer::VkCullStats mixed_stats{};
    CHECK(renderer.cull_stats(mixed_stats, error),
          error.empty() ? "read mixed-size stats" : error.c_str());
    CHECK(mixed_stats.emitted == 4,
          "all mixed-size live clusters cull and emit correctly");
    const VkDeviceSize stable_cluster_bytes = renderer.cluster_buffer_size();
    const VkDeviceSize stable_command_bytes = renderer.command_buffer_size();
    const VkDeviceSize stable_transform_bytes =
        renderer.draw_transform_buffer_size();
    CHECK(stable_cluster_bytes > initial_cluster_bytes &&
              stable_command_bytes > initial_command_bytes &&
              stable_transform_bytes > initial_transform_bytes,
          "scene buffers grow safely across reallocations");

    for (int cycle = 0; cycle < 4; ++cycle) {
        renderer.release_part(77);
        CHECK(renderer.cluster_count() == 4,
              "release keeps uploaded cluster count coherent until dispatch");
        CHECK(renderer.ensure_part(part, error) >= 0,
              error.empty() ? "re-add churn part" : error.c_str());
        CHECK(renderer.update_instances({mixed_instance, near_instance}, error),
              error.empty() ? "upload churn instances" : error.c_str());
        CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error),
              error.empty() ? "dispatch churn culling" : error.c_str());
        CHECK(renderer.cull_stats(mixed_stats, error),
              error.empty() ? "read churn stats" : error.c_str());
        CHECK(mixed_stats.emitted == 4,
              "slot remapping preserves culling after release and re-add");
        CHECK(renderer.readback_commands(commands, error),
              error.empty() ? "read churn commands" : error.c_str());
        CHECK(renderer.readback_draw_transforms(transforms, error),
              error.empty() ? "read churn transforms" : error.c_str());
        std::vector<viewer::VkSceneRenderer::RtInstance> churn_rt;
        CHECK(renderer.fill_rt_instances(churn_rt) == 2 &&
                  churn_rt[0].part_hash == 88 && churn_rt[1].part_hash == 77 &&
                  rt_matrix_equal(churn_rt[0].transform,
                                  mixed_instance.object_to_world) &&
                  rt_matrix_equal(churn_rt[1].transform,
                                  near_instance.object_to_world),
              "churn preserves exact surviving RT instances");
        const uint32_t expected_buckets[] = {1, 9, 19, 27};
        bool churn_commands_exact = commands.size() == 4 * viewer::kVkMaxLod;
        for (size_t bucket = 0; bucket < commands.size(); ++bucket) {
            bool expected = false;
            for (uint32_t expected_bucket : expected_buckets)
                expected = expected || bucket == expected_bucket;
            churn_commands_exact =
                churn_commands_exact &&
                commands[bucket].instance_count == (expected ? 1u : 0u);
            if (expected) {
                churn_commands_exact =
                    churn_commands_exact &&
                    gpu_matrix_equal(
                        transforms[commands[bucket].first_instance],
                        bucket == 27 ? near_instance.object_to_world
                                     : mixed_instance.object_to_world);
            }
        }
        CHECK(churn_commands_exact,
              "churn preserves exact command buckets and transforms");
        CHECK(renderer.cluster_count() == 4,
              "streaming eviction/reload keeps cluster residency bounded");
        CHECK(renderer.draw_command_count() == 4 * viewer::kVkMaxLod,
              "streaming eviction/reload keeps command residency bounded");
        CHECK(renderer.cluster_buffer_size() == stable_cluster_bytes &&
                  renderer.command_buffer_size() == stable_command_bytes &&
                  renderer.draw_transform_buffer_size() ==
                      stable_transform_bytes,
              "streaming eviction/reload re-uploads into stable scene buffers");
    }

    renderer.reset();
    CHECK(renderer.draw_command_count() == 0, "reset clears command storage");
}

void run_vk_scene_checked_size_tests(matter::VulkanDevice& vulkan) {
    std::string error;
    VkDeviceSize bytes = 0;
    CHECK(viewer::vk_scene_detail::checked_mul_to_device_size(
              7, sizeof(uint32_t), bytes, "test values", error) &&
              bytes == 7 * sizeof(uint32_t),
          "checked byte sizing accepts a small product");
    CHECK(!viewer::vk_scene_detail::checked_mul_to_device_size(
              std::numeric_limits<size_t>::max(), 2, bytes,
              "overflow values", error) &&
              error.find("overflow values") != std::string::npos,
          "checked byte sizing rejects multiplication overflow");

    VkDeviceSize capacity = 0;
    CHECK(viewer::vk_scene_detail::checked_grown_capacity(
              16, 65, 100, capacity, "test storage", error) &&
              capacity == 100,
          "buffer growth caps the final allocation at the device limit");
    CHECK(!viewer::vk_scene_detail::checked_grown_capacity(
              16, 101, 100, capacity, "test storage", error) &&
              error.find("device limit") != std::string::npos,
          "buffer growth rejects a required range beyond the device limit");

    uint32_t groups = 0;
    CHECK(viewer::vk_scene_detail::checked_dispatch_groups(
              128, 33, 100, groups, error) && groups == 66,
          "checked dispatch sizing accepts a bounded mixed product");
    CHECK(!viewer::vk_scene_detail::checked_dispatch_groups(
              128, 33, 65, groups, error) &&
              error.find("maxComputeWorkGroupCount") != std::string::npos,
          "checked dispatch sizing rejects a forced small group limit");
    CHECK(!viewer::vk_scene_detail::checked_dispatch_groups(
              std::numeric_limits<uint32_t>::max(),
              std::numeric_limits<uint32_t>::max(),
              std::numeric_limits<uint32_t>::max(), groups, error),
          "checked dispatch sizing rejects group-count narrowing");

    int public_count = 0;
    CHECK(viewer::vk_scene_detail::checked_size_to_int(
              static_cast<size_t>(std::numeric_limits<int>::max()),
              public_count, "RT instance count", error) &&
              public_count == std::numeric_limits<int>::max(),
          "public count conversion accepts INT_MAX");
    CHECK(!viewer::vk_scene_detail::checked_size_to_int(
              static_cast<size_t>(std::numeric_limits<int>::max()) + 1u,
              public_count, "RT instance count", error) &&
              error.find("INT_MAX") != std::string::npos,
          "public count conversion rejects INT_MAX plus one");

    {
        viewer::VkSceneRenderer renderer(vulkan);
        renderer.set_test_device_limits(64, 4096, 4096, 1024, 1024);
        CHECK(!renderer.init(error) &&
                  error.find("storage buffer range") != std::string::npos,
              "renderer rejects forced maxStorageBufferRange before allocation");
    }
    {
        viewer::VkSceneRenderer renderer(vulkan);
        renderer.set_test_device_limits(4096, 128, 4096, 1024, 1024);
        CHECK(!renderer.init(error) &&
                  error.find("uniform buffer range") != std::string::npos,
              "renderer rejects forced maxUniformBufferRange before allocation");
    }
    {
        viewer::VkSceneRenderer renderer(vulkan);
        renderer.set_test_device_limits(4096, 4096, 64, 1024, 1024);
        CHECK(!renderer.init(error) &&
                  error.find("Vulkan device limit") != std::string::npos,
              "renderer rejects forced maxBufferSize before allocation");
    }

    const FixedCullScene scene = make_fixed_cull_scene();
    {
        viewer::VkSceneRenderer renderer(vulkan);
        CHECK(renderer.init(error), "init renderer before post-init limit faults");
        CHECK(renderer.ensure_part(scene.parts[0], error) >= 0,
              "ensure baseline part before growth fault");
        CHECK(renderer.update_instances({scene.instances[0]}, error),
              "stage baseline instance before growth fault");
        CHECK(renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error),
              "dispatch baseline before growth fault");
        const VkBuffer old_indirect = renderer.indirect_buffer();
        const VkDeviceSize old_cluster_bytes = renderer.cluster_buffer_size();
        const VkDeviceSize old_command_bytes = renderer.command_buffer_size();
        const VkDeviceSize old_transform_bytes =
            renderer.draw_transform_buffer_size();
        std::vector<viewer::DrawCommand> baseline_commands;
        CHECK(renderer.readback_commands(baseline_commands, error),
              "read baseline commands before growth fault");
        std::vector<viewer::VkSceneRenderer::RtInstance> baseline_rt;
        CHECK(renderer.fill_rt_instances(baseline_rt) == 1,
              "read baseline RT snapshot before growth fault");
        CHECK(renderer.ensure_part(scene.parts[1], error) >= 0,
              "stage larger scene under normal limits");
        CHECK(renderer.update_instances({scene.instances[0], scene.instances[1]},
                                        error),
              "stage larger instance set under normal limits");
        renderer.set_test_device_limits(256, 4096, 4096, 1024, 1024);
        CHECK(!renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error) &&
                  error.find("maxStorageBufferRange") != std::string::npos,
              "post-init storage limit rejects actual scene-buffer growth");
        CHECK(renderer.indirect_buffer() == old_indirect &&
                  renderer.cluster_buffer_size() == old_cluster_bytes &&
                  renderer.command_buffer_size() == old_command_bytes &&
                  renderer.draw_transform_buffer_size() == old_transform_bytes,
              "failed growth preserves prior renderer buffers");
        renderer.clear_test_device_limits(error);
        CHECK(renderer.draw_command_count() == baseline_commands.size(),
              "failed growth preserves uploaded indirect command count");
        std::vector<viewer::VkSceneRenderer::RtInstance> preserved_rt;
        CHECK(renderer.cluster_count() == 1 &&
                  renderer.fill_rt_instances(preserved_rt) == 1 &&
                  preserved_rt[0].part_hash == baseline_rt[0].part_hash &&
                  rt_matrix_equal(preserved_rt[0].transform,
                                  scene.instances[0].object_to_world),
              "failed preflight preserves coherent uploaded raster and RT scene");
        std::vector<viewer::DrawCommand> preserved_commands;
        CHECK(renderer.readback_commands(preserved_commands, error) &&
                  preserved_commands == baseline_commands,
              "failed growth preserves uploaded indirect command contents");

        renderer.set_test_device_limits(4096, 4096, 256, 1024, 1024);
        CHECK(!renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error) &&
                  error.find("maxBufferSize") != std::string::npos,
              "post-init maxBufferSize rejects actual scene-buffer growth");
        CHECK(renderer.indirect_buffer() == old_indirect &&
                  renderer.command_buffer_size() == old_command_bytes &&
                  renderer.draw_command_count() == baseline_commands.size(),
              "failed maxBufferSize growth preserves prior renderer state");
        renderer.clear_test_device_limits(error);
        CHECK(renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error),
              error.empty() ? "renderer recovers after failed growth"
                            : error.c_str());
        CHECK(renderer.command_buffer_size() > old_command_bytes,
              "recovered dispatch performs deferred buffer growth");

        renderer.set_test_device_limits(4096, 4096, 4096, 0, 1024);
        CHECK(!renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error) &&
                  error.find("maxComputeWorkGroupCount") != std::string::npos,
              "dispatch_culling enforces forced compute group limit");
        renderer.clear_test_device_limits(error);

        renderer.set_test_device_limits(4096, 4096, 4096, 1024, 1);
        CHECK(renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error),
              error.empty()
                  ? "per-call drawCount=1 accepts maxDrawIndirectCount=1"
                  : error.c_str());
        renderer.set_test_device_limits(4096, 4096, 4096, 1024, 0);
        CHECK(!renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error) &&
                  error.find("maxDrawIndirectCount") != std::string::npos,
              "drawCount=1 rejects maxDrawIndirectCount=0");
        renderer.clear_test_device_limits(error);
    }

    {
        viewer::VkSceneRenderer renderer(vulkan);
        CHECK(renderer.init(error), "init renderer before replacement fault");
        CHECK(renderer.ensure_part(scene.parts[0], error) >= 0 &&
                  renderer.update_instances({scene.instances[0]}, error) &&
                  renderer.dispatch_culling(scene.frame, scene.eye, 1.0f,
                                             error),
              error.empty() ? "establish replacement-fault baseline"
                            : error.c_str());
        CHECK(renderer.ensure_part(scene.parts[1], error) >= 0 &&
                  renderer.update_instances(
                      {scene.instances[0], scene.instances[1]}, error),
              error.empty() ? "stage replacement-fault growth"
                            : error.c_str());
        renderer.set_test_scene_failure(1,
            std::numeric_limits<uint32_t>::max());
        CHECK(!renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error) &&
                  error.find("poisoned after partial GPU mutation") !=
                      std::string::npos &&
                  error.find("replacement") != std::string::npos,
              "later replacement failure poisons renderer");
        const std::string poison_reason = error;
        CHECK(renderer.indirect_buffer() == VK_NULL_HANDLE &&
                  renderer.draw_transform_buffer() == VK_NULL_HANDLE &&
                  renderer.draw_command_count() == 0 &&
                  renderer.cluster_count() == 0 &&
                  renderer.cluster_buffer_size() == 0 &&
                  renderer.command_buffer_size() == 0 &&
                  renderer.draw_transform_buffer_size() == 0,
              "poisoned renderer exposes no draw buffers or counts");
        std::vector<viewer::DrawCommand> poisoned_commands(1);
        CHECK(!renderer.readback_commands(poisoned_commands, error) &&
                  poisoned_commands.empty() && error == poison_reason,
              "poisoned command readback fails with stable diagnostic");
        viewer::VkCullStats poisoned_stats{};
        CHECK(!renderer.cull_stats(poisoned_stats, error) &&
                  error == poison_reason,
              "poisoned stats readback fails with stable diagnostic");
        std::vector<viewer::VkSceneRenderer::RtInstance> poisoned_rt(1);
        CHECK(renderer.fill_rt_instances(poisoned_rt) == 0 &&
                  poisoned_rt.empty(),
              "poisoned renderer exposes no RT instances");
        CHECK(!renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error) &&
                  error == poison_reason && !renderer.init(error) &&
                  error == poison_reason,
              "poison is terminal with a stable diagnostic");
        CHECK(renderer.ensure_part(scene.parts[0], error) == -1 &&
                  error == poison_reason &&
                  !renderer.update_instances({scene.instances[0]}, error) &&
                  error == poison_reason,
              "poison blocks scene mutation with the stable diagnostic");
        renderer.reset();
        CHECK(renderer.init(error),
              error.empty() ? "full reset permits renderer reinitialization"
                            : error.c_str());
        CHECK(renderer.ensure_part(scene.parts[0], error) >= 0 &&
                  renderer.update_instances({scene.instances[0]}, error) &&
                  renderer.dispatch_culling(scene.frame, scene.eye, 1.0f,
                                             error),
              error.empty() ? "renderer works after full reset and reinit"
                            : error.c_str());
    }

    {
        viewer::VkSceneRenderer renderer(vulkan);
        CHECK(renderer.init(error), "init renderer before upload fault");
        CHECK(renderer.ensure_part(scene.parts[0], error) >= 0 &&
                  renderer.ensure_part(scene.parts[1], error) >= 0 &&
                  renderer.update_instances(
                      {scene.instances[0], scene.instances[1]}, error) &&
                  renderer.dispatch_culling(scene.frame, scene.eye, 1.0f,
                                             error),
              error.empty() ? "establish upload-fault buffers"
                            : error.c_str());
        renderer.set_test_scene_failure(
            std::numeric_limits<uint32_t>::max(), 1);
        CHECK(!renderer.dispatch_culling(scene.frame, scene.eye, 1.0f, error) &&
                  error.find("poisoned after partial GPU mutation") !=
                      std::string::npos &&
                  error.find("upload") != std::string::npos,
              "later upload failure poisons renderer");
        const std::string poison_reason = error;
        std::vector<viewer::GpuMat4> poisoned_transforms(1);
        CHECK(!renderer.readback_draw_transforms(poisoned_transforms, error) &&
                  poisoned_transforms.empty() && error == poison_reason &&
                  renderer.indirect_buffer() == VK_NULL_HANDLE &&
                  renderer.draw_transform_buffer() == VK_NULL_HANDLE &&
                  renderer.draw_command_count() == 0,
              "upload-poisoned renderer fails closed with stable diagnostic");
    }
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
    run_vulkan_gi_math_tests();
    run_raster_mesh_material_contract_tests();
    run_ray_tracing_capability_contract_tests();
    run_vulkan_instance_cache_tests();
    run_vulkan_temporal_tests();
    run_vulkan_gi_temporal_sequence_tests();
    run_streamline_bridge_fallback_tests();
    run_dlss_bridge_contract_tests();
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

    const char* requested_smoke_mode = std::getenv("MATTER_VK_SMOKE_MODE");
    if (requested_smoke_mode &&
        std::string(requested_smoke_mode) == "rt-unavailable") {
        _putenv_s("MATTER_VK_TEST_FORCE_RT_UNAVAILABLE", "1");
    }

    std::string error;
    auto vulkan =
        window ? matter::VulkanDevice::create(window, true, error) : nullptr;
    CHECK(vulkan != nullptr, error.empty() ? "create Vulkan device" : error.c_str());

    if (vulkan) {
        {
            viewer::VkSceneRenderer device_bridge_renderer(*vulkan);
            CHECK(device_bridge_renderer.test_uses_device_streamline_bridge(),
                  "scene renderer uses the Vulkan device-owned Streamline bridge");
        }
        run_streamline_presentation_funnel_tests(*vulkan);
        CHECK(!vulkan->dlss_available(),
              "Vulkan device reports DLSS unavailable without Streamline");
        std::printf("DLSS fallback: %s\n",
                    vulkan->dlss_unavailable_reason().c_str());
        CHECK(!vulkan->dlss_unavailable_reason().empty() &&
                  vulkan->dlss_unavailable_reason().find("Streamline") !=
                      std::string::npos,
              "Vulkan device exposes the Streamline fallback reason");
        const std::string active_smoke_mode =
            requested_smoke_mode ? requested_smoke_mode : "";
        if (active_smoke_mode == "streamline-missing-instance-proxy" ||
            active_smoke_mode == "streamline-missing-device-proxy") {
            CHECK(vulkan->dlss_unavailable_reason().find("retried native") !=
                      std::string::npos,
                  "missing Streamline proxy tears down and retries native Vulkan");
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        CHECK(vulkan->draw_indirect_first_instance_enabled(),
              "drawIndirectFirstInstance is enabled on the logical device");
        CHECK(vulkan->multi_draw_indirect_enabled(),
              "multiDrawIndirect is enabled on the logical device");
        const char* smoke_mode = std::getenv("MATTER_VK_SMOKE_MODE");
        if (smoke_mode && std::string(smoke_mode) == "rt-unavailable") {
            run_forced_ray_tracing_unavailable_path(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
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
            run_frame_upload_tests(*vulkan);
            run_frame_record_tests(*vulkan);
            run_frame_resource_recovery_tests(*vulkan);
            run_vk_scene_checked_size_tests(*vulkan);
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
        if (smoke_mode &&
            (std::string(smoke_mode) == "raster" ||
             std::string(smoke_mode) == "rt-disabled")) {
            run_raster_path(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "rt") {
            run_native_ray_tracing_path(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "raster-fault") {
            run_raster_submission_fault(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "interop") {
            run_cuda_vulkan_interop(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode &&
            std::string(smoke_mode).rfind("interop-fault-", 0) == 0) {
            const bool preserve_process = run_cuda_vulkan_interop_fault(
                *vulkan, std::string(smoke_mode).substr(14).c_str());
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            if (preserve_process) {
                CHECK(vulkan->validation_error_count() == 0,
                      "unproven completion reports no Vulkan validation errors");
                const int result = check_summary();
                std::fflush(stdout);
                std::fflush(stderr);
                // ExitProcess runs DLL detach and the NVIDIA driver may block
                // trying to tear down the deliberately preserved live CUDA
                // context. TerminateProcess is the test's explicit proof that
                // OS cleanup, not unsafe API destruction, owns this terminal
                // unproven-completion case.
                TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
                std::abort();
            }
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "handle-diag-vulkan") {
            run_vulkan_only_handle_diagnostic(*vulkan);
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }

        uint32_t retained_probe_destroyed = 0;
        run_frame_upload_tests(*vulkan);
        run_frame_record_tests(*vulkan);
        run_frame_resource_recovery_tests(*vulkan);
        for (int i = 0; i < 3; ++i) {
            matter::VulkanFrame frame{};
            const bool began = vulkan->begin_frame(frame, error);
            CHECK(began, error.empty() ? "begin frame" : error.c_str());
            if (!began) break;

            if (i == 0) {
                CHECK(frame.frame_slot_count == 2,
                      "Vulkan frame reports the configured two slots in flight");
                CHECK(frame.frame_slot < frame.frame_slot_count,
                      "Vulkan frame slot identity is in range");

                auto probe = std::make_shared<RetainProbe>();
                probe->destroyed = &retained_probe_destroyed;
                std::vector<std::shared_ptr<void>> retained{probe};
                CHECK(vulkan->retain_for_frame(frame, std::move(retained), error),
                      error.empty() ? "retain active-frame dependency"
                                    : error.c_str());
                probe.reset();
                CHECK(retained_probe_destroyed == 0,
                      "active frame owns retained dependency");
            } else if (i == 2) {
                CHECK(retained_probe_destroyed == 1,
                      "retained dependency releases when its frame slot is reused");
            }

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
            vulkan->test_clear_presentation_events();
            const bool began_resized = vulkan->begin_frame(resized, error);
            CHECK(began_resized,
                  error.empty() ? "begin resized frame" : error.c_str());
            if (began_resized) {
                CHECK(resized.swapchain_recreated,
                      "resize recreates the swapchain once");
                const bool ended_resized = vulkan->end_frame(resized, error);
                CHECK(ended_resized,
                      error.empty() ? "end resized frame" : error.c_str());

                const auto& resize_events = vulkan->test_presentation_events();
                const auto contains_resize_event = [&resize_events](const char* event) {
                    return std::find(resize_events.begin(), resize_events.end(),
                                     event) != resize_events.end();
                };
                CHECK(contains_resize_event("device_wait_idle") &&
                          contains_resize_event("destroy_swapchain") &&
                          contains_resize_event("create_swapchain"),
                      "actual resize routes idle and swapchain recreation through bridge");

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
