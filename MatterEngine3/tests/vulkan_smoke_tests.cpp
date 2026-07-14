#include "check.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "matter/vulkan_device.h"
#include "render/gpu_matrix_pack.h"
#include "render/matrix_math.h"
#include "render/vk_cuda_interop.h"
#include "render/vk_device_internal.h"
#include "render/vk_instance_cache.h"
#include "render/vk_pipeline.h"
#include "render/vk_resources.h"
#include "render/vk_scene_renderer.h"
#include "provider/sector_resolver.h"

namespace {

bool close4(matter::Float4 actual, matter::Float4 expected, float epsilon);

struct RetainProbe {
    uint32_t* destroyed = nullptr;
    ~RetainProbe() { ++*destroyed; }
};

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

void run_vulkan_instance_cache_tests() {
    viewer::ResolvedInstance a{};
    a.part_hash = 11;
    a.segment = 0;
    a.transform[0] = a.transform[5] = a.transform[10] = a.transform[15] = 1.0f;
    viewer::ResolvedInstance b = a;
    b.part_hash = 12;
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

viewer::VkScenePart fixed_part(uint64_t hash, matter::Float3 minimum,
                               matter::Float3 maximum,
                               uint32_t first_vertex);

viewer::VkScenePart known_raster_triangle(uint64_t hash,
                                          float emission = 5.0f) {
    viewer::VkScenePart part = fixed_part(
        hash, {-0.75f, -0.75f, -2.0f}, {0.75f, 0.75f, -2.0f}, 0);
    const matter::Float3 normal{0.0f, 1.0f, 0.0f};
    const matter::Float4 albedo{0.25f, 0.5f, 0.75f, 1.0f};
    const matter::Float4 orm{0.2f, 0.7f, 0.4f,
                             viewer::vulkan_encode_emission(emission)};
    part.vertices = {
        {{-0.75f, -0.75f, -2.0f}, normal, albedo, orm},
        {{0.75f, -0.75f, -2.0f}, normal, albedo, orm},
        {{0.0f, 0.75f, -2.0f}, normal, albedo, orm},
    };
    return part;
}

void run_raster_path(matter::VulkanDevice& vulkan) {
    constexpr uint32_t width = 160;
    constexpr uint32_t height = 160;
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
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
    CHECK(renderer.ensure_part(dummy, error) >= 0,
          error.empty() ? "ensure raster dummy part" : error.c_str());
    CHECK(renderer.ensure_part(triangle, error) >= 0,
          error.empty() ? "ensure known raster triangle" : error.c_str());

    const matter::Mat4f identity = identity_matrix();
    CHECK(renderer.update_instances({{900, identity}, {901, identity}}, error),
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
    CHECK(renderer.raster_draw_command_count() == 1,
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
    CHECK(attachments.depth.format == VK_FORMAT_D32_SFLOAT,
          "depth attachment format");
    CHECK(attachments.hdr.format == VK_FORMAT_R16G16B16A16_SFLOAT,
          "HDR attachment format");
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
                 {0.2f, 0.7f, 0.4f, 1.0f},
                 6e-3f),
          "known center ORM with reserved alpha default");
    CHECK(std::isfinite(center.depth) && center.depth >= 0.0f &&
              center.depth <= 1.0f,
          "known center Vulkan depth range");
    CHECK(std::fabs(center.depth - 0.959596f) <= 2e-3f,
          "known center projected depth");
    CHECK(lower_right_inside.albedo.w > 0.99f,
          "negative-height viewport preserves top-left framebuffer convention");
    CHECK(background.albedo.w < 0.01f && background.depth >= 0.999f,
          "background color and depth remain clear");
    CHECK(std::isfinite(background.hdr.x) &&
              std::isfinite(background.hdr.y) &&
              std::isfinite(background.hdr.z) &&
              std::isfinite(background.hdr.w),
          "cleared background produces finite HDR");
    CHECK(close4(background.hdr, {0.0f, 0.0f, 0.0f, 1.0f}, 2e-3f),
          "cleared background produces deterministic black HDR");
    CHECK(center.hdr.x > background.hdr.x &&
              center.hdr.y > background.hdr.y &&
              center.hdr.z > background.hdr.z,
          "composite samples G-buffer into HDR output");

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

    renderer.release_part(901);
    CHECK(renderer.ensure_part(known_raster_triangle(901, 1000.0f), error) >= 0 &&
              renderer.update_instances({{900, identity}, {901, identity}}, error) &&
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

    renderer.release_part(901);
    CHECK(renderer.ensure_part(known_raster_triangle(
              901, std::numeric_limits<float>::max()), error) >= 0 &&
              renderer.update_instances({{900, identity}, {901, identity}}, error) &&
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

    renderer.release_part(901);
    CHECK(renderer.ensure_part(known_raster_triangle(901, 5.0f), error) >= 0 &&
              renderer.update_instances({{900, identity}, {901, identity}},
                                        error) &&
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
    CHECK(renderer.ensure_part(first, error) >= 0,
          error.empty() ? "ensure persistent Vulkan part" : error.c_str());
    std::vector<viewer::VkSceneInstance> instances{{970, identity},
                                                    {970, identity}};
    CHECK(renderer.update_instances(instances, error),
          error.empty() ? "upload persistent Vulkan instances" : error.c_str());

    const FixedCullScene scene = make_fixed_cull_scene();
    const auto prepare = [&](const viewer::FrameMatrices& matrices) {
        matter::VulkanFrame frame{};
        if (!vulkan.begin_frame(frame, error)) return false;
        const bool prepared = renderer.prepare_frame(frame, matrices, scene.eye,
                                                     1.0f, error);
        const bool ended = vulkan.end_frame(frame, error);
        return prepared && ended;
    };

    CHECK(prepare(scene.frame),
          error.empty() ? "prepare initial persistent Vulkan frame"
                        : error.c_str());
    const viewer::VkSceneUploadCounters warm = renderer.upload_counters();

    // Warm the second slot. Reusing the first slot below must leave all
    // scene uploads unchanged for identical CPU scene data.
    CHECK(renderer.update_instances(instances, error) && prepare(scene.frame),
          error.empty() ? "prepare second persistent Vulkan slot"
                        : error.c_str());
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
    CHECK(renderer.prepare_frame(frame, scene.frame, scene.eye, 1.0f, error),
          error.empty() ? "prepare asynchronous Vulkan record frame"
                        : error.c_str());
    const uint64_t immediate_before = matter::immediate_submit_count();
    CHECK(renderer.record_cull_and_render(frame, scene.frame, scene.eye, 1.0f,
                                          error),
          error.empty() ? "record Vulkan cull and raster" : error.c_str());
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
    run_vulkan_instance_cache_tests();
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
        CHECK(vulkan->draw_indirect_first_instance_enabled(),
              "drawIndirectFirstInstance is enabled on the logical device");
        CHECK(vulkan->multi_draw_indirect_enabled(),
              "multiDrawIndirect is enabled on the logical device");
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
        if (smoke_mode && std::string(smoke_mode) == "raster") {
            run_raster_path(*vulkan);
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
