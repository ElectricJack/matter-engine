#include "check.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "matter/vulkan_device.h"
#include "matter/display_dither.h"
#include "render/gpu_matrix_pack.h"
#include "render/lod_distance.h"
#include "render/matrix_math.h"
#include "render/raster_mesh.h"
#include "render/streamline_bridge.h"
#include "render/vk_temporal.h"
#include "render/vk_gi_math.h"
#include "render/vk_device_internal.h"
#include "render/vk_instance_cache.h"
#include "render/vk_lighting_controls.h"
#include "render/vk_pipeline.h"
#include "render/vk_resources.h"
#include "render/vk_scene_renderer.h"
#include "render/vk_volumetrics.h"
#include "render/vk_atmosphere.h"
#include "render/vk_cloud_shadows.h"
#include "matter/atmosphere_lighting.h"
#include "provider/sector_resolver.h"
#include "tileset_gtex.h"
#include "../../MatterEditor/src/ui.h"
#include "../../MatterEditor/src/viewer_commands.h"
// LAST on purpose: impostor_bake.h reaches precomp.h, whose `using namespace
// std;` makes `byte` ambiguous inside any <windows.h> pulled in after it.
#include "impostor_bake.h"   // M2.5 kQuadMarker, the billboard sentinel

namespace {

void test_atmosphere_acceptance_fifo_parser_and_present_sequencer() {
    {
        const auto parsed = viewer::parse_fifo_line("render_path raster");
        const auto* command =
            std::get_if<viewer::FifoRenderPath>(&parsed.command);
        CHECK(parsed.recognized && parsed.success && command &&
                  command->requested == matter::RenderPath::GpuDriven,
              "FIFO parser types render_path raster");
    }
    {
        const auto parsed = viewer::parse_fifo_line("render_path native_rt");
        const auto* command =
            std::get_if<viewer::FifoRenderPath>(&parsed.command);
        CHECK(parsed.recognized && parsed.success && command &&
                  command->requested == matter::RenderPath::Raytrace,
              "FIFO parser types render_path native_rt");
    }
    {
        const auto parsed = viewer::parse_fifo_line("history_reset");
        CHECK(parsed.recognized && parsed.success &&
                  std::holds_alternative<viewer::FifoHistoryReset>(
                      parsed.command),
              "FIFO parser types history_reset");
    }
    {
        const auto parsed = viewer::parse_fifo_line("wait_frames 3");
        const auto* command =
            std::get_if<viewer::FifoWaitFrames>(&parsed.command);
        CHECK(parsed.recognized && parsed.success && command &&
                  command->count == 3u,
              "FIFO parser types a positive wait_frames count");
    }
    {
        const auto parsed =
            viewer::parse_fifo_line("shot_now C:\\absolute\\frame.png");
        const auto* command =
            std::get_if<viewer::FifoScreenshotNow>(&parsed.command);
        CHECK(parsed.recognized && parsed.success && command &&
                  command->path == "C:\\absolute\\frame.png",
              "FIFO parser types an absolute shot_now path");
    }

    for (const char* line : {"wait_frames 0", "wait_frames -1",
                             "wait_frames 1.5", "wait_frames 4294967296",
                             "shot_now relative.png",
                             "render_path pathtrace"}) {
        const auto parsed = viewer::parse_fifo_line(line);
        CHECK(parsed.recognized && !parsed.success,
              "FIFO parser rejects malformed presentation commands");
    }

    for (const char* line : {"render_pathology native_rt",
                             "history_reset_extra",
                             "wait_frames_extra 3",
                             "shot_now_extra C:\\absolute\\frame.png"}) {
        const auto parsed = viewer::parse_fifo_line(line);
        CHECK(!parsed.recognized,
              "FIFO parser requires an exact presentation command token");
    }

    for (const char* line : {"shot_now \\frame.png",
                             "shot_now /frame.png",
                             "shot_now C:\\absolute\\frame.jpg",
                             "shot_now C:\\absolute\\..\\frame.png",
                             "shot_now \\\\server\\..\\frame.png"}) {
        const auto parsed = viewer::parse_fifo_line(line);
        CHECK(parsed.recognized && !parsed.success,
              "FIFO parser rejects drive-relative, non-PNG, and traversal shots");
    }

    viewer::FifoPresentSequencer sequencing;
    sequencing.queue_wait(3u);
    sequencing.queue_screenshot("C:\\absolute\\next.png");
    CHECK(sequencing.presented_frame_serial() == 0u &&
              sequencing.pending_screenshot_path() ==
                  "C:\\absolute\\next.png",
          "present sequencer queues wait and zero-settle screenshot");
    const auto failed = sequencing.advance(false);
    CHECK(sequencing.presented_frame_serial() == 0u &&
              failed.completed_waits.empty() &&
              failed.screenshot_path.empty() &&
              sequencing.pending_screenshot_path() ==
                  "C:\\absolute\\next.png",
          "failed/acquire-only frame neither advances nor consumes queued work");
    const auto first = sequencing.advance(true);
    CHECK(sequencing.presented_frame_serial() == 1u &&
              first.completed_waits.empty() &&
              first.screenshot_path == "C:\\absolute\\next.png",
          "shot_now captures the next successful present with zero settle");
    const auto intervening_failure = sequencing.advance(false);
    CHECK(sequencing.presented_frame_serial() == 1u &&
              intervening_failure.completed_waits.empty(),
          "intervening failed present cannot complete a queued wait");
    const auto second = sequencing.advance(true);
    CHECK(second.completed_waits.empty(),
          "wait_frames does not print success before its target serial");
    const auto third = sequencing.advance(true);
    CHECK(third.completed_waits.size() == 1u &&
              third.completed_waits[0].count == 3u &&
              third.completed_waits[0].frame_serial == 3u,
          "wait_frames completes on the third successful present only");
}

bool close4(matter::Float4 actual, matter::Float4 expected, float epsilon);

constexpr std::array<float, 6> kAtmosphereGpuElevations{
    90.0f, 45.0f, 5.0f, 0.0f, -5.0f, -12.0f};
constexpr std::array<float, 6> kAtmosphereGpuRatios{
    1.0f, 1.0f, 0.25f, 0.0f, 0.0f, 0.0f};
std::array<matter::Float3, 6> g_atmosphere_raster_direct_rgb{};
bool g_atmosphere_raster_direct_valid = false;

static matter::Float3 aces_reference(matter::Float3 hdr, float exposure_ev) {
    const float scale = std::exp2(exposure_ev);
    const auto map = [scale](float value) {
        const float x = std::max(value * scale, 0.0f);
        return std::clamp((x * (2.51f * x + 0.03f)) /
                              (x * (2.43f * x + 0.59f) + 0.14f),
                          0.0f, 1.0f);
    };
    return {map(hdr.x), map(hdr.y), map(hdr.z)};
}

static float srgb_encode(float linear) {
    return linear <= 0.0031308f ? linear * 12.92f
        : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

static float srgb_decode(float encoded) {
    return encoded <= 0.04045f ? encoded / 12.92f
        : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

static float inverse_aces(float mapped) {
    if (mapped <= 0.0f) return 0.0f;
    if (mapped >= 1.0f) return 8.0f;
    const float a = 2.43f * mapped - 2.51f;
    const float b = 0.59f * mapped - 0.03f;
    const float c = 0.14f * mapped;
    const float discriminant = std::max(0.0f, b * b - 4.0f * a * c);
    return (-b - std::sqrt(discriminant)) / (2.0f * a);
}

static matter::Float3 sample_periodic_sky_cpu(
    const viewer::VkSceneRenderer::EnvironmentSamplingGpuFixture& fixture,
    matter::Float2 uv) {
    const float u = uv.x - std::floor(uv.x);
    const float v = std::clamp(uv.y, 0.5f / 108.0f, 107.5f / 108.0f);
    const float px = u * 192.0f - 0.5f;
    const float py = v * 108.0f - 0.5f;
    const int x0_raw = static_cast<int>(std::floor(px));
    const int y0 = std::clamp(static_cast<int>(std::floor(py)), 0, 107);
    const int y1 = std::min(y0 + 1, 107);
    const int x0 = (x0_raw % 192 + 192) % 192;
    const int x1 = (x0 + 1) % 192;
    const float tx = px - std::floor(px);
    const float ty = py - std::floor(py);
    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const matter::Float3& a = fixture.lut[static_cast<size_t>(y0) * 192 + x0];
    const matter::Float3& b = fixture.lut[static_cast<size_t>(y0) * 192 + x1];
    const matter::Float3& c = fixture.lut[static_cast<size_t>(y1) * 192 + x0];
    const matter::Float3& d = fixture.lut[static_cast<size_t>(y1) * 192 + x1];
    return {lerp(lerp(a.x, b.x, tx), lerp(c.x, d.x, tx), ty),
            lerp(lerp(a.y, b.y, tx), lerp(c.y, d.y, tx), ty),
            lerp(lerp(a.z, b.z, tx), lerp(c.z, d.z, tx), ty)};
}

void run_atmosphere_presentation_sampling_tests(matter::VulkanDevice& vulkan) {
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error), error.empty() ? "initialize atmosphere presentation fixture" : error.c_str());
    if (!error.empty()) return;
    CHECK(renderer.test_sky_view_sampler() != VK_NULL_HANDLE &&
              renderer.test_sky_view_sampler() != renderer.test_composite_sampler(),
          "physical sky uses a dedicated sampler rather than the nearest G-buffer sampler");

    viewer::VkSceneRenderer::EnvironmentSamplingGpuFixture fixture;
    for (uint32_t y = 0; y < 108; ++y) for (uint32_t x = 0; x < 192; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / 192.0f;
        const float v = (static_cast<float>(y) + 0.5f) / 108.0f;
        fixture.lut[static_cast<size_t>(y) * 192 + x] =
            {u, v, 0.25f * u + 0.75f * v};
    }
    for (uint32_t i = 0; i < 432; ++i)
        fixture.uv.push_back({0.37f, (static_cast<float>(i) + 0.5f) / 432.0f});
    fixture.uv.push_back({0.37f, 0.0f});
    fixture.uv.push_back({0.37f, 1.0f});
    constexpr float epsilon = 1.0e-5f;
    for (uint32_t i = 0; i < 256; ++i) {
        const float v = (static_cast<float>(i) + 0.5f) / 256.0f;
        fixture.uv.push_back({epsilon, v});
        fixture.uv.push_back({1.0f - epsilon, v});
    }
    std::vector<matter::Float3> sampled;
    CHECK(renderer.test_dispatch_environment_sampling_fixture(fixture, sampled, error),
          error.empty() ? "dispatch periodic linear sky sampling fixture" : error.c_str());
    if (sampled.size() != fixture.uv.size()) {
        CHECK(false, "sky sampling fixture returns every requested UV");
        return;
    }
    size_t plateau = 1, max_plateau = 1, mismatch_count = 0;
    for (size_t i = 0; i < 432; ++i) {
        const matter::Float3 expected = sample_periodic_sky_cpu(fixture, fixture.uv[i]);
        const bool matches = std::fabs(sampled[i].x - expected.x) <= 1.0e-3f &&
                             std::fabs(sampled[i].y - expected.y) <= 1.0e-3f &&
                             std::fabs(sampled[i].z - expected.z) <= 1.0e-3f;
        if (!matches && mismatch_count++ < 8)
            std::printf("sky sample mismatch[%zu] uv=(%.8f,%.8f) actual=(%.6f,%.6f,%.6f) expected=(%.6f,%.6f,%.6f)\n",
                        i, fixture.uv[i].x, fixture.uv[i].y,
                        sampled[i].x, sampled[i].y, sampled[i].z,
                        expected.x, expected.y, expected.z);
        CHECK(matches,
              "GPU sky sample matches periodic-U clamped-V bilinear oracle");
        if (i > 0) {
            const float reference_slope = std::fabs(
                sample_periodic_sky_cpu(fixture, fixture.uv[i]).y -
                sample_periodic_sky_cpu(fixture, fixture.uv[i - 1]).y);
            if (reference_slope > 1.0e-4f && sampled[i].y == sampled[i - 1].y)
                ++plateau;
            else
                plateau = 1;
            max_plateau = std::max(max_plateau, plateau);
        }
    }
    CHECK(max_plateau <= 2, "linear sky sampling avoids visible vertical plateaus");
    CHECK(std::fabs(sampled[432].y - fixture.lut[0].y) <= 1.0e-3f &&
              std::fabs(sampled[433].y - fixture.lut[107 * 192].y) <= 1.0e-3f,
          "sky V edge probes clamp to edge texel centres");
    std::vector<float> adjacent;
    for (uint32_t i = 1; i < 191; ++i) {
        const matter::Float2 left{(static_cast<float>(i) - epsilon) / 192.0f, 0.5f};
        const matter::Float2 right{(static_cast<float>(i) + epsilon) / 192.0f, 0.5f};
        const auto a = sample_periodic_sky_cpu(fixture, left);
        const auto b = sample_periodic_sky_cpu(fixture, right);
        adjacent.push_back(std::fabs(a.x - b.x));
    }
    std::sort(adjacent.begin(), adjacent.end());
    const float median_adjacent = adjacent[adjacent.size() / 2];
    for (uint32_t i = 0; i < 256; ++i) {
        const auto& a = sampled[434 + i * 2];
        const auto& b = sampled[435 + i * 2];
        const float absolute = std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y),
                                         std::fabs(a.z - b.z)});
        const float scale = std::max({std::fabs(a.x), std::fabs(a.y), std::fabs(a.z),
                                      std::fabs(b.x), std::fabs(b.y), std::fabs(b.z)});
        CHECK(std::isfinite(absolute) && (scale <= 1.0e-3f ? absolute <= 1.0e-3f
                                                               : absolute / scale <= 0.005f),
              "periodic sky seam pair is finite and bounded");
        CHECK(std::fabs(a.x - b.x) <= 2.0f * median_adjacent + 1.0e-6f,
              "sky seam finite difference is no larger than local sampling variation");
    }

    const float target_codes[] = {0.5f, 0.0f, 1.0f};
    for (float target_code : target_codes) {
        const float target_linear = srgb_decode(target_code);
        const float hdr = inverse_aces(target_linear);
        viewer::VkSceneRenderer::DisplayTransformGpuFixture display;
        display.width = 16;
        display.height = 16;
        display.hdr = {hdr, hdr, hdr};
        std::vector<matter::Float3> unorm_a, unorm_b, srgb;
        CHECK(renderer.test_dispatch_display_transform_fixture(display, unorm_a, error),
              error.empty() ? "dispatch UNORM display dither fixture" : error.c_str());
        CHECK(renderer.test_dispatch_display_transform_fixture(display, unorm_b, error),
              error.empty() ? "repeat static display dither fixture" : error.c_str());
        display.srgb_output = true;
        CHECK(renderer.test_dispatch_display_transform_fixture(display, srgb, error),
              error.empty() ? "dispatch sRGB display dither fixture" : error.c_str());
        CHECK(unorm_a.size() == 256 && unorm_b.size() == 256 && srgb.size() == 256,
              "display fixture returns the complete 16x16 interior");
        if (unorm_a.size() != 256 || unorm_b.size() != 256 || srgb.size() != 256) continue;
        float tile_sums[4]{};
        float minimum_offset = 1.0f, maximum_offset = -1.0f;
        bool identical = true;
        for (uint32_t y = 0; y < 16; ++y) for (uint32_t x = 0; x < 16; ++x) {
            const size_t index = static_cast<size_t>(y) * 16 + x;
            const matter::Float3 expected = matter::apply_display_dither_code(
                {target_code, target_code, target_code}, x, y);
            const matter::Float3 actual = unorm_a[index];
            const float offset = actual.x - target_code;
            minimum_offset = std::min(minimum_offset, offset);
            maximum_offset = std::max(maximum_offset, offset);
            tile_sums[(y / 8) * 2 + x / 8] += offset;
            identical = identical && std::memcmp(&actual, &unorm_b[index], sizeof(actual)) == 0;
            CHECK(std::fabs(actual.x - expected.x) <= 1.0e-6f &&
                      std::fabs(actual.y - expected.y) <= 1.0e-6f &&
                      std::fabs(actual.z - expected.z) <= 1.0e-6f &&
                      std::fabs((actual.x - target_code) - (actual.y - target_code)) <= 1.0e-7f &&
                      std::fabs((actual.x - target_code) - (actual.z - target_code)) <= 1.0e-7f,
                  "display shader applies the exact achromatic CPU dither oracle");
            CHECK(actual.x >= 0.0f && actual.x <= 1.0f &&
                      expected.x >= 0.0f && expected.x <= 1.0f &&
                      std::fabs(matter::display_dither_code_offset(x, y)) <= 0.5f / 255.0f,
                  "display dither clamps rails while its oracle remains half-LSB bounded");
            CHECK(std::fabs(srgb_encode(srgb[index].x) - actual.x) <= 1.0e-6f &&
                      std::fabs(srgb_encode(srgb[index].y) - actual.y) <= 1.0e-6f &&
                      std::fabs(srgb_encode(srgb[index].z) - actual.z) <= 1.0e-6f,
                  "UNORM and sRGB branches agree in encoded display code space");
        }
        CHECK(identical, "two static display submissions are byte-identical");
        if (target_code == 0.5f) {
            CHECK(std::fabs(minimum_offset + 0.5f / 255.0f) <= 1.0e-6f &&
                      std::fabs(maximum_offset - 0.5f / 255.0f) <= 1.0e-6f,
                  "display dither reaches both exact half-LSB extrema");
            for (float sum : tile_sums)
                CHECK(std::fabs(sum / 64.0f) <= 1.0e-8f,
                      "each complete 8x8 display dither tile has zero mean");
        }
    }
}

static int display_unorm_code(float linear, VkFormat swapchain_format) {
    const bool srgb = swapchain_format == VK_FORMAT_R8G8B8A8_SRGB ||
                      swapchain_format == VK_FORMAT_B8G8R8A8_SRGB;
    const float encoded = !srgb ? linear
        : linear <= 0.0031308f ? linear * 12.92f
        : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return static_cast<int>(std::lround(std::clamp(encoded, 0.0f, 1.0f) *
                                        255.0f));
}

static void test_viewer_lighting_controls() {
    // These seams assert that a reset restores THE DEFAULT, not that the
    // default holds any particular value — so they compare against the struct
    // rather than repeating its literals. test_vulkan_lighting_override_contract
    // below is the one place the values themselves are pinned.
    const matter::VulkanLightingOverrides world{};
    auto matches_world = [&world](const matter::VulkanLightingOverrides& v) {
        for (int i = 0; i < 3; ++i) {
            if (v.sun_tint[i] != world.sun_tint[i]) return false;
            if (v.sky_tint[i] != world.sky_tint[i]) return false;
        }
        return v.sun_multiplier == world.sun_multiplier &&
               v.sky_multiplier == world.sky_multiplier &&
               v.emission_multiplier == world.emission_multiplier &&
               v.exposure_ev == world.exposure_ev;
    };

    viewer::ViewerStats stats{};
    stats.lighting.sun_multiplier = 0.25f;
    stats.lighting.sky_multiplier = 0.5f;
    stats.lighting.emission_multiplier = 0.75f;
    stats.lighting.exposure_ev = 3.0f;
    stats.lighting.sun_tint[0] = 0.2f;
    stats.lighting.sky_tint[2] = 0.4f;
    viewer::reset_lighting_controls(stats);
    CHECK(matches_world(stats.lighting),
          "world reset restores the sun/sky tints along with the multipliers");
    CHECK(stats.lighting.sun_multiplier == world.sun_multiplier,
          "world reset restores authored sun multiplier");
    CHECK(stats.lighting.sky_multiplier == world.sky_multiplier,
          "world reset restores authored sky multiplier");
    CHECK(stats.lighting.emission_multiplier == world.emission_multiplier,
          "world reset restores authored emission multiplier");
    CHECK(stats.lighting.exposure_ev == world.exposure_ev,
          "world reset restores default display exposure");

    stats.lighting = {0.25f, 0.5f, 0.75f, 3.0f};
    viewer::prepare_world_reload(stats);
    CHECK(matches_world(stats.lighting),
          "reload seam resets all Viewer lighting controls");

    // Positional aggregate init still names only the first four members; the
    // tints ride their NSDMIs, which is exactly why they were appended.
    stats.lighting = {0.25f, 0.5f, 0.75f, 3.0f};
    stats.lighting.sun_tint[1] = 0.3f;
    viewer::complete_world_switch(stats, false);
    CHECK(stats.lighting.sun_multiplier == 0.25f &&
              stats.lighting.sky_multiplier == 0.5f &&
              stats.lighting.emission_multiplier == 0.75f &&
              stats.lighting.exposure_ev == 3.0f &&
              stats.lighting.sun_tint[1] == 0.3f,
          "failed world-switch seam preserves Viewer lighting controls");
    viewer::complete_world_switch(stats, true);
    CHECK(matches_world(stats.lighting),
          "successful world-switch seam resets all Viewer lighting controls");
}

static void test_vulkan_lighting_override_contract() {
    // 2026-07-30 tuning pass: sun/sky moved off 1.0 (see the struct comment in
    // world_session.h). Pinned here on purpose — these are what every world
    // loads with and what "Reset to World" restores.
    matter::VulkanLightingOverrides defaults{};
    CHECK(defaults.sun_multiplier == 1.67f, "sun override defaults to 1.67");
    CHECK(defaults.sky_multiplier == 0.77f, "sky override defaults to 0.77");
    CHECK(defaults.emission_multiplier == 1.0f,
          "emission override defaults to one");
    CHECK(defaults.exposure_ev == -2.0f, "display exposure defaults to -2 EV");

    matter::VulkanLightingOverrides bad{};
    bad.sun_multiplier = -4.0f;
    bad.sky_multiplier = 9.0f;
    bad.emission_multiplier = std::numeric_limits<float>::infinity();
    bad.exposure_ev = std::numeric_limits<float>::quiet_NaN();
    const auto clean = viewer::sanitize_vulkan_lighting_overrides(bad);
    CHECK(clean.sun_multiplier == 0.0f, "sun override clamps low");
    CHECK(clean.sky_multiplier == 4.0f, "sky override clamps high");
    CHECK(clean.emission_multiplier == 1.0f,
          "invalid emission override uses default");
    CHECK(clean.exposure_ev == -2.0f, "invalid exposure uses default");
    CHECK(std::fabs(viewer::vulkan_exposure_scale(-2.0f) - 0.25f) < 1e-6f,
          "-2 EV maps to quarter exposure");

    auto exposure_only = defaults;
    exposure_only.exposure_ev = 1.0f;
    CHECK(!viewer::vulkan_source_lighting_changed(defaults, exposure_only),
          "exposure-only change preserves lighting history");
    auto source_change = defaults;
    source_change.emission_multiplier = 0.5f;
    CHECK(viewer::vulkan_source_lighting_changed(defaults, source_change),
          "emission change invalidates lighting history");

    // ---- Sun/sky tints ---------------------------------------------------
    // White by default, which must be a BIT-EXACT no-op: the tint multiplies
    // the same authored colour the scalar multiplier does (matter_engine.cpp),
    // and every consumer — composite, GI constants, volumetric scatter —
    // reads that one pair of colours.
    CHECK(defaults.sun_tint[0] == 1.0f && defaults.sun_tint[1] == 1.0f &&
              defaults.sun_tint[2] == 1.0f,
          "sun tint defaults to white");
    CHECK(defaults.sky_tint[0] == 1.0f && defaults.sky_tint[1] == 1.0f &&
              defaults.sky_tint[2] == 1.0f,
          "sky tint defaults to white");
    {
        // The exact expression matter_engine.cpp evaluates, on a colour with
        // no representable slack left.
        const float authored = 0.1234567f;
        const float mul = 1.67f;
        CHECK(authored * mul * 1.0f == authored * mul,
              "a white tint is bit-exact on the assembled sun colour");
    }

    matter::VulkanLightingOverrides bad_tint{};
    bad_tint.sun_tint[0] = -1.0f;
    bad_tint.sun_tint[1] = 9.0f;
    bad_tint.sun_tint[2] = std::numeric_limits<float>::quiet_NaN();
    bad_tint.sky_tint[0] = std::numeric_limits<float>::infinity();
    const auto clean_tint = viewer::sanitize_vulkan_lighting_overrides(bad_tint);
    CHECK(clean_tint.sun_tint[0] == 0.0f, "sun tint clamps low");
    CHECK(clean_tint.sun_tint[1] == 4.0f, "sun tint clamps high");
    CHECK(clean_tint.sun_tint[2] == 1.0f,
          "a non-finite tint channel falls back to white, not black");
    CHECK(clean_tint.sky_tint[0] == 1.0f,
          "an infinite sky tint channel falls back to white");
    CHECK(clean_tint.sky_tint[1] == 1.0f && clean_tint.sky_tint[2] == 1.0f,
          "untouched tint channels survive sanitizing");

    auto tint_change = defaults;
    tint_change.sun_tint[2] = 0.6f;
    CHECK(viewer::vulkan_source_lighting_changed(defaults, tint_change),
          "a sun tint change invalidates lighting history");
    auto sky_tint_change = defaults;
    sky_tint_change.sky_tint[0] = 0.9f;
    CHECK(viewer::vulkan_source_lighting_changed(defaults, sky_tint_change),
          "a sky tint change invalidates lighting history");
}

void run_vulkan_gi_math_tests() {
    CHECK(VULKAN_GI_REJECT_HIT_DISTANCE == (1u << 6),
          "CPU and temporal shader reserve bit 6 for specular hit-distance rejection");
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
    // Scene set storage buffers: bindings 0-5 plus binding 8 (the C3 dynamic
    // cluster AABB override) is 7 per set. Compute sees all of those except
    // binding 5, which is VERTEX|FRAGMENT-only, so 6 per stage.
    CHECK(viewer::vk_scene_detail::scene_storage_limits_supported(6, 7) &&
              !viewer::vk_scene_detail::scene_storage_limits_supported(5, 7) &&
              !viewer::vk_scene_detail::scene_storage_limits_supported(6, 6),
          "scene capability accounting requires six compute and seven set buffers");
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
    complete.ray_query_extension = true;
    complete.buffer_device_address = true;
    complete.acceleration_structure = true;
    complete.ray_tracing_pipeline = true;
    complete.ray_query = true;
    complete.storage_image_r8 = true;
    complete.shader_storage_image_extended_formats = true;
    CHECK(matter::supports_native_ray_tracing(complete, reason) &&
              reason.empty(),
          "complete fake RTX capability set enables native ray tracing");
    // Ray query is a hard requirement, not a bonus: vol_scatter.comp and the
    // tileset bake shaders declare RayQueryKHR, so a device that reports the
    // ray-tracing pipeline without ray query must not claim native RT.
    complete.ray_query = false;
    CHECK(!matter::supports_native_ray_tracing(complete, reason) &&
              reason.find("rayQuery") != std::string::npos,
          "missing rayQuery cleanly disables native ray tracing");
    complete.ray_query = true;
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
    CHECK(fake.free_dlss_resources(error) &&
              fake.test_dlss_resource_free_count() == 1 &&
              fake.active_dlss_mode() == matter::DlssMode::Native,
          "DLSS resize releases viewport resources before tagged images change");

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

void run_vulkan_only_handle_diagnostic(matter::VulkanDevice& vulkan) {
    std::string error;
    const auto trace = [](const char* label) {
        // matter::win32_process_handle_count() no longer exists in the engine;
        // query the Win32 process handle count directly (windows.h reaches
        // this TU via VK_USE_PLATFORM_WIN32_KHR -> vulkan_win32.h).
        DWORD handle_count = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handle_count);
        std::printf("HANDLE_DIAG %-38s count=%u result=0 sync=n/a\n", label,
                    static_cast<unsigned>(handle_count));
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

struct AtmosphereRecordRequest {
    viewer::VkAtmosphere* atmosphere = nullptr;
    float camera_world_y = 0.0f;
    matter::Float3 to_sun{};
    bool recorded = false;
    std::string error;
};

void record_atmosphere_luts(VkCommandBuffer command_buffer, void* user_data) {
    auto& request = *static_cast<AtmosphereRecordRequest*>(user_data);
    request.recorded = request.atmosphere->record(
        command_buffer, request.camera_world_y, request.to_sun, request.error);
}

void run_atmosphere_lut_smoke(matter::VulkanDevice& vulkan) {
    viewer::VkAtmosphere atmosphere;
    std::string error;
    CHECK(atmosphere.init(vulkan, error),
          error.empty() ? "atmosphere LUT init" : error.c_str());
    if (!error.empty()) return;

    const matter::AtmosphereSettings settings{};
    atmosphere.request_settings(settings);
    const matter::Float3 to_sun{0.0f, 1.0f, 0.0f};
    auto record = [&](float altitude) {
        AtmosphereRecordRequest request{&atmosphere, altitude, to_sun};
        std::string submit_error;
        const bool submitted = matter::submit_immediate(
            vulkan, record_atmosphere_luts, &request, submit_error,
            matter::ImmediateSubmitPhase::compute_dispatch);
        CHECK(submitted && request.recorded,
              !submit_error.empty() ? submit_error.c_str() : request.error.c_str());
    };
    record(0.0f);

    const auto check_sample = [&](uint32_t x, uint32_t y, const char* label) {
        matter::Float3 actual{};
        std::string read_error;
        CHECK(atmosphere.readback_transmittance_for_test(
                  vulkan, x, y, actual, read_error),
              read_error.empty() ? label : read_error.c_str());
        const float mu = -1.0f + 2.0f * static_cast<float>(x) / 255.0f;
        const float altitude = 100000.0f * static_cast<float>(y) / 63.0f;
        const float horizontal = std::sqrt(std::max(0.0f, 1.0f - mu * mu));
        const matter::Float3 expected = matter::atmosphere_transmittance_reference(
            settings, altitude, {horizontal, mu, 0.0f}, 256).transmittance;
        const bool finite = std::isfinite(actual.x) && std::isfinite(actual.y) &&
                            std::isfinite(actual.z);
        const bool bounded = actual.x >= 0.0f && actual.x <= 1.0f &&
                             actual.y >= 0.0f && actual.y <= 1.0f &&
                             actual.z >= 0.0f && actual.z <= 1.0f;
        const bool close = std::fabs(actual.x - expected.x) <= 0.025f &&
                           std::fabs(actual.y - expected.y) <= 0.025f &&
                           std::fabs(actual.z - expected.z) <= 0.025f;
        CHECK(finite && bounded && close, label);
    };
    check_sample(255, 0, "atmosphere sea-level zenith transmittance matches CPU");
    check_sample(128, 0, "atmosphere sea-level near-horizon transmittance matches CPU");
    check_sample(255, 16, "atmosphere 25 km zenith transmittance matches CPU");

    const uint64_t first_generation = atmosphere.generation_serial();
    record(0.0f);
    CHECK(atmosphere.generation_serial() == first_generation &&
              !atmosphere.generated_this_frame(),
          "unchanged atmosphere frame does not regenerate LUTs");
    matter::AtmosphereSettings changed = settings;
    changed.mie_scale = 1.25f;
    atmosphere.request_settings(changed);
    record(0.0f);
    CHECK(atmosphere.generation_serial() == first_generation + 1 &&
              atmosphere.generated_this_frame(),
          "atmosphere coefficient change advances generation serial once");
    atmosphere.destroy();
}

void test_atmosphere_lighting_control_sanitization() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    matter::VulkanLightingOverrides invalid{};
    invalid.day_ambient_multiplier = nan;
    invalid.twilight_ambient_multiplier = inf;
    invalid.sky_irradiance_multiplier = -inf;
    invalid.sunset_direct_ratio = nan;
    invalid.sun_elevation_deg = inf;
    const auto fallback = viewer::sanitize_vulkan_lighting_overrides(invalid);
    CHECK(fallback.day_ambient_multiplier == 0.25f &&
              fallback.twilight_ambient_multiplier == 1.0f &&
              fallback.sky_irradiance_multiplier == 1.0f &&
              fallback.sunset_direct_ratio == 0.25f,
          "atmosphere lighting controls use their exact non-finite fallbacks");
    const matter::VulkanLightingOverrides defaults{};
    CHECK(fallback.sun_elevation_deg == defaults.sun_elevation_deg,
          "sun elevation keeps its established non-finite fallback");

    matter::VulkanLightingOverrides outside{};
    outside.day_ambient_multiplier = -1.0f;
    outside.twilight_ambient_multiplier = 9.0f;
    outside.sky_irradiance_multiplier = 8.0f;
    outside.sunset_direct_ratio = 2.0f;
    outside.sun_elevation_deg = -120.0f;
    const auto clamped = viewer::sanitize_vulkan_lighting_overrides(outside);
    CHECK(clamped.day_ambient_multiplier == 0.0f &&
              clamped.twilight_ambient_multiplier == 4.0f &&
              clamped.sky_irradiance_multiplier == 4.0f &&
              clamped.sunset_direct_ratio == 1.0f &&
              clamped.sun_elevation_deg == -90.0f,
          "atmosphere lighting and elevation controls clamp to exact ranges");
}

void run_atmosphere_irradiance_last_valid_test(
    matter::VulkanDevice& vulkan) {
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error),
          error.empty() ? "initialize irradiance last-valid fixture"
                        : error.c_str());
    if (!error.empty()) return;

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.0f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices matrices{};
    CHECK(viewer::build_frame_matrices(camera, 16, 16, matrices, error),
          error.empty() ? "build irradiance last-valid matrices"
                        : error.c_str());
    const auto prepare_once = [&]() {
        error.clear();
        matter::VulkanFrame frame{};
        if (!vulkan.begin_frame(frame, error)) return false;
        const bool prepared = renderer.prepare_frame(
            frame, matrices, camera.position, 1.0f, error);
        const bool ended = vulkan.end_frame(frame, error);
        renderer.finish_ray_tracing_frame(frame.serial, prepared && ended);
        vulkan.wait_idle();
        return prepared && ended;
    };

    viewer::VkSceneLighting lighting{};
    lighting.sun_direction = {0.0f, -1.0f, 0.0f};
    const float largest = std::numeric_limits<float>::max();
    lighting.atmosphere_sources.authored_irradiance_chroma_rgb =
        {largest, largest, largest};
    lighting.atmosphere_sources.sky_irradiance_multiplier = 4.0f;
    lighting.atmosphere_sources.day_ambient_multiplier = 4.0f;
    renderer.set_lighting(lighting);
    CHECK(prepare_once(),
          error.empty() ? "publish initial overflow-safe irradiance"
                        : error.c_str());
    const auto initial = renderer.test_resolved_atmosphere_status();
    CHECK(initial.sky_irradiance_modifier_rgb.x == 0.0f &&
              initial.sky_irradiance_modifier_rgb.y == 0.0f &&
              initial.sky_irradiance_modifier_rgb.z == 0.0f,
          "invalid derived irradiance is zero when no last-valid value exists");

    lighting.atmosphere_sources.authored_irradiance_chroma_rgb =
        {0.5f, 0.6f, 0.7f};
    lighting.atmosphere_sources.sky_irradiance_multiplier = 1.0f;
    lighting.atmosphere_sources.day_ambient_multiplier = 1.0f;
    renderer.set_lighting(lighting);
    CHECK(prepare_once(),
          error.empty() ? "publish finite irradiance modifier"
                        : error.c_str());
    const matter::Float3 last_valid =
        renderer.test_resolved_atmosphere_status()
            .sky_irradiance_modifier_rgb;
    CHECK(last_valid.x == 0.5f && last_valid.y == 0.6f &&
              last_valid.z == 0.7f,
          "finite derived irradiance becomes the last-valid modifier");

    lighting.atmosphere_sources.authored_irradiance_chroma_rgb =
        {largest, largest, largest};
    lighting.atmosphere_sources.sky_irradiance_multiplier = 4.0f;
    lighting.atmosphere_sources.day_ambient_multiplier = 4.0f;
    renderer.set_lighting(lighting);
    CHECK(prepare_once(),
          error.empty() ? "replay overflow after a finite irradiance value"
                        : error.c_str());
    const matter::Float3 retained =
        renderer.test_resolved_atmosphere_status()
            .sky_irradiance_modifier_rgb;
    CHECK(std::memcmp(&retained, &last_valid, sizeof(retained)) == 0,
          "overflowed derived irradiance retains the prior finite modifier");
}

bool same_atmosphere_handles(const viewer::AtmosphereLutHandles& a,
                             const viewer::AtmosphereLutHandles& b) {
    return std::memcmp(a.images.data(), b.images.data(), sizeof(a.images)) == 0 &&
           std::memcmp(a.views.data(), b.views.data(), sizeof(a.views)) == 0;
}

bool same_committed_atmosphere(const viewer::ResolvedAtmosphereStatus& a,
                               const viewer::ResolvedAtmosphereStatus& b) {
    return a.generation_serial == b.generation_serial &&
           std::memcmp(&a.normalized_to_sun, &b.normalized_to_sun,
                       sizeof(matter::Float3)) == 0 &&
           std::memcmp(a.irradiance_sh.data(), b.irradiance_sh.data(),
                       sizeof(a.irradiance_sh)) == 0 &&
           std::memcmp(&a.atmospheric_direct_base_rgb,
                       &b.atmospheric_direct_base_rgb,
                       sizeof(matter::Float3)) == 0 &&
           std::memcmp(&a.atmospheric_noon_direct_base_rgb,
                       &b.atmospheric_noon_direct_base_rgb,
                       sizeof(matter::Float3)) == 0;
}

void run_atmosphere_transaction_failure_tests(matter::VulkanDevice& vulkan) {
    std::string error;
    viewer::AtmosphereCommittedState committed_abi{};
    CHECK(committed_abi.atmospheric_noon_direct_base_rgb.x == 0.0f,
          "committed atmosphere state exposes the noon direct reference ABI");
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error),
          error.empty() ? "initialize atmosphere transaction renderer"
                        : error.c_str());
    if (!error.empty()) return;

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.0f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices matrices{};
    CHECK(viewer::build_frame_matrices(camera, 16, 16, matrices, error),
          error.empty() ? "build atmosphere transaction matrices"
                        : error.c_str());

    const auto prepare_once = [&](std::string& observed_error) {
        observed_error.clear();
        matter::VulkanFrame frame{};
        if (!vulkan.begin_frame(frame, observed_error)) return false;
        const bool prepared = renderer.prepare_frame(
            frame, matrices, camera.position, 1.0f, observed_error);
        const std::string prepare_error = observed_error;
        std::string end_error;
        const bool ended = vulkan.end_frame(frame, end_error);
        renderer.finish_ray_tracing_frame(frame.serial, prepared && ended);
        vulkan.wait_idle();
        if (!prepared) observed_error = prepare_error;
        else if (!ended) observed_error = end_error;
        return prepared && ended;
    };

    viewer::VkSceneLighting baseline_lighting{};
    baseline_lighting.sun_direction = {0.0f, -1.0f, 0.0f};
    baseline_lighting.authored_sun_rgb = {0.9f, 0.7f, 0.5f};
    baseline_lighting.atmosphere_sources.authored_display_sky_chroma_rgb =
        {0.7f, 0.8f, 0.9f};
    baseline_lighting.atmosphere_sources.authored_irradiance_chroma_rgb =
        {0.7f, 0.8f, 0.9f};
    renderer.set_atmosphere_settings({});
    renderer.set_lighting(baseline_lighting);
    CHECK(prepare_once(error),
          error.empty() ? "commit baseline atmosphere transaction"
                        : error.c_str());
    const auto baseline_status = renderer.test_resolved_atmosphere_status();
    const auto baseline_handles = renderer.test_atmosphere_lut_handles();
    const auto baseline_histories = renderer.test_atmosphere_history_counters();
    CHECK(baseline_status.generation_serial == 1 &&
              baseline_histories.diffuse_gi == 1 &&
              baseline_histories.reflection_miss == 0 &&
              baseline_histories.volumetric == 1,
          "baseline atmosphere commit advances serial and narrow histories once");

    matter::AtmosphereSettings pending{};
    pending.mie_scale = 1.25f;
    renderer.set_atmosphere_settings(pending);
    const auto generation_attempts_before =
        renderer.test_atmosphere_candidate_counters();
    const uint64_t generation_submits_before =
        matter::immediate_submit_count();
    renderer.test_fail_next_atmosphere_generation();
    CHECK(!prepare_once(error) &&
              error.find("injected atmosphere generation failure") !=
                  std::string::npos,
          "candidate generation failure is reported precisely");
    const auto generation_no_edit_handles = renderer.test_atmosphere_lut_handles();
    const auto generation_no_edit_status = renderer.test_resolved_atmosphere_status();
    const auto generation_no_edit_histories = renderer.test_atmosphere_history_counters();
    const auto generation_attempts_after =
        renderer.test_atmosphere_candidate_counters();
    CHECK(same_atmosphere_handles(baseline_handles, generation_no_edit_handles) &&
              std::memcmp(&baseline_status, &generation_no_edit_status,
                          sizeof(baseline_status)) == 0 &&
              std::memcmp(&baseline_histories, &generation_no_edit_histories,
                          sizeof(baseline_histories)) == 0,
          "generation failure without a live edit preserves the complete transaction");
    CHECK(generation_attempts_after.image_sets_allocated ==
                  generation_attempts_before.image_sets_allocated + 1 &&
              generation_attempts_after.generation_stages_completed ==
                  generation_attempts_before.generation_stages_completed + 1 &&
              generation_attempts_after.image_sets_discarded ==
                  generation_attempts_before.image_sets_discarded + 1 &&
              matter::immediate_submit_count() >=
                  generation_submits_before + 2,
          "injected generation failure allocates, dispatches, reads back, restores, and discards one candidate set");

    renderer.test_fail_next_atmosphere_descriptor_publication();
    CHECK(!prepare_once(error) &&
              error.find("injected atmosphere descriptor publication failure") !=
                  std::string::npos,
          "candidate descriptor publication failure is reported precisely");
    const auto publication_no_edit_status =
        renderer.test_resolved_atmosphere_status();
    const auto publication_no_edit_histories =
        renderer.test_atmosphere_history_counters();
    const auto publication_no_edit_handles = renderer.test_atmosphere_lut_handles();
    CHECK(same_atmosphere_handles(baseline_handles,
                                  publication_no_edit_handles) &&
              std::memcmp(&baseline_status, &publication_no_edit_status,
                          sizeof(baseline_status)) == 0 &&
              std::memcmp(&baseline_histories, &publication_no_edit_histories,
                          sizeof(baseline_histories)) == 0,
          "publication failure without a live edit preserves the complete transaction");

    const auto make_replay = [&](float scale) {
        viewer::VkSceneLighting value = baseline_lighting;
        const matter::Float3 replay_to_sun =
            matter::atmosphere_to_sun_from_elevation_deg(5.0f * scale);
        value.sun_direction = {-replay_to_sun.x, -replay_to_sun.y,
                               -replay_to_sun.z};
        value.atmosphere_sources.live_sun_tint_rgb =
            {0.4f * scale, 0.7f, 1.3f};
        value.atmosphere_sources.sun_multiplier = 1.2f * scale;
        value.atmosphere_sources.authored_display_sky_chroma_rgb =
            {0.2f, 1.1f * scale, 0.6f};
        value.atmosphere_sources.sky_multiplier = 1.4f * scale;
        value.atmosphere_sources.live_sky_tint_rgb = {1.5f, 0.3f, 0.8f};
        value.atmosphere_sources.authored_irradiance_chroma_rgb =
            {1.2f, 0.5f * scale, 0.9f};
        value.atmosphere_sources.sky_irradiance_multiplier = 1.8f * scale;
        value.atmosphere_sources.day_ambient_multiplier = 0.4f * scale;
        value.atmosphere_sources.twilight_ambient_multiplier = 1.3f * scale;
        value.atmosphere_sources.sunset_direct_ratio = 0.35f * scale;
        value.emission_multiplier = 0.6f * scale;
        value.sun_angular_diameter_deg = 0.8f * scale;
        value.sun_shadow_samples = scale < 1.1f ? 5 : 7;
        return value;
    };
    const auto assert_failure_replay = [&](
        const viewer::ResolvedAtmosphereStatus& value,
        const viewer::VkSceneLighting& live, float expected_exposure,
        const char* message) {
        matter::AtmosphereLightingSources expected_sources =
            live.atmosphere_sources;
        expected_sources.atmospheric_direct_base_rgb =
            baseline_status.atmospheric_direct_base_rgb;
        expected_sources.atmospheric_noon_direct_base_rgb =
            baseline_status.atmospheric_noon_direct_base_rgb;
        expected_sources.elevation_deg = baseline_status.resolved_elevation_deg;
        const auto expected = matter::resolve_atmosphere_lighting(expected_sources);
        const auto push = renderer.test_atmosphere_replay_constants();
        const matter::Float3 expected_composite_direction{
            -baseline_status.normalized_to_sun.x,
            -baseline_status.normalized_to_sun.y,
            -baseline_status.normalized_to_sun.z};
        const matter::Float3 expected_to_sun =
            baseline_status.normalized_to_sun;
        CHECK(same_atmosphere_handles(
                  baseline_handles,
                  renderer.test_atmosphere_lut_handles()) &&
                  same_committed_atmosphere(baseline_status, value) &&
                  value.resolved_elevation_deg ==
                      baseline_status.resolved_elevation_deg &&
                  std::memcmp(&value.direct_base_rgb, &expected.direct_base_rgb,
                              sizeof(matter::Float3)) == 0 &&
                  value.direct_world_ratio == expected.direct_world_ratio &&
                  std::memcmp(&value.direct_world_sun_rgb,
                              &expected.direct_world_sun_rgb,
                              sizeof(matter::Float3)) == 0 &&
                  std::memcmp(&value.sun_disc_rgb, &expected.sun_disc_rgb,
                              sizeof(matter::Float3)) == 0 &&
                  value.sky_ambient_ratio == expected.sky_ambient_ratio &&
                  std::memcmp(&value.sky_display_modifier_rgb,
                              &expected.sky_display_modifier_rgb,
                              sizeof(matter::Float3)) == 0 &&
                  std::memcmp(&value.sky_irradiance_modifier_rgb,
                              &expected.sky_irradiance_modifier_rgb,
                              sizeof(matter::Float3)) == 0,
              message);
        CHECK(std::memcmp(&push.composite_sun_direction,
                          &expected_composite_direction,
                          sizeof(matter::Float3)) == 0 &&
                  std::memcmp(&live.sun_direction,
                              &expected_composite_direction,
                              sizeof(matter::Float3)) != 0 &&
                  push.composite_emission_multiplier ==
                      live.emission_multiplier &&
                  push.display_exposure_ev == expected_exposure &&
                  push.composite_sun_disc_cos_edge ==
                      matter::sun_disc_cos_edge(
                          live.sun_angular_diameter_deg) &&
                  push.composite_sun_disc_cos_core ==
                      matter::sun_disc_cos_core(
                          live.sun_angular_diameter_deg) &&
                  std::memcmp(&push.rt_to_sun, &expected_to_sun,
                              sizeof(matter::Float3)) == 0 &&
                  push.rt_shadow_samples ==
                      static_cast<uint32_t>(live.sun_shadow_samples) &&
                  push.rt_shadow_sun_cone_scale ==
                      matter::sun_size_scale(
                          live.sun_angular_diameter_deg) &&
                  push.rt_gi_emission_multiplier ==
                      live.emission_multiplier &&
                  push.rt_gi_sun_disc_cos_edge ==
                      matter::sun_disc_cos_edge(
                          live.sun_angular_diameter_deg) &&
                  push.rt_gi_sun_disc_cos_core ==
                      matter::sun_disc_cos_core(
                          live.sun_angular_diameter_deg) &&
                  push.rt_gi_sun_size_scale ==
                      matter::sun_size_scale(
                          live.sun_angular_diameter_deg),
              "failure replay keeps composite/RT direction committed while exposing every permitted live push input");
    };

    const viewer::VkSceneLighting generation_replay = make_replay(1.0f);
    renderer.set_lighting(generation_replay);
    matter::VulkanRayTracingSettings replay_rt{};
    replay_rt.samples = static_cast<uint32_t>(
        generation_replay.sun_shadow_samples);
    renderer.set_ray_tracing_settings(replay_rt);
    renderer.set_display_exposure(1.25f);
    renderer.test_fail_next_atmosphere_generation();
    const auto before_generation_replay =
        renderer.test_atmosphere_history_counters();
    CHECK(!prepare_once(error),
          "generation replay fixture observes the injected failure");
    assert_failure_replay(renderer.test_resolved_atmosphere_status(),
                          generation_replay, 1.25f,
                          "generation failure replays every current live constant over the old physical state");
    const auto after_generation_replay =
        renderer.test_atmosphere_history_counters();
    CHECK(after_generation_replay.diffuse_gi ==
                  before_generation_replay.diffuse_gi + 1 &&
              after_generation_replay.reflection_miss ==
                  before_generation_replay.reflection_miss + 1 &&
              after_generation_replay.volumetric ==
                  before_generation_replay.volumetric + 1,
          "combined generation-failure replay applies the union of narrow resets once");

    renderer.set_atmosphere_settings({});
    renderer.set_lighting(baseline_lighting);
    renderer.set_display_exposure(-2.0f);
    CHECK(prepare_once(error),
          error.empty() ? "restore constants without regenerating atmosphere"
                        : error.c_str());
    renderer.set_atmosphere_settings(pending);
    const viewer::VkSceneLighting publication_replay = make_replay(1.2f);
    renderer.set_lighting(publication_replay);
    replay_rt.samples = static_cast<uint32_t>(
        publication_replay.sun_shadow_samples);
    renderer.set_ray_tracing_settings(replay_rt);
    renderer.set_display_exposure(2.0f);
    renderer.test_fail_next_atmosphere_descriptor_publication();
    const auto before_publication_replay =
        renderer.test_atmosphere_history_counters();
    CHECK(!prepare_once(error),
          "publication replay fixture observes the injected failure");
    assert_failure_replay(renderer.test_resolved_atmosphere_status(),
                          publication_replay, 2.0f,
                          "publication failure replays every current live constant over the old physical state");
    const auto after_publication_replay =
        renderer.test_atmosphere_history_counters();
    CHECK(after_publication_replay.diffuse_gi ==
                  before_publication_replay.diffuse_gi + 1 &&
              after_publication_replay.reflection_miss ==
                  before_publication_replay.reflection_miss + 1 &&
              after_publication_replay.volumetric ==
                  before_publication_replay.volumetric + 1,
          "combined publication-failure replay applies the union of narrow resets once");

    const auto before_success = renderer.test_atmosphere_history_counters();
    const uint64_t serial_before_success =
        renderer.test_resolved_atmosphere_status().generation_serial;
    CHECK(prepare_once(error),
          error.empty() ? "commit successful candidate after failure replay"
                        : error.c_str());
    const auto after_success = renderer.test_atmosphere_history_counters();
    CHECK(renderer.test_resolved_atmosphere_status().generation_serial ==
                  serial_before_success + 1 &&
              after_success.diffuse_gi == before_success.diffuse_gi + 1 &&
              after_success.reflection_miss == before_success.reflection_miss &&
              after_success.volumetric == before_success.volumetric + 1,
          "successful candidate advances serial, diffuse GI, and volumetrics exactly once");

    const auto altitude_counters_before =
        renderer.test_atmosphere_candidate_counters();
    const auto altitude_histories_before =
        renderer.test_atmosphere_history_counters();
    const uint64_t altitude_serial_before =
        renderer.test_resolved_atmosphere_status().generation_serial;
    for (float camera_y : {2.0f, 7.5f, 10.0f}) {
        camera.position.y = camera_y;
        CHECK(prepare_once(error),
              error.empty() ? "prepare sub-threshold atmosphere altitude motion"
                            : error.c_str());
    }
    const auto altitude_counters_subthreshold =
        renderer.test_atmosphere_candidate_counters();
    const auto altitude_histories_subthreshold =
        renderer.test_atmosphere_history_counters();
    CHECK(std::memcmp(&altitude_counters_before,
                      &altitude_counters_subthreshold,
                      sizeof(altitude_counters_before)) == 0 &&
              std::memcmp(&altitude_histories_before,
                          &altitude_histories_subthreshold,
                          sizeof(altitude_histories_before)) == 0 &&
              renderer.test_resolved_atmosphere_status().generation_serial ==
                  altitude_serial_before,
          "cumulative camera altitude motion through 10 m does not rebuild atmosphere resources");

    camera.position.y = 10.25f;
    CHECK(prepare_once(error),
          error.empty() ? "prepare meaningful atmosphere altitude motion"
                        : error.c_str());
    const auto altitude_counters_after =
        renderer.test_atmosphere_candidate_counters();
    const auto altitude_histories_after =
        renderer.test_atmosphere_history_counters();
    CHECK(altitude_counters_after.image_sets_allocated ==
                  altitude_counters_before.image_sets_allocated + 1 &&
              altitude_counters_after.generation_stages_completed ==
                  altitude_counters_before.generation_stages_completed + 1 &&
              altitude_counters_after.image_sets_discarded ==
                  altitude_counters_before.image_sets_discarded &&
              renderer.test_resolved_atmosphere_status().generation_serial ==
                  altitude_serial_before + 1 &&
              altitude_histories_after.diffuse_gi ==
                  altitude_histories_before.diffuse_gi + 1 &&
              altitude_histories_after.reflection_miss ==
                  altitude_histories_before.reflection_miss &&
              altitude_histories_after.volumetric ==
                  altitude_histories_before.volumetric + 1,
          "camera altitude motion above 10 m rebuilds and commits one atmosphere transaction");
}

void test_atmosphere_irradiance_dispatch_contract() {
    std::ifstream shader("MatterEngine3/shaders_vk/atmosphere_irradiance.comp",
                         std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(shader)),
                             std::istreambuf_iterator<char>());
    CHECK(shader.good() || !source.empty(),
          "atmosphere irradiance shader source is readable from smoke cwd");
    CHECK(source.find("gl_GlobalInvocationID.y * 3u + gl_GlobalInvocationID.x") !=
              std::string::npos &&
              source.find("if (coefficient >= 9u) return;") != std::string::npos,
          "irradiance shader assigns exactly one row-major SH coefficient per invocation");
    std::ifstream implementation("MatterEngine3/src/render/vk_atmosphere.cpp",
                                std::ios::binary);
    const std::string implementation_source(
        (std::istreambuf_iterator<char>(implementation)), std::istreambuf_iterator<char>());
    CHECK(implementation_source.find("bind_dispatch(irradiance_pass_, 3, 3)") !=
              std::string::npos,
          "atmosphere records one irradiance dispatch group for each SH coefficient");
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

    viewer::TemporalState native_temporal;
    viewer::TemporalFrame native_frame = native_temporal.begin(
        camera, {100, 80}, {100, 80}, {still}, false, {});
    CHECK(native_frame.jitter_pixels[0] == 0.0f &&
              native_frame.jitter_pixels[1] == 0.0f &&
              std::equal(std::begin(native_frame.current_jittered.world_to_clip.m),
                         std::end(native_frame.current_jittered.world_to_clip.m),
                         std::begin(native_frame.current_unjittered.world_to_clip.m)),
          "native rendering uses the exact unjittered projection");

    viewer::TemporalFrame first = temporal.begin(
        camera, {100, 80}, {100, 80}, {still}, true, {});
    CHECK(first.reset && !first.instances[0].history_valid,
          "first temporal frame resets invalid history");
    CHECK(first.presented_frame_index == 0,
          "first candidate seeds from zero successfully presented frames");
    CHECK(temporal.commit_presented(first.attempt_token),
          "successful presentation commits temporal candidate");

    viewer::TemporalFrame static_frame = temporal.begin(
        camera, {100, 80}, {100, 80}, {still}, true, {});
    const matter::Float3 static_velocity =
        viewer::temporal_velocity_pixels(static_frame, 7, {0.0f, 0.0f, 0.0f});
    // Y expectations here (and in the two deltas below) follow the Y-down
    // jitter convention d5f97aa7 gave jitter_frame; the old +Y-NDC values
    // predate that fix and went stale while this suite could not build.
    CHECK(!static_frame.reset && static_frame.instances[0].history_valid &&
              std::fabs(static_velocity.x + 0.25f) < 1e-6f &&
              std::fabs(static_velocity.y - 1.0f / 3.0f) < 1e-6f,
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
        moved_camera, {100, 80}, {100, 80}, {still}, true, {});
    const matter::Float3 camera_velocity = viewer::temporal_velocity_pixels(
        camera_motion, 7, {0.0f, 0.0f, 0.0f});
    CHECK(std::fabs(camera_velocity.x + 9.5f) < 1e-5f &&
              std::fabs(camera_velocity.y + 5.0f / 9.0f) < 1e-5f,
          "known camera translation includes the presented Halton delta");
    CHECK(temporal.commit_presented(camera_motion.attempt_token),
          "camera-motion candidate commits");

    const viewer::TemporalInstance moved_object{
        7, viewer::mat4_translation({0.4f, 0.0f, 0.0f})};
    viewer::TemporalFrame object_motion = temporal.begin(
        moved_camera, {100, 80}, {100, 80}, {moved_object}, true, {});
    const matter::Float3 object_velocity = viewer::temporal_velocity_pixels(
        object_motion, 7, {0.0f, 0.0f, 0.0f});
    CHECK(std::fabs(object_velocity.x - 19.375f) < 1e-5f &&
              std::fabs(object_velocity.y - 1.0f / 3.0f) < 1e-5f,
          "known rigid-instance translation includes the presented Halton delta");
    CHECK(temporal.commit_presented(object_motion.attempt_token),
          "object-motion candidate commits");

    const auto expect_one_reset = [&](VkExtent2D internal,
                                      viewer::TemporalInvalidation invalidation,
                                      std::vector<viewer::TemporalInstance> instances,
                                      const char* label) {
        viewer::TemporalFrame reset = temporal.begin(
            moved_camera, internal, {100, 80}, instances, true, invalidation);
        CHECK(reset.reset, label);
        CHECK(temporal.commit_presented(reset.attempt_token), label);
        viewer::TemporalFrame stable = temporal.begin(
            moved_camera, internal, {100, 80}, instances, true, {});
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

    // A newcomer id is a per-instance event, not a global cut: it enters with
    // invalid history while survivors keep theirs. Escalating it to a full
    // reset starved DLSS/GI in streaming worlds, which introduce new ids
    // nearly every frame (issues/render-dlss-not-applied).
    viewer::TemporalFrame streamed_in = temporal.begin(
        moved_camera, {120, 80}, {100, 80},
        {moved_object, {99, identity_matrix()}}, true, {});
    CHECK(!streamed_in.reset && streamed_in.instances.size() == 2 &&
              streamed_in.instances[0].history_valid &&
              !streamed_in.instances[1].history_valid,
          "streamed-in instance joins without resetting global history");
    CHECK(temporal.commit_presented(streamed_in.attempt_token),
          "streamed-in candidate commits");

    viewer::TemporalFrame failed = temporal.begin(
        moved_camera, {120, 80}, {100, 80}, {{99, identity_matrix()}}, true, {});
    const uint64_t failed_token = failed.attempt_token;
    CHECK(temporal.discard_failed_attempt(failed_token),
          "failed presentation discards uncommitted candidate");
    viewer::TemporalFrame after_failure = temporal.begin(
        moved_camera, {120, 80}, {100, 80}, {{99, identity_matrix()}}, true, {});
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
        {{a_id, identity_matrix()}, {b_id, identity_matrix()}}, true, {});
    CHECK(stable_ids.commit_presented(two.attempt_token),
          "two-instance temporal baseline commits");
    viewer::TemporalFrame only_b = stable_ids.begin(
        camera, {100, 80}, {100, 80}, {{b_id, identity_matrix()}}, true, {});
    CHECK(!only_b.reset && only_b.instances.size() == 1 &&
              only_b.instances[0].instance_id == b_id &&
              only_b.instances[0].history_valid,
          "stable rigid identity survives [A,B] to [B] compaction");
    CHECK(stable_ids.commit_presented(only_b.attempt_token),
          "single surviving instance commits");
    viewer::TemporalFrame empty = stable_ids.begin(
        camera, {100, 80}, {100, 80}, {}, true, {});
    CHECK(stable_ids.commit_presented(empty.attempt_token),
          "presented clear frame advances empty temporal history");
    viewer::TemporalFrame returning_b = stable_ids.begin(
        camera, {100, 80}, {100, 80}, {{b_id, identity_matrix()}}, true, {});
    CHECK(!returning_b.reset && !returning_b.instances[0].history_valid,
          "instance returning after a presented clear frame starts fresh "
          "without a global reset");

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

    // Per-source memo: a publish only adds a source, so every pre-existing
    // source must still be served from its memo on the rebuild that follows.
    std::vector<viewer::VkSceneInstance> a_expansion(3);
    a_expansion[0].part_hash = 31;
    CHECK(cache.find_source(a) == nullptr, "empty memo misses");
    cache.store_source(a, a_expansion);
    const std::vector<viewer::VkSceneInstance>* hit = cache.find_source(a);
    CHECK(hit != nullptr && hit->size() == 3 && (*hit)[0].part_hash == 31,
          "memoised source round-trips its expansion");
    CHECK(cache.find_source(b) == nullptr, "a different source misses the memo");

    viewer::ResolvedInstance moved = a;
    moved.transform[3] = 5.0f;
    CHECK(cache.find_source(moved) == nullptr,
          "same stable id with a changed transform misses the memo");
    viewer::ResolvedInstance repointed = a;
    repointed.part_hash = 99;
    CHECK(cache.find_source(repointed) == nullptr,
          "same stable id naming a different part misses the memo");

    // invalidate_expansion keeps memos (the publish path); invalidate drops
    // them (every release path).
    cache.store(roots, std::vector<viewer::VkSceneInstance>(2));
    cache.invalidate_expansion();
    CHECK(!cache.matches(roots) && cache.find_source(a) != nullptr,
          "invalidate_expansion drops the flat set but keeps memos");
    cache.store(roots, std::vector<viewer::VkSceneInstance>(2));
    cache.invalidate_sources();
    CHECK(cache.matches(roots) && cache.find_source(a) == nullptr,
          "invalidate_sources drops memos but keeps the flat set");
    cache.store_source(a, a_expansion);
    cache.invalidate();
    CHECK(cache.find_source(a) == nullptr,
          "full invalidation drops memos too");

    // Pruning bounds the memo to the live source set.
    cache.store_source(a, a_expansion);
    cache.store_source(b, a_expansion);
    cache.prune_sources(std::vector<viewer::ResolvedInstance>{a});
    CHECK(cache.find_source(a) != nullptr && cache.find_source(b) == nullptr,
          "prune_sources keeps live sources and drops absent ones");
    CHECK(cache.source_memo_size() == 1, "pruned memo holds only live sources");
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
                               uint32_t first_index);

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
    // The raster path is indexed (vkCmdDrawIndexedIndirect), so a part without
    // indices uploads zero of them and render_gbuffer_and_composite fails
    // closed with "raster render requires uploaded draw commands, vertices,
    // and indices". Callers used to patch this in locally one fixture at a
    // time; supply the triangle's own list here so every fixture is renderable.
    part.indices = {0, 1, 2};
    return part;
}

void run_rt_lod_payload_contract_tests() {
    viewer::VkScenePart part{};
    part.part_hash = 0x4c4f4452u;
    part.clusters = {
        {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 20.0f,
         {{0, 6, 1.0f}, {6, 3, 0.0f}}},
        {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f,
         {{9, 6, 1.0f}, {15, 3, 0.0f}}},
    };
    part.vertices.resize(18);
    const auto selected = viewer::vk_scene_detail::select_rt_instance_geometry(
        part, identity_matrix(), {0.0f, 0.0f, 10.0f}, 1.0f);
    CHECK(selected.size() == 2 &&
              selected[0].cluster_index == 0 &&
              selected[0].lod_index == 0 &&
              selected[0].first_index == 0 &&
              selected[0].index_count == 6 &&
              selected[1].cluster_index == 1 &&
              selected[1].lod_index == 1 &&
              selected[1].first_index == 15 &&
              selected[1].index_count == 3,
          "RT instance payload contains exactly one raster-selected LOD per cluster");
    const auto offsets = viewer::vk_scene_detail::dense_rt_lod_offsets(part);
    uint32_t record_index = UINT32_MAX;
    CHECK(offsets == std::vector<uint32_t>({0, 2, 4}) &&
              viewer::vk_scene_detail::dense_rt_lod_index(
                  offsets, 1, 1, record_index) &&
              record_index == 3 &&
              !viewer::vk_scene_detail::dense_rt_lod_index(
                  offsets, 1, 2, record_index),
          "dense RT LOD offsets provide bounded O(1) cluster/LOD indexing");
    // M1: select_cluster_lod_view reads normalized SWITCH DISTANCES, not
    // thresholds. Authored through the conversion rather than as bare
    // constants, so the unit is visible at the callsite -- the old {1.0f, 0.0f}
    // would still compile here and would silently mean something else.
    // Intent is unchanged: rung 0 reaches one radius-scaled unit, rung 1 is
    // open, and a camera 10 units out lands on rung 1.
    const float switch_distances[] = {
        lod::normalized_switch_distance(1.0f),   // -> 1.0
        lod::normalized_switch_distance(0.0f),   // -> INFINITY, always qualifies
    };
    CHECK(viewer::vk_scene_detail::select_cluster_lod_view(
              part.clusters[1].aabb_min, part.clusters[1].aabb_max,
              part.clusters[1].radius, switch_distances, 2, identity_matrix(),
              {0.0f, 0.0f, 10.0f}, 1.0f) == 1,
          "non-owning RT LOD view matches raster distance selection");

    // M2.5: the terminal billboard is not traceable geometry. It is oriented
    // in the vertex stage, so its BLAS holds the quad UNROTATED -- a ray that
    // hits it sees an axis-fixed rectangle where raster drew a camera-facing
    // card. A cluster whose selected rung is that billboard therefore
    // contributes NOTHING to the RT payload rather than contributing the wrong
    // shape (and rather than being clamped down to the last mesh rung, which
    // buries the card inside the traced mesh -- see build_ray_geometry).
    {
        viewer::VkScenePart billboard{};
        billboard.part_hash = 0x494d504fu;
        // Two rungs: a 2-triangle mesh, then the 2-triangle billboard. Only
        // the second carries the marker, so only the second is peeled.
        billboard.clusters = {
            {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f,
             {{0, 6, 1.0f}, {6, 6, 0.0f}}},
        };
        billboard.indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        billboard.vertices.resize(12);
        // surface.x carries impostor::kQuadMarker on the billboard's vertices;
        // build_vulkan_part forwards it from TriEx::uv0.x, and raster.vert /
        // gbuffer.frag branch on the same sentinel.
        for (size_t i = 6; i < billboard.vertices.size(); ++i)
            billboard.vertices[i].surface.x = impostor::kQuadMarker;
        CHECK(viewer::vk_scene_detail::cluster_mesh_lod_count(billboard, 0) == 1,
              "the trailing billboard rung is not counted as a mesh rung");
        // A near camera selects rung 0 (the mesh): still traced.
        const auto near_selection =
            viewer::vk_scene_detail::select_rt_instance_geometry(
                billboard, identity_matrix(), {0.0f, 0.0f, 1.0f}, 1.0f);
        // A far camera selects rung 1 (the billboard): traced by nothing.
        const auto far_selection =
            viewer::vk_scene_detail::select_rt_instance_geometry(
                billboard, identity_matrix(), {0.0f, 0.0f, 10000.0f}, 1.0f);
        CHECK(near_selection.size() == 1 && near_selection[0].lod_index == 0 &&
                  near_selection[0].index_count == 6 && far_selection.empty(),
              "RT traces the mesh rung and drops the cluster at its billboard");
    }
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
    // emission_strength.rgb is the emission *color*, and both composite.frag
    // and rt_lighting.rgen read it unconditionally. material_registry.c
    // guarantees that by normalizing a legacy "emission > 0 with a black
    // emission color" material to its albedo; a fixture that builds
    // MaterialGpuRecord by hand has to honor the same contract or the emissive
    // term multiplies by zero. This fixture set only .w for as long as the
    // suite was dark -- it predates f68e0f03, which moved the composite from
    // `albedo.rgb * strength` to the authored emission color.
    materials[7].emission_strength[0] = materials[7].base_roughness[0];
    materials[7].emission_strength[1] = materials[7].base_roughness[1];
    materials[7].emission_strength[2] = materials[7].base_roughness[2];
    materials[8] = materials[7];
    materials[8].base_roughness[0] = 0.8f;
    materials[8].emission_strength[0] = materials[8].base_roughness[0];
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
    // Reversed-Z: triangle at view distance 2.0 with near=0.1/far=10 projects
    // to near*(far-d)/((far-near)*d) = 0.1*8/(9.9*2) = 0.040404 (was 0.959596
    // under the standard-Z convention).
    CHECK(std::fabs(center.depth - 0.040404f) <= 2e-3f,
          "known center projected depth");
    CHECK(lower_right_inside.albedo.w > 0.99f,
          "negative-height viewport preserves top-left framebuffer convention");
    // Reversed-Z: the depth clear value (background) is 0.0, not 1.0.
    CHECK(background.albedo.w < 0.01f && background.depth <= 0.001f,
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
    // Not black any more: composite.frag's depth-miss branch writes
    // sky_with_sun() for pixels the G-buffer never covered (added by 97796e69),
    // so the old all-zero expectation asserted the absence of the sky. Opaque,
    // finite and strictly positive is what the sky path now guarantees; the
    // authored-sky *response* is covered by the bright/dark pair below.
    CHECK(background.hdr.w == 1.0f && background.hdr.x > 1e-3f &&
              background.hdr.y > 1e-3f && background.hdr.z > 1e-3f,
          "cleared background composites the authored sky, opaque and positive");
    CHECK(close3(center.visibility, {1.0f, 1.0f, 1.0f}, 1e-6f) &&
              close3(background.visibility, {1.0f, 1.0f, 1.0f}, 1e-6f),
          "disabled RT GPU clear writes full visibility");
    // The geometry pixel must come from the shaded G-buffer, not from the sky
    // branch: if the composite ever stopped sampling the G-buffer, the covered
    // pixel would fall through and match the background exactly. Ordering is no
    // longer the discriminator (the sky is brighter than this lit surface), so
    // require a real difference plus a plausible lit value.
    CHECK(std::isfinite(center.hdr.x) && center.hdr.x > 1e-3f &&
              (std::fabs(center.hdr.x - background.hdr.x) > 1e-2f ||
               std::fabs(center.hdr.y - background.hdr.y) > 1e-2f ||
               std::fabs(center.hdr.z - background.hdr.z) > 1e-2f),
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
    dark.atmosphere_sources.authored_display_sky_chroma_rgb = {};
    dark.atmosphere_sources.authored_irradiance_chroma_rgb = {};
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
    bright.atmosphere_sources.authored_display_sky_chroma_rgb =
        {2.0f, 2.0f, 2.0f};
    bright.atmosphere_sources.authored_irradiance_chroma_rgb =
        {2.0f, 2.0f, 2.0f};
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

    // Task 7 must make this a real physical-atmosphere readback, rather than
    // merely changing the final sun RGB on the CPU.  (4,4) is a reliable
    // depth-miss, away from the analytic sun disc, while the center triangle
    // is an upward-facing receiver.  Keep the sky modifier non-zero: in the
    // physical ABI it is a componentwise LUT modifier, not an on/off switch.
    const MaterialGpuRecord physical_receiver = materials[7];
    materials[7].base_roughness[0] = 1.0f;
    materials[7].base_roughness[1] = 1.0f;
    materials[7].base_roughness[2] = 1.0f;
    materials[7].emission_strength[0] = 0.0f;
    materials[7].emission_strength[1] = 0.0f;
    materials[7].emission_strength[2] = 0.0f;
    materials[7].emission_strength[3] = 0.0f;
    CHECK(renderer.update_materials(materials, 7, 2, error) &&
              renderer.dispatch_culling(frame, camera.position, 1.0f, error),
          error.empty() ? "stage neutral physical-atmosphere receiver"
                        : error.c_str());

    struct AtmosphereRasterCase {
        viewer::VkRasterPixel direct_on{};
        viewer::VkRasterPixel direct_off{};
        viewer::VkRasterPixel sky{};
        viewer::ResolvedAtmosphereStatus status{};
    };
    const auto render_atmosphere_case = [&](float elevation_deg,
                                            AtmosphereRasterCase& result) {
        const matter::Float3 to_sun =
            matter::atmosphere_to_sun_from_elevation_deg(elevation_deg);
        viewer::VkSceneLighting lighting{};
        // VkSceneLighting stores the engine convention (sun -> scene), while
        // the physical helpers above first construct to_sun.
        lighting.sun_direction = {-to_sun.x, -to_sun.y, -to_sun.z};
        lighting.authored_sun_rgb = {1.0f, 1.0f, 1.0f};
        lighting.sun_intensity = 1.0f;
        lighting.atmosphere_sources.authored_display_sky_chroma_rgb =
            {1.0f, 1.0f, 1.0f};
        lighting.atmosphere_sources.authored_irradiance_chroma_rgb =
            {1.0f, 1.0f, 1.0f};
        lighting.atmosphere_sources.sun_multiplier = 1.0f;
        lighting.atmosphere_sources.sky_multiplier = 1.0f;
        renderer.set_lighting(lighting);
        CHECK(renderer.render_gbuffer_and_composite(width, height, error),
              error.empty() ? "render physical-atmosphere direct-on case"
                            : error.c_str());
        CHECK(renderer.readback_raster_pixel(width / 2, height / 2,
                                             result.direct_on, error) &&
                  renderer.readback_raster_pixel(4, 4, result.sky, error),
              error.empty() ? "read physical-atmosphere direct-on pixels"
                            : error.c_str());
        result.status = renderer.test_resolved_atmosphere_status();

        lighting.sun_intensity = 0.0f;
        renderer.set_lighting(lighting);
        CHECK(renderer.render_gbuffer_and_composite(width, height, error) &&
                  renderer.readback_raster_pixel(width / 2, height / 2,
                                                 result.direct_off, error),
              error.empty() ? "read physical-atmosphere direct-off receiver"
                            : error.c_str());
    };
    AtmosphereRasterCase atmosphere_cases[6]{};
    for (size_t index = 0; index < 6; ++index) {
        render_atmosphere_case(kAtmosphereGpuElevations[index],
                               atmosphere_cases[index]);
        CHECK(std::fabs(atmosphere_cases[index].status.direct_world_ratio -
                        kAtmosphereGpuRatios[index]) <= 1.0e-6f,
              "GPU-published direct-world ratio matches its exact elevation anchor");
        g_atmosphere_raster_direct_rgb[index] =
            atmosphere_cases[index].status.direct_world_sun_rgb;
    }

    // Task 13 RED/GREEN: production composite lighting must consume the same
    // cumulative cloud field as the diagnostic, without touching evaluated SH
    // ambient. The fixed triangle is a depth-covered object receiver at y=0;
    // slice 12 is an overhead slab while slice 2 is wholly below it.
    const auto raster_receiver_capture = [&](float sun_intensity,
                                             viewer::VkRasterPixel& pixel,
                                             float cloud_debug = 0.0f,
                                             float cloud_top = 5.0f) {
        viewer::VkSceneLighting receiver_lighting{};
        receiver_lighting.sun_direction = {0.0f, -1.0f, 0.0f};
        receiver_lighting.sun_intensity = sun_intensity;
        receiver_lighting.authored_sun_rgb = {1.0f, 1.0f, 1.0f};
        receiver_lighting.atmosphere_sources.authored_display_sky_chroma_rgb =
            {1.0f, 1.0f, 1.0f};
        receiver_lighting.atmosphere_sources.authored_irradiance_chroma_rgb =
            {0.35f, 0.35f, 0.35f};
        receiver_lighting.atmosphere_sources.sun_multiplier = 1.0f;
        receiver_lighting.atmosphere_sources.sky_multiplier = 1.0f;
        receiver_lighting.camera_near = camera.near_plane;
        receiver_lighting.camera_far = camera.far_plane;
        receiver_lighting.vol_cloud_top = cloud_top;
        receiver_lighting.vol_debug_view = cloud_debug;
        renderer.set_lighting(receiver_lighting);
        return renderer.render_gbuffer_and_composite(width, height, error) &&
               renderer.readback_raster_pixel(width / 2, height / 2, pixel,
                                              error);
    };
    const auto receiver_luma = [](const matter::Float4& value) {
        return value.x * 0.2126f + value.y * 0.7152f + value.z * 0.0722f;
    };
    matter::VulkanVolumetricsSettings receiver_volumetrics{};
    receiver_volumetrics.enabled = false;
    matter::FogSettings receiver_deck{};
    receiver_deck.cloud_count = 1;
    receiver_deck.clouds[0].enabled = true;
    receiver_deck.clouds[0].min_height = 4.0f;
    receiver_deck.clouds[0].max_height = 5.0f;
    receiver_deck.clouds[0].coverage = 1.0f;
    receiver_deck.clouds[0].max_density = 0.0f;
    matter::CloudShadowSettings receiver_shadows{};
    receiver_shadows.enabled = true;
    receiver_shadows.near_resolution = 0;
    receiver_shadows.near_depth_slices = 0;
    receiver_shadows.near_coverage_m = 16.0f;
    receiver_shadows.far_resolution = 0;
    receiver_shadows.far_depth_slices = 0;
    receiver_shadows.far_coverage_m = 16.0f;
    receiver_shadows.filter_scale = 0.0f;
    receiver_shadows.update_fraction = 1.0f;
    matter::CloudShadowSettings receiver_shadows_disabled = receiver_shadows;
    receiver_shadows_disabled.enabled = false;

    renderer.set_volumetrics_settings(receiver_volumetrics, receiver_deck,
                                      receiver_shadows_disabled);
    CHECK(renderer.generate_cloud_shadows_for_test(
              0u, camera.position, {0.0f, -1.0f, 0.0f}, 0.0f, error),
          error.empty() ? "generate disabled raster cloud receiver control"
                        : error.c_str());
    viewer::VkRasterPixel raster_clear_sun{}, raster_clear_ambient{};
    const bool raster_clear_read =
        raster_receiver_capture(1.0f, raster_clear_sun) &&
        raster_receiver_capture(0.0f, raster_clear_ambient);

    renderer.set_volumetrics_settings(receiver_volumetrics, receiver_deck,
                                      receiver_shadows);
    renderer.set_cloud_shadow_density_layers_for_test(
        12u, 2.0f, 2u, 0.0f, true);
    CHECK(renderer.generate_cloud_shadows_for_test(
              0u, camera.position, {0.0f, -1.0f, 0.0f}, 1.0f, error),
          error.empty() ? "generate overhead raster cloud slab"
                        : error.c_str());
    viewer::VkRasterPixel raster_shadow_sun{}, raster_shadow_ambient{};
    const bool raster_shadow_read =
        raster_receiver_capture(1.0f, raster_shadow_sun) &&
        raster_receiver_capture(0.0f, raster_shadow_ambient);
    viewer::VkRasterPixel raster_shadow_debug{};
    const bool raster_shadow_debug_read =
        raster_receiver_capture(1.0f, raster_shadow_debug, 5.0f);
    const float raster_clear_direct = receiver_luma({
        raster_clear_sun.hdr.x - raster_clear_ambient.hdr.x,
        raster_clear_sun.hdr.y - raster_clear_ambient.hdr.y,
        raster_clear_sun.hdr.z - raster_clear_ambient.hdr.z, 0.0f});
    const float raster_shadow_direct = receiver_luma({
        raster_shadow_sun.hdr.x - raster_shadow_ambient.hdr.x,
        raster_shadow_sun.hdr.y - raster_shadow_ambient.hdr.y,
        raster_shadow_sun.hdr.z - raster_shadow_ambient.hdr.z, 0.0f});
    std::fprintf(stderr,
                 "task13 raster direct clear=%.7f shadow=%.7f debug=%d/%.5f cloud_state=%.1f/%.1f/%.1f/%.1f\n",
                 raster_clear_direct, raster_shadow_direct,
                 raster_shadow_debug_read ? 1 : 0, raster_shadow_debug.hdr.x,
                 renderer.cloud_shadow_environment_state_for_test(0),
                 renderer.cloud_shadow_environment_state_for_test(1),
                 renderer.cloud_shadow_environment_state_for_test(2),
                 renderer.cloud_shadow_environment_state_for_test(3));
    CHECK(raster_clear_read && raster_shadow_read &&
              raster_clear_direct > 1.0e-4f &&
              raster_shadow_direct < raster_clear_direct * 0.35f,
          error.empty()
              ? "overhead cloud slab attenuates raster object direct lighting"
              : error.c_str());
    CHECK((materials[7].flags_misc[0] & MATERIAL_ALPHA_TESTED) != 0u &&
              raster_shadow_read &&
              raster_shadow_direct < raster_clear_direct * 0.35f,
          "overhead cloud slab attenuates alpha-tested vegetation-style geometry");
    CHECK(raster_clear_read && raster_shadow_read &&
              close4(raster_clear_ambient.hdr, raster_shadow_ambient.hdr,
                     2.0e-4f),
          "raster cloud attenuation leaves evaluated SH ambient unchanged");

    renderer.set_cloud_shadow_density_layers_for_test(
        2u, 2.0f, 1u, 0.0f, true);
    CHECK(renderer.generate_cloud_shadows_for_test(
              0u, camera.position, {0.0f, -1.0f, 0.0f}, 2.0f, error),
          error.empty() ? "generate below-receiver raster cloud slab"
                        : error.c_str());
    viewer::VkRasterPixel raster_above_sun{}, raster_above_ambient{};
    const bool raster_above_read =
        raster_receiver_capture(1.0f, raster_above_sun) &&
        raster_receiver_capture(0.0f, raster_above_ambient);
    const float raster_above_direct = receiver_luma({
        raster_above_sun.hdr.x - raster_above_ambient.hdr.x,
        raster_above_sun.hdr.y - raster_above_ambient.hdr.y,
        raster_above_sun.hdr.z - raster_above_ambient.hdr.z, 0.0f});
    CHECK(raster_above_read &&
              std::fabs(raster_above_direct - raster_clear_direct) <
                  raster_clear_direct * 0.05f,
          "raster receiver above the authored slab remains clear");

    renderer.set_volumetrics_settings(receiver_volumetrics, receiver_deck,
                                      receiver_shadows_disabled);
    CHECK(renderer.generate_cloud_shadows_for_test(
              0u, camera.position, {0.0f, -1.0f, 0.0f}, 3.0f, error),
          error.empty() ? "disable raster receiver cloud shadows"
                        : error.c_str());
    viewer::VkRasterPixel raster_disabled_sun{}, raster_disabled_ambient{};
    const bool raster_disabled_read =
        raster_receiver_capture(1.0f, raster_disabled_sun) &&
        raster_receiver_capture(0.0f, raster_disabled_ambient);
    const float raster_disabled_direct = receiver_luma({
        raster_disabled_sun.hdr.x - raster_disabled_ambient.hdr.x,
        raster_disabled_sun.hdr.y - raster_disabled_ambient.hdr.y,
        raster_disabled_sun.hdr.z - raster_disabled_ambient.hdr.z, 0.0f});
    CHECK(raster_disabled_read &&
              std::fabs(raster_disabled_direct - raster_clear_direct) <
                  raster_clear_direct * 0.05f,
          "disabled raster cloud shadows return the receiver to clear control");

    matter::FogSettings receiver_prefix_hole = receiver_deck;
    receiver_prefix_hole.clouds[1].enabled = false;
    receiver_prefix_hole.clouds[2].enabled = true;
    receiver_prefix_hole.clouds[2].min_height = 900.0f;
    receiver_prefix_hole.clouds[2].max_height = 1000.0f;
    renderer.set_volumetrics_settings(receiver_volumetrics,
                                      receiver_prefix_hole,
                                      receiver_shadows);
    CHECK(renderer.generate_cloud_shadows_for_test(
              0u, camera.position, {0.0f, -1.0f, 0.0f}, 3.5f, error) &&
              renderer.cloud_shadow_environment_state_for_test(3) == 5.0f,
          error.empty()
              ? "receiver cloud top ignores enabled layers parked after a prefix hole"
              : error.c_str());

    // A high cloud top and nonzero filter scale must widen the transition
    // across the deterministic checker deck while conserving mean optical
    // depth. This reads the production fragment receiver sampler, not a CPU
    // approximation, and therefore exercises receiver-distance filtering.
    matter::FogSettings receiver_high_deck = receiver_deck;
    receiver_high_deck.clouds[0].min_height = 999.0f;
    receiver_high_deck.clouds[0].max_height = 1000.0f;
    const auto read_filter_profile = [&](std::vector<float>& tau) {
        viewer::VkRasterPixel rendered{};
        if (!raster_receiver_capture(1.0f, rendered, 5.0f, 1000.0f))
            return false;
        for (uint32_t x = width / 2 - 24; x <= width / 2 + 24; ++x) {
            viewer::VkRasterPixel sample{};
            if (!renderer.readback_raster_pixel(x, height / 2, sample, error))
                return false;
            if (sample.depth <= 0.0f) continue;
            tau.push_back(-std::log(std::max(sample.hdr.x, 1.0e-6f)));
        }
        return !tau.empty();
    };
    matter::CloudShadowSettings receiver_sharp_filter = receiver_shadows;
    receiver_sharp_filter.filter_scale = 0.0f;
    renderer.set_volumetrics_settings(receiver_volumetrics,
                                      receiver_high_deck,
                                      receiver_sharp_filter);
    renderer.set_cloud_shadow_density_checker_for_test(0.0f, 0.25f, true);
    CHECK(renderer.generate_cloud_shadows_for_test(
              0u, camera.position, {0.0f, -1.0f, 0.0f}, 4.0f, error),
          error.empty() ? "generate sharp high-cloud receiver checker"
                        : error.c_str());
    std::vector<float> sharp_tau;
    const bool sharp_profile_read = read_filter_profile(sharp_tau);

    matter::CloudShadowSettings receiver_wide_filter = receiver_shadows;
    receiver_wide_filter.filter_scale = 4.0f;
    renderer.set_volumetrics_settings(receiver_volumetrics,
                                      receiver_high_deck,
                                      receiver_wide_filter);
    CHECK(renderer.generate_cloud_shadows_for_test(
              0u, camera.position, {0.0f, -1.0f, 0.0f}, 5.0f, error),
          error.empty() ? "generate filtered high-cloud receiver checker"
                        : error.c_str());
    std::vector<float> wide_tau;
    const bool wide_profile_read = read_filter_profile(wide_tau);
    const auto profile_stats = [](const std::vector<float>& values) {
        std::array<float, 4> result{
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::lowest(), 0.0f, 0.0f};
        for (float value : values) {
            result[0] = std::min(result[0], value);
            result[1] = std::max(result[1], value);
            result[2] += value;
        }
        if (!values.empty()) result[2] /= static_cast<float>(values.size());
        return result;
    };
    const auto sharp_stats = profile_stats(sharp_tau);
    const auto wide_stats = profile_stats(wide_tau);
    const float sharp_range = sharp_stats[1] - sharp_stats[0];
    const float penumbra_low = sharp_stats[0] + sharp_range * 0.15f;
    const float penumbra_high = sharp_stats[1] - sharp_range * 0.15f;
    uint32_t sharp_penumbra = 0u;
    uint32_t wide_penumbra = 0u;
    for (float value : sharp_tau)
        if (value > penumbra_low && value < penumbra_high)
            ++sharp_penumbra;
    for (float value : wide_tau)
        if (value > penumbra_low && value < penumbra_high)
            ++wide_penumbra;
    std::fprintf(stderr,
                 "task13 filter sharp=%.4f..%.4f mean=%.4f penumbra=%.0f "
                 "wide=%.4f..%.4f mean=%.4f penumbra=%.0f samples=%zu/%zu\n",
                 sharp_stats[0], sharp_stats[1], sharp_stats[2],
                 static_cast<double>(sharp_penumbra), wide_stats[0],
                 wide_stats[1], wide_stats[2],
                 static_cast<double>(wide_penumbra),
                 sharp_tau.size(), wide_tau.size());
    CHECK(sharp_profile_read && wide_profile_read &&
              sharp_tau.size() == wide_tau.size() && sharp_tau.size() >= 16u &&
              sharp_range > 0.5f &&
              wide_stats[1] - wide_stats[0] < sharp_range * 0.6f &&
              wide_penumbra > sharp_penumbra &&
              std::fabs(wide_stats[2] - sharp_stats[2]) < sharp_range * 0.12f,
          "high-cloud receiver filtering widens penumbra while conserving mean optical depth");
    renderer.clear_cloud_shadow_density_override_for_test(true);
    g_atmosphere_raster_direct_valid = true;
    const AtmosphereRasterCase& noon = atmosphere_cases[0];
    const AtmosphereRasterCase& low_sun = atmosphere_cases[2];
    const AtmosphereRasterCase& twilight = atmosphere_cases[4];
    const auto finite_hdr = [](const viewer::VkRasterPixel& pixel) {
        return std::isfinite(pixel.hdr.x) && std::isfinite(pixel.hdr.y) &&
               std::isfinite(pixel.hdr.z) && std::isfinite(pixel.hdr.w);
    };
    const auto rgb_luma = [](const viewer::VkRasterPixel& pixel) {
        return 0.2126f * pixel.hdr.x + 0.7152f * pixel.hdr.y +
               0.0722f * pixel.hdr.z;
    };
    const auto direct_luma = [&](const AtmosphereRasterCase& value) {
        return std::max(0.0f, rgb_luma(value.direct_on) -
                                  rgb_luma(value.direct_off));
    };
    CHECK(finite_hdr(noon.sky) && finite_hdr(low_sun.sky) &&
              finite_hdr(twilight.sky) && noon.sky.hdr.x > 1e-4f &&
              noon.sky.hdr.y > 1e-4f && noon.sky.hdr.z > 1e-4f &&
              low_sun.sky.hdr.x > 1e-4f && low_sun.sky.hdr.y > 1e-4f &&
              low_sun.sky.hdr.z > 1e-4f && twilight.sky.hdr.x > 1e-4f &&
              twilight.sky.hdr.y > 1e-4f && twilight.sky.hdr.z > 1e-4f,
          "physical sky is finite and nonblack at 90, 5, and -5 degrees");
    CHECK(std::fabs(noon.sky.hdr.x - low_sun.sky.hdr.x) > 1e-2f ||
              std::fabs(noon.sky.hdr.y - low_sun.sky.hdr.y) > 1e-2f ||
              std::fabs(noon.sky.hdr.z - low_sun.sky.hdr.z) > 1e-2f ||
              std::fabs(low_sun.sky.hdr.x - twilight.sky.hdr.x) > 1e-2f ||
              std::fabs(low_sun.sky.hdr.y - twilight.sky.hdr.y) > 1e-2f ||
              std::fabs(low_sun.sky.hdr.z - twilight.sky.hdr.z) > 1e-2f,
          "physical sky readback changes with solar elevation");
    const float noon_direct = direct_luma(noon);
    const float low_sun_direct = direct_luma(low_sun);
    const float twilight_direct = direct_luma(twilight);
    CHECK(noon_direct > low_sun_direct && low_sun_direct > 1e-4f &&
              twilight_direct < 1e-4f,
          "GPU direct sun is positive/dimmer near the horizon and zero below it");
    for (size_t index = 3; index < 6; ++index) {
        CHECK(atmosphere_cases[index].direct_on.hdr.x ==
                  atmosphere_cases[index].direct_off.hdr.x &&
                  atmosphere_cases[index].direct_on.hdr.y ==
                  atmosphere_cases[index].direct_off.hdr.y &&
                  atmosphere_cases[index].direct_on.hdr.z ==
                  atmosphere_cases[index].direct_off.hdr.z,
              "raster direct contribution is exactly zero at and below the horizon");
    }
    CHECK(rgb_luma(twilight.direct_off) > 1.0e-4f,
          "-5 degree upward receiver remains positively lit by evaluated SH");
    const float noon_blue = std::max(noon.direct_on.hdr.z -
                                         noon.direct_off.hdr.z,
                                     1e-5f);
    const float low_blue = std::max(low_sun.direct_on.hdr.z -
                                        low_sun.direct_off.hdr.z,
                                    1e-5f);
    const float noon_warmth = (noon.direct_on.hdr.x - noon.direct_off.hdr.x) /
                              noon_blue;
    const float low_warmth =
        (low_sun.direct_on.hdr.x - low_sun.direct_off.hdr.x) / low_blue;
    CHECK(low_warmth > noon_warmth,
          "GPU direct sun becomes warmer near the horizon");
    std::printf("physical atmosphere raster: noon=%.5f low=%.5f twilight=%.5f "
                "sky90=%.5f %.5f %.5f sky5=%.5f %.5f %.5f sky-5=%.5f %.5f %.5f\n",
                noon_direct, low_sun_direct, twilight_direct, noon.sky.hdr.x,
                noon.sky.hdr.y, noon.sky.hdr.z, low_sun.sky.hdr.x,
                low_sun.sky.hdr.y, low_sun.sky.hdr.z, twilight.sky.hdr.x,
                twilight.sky.hdr.y, twilight.sky.hdr.z);

    materials[7] = physical_receiver;
    CHECK(renderer.update_materials(materials, 8, 2, error) &&
              renderer.dispatch_culling(frame, camera.position, 1.0f, error),
          error.empty() ? "restore raster material after physical-atmosphere case"
                        : error.c_str());

    const VkImage old_albedo = attachments.albedo.image;
    const VkDeviceSize initial_vertex_capacity =
        renderer.raster_vertex_buffer_size();
    const uint32_t initial_vertex_count = renderer.raster_vertex_count();
    renderer.release_part(901);
    // Free-range recycling (issues/render-streaming-build-cpu movement
    // hitches): a release quarantines the part's ranges for the in-flight
    // window instead of compacting O(world), so the staging high-water mark
    // is unchanged. What must hold instead is that churn REUSES the ranges —
    // bounded residency across an evict/publish cycle.
    CHECK(renderer.raster_vertex_count() == initial_vertex_count,
          "releasing a raster part keeps the staging high-water mark");
    CHECK(renderer.uploaded_raster_draw_command_count() == 1,
          "staging release preserves the last uploaded raster mask");
    for (uint64_t hash = 902; hash <= 906; ++hash) {
        // Advance past the in-flight window so the freed range settles and
        // the next registration reuses it instead of growing the tail.
        for (int settle = 0; settle < 4; ++settle) {
            CHECK(renderer.update_instances({{900, identity}}, error) &&
                      renderer.dispatch_culling(frame, camera.position, 1.0f,
                                                error),
                  error.empty() ? "settle freed raster range"
                                : error.c_str());
        }
        CHECK(renderer.ensure_part(known_raster_triangle(hash), error) >= 0 &&
                  renderer.raster_vertex_count() == initial_vertex_count,
              error.empty()
                  ? "re-added raster part reuses the freed vertex range"
                  : error.c_str());
        CHECK(renderer.update_instances({{900, identity}, {hash, identity}},
                                        error) &&
                  renderer.dispatch_culling(frame, camera.position, 1.0f,
                                             error),
              error.empty() ? "dispatch re-added raster part"
                            : error.c_str());
        CHECK(renderer.raster_vertex_buffer_size() == initial_vertex_capacity,
              "streaming eviction/reload keeps raster vertex residency bounded");
        if (hash != 906) renderer.release_part(hash);
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
    // --- LOD debug tint (viewer.debug "LOD levels") -------------------------
    //
    // Two rungs of the SAME near, covered geometry, so nothing about the pixel
    // can move except which rung cull.comp selected for it. A transform-tail
    // regression, or a varying-location collision, reads out here as the wrong
    // hue rather than as a subtle shading difference nobody notices.
    {
        viewer::VkScenePart two_rung = known_raster_triangle(907);
        // 1000 normalizes to a 0.001 switch distance, which this triangle at
        // 2 m fails; rung 1 is open (threshold 0) and therefore wins.
        two_rung.clusters[0].lods[0].threshold = 1000.0f;
        two_rung.clusters[0].lods.push_back({0, 3, 0.0f});
        CHECK(renderer.ensure_part(two_rung, error) >= 0,
              error.empty() ? "ensure two-rung LOD tint part" : error.c_str());
        CHECK(renderer.update_instances({{907, identity, 907}}, error) &&
                  renderer.dispatch_culling(frame, camera.position, 1.0f,
                                            error) &&
                  renderer.render_gbuffer_and_composite(width, height, error),
              error.empty() ? "render two-rung part with the view off"
                            : error.c_str());
        viewer::VkRasterPixel untinted{};
        CHECK(renderer.readback_raster_pixel(width / 2, height / 2, untinted,
                                             error) &&
                  untinted.albedo.w > 0.99f,
              error.empty() ? "read two-rung center with the view off"
                            : error.c_str());

        renderer.set_geometry_debug_view(matter::GeometryDebugView::LodTint);
        viewer::VkRasterPixel tinted{};
        CHECK(renderer.render_gbuffer_and_composite(width, height, error) &&
                  renderer.readback_raster_pixel(width / 2, height / 2, tinted,
                                                 error),
              error.empty() ? "render and read the LOD-tinted center"
                            : error.c_str());
        const matter::DebugRgb rung0 = matter::lod_debug_color(0);
        const matter::DebugRgb rung1 = matter::lod_debug_color(1);
        CHECK(std::fabs(tinted.albedo.x -
                        (0.85f * rung1.r + 0.15f * untinted.albedo.x)) < 0.02f &&
                  std::fabs(tinted.albedo.y -
                            (0.85f * rung1.g + 0.15f * untinted.albedo.y)) <
                      0.02f &&
                  std::fabs(tinted.albedo.z -
                            (0.85f * rung1.b + 0.15f * untinted.albedo.z)) <
                      0.02f,
              "LOD tint paints the exact rung cull.comp selected, not rung 0");
        CHECK(std::fabs(rung0.r - rung1.r) + std::fabs(rung0.g - rung1.g) +
                      std::fabs(rung0.b - rung1.b) >
                  0.2f,
              "adjacent rungs are far enough apart to read as different rungs");
        CHECK(tinted.material_index == untinted.material_index &&
                  tinted.instance_token == untinted.instance_token &&
                  std::fabs(tinted.depth - untinted.depth) < 1e-4f,
              "the tint rewrites albedo only, leaving identity and depth alone");

        renderer.set_geometry_debug_view(matter::GeometryDebugView::None);
        viewer::VkRasterPixel restored{};
        CHECK(renderer.render_gbuffer_and_composite(width, height, error) &&
                  renderer.readback_raster_pixel(width / 2, height / 2, restored,
                                                 error),
              error.empty() ? "render and read the center with the view off "
                              "again"
                            : error.c_str());
        CHECK(close4(restored.albedo, untinted.albedo, 1e-6f),
              "switching the view off restores the pixel exactly");

        CHECK(renderer.update_instances({{900, identity}, {906, identity}},
                                        error) &&
                  renderer.dispatch_culling(frame, camera.position, 1.0f,
                                            error),
              error.empty() ? "restore the pre-tint raster scene"
                            : error.c_str());
        renderer.release_part(907);
        renderer.consume_gi_history_reset();
    }

    // --- Wireframe debug view (viewer.debug "Wireframe") --------------------
    //
    // The claim "this is wireframe" is only worth making if the INTERIOR is
    // gone. One filled triangle, one scanline through its middle: with
    // VK_POLYGON_MODE_FILL that row is a solid covered run; with
    // VK_POLYGON_MODE_LINE only the two slanted edges survive. Counting
    // covered samples on that row is a number that cannot hold for a filled
    // frame, which is exactly what a screenshot alone cannot establish.
    {
        viewer::VkScenePart wire_part = known_raster_triangle(908);
        CHECK(renderer.ensure_part(wire_part, error) >= 0,
              error.empty() ? "ensure wireframe fixture part" : error.c_str());
        CHECK(renderer.update_instances({{908, identity, 908}}, error) &&
                  renderer.dispatch_culling(frame, camera.position, 1.0f,
                                            error) &&
                  renderer.render_gbuffer_and_composite(width, height, error),
              error.empty() ? "render the wireframe fixture filled"
                            : error.c_str());

        // The triangle spans y in [-0.75, 1.5] at z = -2; this row sits inside
        // it, below the apex and above the base, so a fill covers a long run.
        const uint32_t scan_row = height / 2;
        auto scan_covered = [&](uint32_t& covered) {
            covered = 0;
            for (uint32_t x = 0; x < width; ++x) {
                viewer::VkRasterPixel pixel{};
                if (!renderer.readback_raster_pixel(x, scan_row, pixel, error))
                    return false;
                if (pixel.albedo.w > 0.5f) ++covered;
            }
            return true;
        };
        uint32_t filled_covered = 0;
        CHECK(scan_covered(filled_covered),
              error.empty() ? "scan the filled scanline" : error.c_str());
        viewer::VkRasterPixel filled_center{};
        CHECK(renderer.readback_raster_pixel(width / 2, scan_row, filled_center,
                                             error) &&
                  filled_center.albedo.w > 0.99f,
              error.empty() ? "the filled interior is covered" : error.c_str());

        renderer.set_wireframe(true);
        // Report, never lie. A device without fillModeNonSolid has no line
        // pipelines, and the renderer must say so rather than record a filled
        // frame with the wireframe push-constant word raised.
        CHECK(renderer.wireframe_available() ==
                  vulkan.wireframe_available(),
              "renderer wireframe availability tracks the device capability");
        if (!renderer.wireframe_available()) {
            std::printf(
                "  wireframe view UNAVAILABLE on this device (%s); the view "
                "must stay inert rather than render solid\n",
                vulkan.wireframe_unavailable_reason().c_str());
            CHECK(renderer.render_gbuffer_and_composite(width, height, error),
                  error.empty() ? "render with wireframe requested but "
                                  "unsupported"
                                : error.c_str());
            CHECK(!renderer.test_last_raster_pipeline_draw().wireframe_enabled &&
                      !renderer.test_last_raster_pipeline_draw()
                           .static_mesh_wireframe,
                  "an unsupported wireframe request degrades to fill WITHOUT "
                  "claiming wireframe in the push constant");
        } else {
            CHECK(renderer.render_gbuffer_and_composite(width, height, error),
                  error.empty() ? "render the wireframe fixture as lines"
                                : error.c_str());
            const auto bound = renderer.test_last_raster_pipeline_draw();
            CHECK(bound.wireframe_enabled && bound.static_mesh_wireframe &&
                      bound.skinned_mesh_wireframe,
                  "the whole raster pass binds line pipelines together with "
                  "the shader flag that matches them");
            uint32_t line_covered = 0;
            CHECK(scan_covered(line_covered),
                  error.empty() ? "scan the wireframe scanline" : error.c_str());
            std::printf(
                "  wireframe scanline coverage: fill %u px, line %u px of %u\n",
                filled_covered, line_covered, width);
            // Measured: 40 covered samples filled, 2 in line mode.
            CHECK(filled_covered > 30u,
                  "the filled triangle covers a long run on the scan row");
            // Two 1-px edges, plus a little slack for the rasterizer's
            // diamond-exit rule on a steeply sloped edge. The upper bound is
            // the assertion that matters: a filled frame cannot produce it.
            CHECK(line_covered >= 2u && line_covered <= 8u,
                  "line mode leaves only the two slanted edges on that row");
            CHECK(line_covered * 8u < filled_covered,
                  "wireframe coverage is an order of magnitude below fill");

            viewer::VkRasterPixel wire_center{};
            CHECK(renderer.readback_raster_pixel(width / 2, scan_row,
                                                 wire_center, error),
                  error.empty() ? "read the wireframe interior" : error.c_str());
            CHECK(wire_center.albedo.w < 0.5f,
                  "the triangle INTERIOR is empty in line mode -- the single "
                  "fact a solid render can never satisfy");

            // Composition with the LOD tint: edges take the rung colour rather
            // than the standalone cyan, so density and rung read together.
            uint32_t edge_x = width;
            for (uint32_t x = 0; x < width && edge_x == width; ++x) {
                viewer::VkRasterPixel pixel{};
                if (!renderer.readback_raster_pixel(x, scan_row, pixel, error))
                    break;
                if (pixel.albedo.w > 0.5f) edge_x = x;
            }
            CHECK(edge_x < width, "found a lit wireframe edge on the scan row");
            viewer::VkRasterPixel cyan_edge{};
            CHECK(renderer.readback_raster_pixel(edge_x, scan_row, cyan_edge,
                                                 error) &&
                      cyan_edge.albedo.x < 0.05f && cyan_edge.albedo.y > 0.95f &&
                      cyan_edge.albedo.z > 0.95f,
                  "an edge with the tint off is flat cyan");
            renderer.set_geometry_debug_view(matter::GeometryDebugView::LodTint);
            viewer::VkRasterPixel tinted_edge{};
            CHECK(renderer.render_gbuffer_and_composite(width, height, error) &&
                      renderer.readback_raster_pixel(edge_x, scan_row,
                                                     tinted_edge, error),
                  error.empty() ? "render wireframe with the LOD tint on"
                                : error.c_str());
            const matter::DebugRgb rung0 = matter::lod_debug_color(0);
            CHECK(std::fabs(tinted_edge.albedo.x - rung0.r) < 0.02f &&
                      std::fabs(tinted_edge.albedo.y - rung0.g) < 0.02f &&
                      std::fabs(tinted_edge.albedo.z - rung0.b) < 0.02f,
                  "wireframe + LOD tint paints the edge the rung's own colour, "
                  "not cyan and not a blend");
            renderer.set_geometry_debug_view(matter::GeometryDebugView::None);
        }

        renderer.set_wireframe(false);
        viewer::VkRasterPixel restored_center{};
        CHECK(renderer.render_gbuffer_and_composite(width, height, error) &&
                  renderer.readback_raster_pixel(width / 2, scan_row,
                                                 restored_center, error),
              error.empty() ? "render the fixture filled again" : error.c_str());
        CHECK(close4(restored_center.albedo, filled_center.albedo, 1e-6f),
              "switching the wireframe view off restores the pixel exactly");

        CHECK(renderer.update_instances({{900, identity}, {906, identity}},
                                        error) &&
                  renderer.dispatch_culling(frame, camera.position, 1.0f,
                                            error),
              error.empty() ? "restore the pre-wireframe raster scene"
                            : error.c_str());
        renderer.release_part(908);
        renderer.consume_gi_history_reset();
    }

    // Task 8 regression A: an enabled volume on a renderer that cannot record
    // RT/ray-query scatter must bind the initialized neutral volume, never an
    // undefined active bundle. This is intentionally separate from the
    // resize sequence below, which needs a traceable TLAS.
    matter::FogSettings no_rt_fog{};
    matter::VulkanVolumetricsSettings no_rt_volumetrics{};
    no_rt_volumetrics.enabled = true;
    renderer.set_volumetrics_settings(no_rt_volumetrics, no_rt_fog);
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
        vulkan.wait_idle();
        viewer::VkRasterPixel enabled_fallback{};
        CHECK(renderer.readback_raster_pixel(4, 4, enabled_fallback, error),
              error.empty() ? "read enabled no-RT fallback" : error.c_str());
        no_rt_volumetrics.enabled = false;
        renderer.set_volumetrics_settings(no_rt_volumetrics, no_rt_fog);
        matter::VulkanFrame neutral{};
        CHECK(vulkan.begin_frame(neutral, error) &&
                  renderer.prepare_frame(neutral, frame, camera.position, 1.0f,
                                         error) &&
                  renderer.record_cull_and_render(
                      neutral, frame, camera.position, 1.0f, error) &&
                  renderer.record_composite_to_swapchain(neutral, error) &&
                  vulkan.end_frame(neutral, error),
              error.empty() ? "submit disabled volumetric neutral frame"
                            : error.c_str());
        vulkan.wait_idle();
        viewer::VkRasterPixel disabled_neutral{};
        CHECK(renderer.readback_raster_pixel(4, 4, disabled_neutral, error) &&
                  close4(enabled_fallback.hdr, disabled_neutral.hdr, 2e-3f),
              error.empty() ? "enabled no-RT fallback matches disabled neutral composite"
                            : error.c_str());
        CHECK(vulkan.validation_error_count() == 0,
              "enabled no-RT volume composites through initialized neutral resources");
    }
}

// Phase 1 tileset Vulkan port: end-to-end load_tileset_slot exercise against a
// synthetic .gtex (no world content required). Asserts the fail-closed
// negative paths, that a loaded slot's Wang-sampled albedo/ORM replace the
// material's flat values in the G-buffer (gbuffer.frag Task 7 branch, keyed by
// MaterialGpu.flags_misc[1] low byte = detailSlot + 1), the slot-replacement
// path, and that unload falls back to the dummy descriptors without
// validation complaints.
// WP-E (chart-space virtual texturing): MATTER_VK_SMOKE_MODE=vt.
//
// A synthetic two-chart part is registered with chart tables + chart UVs, so
// the residency runtime starts, pins a tail page, stub-fills it and (via the
// feedback loop) its finer pages. Then:
//   (a) the rendered albedo is what the installed filler bakes into the page.
//       WP-D's tier-1 compositor takes its material inputs from the renderer's
//       set_materials() push (the MaterialGpuRecord table), NOT from
//       VtPartContext::material_table — which only the WP-E stub filler reads.
//       The fixture therefore loads the two tables with DIFFERENT albedos, so
//       the pixel says which filler is installed as well as proving the VT
//       sample happened at all (a page that never filled reads as the
//       compositor's neutral 0.5 grey);
//   (b) pixels either side of the chart boundary agree, i.e. the page/border
//       addressing does not produce a seam where two charts meet;
//   (c) a chartless part rendered in the same scene is bit-identical to the
//       same part rendered before VT ever started (the regression gate).
void run_vt_path(matter::VulkanDevice& vulkan) {
    constexpr uint32_t width = 160;
    constexpr uint32_t height = 160;
    // Keep the physical pool to a single array layer; the production default
    // (8192 pages, ~0.9 GB) is pointless for a 2-chart fixture.
#ifdef _WIN32
    _putenv_s("MATTER_VT_POOL_PAGES", "256");
#else
    setenv("MATTER_VT_POOL_PAGES", "256", 1);
#endif
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error),
          error.empty() ? "vt: renderer init" : error.c_str());
    CHECK(!renderer.vt_active(),
          "vt: the residency runtime does not start before a chart-bearing part");

    // The chart part and the chartless control share this material index.
    //   `page_albedo`  — the uploaded MaterialGpuRecord. Both the legacy
    //                    G-buffer path AND the tier-1 compositor (via
    //                    set_materials) resolve this material to it, so a
    //                    detail-less material makes VT and legacy agree.
    //                    That agreement IS the Phase-2 parity property.
    //   `stub_albedo`  — VtPartContext::material_table, read only by the WP-E
    //                    stub filler. A page carrying this colour means the
    //                    compositor is not installed.
    constexpr uint32_t kMaterial = 5u;
    const matter::Float3 stub_albedo{0.20f, 0.60f, 0.85f};
    const matter::Float3 page_albedo{0.90f, 0.05f, 0.10f};
    constexpr float kMaterialRoughness = 0.40f;
    constexpr float kMaterialMetallic = 0.0f;
    // MaterialRegistryPackForGPU's layout: [albedo.xyz, roughness],
    // [metallic, emission, pad, translucency], [ior, flat, merge, slot].
    constexpr uint32_t kMaterialStride = 12u;
    std::vector<float> chart_material_table((kMaterial + 1) * kMaterialStride,
                                            0.0f);
    chart_material_table[kMaterial * kMaterialStride + 0] = stub_albedo.x;
    chart_material_table[kMaterial * kMaterialStride + 1] = stub_albedo.y;
    chart_material_table[kMaterial * kMaterialStride + 2] = stub_albedo.z;
    chart_material_table[kMaterial * kMaterialStride + 3] = 0.55f;  // roughness
    chart_material_table[kMaterial * kMaterialStride + 4] = 0.10f;  // metallic
    chart_material_table[kMaterial * kMaterialStride + 11] = -1.0f; // no slot

    std::vector<MaterialGpuRecord> materials(kMaterial + 1);
    materials[kMaterial].base_roughness[0] = page_albedo.x;
    materials[kMaterial].base_roughness[1] = page_albedo.y;
    materials[kMaterial].base_roughness[2] = page_albedo.z;
    materials[kMaterial].base_roughness[3] = kMaterialRoughness;
    materials[kMaterial].metal_opacity_spec_coat[0] = kMaterialMetallic;
    materials[kMaterial].metal_opacity_spec_coat[1] = 1.0f;   // opacity
    // No detail slot: flags_misc[1] stays 0, so the compositor takes the
    // scalar-fallback branch and bakes exactly this albedo/ORM into the page.
    CHECK(renderer.update_materials(materials, 1, 1, error),
          error.empty() ? "vt: stage materials" : error.c_str());

    // --- chartless control part (a quad off to the left, nearer the camera) --
    const auto quad = [](uint64_t hash, float x0, float x1, float y0, float y1,
                         float z, float u0, float u1, float v0, float v1,
                         uint32_t material) {
        viewer::VkScenePart part = fixed_part(hash, {x0, y0, z}, {x1, y1, z}, 0);
        const matter::Float3 normal{0.0f, 0.0f, 1.0f};
        const matter::Float4 tint{1.0f, 1.0f, 1.0f, 0.0f};   // a = 0: no tint
        part.vertices = {
            {{x0, y0, z}, normal, tint, {u0, v0, 1.0f, 1.0f}, material, {}},
            {{x1, y0, z}, normal, tint, {u1, v0, 1.0f, 1.0f}, material, {}},
            {{x1, y1, z}, normal, tint, {u1, v1, 1.0f, 1.0f}, material, {}},
            {{x0, y1, z}, normal, tint, {u0, v1, 1.0f, 1.0f}, material, {}},
        };
        part.indices = {0, 1, 2, 0, 2, 3};
        part.clusters[0].lods[0] = {0, 6, 0.0f, UINT32_MAX};
        return part;
    };

    const viewer::VkScenePart control =
        quad(0x7601, -1.4f, -1.0f, -0.4f, 0.4f, -1.9f, 0.0f, 0.0f, 0.0f, 0.0f,
             kMaterial);
    CHECK(renderer.ensure_part(control, error) >= 0,
          error.empty() ? "vt: ensure chartless control part" : error.c_str());

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices frame{};
    CHECK(viewer::build_frame_matrices(camera, width, height, frame, error),
          error.empty() ? "vt: build frame matrices" : error.c_str());

    const matter::Mat4f identity = identity_matrix();
    // render_gbuffer_and_composite submits and waits, so the wall-clock delta
    // across it is an honest end-to-end cost for the frame INCLUDING the VT
    // fill pass. Frames that fill pages are compared against frames that do
    // not, which brackets the integrated per-fill cost.
    double last_render_ms = 0.0;
    const auto render_once = [&](const char* label) {
        std::string local;
        CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, local),
              local.empty() ? label : local.c_str());
        const auto started = std::chrono::steady_clock::now();
        CHECK(renderer.render_gbuffer_and_composite(width, height, local),
              local.empty() ? label : local.c_str());
        last_render_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
    };
    const auto pixel_at = [&](uint32_t x, uint32_t y) {
        viewer::VkRasterPixel pixel{};
        std::string local;
        CHECK(renderer.readback_raster_pixel(x, y, pixel, local),
              local.empty() ? "vt: readback pixel" : local.c_str());
        return pixel;
    };

    CHECK(renderer.update_instances({{0x7601, identity, 1}}, error),
          error.empty() ? "vt: upload control instance" : error.c_str());
    render_once("vt: render chartless-only frame");
    // Control quad centre: z = -1.9 gives a 1.9 half-extent, so world x -1.2
    // lands near screen x 29.
    const viewer::VkRasterPixel control_before = pixel_at(29, 80);
    CHECK(control_before.material_index == kMaterial,
          "vt: the control quad is the surface under the probe");
    CHECK(close4(control_before.albedo,
                 {page_albedo.x, page_albedo.y, page_albedo.z, 1.0f},
                 6e-3f),
          "vt: the chartless control shades from the uploaded material record");
    CHECK(!renderer.vt_active(),
          "vt: a chartless-only scene never starts the residency runtime");

    // --- the two-chart part -------------------------------------------------
    // Atlas 256x128: chart 0 owns page column 0, chart 1 owns page column 1,
    // so the chart boundary is also a PAGE boundary at the finest mip — the
    // configuration a border/addressing bug shows up in.
    //
    // The chart entries are GEOMETRICALLY CONSISTENT with the UVs (chart_atlas.h's
    // convention: texel = rect + gutter + (dot(p,T) - dot(origin,T)) * tpm).
    // The tier-1 compositor analytically rasterizes charts from exactly these
    // fields, so an entry that disagrees with the vertex UVs bakes a page that
    // does not correspond to the surface. Both quads are 0.8 m square at
    // 150 texels/m => 120 texels of content inside a 128-texel rect, leaving
    // the 4-texel gutter on every edge.
    viewer::VkScenePart charted =
        quad(0x7602, -0.8f, 0.8f, -0.4f, 0.4f, -2.0f, 0.0f, 0.0f, 0.0f, 0.0f,
             kMaterial);
    {
        const float z = -2.0f;
        const float tpm = 150.0f;
        const matter::Float3 normal{0.0f, 0.0f, 1.0f};
        const matter::Float4 tint{1.0f, 1.0f, 1.0f, 0.0f};
        const float u0a = 4.0f / 256.0f, u1a = 124.0f / 256.0f;
        const float u0b = 132.0f / 256.0f, u1b = 252.0f / 256.0f;
        const float v0 = 4.0f / 128.0f, v1 = 124.0f / 128.0f;
        charted.vertices = {
            {{-0.8f, -0.4f, z}, normal, tint, {u0a, v0, 1.0f, 1.0f}, kMaterial, {}},
            {{ 0.0f, -0.4f, z}, normal, tint, {u1a, v0, 1.0f, 1.0f}, kMaterial, {}},
            {{ 0.0f,  0.4f, z}, normal, tint, {u1a, v1, 1.0f, 1.0f}, kMaterial, {}},
            {{-0.8f,  0.4f, z}, normal, tint, {u0a, v1, 1.0f, 1.0f}, kMaterial, {}},
            {{ 0.0f, -0.4f, z}, normal, tint, {u0b, v0, 1.0f, 1.0f}, kMaterial, {}},
            {{ 0.8f, -0.4f, z}, normal, tint, {u1b, v0, 1.0f, 1.0f}, kMaterial, {}},
            {{ 0.8f,  0.4f, z}, normal, tint, {u1b, v1, 1.0f, 1.0f}, kMaterial, {}},
            {{ 0.0f,  0.4f, z}, normal, tint, {u0b, v1, 1.0f, 1.0f}, kMaterial, {}},
        };
        charted.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
        charted.clusters[0].lods[0] = {0, 12, 0.0f, /*chart_rung=*/0u};

        chart_atlas::ChartAtlasRung rung;
        rung.atlas_w = 256;
        rung.atlas_h = 128;
        rung.charts.resize(2);
        const float chart_origin_x[2] = {-0.8f, 0.0f};
        for (int c = 0; c < 2; ++c) {
            chart_atlas::ChartEntry& entry = rung.charts[c];
            entry.origin[0] = chart_origin_x[c];
            entry.origin[1] = -0.4f;
            entry.origin[2] = z;
            entry.tangent[0] = 1.0f; entry.tangent[1] = 0.0f; entry.tangent[2] = 0.0f;
            entry.bitangent[0] = 0.0f; entry.bitangent[1] = 1.0f; entry.bitangent[2] = 0.0f;
            entry.rect_x = static_cast<uint32_t>(c) * 128u;
            entry.rect_y = 0;
            entry.rect_w = 128;
            entry.rect_h = 128;
            entry.texels_per_meter = tpm;
            entry.first_tri = static_cast<uint32_t>(c) * 2u;
            entry.tri_count = 2;
        }
        rung.tri_order = {0, 1, 2, 3};
        charted.lod_charts = {rung};

        viewer::VkScenePartChartMesh mesh;
        mesh.vertex_count = 8;
        for (const viewer::VkRasterVertex& vertex : charted.vertices) {
            mesh.positions.push_back(vertex.position.x);
            mesh.positions.push_back(vertex.position.y);
            mesh.positions.push_back(vertex.position.z);
            mesh.normals.push_back(vertex.normal.x);
            mesh.normals.push_back(vertex.normal.y);
            mesh.normals.push_back(vertex.normal.z);
            mesh.surface_uvs.push_back(vertex.surface.x);
            mesh.surface_uvs.push_back(vertex.surface.y);
            mesh.material_ids.push_back(kMaterial);
        }
        mesh.indices = charted.indices;
        mesh.dominant_material = kMaterial;
        charted.lod_chart_meshes = {std::move(mesh)};
        charted.chart_material_table = chart_material_table;
        charted.chart_material_stride = kMaterialStride;
    }
    CHECK(renderer.ensure_part(charted, error) >= 0,
          error.empty() ? "vt: ensure two-chart part" : error.c_str());
    CHECK(renderer.vt_active(),
          "vt: registering a chart-bearing part starts the residency runtime");
    {
        const vt::VtResidency::Stats stats = renderer.vt_stats();
        CHECK(stats.variants == 1, "vt: exactly one (variant, rung) registered");
        CHECK(stats.pool_pinned == 1, "vt: the variant's tail page is pinned");
        CHECK(stats.pool_capacity == 256,
              "vt: MATTER_VT_POOL_PAGES sized the pool");
    }

    CHECK(renderer.update_instances({{0x7601, identity, 1},
                                     {0x7602, identity, 2}}, error),
          error.empty() ? "vt: upload both instances" : error.c_str());
    // Frame 1 fills the pinned tail but draws LEGACY (the tail gate holds
    // vt_slot at 0 until the tail's fill frame is submitted — the streaming
    // black-flash fix); VT draws start on frame 2, their feedback comes back
    // three frames later (the readback ring), and the finest pages under the
    // probes fill the frame after that. Seven frames is well past all of it.
    double filling_frame_ms = 0.0;
    double settled_frame_ms = 0.0;
    uint64_t fills_before_frame = renderer.vt_stats().fills_total;
    for (int i = 0; i < 7; ++i) {
        render_once("vt: render VT frame");
        const uint64_t fills_now = renderer.vt_stats().fills_total;
        if (fills_now != fills_before_frame)
            filling_frame_ms = last_render_ms;
        else
            settled_frame_ms = last_render_ms;
        fills_before_frame = fills_now;
    }
    std::printf("vt frame cost: filling=%.3f ms settled=%.3f ms "
                "(submit+wait, 160x160)\n",
                filling_frame_ms, settled_frame_ms);

    const vt::VtResidency::Stats stats = renderer.vt_stats();
    std::printf(
        "vt stats: variants=%u pool=%u/%u pinned=%u fills_total=%llu "
        "evictions=%llu queue=%u requests_last=%u mesh_bytes=%llu "
        "pool_bytes=%llu\n",
        stats.variants, stats.pool_used, stats.pool_capacity, stats.pool_pinned,
        static_cast<unsigned long long>(stats.fills_total),
        static_cast<unsigned long long>(stats.evictions_total),
        stats.queue_depth, stats.requests_last_frame,
        static_cast<unsigned long long>(stats.mesh_bytes),
        static_cast<unsigned long long>(stats.pool_bytes));
    CHECK(stats.fills_total >= 1,
          "vt: at least the pinned tail page was filled");
    CHECK(stats.pool_used >= 1, "vt: the pool holds the filled pages");
    CHECK(stats.evictions_total == 0,
          "vt: a 2-chart fixture never pressures a 256-page pool");
    CHECK(stats.requests_last_frame > 0,
          "vt: the G-buffer feedback pass produced page requests");
    CHECK(stats.fills_total >= 2,
          "vt: the feedback loop drained a page beyond the pinned tail");
    // Every occupied slot must have been written by a fill. A pinned tail
    // whose fill allocated a SECOND slot instead of writing the pinned one
    // shows up here as pool_used > fills_total, and leaves the slot every
    // unmapped entry falls back to holding undefined bytes.
    CHECK(stats.pool_used <= stats.fills_total,
          "vt: every resident slot was actually filled (tail included)");

    // (a) the VT sample is what shades the charted quad, and it carries the
    // COMPOSITOR's material resolution (set_materials), not the stub's.
    // World x -0.4 / +0.4 at z = -2 (half-extent 2.0) land on screen x 64 / 96.
    const viewer::VkRasterPixel chart0 = pixel_at(64, 80);
    const viewer::VkRasterPixel chart1 = pixel_at(96, 80);
    CHECK(chart0.material_index == kMaterial && chart1.material_index == kMaterial,
          "vt: both probes land on the charted quad");
    // BC7 of a (locally) constant page plus unorm8 quantization: ~1/255.
    CHECK(close4(chart0.albedo,
                 {page_albedo.x, page_albedo.y, page_albedo.z, 1.0f}, 2.0e-2f),
          "vt: chart 0 shades from the composited page");
    CHECK(close4(chart1.albedo,
                 {page_albedo.x, page_albedo.y, page_albedo.z, 1.0f}, 2.0e-2f),
          "vt: chart 1 shades from the composited page");
    // Decisive: the stub filler resolves this material out of
    // VtPartContext::material_table, which holds a very different colour.
    CHECK(std::fabs(chart0.albedo.x - stub_albedo.x) > 0.2f ||
              std::fabs(chart0.albedo.y - stub_albedo.y) > 0.2f ||
              std::fabs(chart0.albedo.z - stub_albedo.z) > 0.2f,
          "vt: the tier-1 compositor is the installed filler, not the stub");
    // A page that never filled reads as the compositor's neutral clear.
    CHECK(std::fabs(chart0.albedo.x - 0.5f) > 0.1f ||
              std::fabs(chart0.albedo.y - 0.5f) > 0.1f ||
              std::fabs(chart0.albedo.z - 0.5f) > 0.1f,
          "vt: the sampled page is composited content, not the neutral clear");
    // The chart frame is T=(1,0,0), B=(0,1,0) => N = T x B = the quad's own
    // +Z normal, and a detail-less material adds no normal delta, so the
    // page's chart-tangent normal decodes back to the geometric normal.
    CHECK(std::fabs(chart0.normal.z - 1.0f) < 2.0e-2f,
          "vt: the page normal decodes to the geometric normal");
    // The G-buffer ORM attachment is (roughness, metallic, ao). Roughness and
    // metallic ride the compositor's scalar fallback; occlusion is 1 at tier 1
    // (WP-H's hemisphere enrichment is what darkens it), so ao is the vertex
    // term alone.
    CHECK(std::fabs(chart0.orm.x - kMaterialRoughness) < 2.0e-2f,
          "vt: page roughness matches the material the compositor was given");
    CHECK(std::fabs(chart0.orm.y - kMaterialMetallic) < 2.0e-2f,
          "vt: page metallic matches the material the compositor was given");
    CHECK(std::fabs(chart0.orm.z - 1.0f) < 2.0e-2f,
          "vt: tier-1 pages carry unoccluded ORM");
    // Phase-2 parity: a detail-less material must shade identically whether it
    // came through a composited page or the legacy path. The chartless control
    // is the same material shaded the legacy way.
    CHECK(std::fabs(chart0.albedo.x - control_before.albedo.x) < 2.0e-2f &&
              std::fabs(chart0.albedo.y - control_before.albedo.y) < 2.0e-2f &&
              std::fabs(chart0.albedo.z - control_before.albedo.z) < 2.0e-2f,
          "vt: a detail-less material composites to its legacy shading");

    // (b) no seam where the two charts (and the two finest-mip pages) meet.
    const viewer::VkRasterPixel left = pixel_at(76, 80);
    const viewer::VkRasterPixel right = pixel_at(84, 80);
    CHECK(left.material_index == kMaterial && right.material_index == kMaterial,
          "vt: the seam probes land on the charted quad");
    CHECK(std::fabs(left.albedo.x - right.albedo.x) < 6.0e-3f &&
              std::fabs(left.albedo.y - right.albedo.y) < 6.0e-3f &&
              std::fabs(left.albedo.z - right.albedo.z) < 6.0e-3f,
          "vt: no seam across the chart/page boundary beyond filtering epsilon");
    CHECK(std::fabs(left.albedo.x - chart0.albedo.x) < 6.0e-3f &&
              std::fabs(right.albedo.x - chart1.albedo.x) < 6.0e-3f,
          "vt: the boundary neighbourhood matches each chart's interior");

    // Determinism: the compositor is a pure function of its inputs, and the
    // residency layer must not be re-filling a resident page with something
    // else. Another frame over the same state has to reproduce the same
    // pixels EXACTLY -- not within an epsilon.
    render_once("vt: render VT determinism frame");
    const viewer::VkRasterPixel chart0_again = pixel_at(64, 80);
    const viewer::VkRasterPixel chart1_again = pixel_at(96, 80);
    CHECK(chart0_again.albedo.x == chart0.albedo.x &&
              chart0_again.albedo.y == chart0.albedo.y &&
              chart0_again.albedo.z == chart0.albedo.z &&
              chart0_again.orm.x == chart0.orm.x &&
              chart0_again.orm.y == chart0.orm.y &&
              chart0_again.orm.z == chart0.orm.z,
          "vt: chart 0 is bit-identical across consecutive frames");
    CHECK(chart1_again.albedo.x == chart1.albedo.x &&
              chart1_again.albedo.y == chart1.albedo.y &&
              chart1_again.albedo.z == chart1.albedo.z,
          "vt: chart 1 is bit-identical across consecutive frames");

    // (c) the chartless control is bit-identical to the pre-VT frame.
    const viewer::VkRasterPixel control_after = pixel_at(29, 80);
    CHECK(control_after.material_index == kMaterial,
          "vt: the control quad still shades under the probe");
    CHECK(control_after.albedo.x == control_before.albedo.x &&
              control_after.albedo.y == control_before.albedo.y &&
              control_after.albedo.z == control_before.albedo.z &&
              control_after.albedo.w == control_before.albedo.w,
          "vt: a chartless part renders byte-identically once VT is live");
    CHECK(control_after.orm.x == control_before.orm.x &&
              control_after.orm.y == control_before.orm.y &&
              control_after.orm.z == control_before.orm.z,
          "vt: a chartless part's ORM is byte-identical once VT is live");
    CHECK(control_after.normal.x == control_before.normal.x &&
              control_after.normal.y == control_before.normal.y &&
              control_after.normal.z == control_before.normal.z,
          "vt: a chartless part's normal is byte-identical once VT is live");

    // (d) compositor-input invalidation. Editing the material table changes
    // what the compositor bakes into a page. Rebinding the compositor's inputs
    // only fixes FUTURE fills: the pages already in the pool -- and above all
    // the pinned tail, which never expires and is what every unmapped entry
    // resolves to -- were baked from the OLD table. The renderer's next
    // push_vt_compositor_inputs() therefore also calls
    // VtResidency::invalidate_all_content(), which drops the resident unpinned
    // pages and re-queues every tail for an in-place re-fill. Without it these
    // probes keep reading page_albedo for the rest of the session.
    const matter::Float3 edited_albedo{0.10f, 0.85f, 0.35f};
    const vt::VtResidency::Stats before_invalidate = renderer.vt_stats();
    materials[kMaterial].base_roughness[0] = edited_albedo.x;
    materials[kMaterial].base_roughness[1] = edited_albedo.y;
    materials[kMaterial].base_roughness[2] = edited_albedo.z;
    CHECK(renderer.update_materials(materials, 2, 1, error),
          error.empty() ? "vt: edit the material table" : error.c_str());
    // The next frame pushes the new inputs, invalidates, and re-fills the tail;
    // the feedback loop takes a few more to bring the finest pages under the
    // probes back. Six is well past that for a 2-chart fixture.
    for (int i = 0; i < 6; ++i) render_once("vt: render post-edit VT frame");

    const vt::VtResidency::Stats after_invalidate = renderer.vt_stats();
    std::printf("vt post-edit stats: invalidations=%llu dropped=%llu "
                "pool=%u/%u pinned=%u fills_total=%llu\n",
                static_cast<unsigned long long>(
                    after_invalidate.invalidations_total),
                static_cast<unsigned long long>(
                    after_invalidate.pages_dropped_total),
                after_invalidate.pool_used, after_invalidate.pool_capacity,
                after_invalidate.pool_pinned,
                static_cast<unsigned long long>(after_invalidate.fills_total));
    CHECK(before_invalidate.invalidations_total == 0,
          "vt: the runtime's own first input push does not invalidate");
    CHECK(after_invalidate.invalidations_total == 1,
          "vt: a material-table edit invalidates resident VT content exactly once");
    CHECK(after_invalidate.pages_dropped_total >= 1,
          "vt: the invalidation dropped the pages baked from the old table");
    CHECK(after_invalidate.pool_pinned == before_invalidate.pool_pinned,
          "vt: invalidation never drops a pinned tail");
    CHECK(after_invalidate.fills_total > before_invalidate.fills_total,
          "vt: the invalidated pages were re-filled");
    // As above: a tail re-fill that allocated a fresh slot instead of rewriting
    // its pinned one would leave pool_used ahead of the fills that wrote it.
    CHECK(after_invalidate.pool_used <= after_invalidate.fills_total,
          "vt: the in-place tail re-fill did not burn a second slot");
    const viewer::VkRasterPixel chart0_edited = pixel_at(64, 80);
    const viewer::VkRasterPixel chart1_edited = pixel_at(96, 80);
    CHECK(chart0_edited.material_index == kMaterial &&
              chart1_edited.material_index == kMaterial,
          "vt: both probes still land on the charted quad after the edit");
    CHECK(close4(chart0_edited.albedo,
                 {edited_albedo.x, edited_albedo.y, edited_albedo.z, 1.0f},
                 2.0e-2f),
          "vt: chart 0 re-composited from the edited material table");
    CHECK(close4(chart1_edited.albedo,
                 {edited_albedo.x, edited_albedo.y, edited_albedo.z, 1.0f},
                 2.0e-2f),
          "vt: chart 1 re-composited from the edited material table");
    // Re-filled pages are as deterministic as first-filled ones.
    render_once("vt: render post-edit determinism frame");
    const viewer::VkRasterPixel chart0_edited_again = pixel_at(64, 80);
    CHECK(chart0_edited_again.albedo.x == chart0_edited.albedo.x &&
              chart0_edited_again.albedo.y == chart0_edited.albedo.y &&
              chart0_edited_again.albedo.z == chart0_edited.albedo.z &&
              chart0_edited_again.orm.x == chart0_edited.orm.x &&
              chart0_edited_again.orm.y == chart0_edited.orm.y,
          "vt: re-filled pages are bit-identical across consecutive frames");
    CHECK(renderer.vt_stats().invalidations_total == 1,
          "vt: a settled frame does not re-invalidate");

    // Releasing the charted part must return its layer, tail and mesh copies.
    renderer.release_part(0x7602);
    const vt::VtResidency::Stats after_release = renderer.vt_stats();
    CHECK(after_release.variants == 0,
          "vt: releasing the part unregisters its variant rung");
    CHECK(after_release.pool_pinned == 0,
          "vt: releasing the part unpins its tail page");
    CHECK(after_release.mesh_bytes == 0,
          "vt: releasing the part frees its CPU mesh copies");
}

// WP-F (surfaces() classifier tape): MATTER_VK_SMOKE_MODE=vt-surfaces.
//
// A three-chart strip whose VtPartContext carries per-vertex tape weights
// (the form the engine's compiled surfaces() tape produces): quad 0 is pure
// material A, quad 1 pure B, quad 2 pure C — all three sharing ONE TriEx
// materialId, so any color difference between the quads can only come from
// the tape (weight-seam mode 2), never from the Phase-2 materialId stub.
// Asserts:
//   (a) different tape regions produce different page content (each quad
//       shades with its tape material's albedo, not the TriEx material's);
//   (b) determinism across frames (bit-identical resampling);
//   (c) an edited tape (renderer begin/update/end vt-surface bracket with a
//       new tape hash) invalidates resident content exactly once and the
//       re-filled pages show the NEW classification (regions swapped).
void run_vt_surfaces_path(matter::VulkanDevice& vulkan) {
    constexpr uint32_t width = 160;
    constexpr uint32_t height = 160;
#ifdef _WIN32
    _putenv_s("MATTER_VT_POOL_PAGES", "256");
#else
    setenv("MATTER_VT_POOL_PAGES", "256", 1);
#endif
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error),
          error.empty() ? "vt-surfaces: renderer init" : error.c_str());

    // Tape materials: three detail-less materials with far-apart albedos.
    // kTriMaterial is the TriEx id every vertex carries — its albedo is a
    // fourth colour that must NEVER appear on the strip while the tape is
    // active.
    constexpr uint32_t kMatGrass = 3u;
    constexpr uint32_t kMatRock = 4u;
    constexpr uint32_t kMatSnow = 5u;
    constexpr uint32_t kTriMaterial = 6u;
    const matter::Float3 grass_albedo{0.10f, 0.70f, 0.15f};
    const matter::Float3 rock_albedo{0.45f, 0.30f, 0.20f};
    const matter::Float3 snow_albedo{0.90f, 0.92f, 0.95f};
    const matter::Float3 tri_albedo{0.85f, 0.05f, 0.80f};
    std::vector<MaterialGpuRecord> materials(kTriMaterial + 1);
    const auto set_material = [&](uint32_t index, const matter::Float3& albedo) {
        materials[index].base_roughness[0] = albedo.x;
        materials[index].base_roughness[1] = albedo.y;
        materials[index].base_roughness[2] = albedo.z;
        materials[index].base_roughness[3] = 0.5f;   // roughness
        materials[index].metal_opacity_spec_coat[0] = 0.0f;
        materials[index].metal_opacity_spec_coat[1] = 1.0f;   // opacity
    };
    set_material(kMatGrass, grass_albedo);
    set_material(kMatRock, rock_albedo);
    set_material(kMatSnow, snow_albedo);
    set_material(kTriMaterial, tri_albedo);
    CHECK(renderer.update_materials(materials, 1, 1, error),
          error.empty() ? "vt-surfaces: stage materials" : error.c_str());

    // ---- the three-chart strip -------------------------------------------
    // Three 0.8 m quads side by side at z = -2 (camera fov 90 deg => the
    // strip spans screen x 32..128), one chart per quad, atlas 384x128.
    const float z = -2.0f;
    const float quad_w = 0.8f;
    const float tpm = 150.0f;
    viewer::VkScenePart part = fixed_part(0x7710, {-1.2f, -0.4f, z},
                                          {1.2f, 0.4f, z}, 0);
    part.vertices.clear();
    part.indices.clear();
    const matter::Float3 normal{0.0f, 0.0f, 1.0f};
    const matter::Float4 tint{1.0f, 1.0f, 1.0f, 0.0f};
    chart_atlas::ChartAtlasRung rung;
    rung.atlas_w = 384;
    rung.atlas_h = 128;
    for (int q = 0; q < 3; ++q) {
        const float x0 = -1.2f + quad_w * float(q);
        const float x1 = x0 + quad_w;
        const float u0 = (float(q) * 128.0f + 4.0f) / 384.0f;
        const float u1 = (float(q) * 128.0f + 124.0f) / 384.0f;
        const float v0 = 4.0f / 128.0f, v1 = 124.0f / 128.0f;
        const uint32_t base = static_cast<uint32_t>(part.vertices.size());
        part.vertices.push_back(
            {{x0, -0.4f, z}, normal, tint, {u0, v0, 1.0f, 1.0f}, kTriMaterial, {}});
        part.vertices.push_back(
            {{x1, -0.4f, z}, normal, tint, {u1, v0, 1.0f, 1.0f}, kTriMaterial, {}});
        part.vertices.push_back(
            {{x1, 0.4f, z}, normal, tint, {u1, v1, 1.0f, 1.0f}, kTriMaterial, {}});
        part.vertices.push_back(
            {{x0, 0.4f, z}, normal, tint, {u0, v1, 1.0f, 1.0f}, kTriMaterial, {}});
        const uint32_t idx[6] = {base, base + 1, base + 2,
                                 base, base + 2, base + 3};
        part.indices.insert(part.indices.end(), idx, idx + 6);

        chart_atlas::ChartEntry entry{};
        entry.origin[0] = x0;
        entry.origin[1] = -0.4f;
        entry.origin[2] = z;
        entry.tangent[0] = 1.0f;
        entry.bitangent[1] = 1.0f;
        entry.rect_x = static_cast<uint32_t>(q) * 128u;
        entry.rect_y = 0;
        entry.rect_w = 128;
        entry.rect_h = 128;
        entry.texels_per_meter = tpm;
        entry.first_tri = static_cast<uint32_t>(q) * 2u;
        entry.tri_count = 2;
        rung.charts.push_back(entry);
        rung.tri_order.push_back(static_cast<uint32_t>(q) * 2u);
        rung.tri_order.push_back(static_cast<uint32_t>(q) * 2u + 1u);
    }
    part.clusters[0].lods[0] = {0, 18, 0.0f, /*chart_rung=*/0u};
    part.lod_charts = {rung};

    viewer::VkScenePartChartMesh mesh;
    mesh.vertex_count = static_cast<uint32_t>(part.vertices.size());
    for (const viewer::VkRasterVertex& vertex : part.vertices) {
        mesh.positions.push_back(vertex.position.x);
        mesh.positions.push_back(vertex.position.y);
        mesh.positions.push_back(vertex.position.z);
        mesh.normals.push_back(vertex.normal.x);
        mesh.normals.push_back(vertex.normal.y);
        mesh.normals.push_back(vertex.normal.z);
        mesh.surface_uvs.push_back(vertex.surface.x);
        mesh.surface_uvs.push_back(vertex.surface.y);
        mesh.material_ids.push_back(kTriMaterial);
    }
    mesh.indices = part.indices;
    mesh.dominant_material = kTriMaterial;
    // The tape: quad q's four vertices give column q weight 255 — exactly
    // what terrain_field::SurfaceRuntime::classify_vertices produces for a
    // saturated classifier.
    const auto weights_for = [&](uint32_t col0, uint32_t col1, uint32_t col2) {
        std::vector<uint8_t> weights(size_t(mesh.vertex_count) * 3u, 0);
        const uint32_t cols[3] = {col0, col1, col2};
        for (uint32_t v = 0; v < mesh.vertex_count; ++v)
            weights[size_t(v) * 3u + cols[v / 4u]] = 255;
        return weights;
    };
    mesh.surface_weights = weights_for(0, 1, 2);   // grass | rock | snow
    part.lod_chart_meshes = {std::move(mesh)};
    part.surface_materials = {kMatGrass, kMatRock, kMatSnow};
    part.surface_tape_hash = 0x5EAF00D100000001ull;

    CHECK(renderer.ensure_part(part, error) >= 0,
          error.empty() ? "vt-surfaces: ensure charted strip" : error.c_str());
    CHECK(renderer.vt_active(),
          "vt-surfaces: chart-bearing part starts the residency runtime");

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices frame{};
    CHECK(viewer::build_frame_matrices(camera, width, height, frame, error),
          error.empty() ? "vt-surfaces: build frame matrices" : error.c_str());
    const matter::Mat4f identity = identity_matrix();
    CHECK(renderer.update_instances({{0x7710, identity, 1}}, error),
          error.empty() ? "vt-surfaces: upload instance" : error.c_str());

    const auto render_once = [&](const char* label) {
        std::string local;
        CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, local),
              local.empty() ? label : local.c_str());
        CHECK(renderer.render_gbuffer_and_composite(width, height, local),
              local.empty() ? label : local.c_str());
    };
    const auto pixel_at = [&](uint32_t x, uint32_t y) {
        viewer::VkRasterPixel pixel{};
        std::string local;
        CHECK(renderer.readback_raster_pixel(x, y, pixel, local),
              local.empty() ? "vt-surfaces: readback pixel" : local.c_str());
        return pixel;
    };
    const auto close_albedo = [](const viewer::VkRasterPixel& pixel,
                                 const matter::Float3& want, float epsilon) {
        return std::fabs(pixel.albedo.x - want.x) < epsilon &&
               std::fabs(pixel.albedo.y - want.y) < epsilon &&
               std::fabs(pixel.albedo.z - want.z) < epsilon;
    };

    // Quad centres (world x -0.8 / 0 / +0.8 at z = -2) land on screen x
    // 48 / 80 / 112 with the 90-degree camera.
    for (int i = 0; i < 5; ++i) render_once("vt-surfaces: render VT frame");
    const viewer::VkRasterPixel p_grass = pixel_at(48, 80);
    const viewer::VkRasterPixel p_rock = pixel_at(80, 80);
    const viewer::VkRasterPixel p_snow = pixel_at(112, 80);
    CHECK(p_grass.material_index == kTriMaterial &&
              p_rock.material_index == kTriMaterial &&
              p_snow.material_index == kTriMaterial,
          "vt-surfaces: all probes land on the strip");
    // (a) three tape regions, three different page contents.
    CHECK(close_albedo(p_grass, grass_albedo, 2.0e-2f),
          "vt-surfaces: quad 0 shades from the tape's grass material");
    CHECK(close_albedo(p_rock, rock_albedo, 2.0e-2f),
          "vt-surfaces: quad 1 shades from the tape's rock material");
    CHECK(close_albedo(p_snow, snow_albedo, 2.0e-2f),
          "vt-surfaces: quad 2 shades from the tape's snow material");
    // Decisive against the Phase-2 stub: the shared TriEx material's albedo
    // appears nowhere.
    CHECK(!close_albedo(p_grass, tri_albedo, 0.2f) &&
              !close_albedo(p_rock, tri_albedo, 0.2f) &&
              !close_albedo(p_snow, tri_albedo, 0.2f),
          "vt-surfaces: the TriEx materialId stub is not what filled the pages");

    // (b) determinism: another frame over settled state is bit-identical.
    render_once("vt-surfaces: determinism frame");
    const viewer::VkRasterPixel p_grass2 = pixel_at(48, 80);
    const viewer::VkRasterPixel p_snow2 = pixel_at(112, 80);
    CHECK(p_grass2.albedo.x == p_grass.albedo.x &&
              p_grass2.albedo.y == p_grass.albedo.y &&
              p_grass2.albedo.z == p_grass.albedo.z &&
              p_snow2.albedo.x == p_snow.albedo.x &&
              p_snow2.albedo.y == p_snow.albedo.y &&
              p_snow2.albedo.z == p_snow.albedo.z,
          "vt-surfaces: tape-classified pages are bit-identical across frames");

    // (c) tape edit: swap grass and snow columns under a NEW tape hash. The
    // renderer bracket must invalidate resident content exactly once and the
    // re-fills must show the swapped classification.
    const vt::VtResidency::Stats before_edit = renderer.vt_stats();
    renderer.begin_vt_surface_update();
    std::vector<std::vector<uint8_t>> new_weights(1);
    new_weights[0] = weights_for(2, 1, 0);   // snow | rock | grass
    CHECK(renderer.update_vt_part_surface(0x7710, new_weights,
                                          {kMatGrass, kMatRock, kMatSnow},
                                          0x5EAF00D100000002ull),
          "vt-surfaces: the registered rung accepts the edited tape");
    renderer.end_vt_surface_update();
    for (int i = 0; i < 6; ++i) render_once("vt-surfaces: post-edit frame");
    const vt::VtResidency::Stats after_edit = renderer.vt_stats();
    std::printf("vt-surfaces stats: fills=%llu -> %llu, invalidations=%llu, "
                "dropped=%llu\n",
                static_cast<unsigned long long>(before_edit.fills_total),
                static_cast<unsigned long long>(after_edit.fills_total),
                static_cast<unsigned long long>(after_edit.invalidations_total),
                static_cast<unsigned long long>(after_edit.pages_dropped_total));
    CHECK(after_edit.invalidations_total ==
              before_edit.invalidations_total + 1,
          "vt-surfaces: the tape edit invalidates resident content exactly once");
    CHECK(after_edit.fills_total > before_edit.fills_total,
          "vt-surfaces: the invalidated pages re-filled");
    const viewer::VkRasterPixel e_left = pixel_at(48, 80);
    const viewer::VkRasterPixel e_mid = pixel_at(80, 80);
    const viewer::VkRasterPixel e_right = pixel_at(112, 80);
    CHECK(close_albedo(e_left, snow_albedo, 2.0e-2f),
          "vt-surfaces: after the edit quad 0 re-filled as snow");
    CHECK(close_albedo(e_mid, rock_albedo, 2.0e-2f),
          "vt-surfaces: after the edit quad 1 is still rock");
    CHECK(close_albedo(e_right, grass_albedo, 2.0e-2f),
          "vt-surfaces: after the edit quad 2 re-filled as grass");
    // Post-edit determinism.
    render_once("vt-surfaces: post-edit determinism frame");
    const viewer::VkRasterPixel e_left2 = pixel_at(48, 80);
    CHECK(e_left2.albedo.x == e_left.albedo.x &&
              e_left2.albedo.y == e_left.albedo.y &&
              e_left2.albedo.z == e_left.albedo.z,
          "vt-surfaces: re-filled pages are bit-identical across frames");

    // Stripping the tape reverts to the TriEx materialId stub — the fourth
    // colour finally appears, proving the mode flag travels per request.
    renderer.begin_vt_surface_update();
    CHECK(renderer.update_vt_part_surface(0x7710, {}, {}, 0),
          "vt-surfaces: stripping the tape updates the registered rung");
    renderer.end_vt_surface_update();
    for (int i = 0; i < 6; ++i) render_once("vt-surfaces: stripped frame");
    const viewer::VkRasterPixel stripped = pixel_at(80, 80);
    CHECK(close_albedo(stripped, tri_albedo, 2.0e-2f),
          "vt-surfaces: without the tape the TriEx materialId stub shades "
          "the strip");

    renderer.release_part(0x7710);
    CHECK(renderer.vt_stats().variants == 0,
          "vt-surfaces: release returns the variant");

    // ---- P2 (texel-rate tape, weight-seam mode 3) --------------------------
    // A part that ALSO carries the canonical tape text: the compositor packs
    // the program and evaluates it per texel on the GPU. The fixture is a
    // step tape splitting one 0.8 m quad at lx = 0 with a 5 mm edge (<1 page
    // texel at 150 tpm). Per-texel evaluation keeps both sides PURE right up
    // to the boundary; the per-vertex mode-2 ramp on this 2-triangle quad
    // would height-blend visibly at the probe columns (~±0.04 m), so pure
    // probes prove the GPU interpreter actually filled the pages. The whole
    // mode still exits through the zero-validation-errors gate.
    {
        viewer::VkScenePart part3 =
            fixed_part(0x7711, {-0.4f, -0.4f, z}, {0.4f, 0.4f, z}, 0);
        part3.vertices.clear();
        part3.indices.clear();
        chart_atlas::ChartAtlasRung rung3;
        rung3.atlas_w = 128;
        rung3.atlas_h = 128;
        {
            const float u0 = 4.0f / 128.0f, u1 = 124.0f / 128.0f;
            part3.vertices.push_back({{-0.4f, -0.4f, z}, normal, tint,
                                      {u0, u0, 1.0f, 1.0f}, kTriMaterial, {}});
            part3.vertices.push_back({{0.4f, -0.4f, z}, normal, tint,
                                      {u1, u0, 1.0f, 1.0f}, kTriMaterial, {}});
            part3.vertices.push_back({{0.4f, 0.4f, z}, normal, tint,
                                      {u1, u1, 1.0f, 1.0f}, kTriMaterial, {}});
            part3.vertices.push_back({{-0.4f, 0.4f, z}, normal, tint,
                                      {u0, u1, 1.0f, 1.0f}, kTriMaterial, {}});
            part3.indices = {0, 1, 2, 0, 2, 3};
            chart_atlas::ChartEntry entry{};
            entry.origin[0] = -0.4f;
            entry.origin[1] = -0.4f;
            entry.origin[2] = z;
            entry.tangent[0] = 1.0f;
            entry.bitangent[1] = 1.0f;
            entry.rect_x = 0;
            entry.rect_y = 0;
            entry.rect_w = 128;
            entry.rect_h = 128;
            entry.texels_per_meter = tpm;
            entry.first_tri = 0;
            entry.tri_count = 2;
            rung3.charts.push_back(entry);
            rung3.tri_order = {0, 1};
        }
        part3.clusters[0].lods[0] = {0, 6, 0.0f, 0u};
        part3.lod_charts = {rung3};
        viewer::VkScenePartChartMesh mesh3;
        mesh3.vertex_count = 4;
        for (const viewer::VkRasterVertex& vertex : part3.vertices) {
            mesh3.positions.push_back(vertex.position.x);
            mesh3.positions.push_back(vertex.position.y);
            mesh3.positions.push_back(vertex.position.z);
            mesh3.normals.push_back(vertex.normal.x);
            mesh3.normals.push_back(vertex.normal.y);
            mesh3.normals.push_back(vertex.normal.z);
            mesh3.surface_uvs.push_back(vertex.surface.x);
            mesh3.surface_uvs.push_back(vertex.surface.y);
            mesh3.material_ids.push_back(kTriMaterial);
        }
        mesh3.indices = part3.indices;
        mesh3.dominant_material = kTriMaterial;
        // The classifier's per-vertex columns for this tape (grass left,
        // rock right) — the mode-2 fallback payload the gate requires.
        mesh3.surface_weights = {255, 0, 0, 255, 0, 255, 255, 0};
        part3.lod_chart_meshes = {std::move(mesh3)};
        part3.surface_materials = {kMatGrass, kMatRock};
        part3.surface_tape_hash = 0x5EAF00D100000003ull;
        part3.surface_tape_text =
            "input lx\n"
            "smoothstep -0.0025 0.0025 r0\n"
            "oneminus r1\n"
            "material 3 r2\n"
            "material 4 r1\n";
        // Not world-anchored, no field lanes: the defaults are the contract.
        CHECK(renderer.ensure_part(part3, error) >= 0,
              error.empty() ? "vt-surfaces: ensure mode-3 quad"
                            : error.c_str());
        CHECK(renderer.update_instances({{0x7711, identity, 1}}, error),
              error.empty() ? "vt-surfaces: upload mode-3 instance"
                            : error.c_str());
        for (int i = 0; i < 6; ++i)
            render_once("vt-surfaces: mode-3 frame");
        // Probe columns ~0.04 m either side of the step (quad spans screen
        // x 64..96 with this camera; the edge is at x = 80).
        const viewer::VkRasterPixel m3_left = pixel_at(78, 80);
        const viewer::VkRasterPixel m3_right = pixel_at(82, 80);
        CHECK(m3_left.material_index == kTriMaterial &&
                  m3_right.material_index == kTriMaterial,
              "vt-surfaces mode 3: probes land on the quad");
        CHECK(close_albedo(m3_left, grass_albedo, 2.0e-2f),
              "vt-surfaces mode 3: pure grass just left of the step edge "
              "(per-texel tape, not the per-vertex ramp)");
        CHECK(close_albedo(m3_right, rock_albedo, 2.0e-2f),
              "vt-surfaces mode 3: pure rock just right of the step edge");
        render_once("vt-surfaces: mode-3 determinism frame");
        const viewer::VkRasterPixel m3_left2 = pixel_at(78, 80);
        CHECK(m3_left2.albedo.x == m3_left.albedo.x &&
                  m3_left2.albedo.y == m3_left.albedo.y &&
                  m3_left2.albedo.z == m3_left.albedo.z,
              "vt-surfaces mode 3: pages bit-identical across frames");
        renderer.release_part(0x7711);
        CHECK(renderer.vt_stats().variants == 0,
              "vt-surfaces mode 3: release returns the variant");
    }
}

// WP-H (tier-2 hemisphere AO page enrichment): MATTER_VK_SMOKE_MODE=vt-enrich
// and MATTER_VK_SMOKE_MODE=vt-enrich-nort.
//
// FIXTURE — a known occluder, in ONE variant so it lands in that variant's own
// acceleration structure (part-local self-occlusion is all tier 2 bakes):
//   chart 0  a 3 x 3 m wall in the plane z = -2, normal +Z;
//   chart 1  a 1 m deep "fin" standing on it in the plane x = 0, so the wall
//            texels next to x = 0 are occluded and the ones far from it are not.
// Texel density is a deliberate 4 texels/m, so one page texel is 0.25 m ≈ 10
// screen pixels and a probe cannot straddle the contact band. The effective cap
// is min(cap_texels x texel, cap_meters) = min(4 x 0.25, 0.5) = 0.5 m — the
// ABSOLUTE ceiling binds here, which is the point: the near probe sits on the
// texel whose centre is 0.125 m from the fin (a quarter of the cap, strongly
// occluded) and the far probe on one 1.125 m away (outside the cap entirely, so
// exactly unoccluded). The mip fade is 1.0 at this footprint (0.25 m < the
// 0.5 m fade start), so this fixture exercises the un-faded contact regime.
//
// The G-buffer's ORM.z is the shaded occlusion, and for a detail-less material
// gbuffer.frag reduces to `ao = vertex_ao * vt_orm.r` with vertex_ao = 1 (the
// fixture's surface.zw is (1,1)), so ORM.z reads back the page's occlusion
// channel directly.
//
// Asserts:
//   (a) post-enrichment occlusion is darker next to the fin than far from it,
//       and the far probe is still essentially unoccluded;
//   (b) determinism — further frames reproduce the pixels bit-exactly, and the
//       enrichment does not run again over an already-enriched page (which is
//       what stops the in-place multiply from compounding);
//   (c) albedo / roughness / metallic are bit-identical to the un-enriched arm,
//       i.e. tier 2 touches ONLY the occlusion channel;
//   (d) a material-table edit invalidates page content, and the re-filled pages
//       are re-enriched (contrast comes back).
// The `vt-enrich-nort` mode forces the device to report no ray tracing
// (MATTER_VK_TEST_FORCE_RT_UNAVAILABLE, the existing rt-unavailable pattern) and
// runs the same fixture: no enricher loads, nothing is ever queued, and every
// probe reads unoccluded tier-1 content — today's behaviour, unchanged.
void run_vt_enrich_path(matter::VulkanDevice& vulkan) {
    const bool rt = vulkan.ray_tracing_available();
    constexpr uint32_t width = 160;
    constexpr uint32_t height = 160;
#ifdef _WIN32
    _putenv_s("MATTER_VT_POOL_PAGES", "256");
    _putenv_s("MATTER_VT_ENRICH_PER_FRAME", "2");
#else
    setenv("MATTER_VT_POOL_PAGES", "256", 1);
    setenv("MATTER_VT_ENRICH_PER_FRAME", "2", 1);
#endif
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error),
          error.empty() ? "vt-enrich: renderer init" : error.c_str());

    constexpr uint32_t kMaterial = 5u;
    constexpr float kRoughness = 0.40f;
    constexpr float kMetallic = 0.0f;
    const matter::Float3 page_albedo{0.85f, 0.80f, 0.72f};
    std::vector<MaterialGpuRecord> materials(kMaterial + 1);
    materials[kMaterial].base_roughness[0] = page_albedo.x;
    materials[kMaterial].base_roughness[1] = page_albedo.y;
    materials[kMaterial].base_roughness[2] = page_albedo.z;
    materials[kMaterial].base_roughness[3] = kRoughness;
    materials[kMaterial].metal_opacity_spec_coat[0] = kMetallic;
    materials[kMaterial].metal_opacity_spec_coat[1] = 1.0f;   // opacity
    CHECK(renderer.update_materials(materials, 1, 1, error),
          error.empty() ? "vt-enrich: stage materials" : error.c_str());

    constexpr float kWallZ = -2.0f;
    constexpr float kHalf = 1.5f;      // wall half-extent, metres
    constexpr float kFinDepth = 1.0f;  // how far the fin stands off the wall
    constexpr float kTpm = 4.0f;       // texels/m => 0.25 m texels, 1.0 m cap
    viewer::VkScenePart charted =
        fixed_part(0x7801, {-kHalf, -kHalf, kWallZ}, {kHalf, kHalf, kWallZ + kFinDepth},
                   0);
    {
        const matter::Float3 wall_n{0.0f, 0.0f, 1.0f};
        const matter::Float3 fin_n{-1.0f, 0.0f, 0.0f};
        const matter::Float4 tint{1.0f, 1.0f, 1.0f, 0.0f};   // a = 0: no tint
        // Chart 0 (wall): texel = 4 + (p.x + 1.5) * tpm, same in v with p.y.
        const auto wu = [&](float x) { return (4.0f + (x + kHalf) * kTpm) / 256.0f; };
        const auto wv = [&](float y) { return (4.0f + (y + kHalf) * kTpm) / 128.0f; };
        // Chart 1 (fin): rect starts at atlas x 128; plane U = p.z, V = p.y.
        const auto fu = [&](float z) {
            return (128.0f + 4.0f + (z - kWallZ) * kTpm) / 256.0f;
        };
        charted.vertices = {
            {{-kHalf, -kHalf, kWallZ}, wall_n, tint, {wu(-kHalf), wv(-kHalf), 1.0f, 1.0f}, kMaterial, {}},
            {{ kHalf, -kHalf, kWallZ}, wall_n, tint, {wu( kHalf), wv(-kHalf), 1.0f, 1.0f}, kMaterial, {}},
            {{ kHalf,  kHalf, kWallZ}, wall_n, tint, {wu( kHalf), wv( kHalf), 1.0f, 1.0f}, kMaterial, {}},
            {{-kHalf,  kHalf, kWallZ}, wall_n, tint, {wu(-kHalf), wv( kHalf), 1.0f, 1.0f}, kMaterial, {}},
            {{0.0f, -kHalf, kWallZ}, fin_n, tint, {fu(kWallZ), wv(-kHalf), 1.0f, 1.0f}, kMaterial, {}},
            {{0.0f, -kHalf, kWallZ + kFinDepth}, fin_n, tint, {fu(kWallZ + kFinDepth), wv(-kHalf), 1.0f, 1.0f}, kMaterial, {}},
            {{0.0f,  kHalf, kWallZ + kFinDepth}, fin_n, tint, {fu(kWallZ + kFinDepth), wv( kHalf), 1.0f, 1.0f}, kMaterial, {}},
            {{0.0f,  kHalf, kWallZ}, fin_n, tint, {fu(kWallZ), wv( kHalf), 1.0f, 1.0f}, kMaterial, {}},
        };
        charted.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
        charted.clusters[0].lods[0] = {0, 12, 0.0f, /*chart_rung=*/0u};

        chart_atlas::ChartAtlasRung rung;
        rung.atlas_w = 256;
        rung.atlas_h = 128;
        rung.charts.resize(2);
        chart_atlas::ChartEntry& wall = rung.charts[0];
        wall.origin[0] = -kHalf; wall.origin[1] = -kHalf; wall.origin[2] = kWallZ;
        wall.tangent[0] = 1.0f; wall.tangent[1] = 0.0f; wall.tangent[2] = 0.0f;
        wall.bitangent[0] = 0.0f; wall.bitangent[1] = 1.0f; wall.bitangent[2] = 0.0f;
        wall.rect_x = 0; wall.rect_y = 0; wall.rect_w = 128; wall.rect_h = 128;
        wall.texels_per_meter = kTpm;
        wall.first_tri = 0; wall.tri_count = 2;
        chart_atlas::ChartEntry& fin = rung.charts[1];
        fin.origin[0] = 0.0f; fin.origin[1] = -kHalf; fin.origin[2] = kWallZ;
        fin.tangent[0] = 0.0f; fin.tangent[1] = 0.0f; fin.tangent[2] = 1.0f;
        fin.bitangent[0] = 0.0f; fin.bitangent[1] = 1.0f; fin.bitangent[2] = 0.0f;
        fin.rect_x = 128; fin.rect_y = 0; fin.rect_w = 128; fin.rect_h = 128;
        fin.texels_per_meter = kTpm;
        fin.first_tri = 2; fin.tri_count = 2;
        rung.tri_order = {0, 1, 2, 3};
        charted.lod_charts = {rung};

        viewer::VkScenePartChartMesh mesh;
        mesh.vertex_count = 8;
        for (const viewer::VkRasterVertex& vertex : charted.vertices) {
            mesh.positions.push_back(vertex.position.x);
            mesh.positions.push_back(vertex.position.y);
            mesh.positions.push_back(vertex.position.z);
            mesh.normals.push_back(vertex.normal.x);
            mesh.normals.push_back(vertex.normal.y);
            mesh.normals.push_back(vertex.normal.z);
            mesh.surface_uvs.push_back(vertex.surface.x);
            mesh.surface_uvs.push_back(vertex.surface.y);
            mesh.material_ids.push_back(kMaterial);
        }
        mesh.indices = charted.indices;
        mesh.dominant_material = kMaterial;
        charted.lod_chart_meshes = {std::move(mesh)};
    }
    CHECK(renderer.ensure_part(charted, error) >= 0,
          error.empty() ? "vt-enrich: ensure occluder part" : error.c_str());
    CHECK(renderer.vt_active(), "vt-enrich: the residency runtime started");
    {
        const vt::VtResidency::Stats started = renderer.vt_stats();
        if (rt) {
            CHECK(started.enrich_samples >= 8u && started.enrich_samples <= 64u,
                  "vt-enrich: the enricher reports its ray budget");
        } else {
            CHECK(started.enrich_samples == 0u,
                  "vt-enrich-nort: no enricher loads without ray tracing");
        }
    }

    // A second, deliberately COARSE variant: 0.5 texels/m, so even its finest
    // page has 2 m texels — past the enricher's fade end. Tier 2 must never
    // queue it (the guard that stops a streamed terrain sector's coarse mips
    // from being enriched at a scale where the cap used to grow to tens of
    // metres and blacken open slopes). It needs no instance: registration
    // queues its pinned tail, and record_frame fills that regardless of
    // visibility, which is exactly the path queue_enrich sits on.
    {
        viewer::VkScenePart coarse =
            fixed_part(0x7802, {-32.0f, -32.0f, -80.0f}, {32.0f, 32.0f, -80.0f}, 0);
        const matter::Float3 n{0.0f, 0.0f, 1.0f};
        const matter::Float4 tint{1.0f, 1.0f, 1.0f, 0.0f};
        const auto cu = [](float x) { return (4.0f + (x + 32.0f) * 0.5f) / 256.0f; };
        const auto cv = [](float y) { return (4.0f + (y + 32.0f) * 0.5f) / 128.0f; };
        coarse.vertices = {
            {{-32.0f, -32.0f, -80.0f}, n, tint, {cu(-32.0f), cv(-32.0f), 1.0f, 1.0f}, kMaterial, {}},
            {{ 32.0f, -32.0f, -80.0f}, n, tint, {cu( 32.0f), cv(-32.0f), 1.0f, 1.0f}, kMaterial, {}},
            {{ 32.0f,  32.0f, -80.0f}, n, tint, {cu( 32.0f), cv( 32.0f), 1.0f, 1.0f}, kMaterial, {}},
            {{-32.0f,  32.0f, -80.0f}, n, tint, {cu(-32.0f), cv( 32.0f), 1.0f, 1.0f}, kMaterial, {}},
        };
        coarse.indices = {0, 1, 2, 0, 2, 3};
        coarse.clusters[0].lods[0] = {0, 6, 0.0f, /*chart_rung=*/0u};
        chart_atlas::ChartAtlasRung rung;
        rung.atlas_w = 256;
        rung.atlas_h = 128;
        rung.charts.resize(1);
        chart_atlas::ChartEntry& c = rung.charts[0];
        c.origin[0] = -32.0f; c.origin[1] = -32.0f; c.origin[2] = -80.0f;
        c.tangent[0] = 1.0f; c.bitangent[1] = 1.0f;
        c.rect_x = 0; c.rect_y = 0; c.rect_w = 128; c.rect_h = 128;
        c.texels_per_meter = 0.5f;
        c.first_tri = 0; c.tri_count = 2;
        rung.tri_order = {0, 1};
        coarse.lod_charts = {rung};
        viewer::VkScenePartChartMesh mesh;
        mesh.vertex_count = 4;
        for (const viewer::VkRasterVertex& vertex : coarse.vertices) {
            mesh.positions.push_back(vertex.position.x);
            mesh.positions.push_back(vertex.position.y);
            mesh.positions.push_back(vertex.position.z);
            mesh.normals.push_back(vertex.normal.x);
            mesh.normals.push_back(vertex.normal.y);
            mesh.normals.push_back(vertex.normal.z);
            mesh.surface_uvs.push_back(vertex.surface.x);
            mesh.surface_uvs.push_back(vertex.surface.y);
            mesh.material_ids.push_back(kMaterial);
        }
        mesh.indices = coarse.indices;
        mesh.dominant_material = kMaterial;
        coarse.lod_chart_meshes = {std::move(mesh)};
        CHECK(renderer.ensure_part(coarse, error) >= 0,
              error.empty() ? "vt-enrich: ensure coarse part" : error.c_str());
    }

    const matter::Mat4f identity = identity_matrix();
    CHECK(renderer.update_instances({{0x7801, identity, 1}}, error),
          error.empty() ? "vt-enrich: upload instance" : error.c_str());

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices frame{};
    CHECK(viewer::build_frame_matrices(camera, width, height, frame, error),
          error.empty() ? "vt-enrich: build frame matrices" : error.c_str());

    // render_gbuffer_and_composite submits and waits, so the wall-clock delta
    // across it is an honest end-to-end frame cost INCLUDING the tier-2 pass.
    // Bracketing enriching frames against settled ones is what bounds the
    // background cost (the spec's "< 10% GPU on the flight path" criterion).
    double last_render_ms = 0.0;
    const auto render_once = [&](const char* label) {
        std::string local;
        CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, local),
              local.empty() ? label : local.c_str());
        const auto started = std::chrono::steady_clock::now();
        CHECK(renderer.render_gbuffer_and_composite(width, height, local),
              local.empty() ? label : local.c_str());
        last_render_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    };
    const auto pixel_at = [&](uint32_t x, uint32_t y) {
        viewer::VkRasterPixel pixel{};
        std::string local;
        CHECK(renderer.readback_raster_pixel(x, y, pixel, local),
              local.empty() ? "vt-enrich: readback pixel" : local.c_str());
        return pixel;
    };

    // Screen mapping: at z = -2 with a 90 deg vertical fov and a square target,
    // the visible half-extent is 2 m, so screen_x = 80 + 40 * world_x. Both
    // probes land on a TEXEL CENTRE (texels are 0.25 m = 10 px, centres at
    // x = -0.125, -0.375, ...), so the reading is that texel's own baked value
    // rather than a bilinear blend of two:
    //   x = -0.125 m -> 75 : 0.125 m from the fin, a quarter of the 0.5 m cap;
    //   x = -1.125 m -> 35 : 1.125 m from the fin, past the cap entirely.
    constexpr uint32_t kNearX = 75;
    constexpr uint32_t kFarX = 35;
    constexpr uint32_t kProbeY = 80;

    // Frame 1 fills the pinned tail; the feedback loop then drains the finest
    // pages. Enrichment trails the fills by at least a frame by construction
    // (the queue is drained before the fills that feed it), and runs 2 pages a
    // frame, so a dozen frames is well past "everything resident and enriched".
    double enriching_frame_ms = 0.0;
    double settled_frame_ms = 0.0;
    uint64_t enrich_before = renderer.vt_stats().enrich_total;
    for (int i = 0; i < 12; ++i) {
        render_once("vt-enrich: settle frame");
        const uint64_t enrich_now = renderer.vt_stats().enrich_total;
        if (enrich_now != enrich_before)
            enriching_frame_ms = last_render_ms;
        else
            settled_frame_ms = last_render_ms;
        enrich_before = enrich_now;
    }
    std::printf("vt-enrich frame cost: enriching=%.3f ms settled=%.3f ms "
                "(submit+wait, 160x160)\n",
                enriching_frame_ms, settled_frame_ms);

    const vt::VtResidency::Stats settled = renderer.vt_stats();
    std::printf("vt-enrich stats: rt=%d samples=%u fills=%llu failed=%llu "
                "enrich=%llu queue=%u enriched_pages=%u dropped=%llu "
                "skipped_coarse=%llu\n",
                rt ? 1 : 0, settled.enrich_samples,
                static_cast<unsigned long long>(settled.fills_total),
                static_cast<unsigned long long>(settled.fills_failed_total),
                static_cast<unsigned long long>(settled.enrich_total),
                settled.enrich_queue_depth, settled.enriched_pages,
                static_cast<unsigned long long>(settled.enrich_dropped_total),
                static_cast<unsigned long long>(
                    settled.enrich_skipped_coarse_total));
    CHECK(settled.fills_total >= 2,
          "vt-enrich: the tail and at least one finer page filled");
    // The map-or-rollback path: nothing may have been dispatched-but-unwritten,
    // because a filled-flag that never comes back true is what used to leave a
    // mapped page pointing at never-written (black) pool memory.
    CHECK(settled.fills_failed_total == 0,
          "vt-enrich: every dispatched fill reported success");
    if (rt) {
        // The coarse variant's pages are past the fade end and must never have
        // been queued; the fine variant's must have been.
        CHECK(settled.enrich_skipped_coarse_total >= 1,
              "vt-enrich: pages coarser than the contact scale are skipped, "
              "not traced and multiplied by zero");
    } else {
        CHECK(settled.enrich_skipped_coarse_total == 0,
              "vt-enrich-nort: with no enricher nothing even reaches the "
              "coarse-page test");
    }

    const viewer::VkRasterPixel near_probe = pixel_at(kNearX, kProbeY);
    const viewer::VkRasterPixel far_probe = pixel_at(kFarX, kProbeY);
    CHECK(near_probe.material_index == kMaterial &&
              far_probe.material_index == kMaterial,
          "vt-enrich: both probes land on the charted wall");
    std::printf("vt-enrich occlusion: near=%.4f far=%.4f\n",
                static_cast<double>(near_probe.orm.z),
                static_cast<double>(far_probe.orm.z));

    if (!rt) {
        // (c), the RT-unavailable arm: no enricher, nothing queued, nothing
        // enriched, and every page still reads as unoccluded tier-1 content.
        CHECK(settled.enrich_total == 0,
              "vt-enrich-nort: no page is ever enriched");
        CHECK(settled.enrich_queue_depth == 0 && settled.enriched_pages == 0,
              "vt-enrich-nort: nothing is even queued for tier 2");
        CHECK(std::fabs(near_probe.orm.z - 1.0f) < 2.0e-2f &&
                  std::fabs(far_probe.orm.z - 1.0f) < 2.0e-2f,
              "vt-enrich-nort: tier-1 pages stay unoccluded");
        CHECK(near_probe.orm.z == far_probe.orm.z,
              "vt-enrich-nort: occlusion is flat across the occluder's shadow");
        CHECK(close4(near_probe.albedo,
                     {page_albedo.x, page_albedo.y, page_albedo.z, 1.0f},
                     2.0e-2f),
              "vt-enrich-nort: albedo is the composited tier-1 page");
        renderer.release_part(0x7801);
        renderer.release_part(0x7802);
        return;
    }

    // (a) baked contact occlusion.
    CHECK(settled.enrich_total >= 1, "vt-enrich: pages were enriched");
    CHECK(std::fabs(far_probe.orm.z - 1.0f) < 3.0e-2f,
          "vt-enrich: a texel beyond the distance cap stays unoccluded");
    CHECK(near_probe.orm.z < far_probe.orm.z - 5.0e-2f,
          "vt-enrich: the texel beside the occluder is measurably darker");
    // The floor (MATTER_VT_ENRICH_MIN_AO, default 0.15) is the structural
    // guarantee that a baked contact term can never zero out ambient, which is
    // what blackened open slopes on StreamMountain before the cap ceiling and
    // this floor went in.
    CHECK(near_probe.orm.z > 0.14f,
          "vt-enrich: the occlusion respects the min-ao floor, so a page can "
          "never go black");
    // Enrichment must not have disturbed the rest of the page.
    CHECK(close4(near_probe.albedo,
                 {page_albedo.x, page_albedo.y, page_albedo.z, 1.0f}, 2.0e-2f),
          "vt-enrich: albedo survives the ORM read-modify-write");
    CHECK(std::fabs(near_probe.orm.x - kRoughness) < 3.0e-2f,
          "vt-enrich: roughness survives the ORM re-encode");
    CHECK(std::fabs(near_probe.orm.y - kMetallic) < 3.0e-2f,
          "vt-enrich: metallic survives the ORM re-encode");

    // (b) determinism, twice over. Further frames must reproduce the pixels
    // bit-exactly, AND the enrichment must not run again over a page it already
    // refined -- the apply is an in-place multiply, so a second pass would
    // darken the page a second time.
    const uint64_t enrich_after_settle = settled.enrich_total;
    for (int i = 0; i < 6; ++i) render_once("vt-enrich: determinism frame");
    const viewer::VkRasterPixel near_again = pixel_at(kNearX, kProbeY);
    const viewer::VkRasterPixel far_again = pixel_at(kFarX, kProbeY);
    CHECK(renderer.vt_stats().enrich_total == enrich_after_settle,
          "vt-enrich: a settled frame never re-enriches a resident page");
    CHECK(near_again.orm.z == near_probe.orm.z &&
              far_again.orm.z == far_probe.orm.z,
          "vt-enrich: enriched occlusion is bit-identical across frames");
    CHECK(near_again.albedo.x == near_probe.albedo.x &&
              near_again.albedo.y == near_probe.albedo.y &&
              near_again.albedo.z == near_probe.albedo.z,
          "vt-enrich: enriched pages are bit-identical across frames");

    // (d) invalidation clears the tier and the re-filled pages re-enrich. The
    // material edit is what push_vt_compositor_inputs turns into an
    // invalidate_all_content, which drops resident content AND every tier-2 bit.
    const matter::Float3 edited_albedo{0.30f, 0.55f, 0.40f};
    materials[kMaterial].base_roughness[0] = edited_albedo.x;
    materials[kMaterial].base_roughness[1] = edited_albedo.y;
    materials[kMaterial].base_roughness[2] = edited_albedo.z;
    CHECK(renderer.update_materials(materials, 2, 1, error),
          error.empty() ? "vt-enrich: edit the material table" : error.c_str());
    for (int i = 0; i < 12; ++i) render_once("vt-enrich: post-edit frame");
    const vt::VtResidency::Stats reenriched = renderer.vt_stats();
    std::printf("vt-enrich post-edit: invalidations=%llu enrich=%llu (was %llu) "
                "enriched_pages=%u\n",
                static_cast<unsigned long long>(reenriched.invalidations_total),
                static_cast<unsigned long long>(reenriched.enrich_total),
                static_cast<unsigned long long>(enrich_after_settle),
                reenriched.enriched_pages);
    CHECK(reenriched.invalidations_total == 1,
          "vt-enrich: the material edit invalidated resident content once");
    CHECK(reenriched.enrich_total > enrich_after_settle,
          "vt-enrich: invalidated pages were re-filled AND re-enriched");
    const viewer::VkRasterPixel near_edited = pixel_at(kNearX, kProbeY);
    const viewer::VkRasterPixel far_edited = pixel_at(kFarX, kProbeY);
    CHECK(close4(near_edited.albedo,
                 {edited_albedo.x, edited_albedo.y, edited_albedo.z, 1.0f},
                 2.0e-2f),
          "vt-enrich: the page re-composited from the edited material table");
    CHECK(std::fabs(far_edited.orm.z - 1.0f) < 3.0e-2f,
          "vt-enrich: the re-enriched far texel is unoccluded again");
    CHECK(near_edited.orm.z < far_edited.orm.z - 5.0e-2f,
          "vt-enrich: the re-enriched near texel carries contact occlusion "
          "again");
    // The occlusion is a function of geometry only, so a re-fill + re-enrich of
    // the same geometry must land on the same value -- if the invalidation had
    // failed to clear the tier bit, the multiply would have compounded and this
    // would be visibly darker.
    CHECK(std::fabs(near_edited.orm.z - near_probe.orm.z) < 2.0e-2f,
          "vt-enrich: re-enrichment reproduces the same occlusion, not a "
          "compounded one");

    renderer.release_part(0x7801);
    renderer.release_part(0x7802);
    const vt::VtResidency::Stats released = renderer.vt_stats();
    CHECK(released.variants == 0,
          "vt-enrich: releasing the part unregisters its variant rung");
    CHECK(released.enriched_pages == 0,
          "vt-enrich: releasing the part forgets its tier-2 state");
}

// WP-G (RT sampling of VT + ray cones): MATTER_VK_SMOKE_MODE=vt-rt.
//
// Same two-chart fixture idea as run_vt_path, but driven through the FULL
// frame path (prepare/record_cull_and_render/composite) so the RT pipeline
// runs, and with the two charts carrying DIFFERENT materials so a page mix-up
// is visible. Asserts the two Phase-5 exit criteria:
//
//   (1) Consistency. A traced hit on a VT part resolves the SAME page the
//       G-buffer fragment at that surface point resolved: per chart, the
//       ray's VT albedo agrees with the G-buffer albedo within filtering
//       epsilon, and the two charts do NOT agree with each other (which is
//       what rules out "any page will do").
//   (2) Cone-mip monotonicity. Tracing the same surface point from
//       increasing distances with a fixed cone spread selects
//       monotonically coarser virtual mips, and the cone footprint at the
//       hit grows with distance. This is the property the deleted
//       RT_TILESET_CONE_SPREAD constant could only fake.
void run_vt_rt_path(matter::VulkanDevice& vulkan) {
    if (!vulkan.ray_tracing_available()) {
        std::printf("vt-rt: ray tracing unavailable, skipping\n");
        return;
    }
    // The frame path renders at the swapchain extent (the hidden 320x200 GLFW
    // window), not at a caller-chosen size, so the matrices and the probe
    // pixel arithmetic below both use it.
    constexpr uint32_t width = 320;
    constexpr uint32_t height = 200;
#ifdef _WIN32
    _putenv_s("MATTER_VT_POOL_PAGES", "256");
#else
    setenv("MATTER_VT_POOL_PAGES", "256", 1);
#endif
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error),
          error.empty() ? "vt-rt: renderer init" : error.c_str());

    // Two detail-less materials: the compositor bakes each one's flat albedo
    // into the pages of the chart whose triangles carry it, so chart 0 and
    // chart 1 end up visibly different colours.
    constexpr uint32_t kMaterialA = 5u;
    constexpr uint32_t kMaterialB = 6u;
    const matter::Float3 albedo_a{0.90f, 0.10f, 0.15f};
    const matter::Float3 albedo_b{0.10f, 0.25f, 0.85f};
    std::vector<MaterialGpuRecord> materials(kMaterialB + 1);
    materials[kMaterialA].base_roughness[0] = albedo_a.x;
    materials[kMaterialA].base_roughness[1] = albedo_a.y;
    materials[kMaterialA].base_roughness[2] = albedo_a.z;
    materials[kMaterialA].base_roughness[3] = 0.40f;
    materials[kMaterialA].metal_opacity_spec_coat[1] = 1.0f;
    materials[kMaterialB].base_roughness[0] = albedo_b.x;
    materials[kMaterialB].base_roughness[1] = albedo_b.y;
    materials[kMaterialB].base_roughness[2] = albedo_b.z;
    materials[kMaterialB].base_roughness[3] = 0.40f;
    materials[kMaterialB].metal_opacity_spec_coat[1] = 1.0f;
    CHECK(renderer.update_materials(materials, 1, 1, error),
          error.empty() ? "vt-rt: stage materials" : error.c_str());

    // Geometry: the run_vt_path two-chart quad (0.8 m square halves at
    // z = -2, 150 texels/m into a 256x128 atlas, page-aligned so the chart
    // boundary is also a finest-mip page boundary), with per-half materials.
    constexpr float kQuadZ = -2.0f;
    viewer::VkScenePart charted =
        fixed_part(0x7611, {-0.8f, -0.4f, kQuadZ}, {0.8f, 0.4f, kQuadZ}, 0);
    {
        const float tpm = 150.0f;
        const matter::Float3 normal{0.0f, 0.0f, 1.0f};
        const matter::Float4 tint{1.0f, 1.0f, 1.0f, 0.0f};   // a = 0: no tint
        const float u0a = 4.0f / 256.0f, u1a = 124.0f / 256.0f;
        const float u0b = 132.0f / 256.0f, u1b = 252.0f / 256.0f;
        const float v0 = 4.0f / 128.0f, v1 = 124.0f / 128.0f;
        charted.vertices = {
            {{-0.8f, -0.4f, kQuadZ}, normal, tint, {u0a, v0, 1.0f, 1.0f}, kMaterialA, {}},
            {{ 0.0f, -0.4f, kQuadZ}, normal, tint, {u1a, v0, 1.0f, 1.0f}, kMaterialA, {}},
            {{ 0.0f,  0.4f, kQuadZ}, normal, tint, {u1a, v1, 1.0f, 1.0f}, kMaterialA, {}},
            {{-0.8f,  0.4f, kQuadZ}, normal, tint, {u0a, v1, 1.0f, 1.0f}, kMaterialA, {}},
            {{ 0.0f, -0.4f, kQuadZ}, normal, tint, {u0b, v0, 1.0f, 1.0f}, kMaterialB, {}},
            {{ 0.8f, -0.4f, kQuadZ}, normal, tint, {u1b, v0, 1.0f, 1.0f}, kMaterialB, {}},
            {{ 0.8f,  0.4f, kQuadZ}, normal, tint, {u1b, v1, 1.0f, 1.0f}, kMaterialB, {}},
            {{ 0.0f,  0.4f, kQuadZ}, normal, tint, {u0b, v1, 1.0f, 1.0f}, kMaterialB, {}},
        };
        charted.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
        charted.clusters[0].lods[0] = {0, 12, 0.0f, /*chart_rung=*/0u};

        chart_atlas::ChartAtlasRung rung;
        rung.atlas_w = 256;
        rung.atlas_h = 128;
        rung.charts.resize(2);
        const float chart_origin_x[2] = {-0.8f, 0.0f};
        for (int c = 0; c < 2; ++c) {
            chart_atlas::ChartEntry& entry = rung.charts[c];
            entry.origin[0] = chart_origin_x[c];
            entry.origin[1] = -0.4f;
            entry.origin[2] = kQuadZ;
            entry.tangent[0] = 1.0f; entry.tangent[1] = 0.0f; entry.tangent[2] = 0.0f;
            entry.bitangent[0] = 0.0f; entry.bitangent[1] = 1.0f; entry.bitangent[2] = 0.0f;
            entry.rect_x = static_cast<uint32_t>(c) * 128u;
            entry.rect_y = 0;
            entry.rect_w = 128;
            entry.rect_h = 128;
            entry.texels_per_meter = tpm;
            entry.first_tri = static_cast<uint32_t>(c) * 2u;
            entry.tri_count = 2;
        }
        rung.tri_order = {0, 1, 2, 3};
        charted.lod_charts = {rung};

        viewer::VkScenePartChartMesh mesh;
        mesh.vertex_count = 8;
        for (const viewer::VkRasterVertex& vertex : charted.vertices) {
            mesh.positions.push_back(vertex.position.x);
            mesh.positions.push_back(vertex.position.y);
            mesh.positions.push_back(vertex.position.z);
            mesh.normals.push_back(vertex.normal.x);
            mesh.normals.push_back(vertex.normal.y);
            mesh.normals.push_back(vertex.normal.z);
            mesh.surface_uvs.push_back(vertex.surface.x);
            mesh.surface_uvs.push_back(vertex.surface.y);
            mesh.material_ids.push_back(vertex.material_index);
        }
        mesh.indices = charted.indices;
        mesh.dominant_material = kMaterialA;
        charted.lod_chart_meshes = {std::move(mesh)};
    }
    CHECK(renderer.ensure_part(charted, error) >= 0,
          error.empty() ? "vt-rt: ensure two-chart part" : error.c_str());
    CHECK(renderer.vt_active(), "vt-rt: the residency runtime started");

    const matter::Mat4f identity = identity_matrix();
    CHECK(renderer.update_instances({{0x7611, identity, 1}}, error),
          error.empty() ? "vt-rt: upload instance" : error.c_str());

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 100.0f;
    viewer::FrameMatrices matrices{};
    CHECK(viewer::build_frame_matrices(camera, width, height, matrices, error),
          error.empty() ? "vt-rt: build frame matrices" : error.c_str());

    matter::VulkanRayTracingSettings rt_settings{};
    rt_settings.enabled = true;
    rt_settings.max_distance = 200.0f;
    renderer.set_ray_tracing_settings(rt_settings);
    matter::VulkanGiSettings gi{};
    gi.enabled = 1;
    gi.max_bounces = 1;
    gi.samples_per_pixel = 1;
    renderer.set_gi_settings(gi);

    // A frame with an optional test surface ray appended. The G-buffer,
    // the TLAS and the VT feedback/fill loop all advance here, so the same
    // frame both shades the raster probes and traces the RT probes.
    viewer::RtSurfaceHit probe_hit{};
    uint32_t probe_invalid = 0;
    const auto frame_with_probe = [&](bool trace, matter::Float3 origin,
                                      matter::Float3 direction,
                                      float cone_width, float cone_spread) {
        std::string local;
        matter::VulkanFrame frame{};
        if (!vulkan.begin_frame(frame, local)) {
            CHECK(false, local.empty() ? "vt-rt: begin frame" : local.c_str());
            return;
        }
        bool recorded =
            renderer.prepare_frame(frame, matrices, camera.position, 1.0f,
                                   local) &&
            renderer.record_cull_and_render(frame, matrices, camera.position,
                                            1.0f, local) &&
            renderer.record_composite_to_swapchain(frame, local);
        if (recorded && trace)
            recorded = renderer.record_test_surface_ray(
                frame, origin, direction, UINT32_MAX, cone_width, cone_spread,
                local);
        const bool submitted = recorded && vulkan.end_frame(frame, local);
        renderer.finish_ray_tracing_frame(frame.serial, submitted);
        CHECK(submitted, local.empty() ? "vt-rt: submit frame" : local.c_str());
        if (!submitted) return;
        vulkan.wait_idle();
        if (trace)
            CHECK(renderer.readback_test_surface_hit(frame.frame_slot,
                                                     probe_hit, probe_invalid,
                                                     local),
                  local.empty() ? "vt-rt: readback probe" : local.c_str());
    };

    // Settle: frame 1 fills the pinned tail, the feedback loop then drains the
    // finest pages under the on-screen probes.
    for (int i = 0; i < 6; ++i)
        frame_with_probe(false, {}, {}, 0.0f, 0.0f);

    const auto pixel_at = [&](uint32_t x, uint32_t y) {
        viewer::VkRasterPixel pixel{};
        std::string local;
        CHECK(renderer.readback_raster_pixel(x, y, pixel, local),
              local.empty() ? "vt-rt: readback pixel" : local.c_str());
        return pixel;
    };
    // 90 deg vertical fov at z = -2 gives a 2.0 half-height; the 320x200
    // aspect widens that to a 3.2 half-width. World x -0.4 / +0.4 therefore
    // land on screen x 140 / 180, and y = 0 on screen y 100.
    const viewer::VkRasterPixel raster_a = pixel_at(140, 100);
    const viewer::VkRasterPixel raster_b = pixel_at(180, 100);
    CHECK(raster_a.material_index == kMaterialA &&
              raster_b.material_index == kMaterialB,
          "vt-rt: the raster probes land on the two charted halves");
    std::printf("vt-rt raster: A=(%.4f %.4f %.4f) B=(%.4f %.4f %.4f)\n",
                raster_a.albedo.x, raster_a.albedo.y, raster_a.albedo.z,
                raster_b.albedo.x, raster_b.albedo.y, raster_b.albedo.z);
    CHECK(close4(raster_a.albedo, {albedo_a.x, albedo_a.y, albedo_a.z, 1.0f},
                 2.0e-2f),
          "vt-rt: chart 0 rasterizes from its own composited page");
    CHECK(close4(raster_b.albedo, {albedo_b.x, albedo_b.y, albedo_b.z, 1.0f},
                 2.0e-2f),
          "vt-rt: chart 1 rasterizes from its own composited page");

    // --- (1) consistency: the same surface point, traced ---------------------
    // A near-degenerate cone (tiny spread) asks for the same finest mip the
    // 160x160 raster fragment did, so the two must agree texel-for-texel up
    // to filtering.
    const float kProbeSpread = 1.0e-4f;
    const matter::Float3 forward{0.0f, 0.0f, -1.0f};
    frame_with_probe(true, {-0.4f, 0.0f, 1.0f}, forward, 0.0f, kProbeSpread);
    const viewer::RtSurfaceHit hit_a = probe_hit;
    frame_with_probe(true, {0.4f, 0.0f, 1.0f}, forward, 0.0f, kProbeSpread);
    const viewer::RtSurfaceHit hit_b = probe_hit;
    std::printf("vt-rt traced: A slot=%u applied=%d albedo=(%.4f %.4f %.4f) "
                "mip=%.2f/%.2f cone=%.5f density=%.4f\n",
                hit_a.vt_slot, hit_a.vt_applied ? 1 : 0, hit_a.vt_albedo.x,
                hit_a.vt_albedo.y, hit_a.vt_albedo.z, hit_a.vt_desired_mip,
                hit_a.vt_mapped_mip, hit_a.cone_width, hit_a.uv_density);
    std::printf("vt-rt traced: B slot=%u applied=%d albedo=(%.4f %.4f %.4f) "
                "mip=%.2f/%.2f cone=%.5f density=%.4f\n",
                hit_b.vt_slot, hit_b.vt_applied ? 1 : 0, hit_b.vt_albedo.x,
                hit_b.vt_albedo.y, hit_b.vt_albedo.z, hit_b.vt_desired_mip,
                hit_b.vt_mapped_mip, hit_b.cone_width, hit_b.uv_density);
    CHECK(probe_invalid == 0, "vt-rt: no invalid RT part records");
    CHECK(hit_a.valid && hit_b.valid, "vt-rt: both probe rays hit the quad");
    CHECK(hit_a.material_index == kMaterialA &&
              hit_b.material_index == kMaterialB,
          "vt-rt: the probe rays hit the halves the raster probes did");
    CHECK(hit_a.vt_slot != 0 && hit_a.vt_slot == hit_b.vt_slot,
          "vt-rt: the hit BLAS carries the part rung's transported VT slot");
    CHECK(hit_a.vt_applied && hit_b.vt_applied,
          "vt-rt: the traced hits resolved a VT page");
    CHECK(hit_a.uv_density > 0.0f,
          "vt-rt: the hit triangle yields a positive atlas-UV density");
    // Never-fault contract: a page the cone asked for that is not resident
    // resolves DOWN the chain (ultimately to the pinned tail), never to a
    // finer mip that does not exist in the pool.
    CHECK(hit_a.vt_mapped_mip >= hit_a.vt_desired_mip &&
              hit_b.vt_mapped_mip >= hit_b.vt_desired_mip,
          "vt-rt: an unmapped page falls back to a coarser resident mip");
    // The Phase-5 exit criterion. 2e-2 is the same filtering/BC7 epsilon the
    // raster assertions above use.
    CHECK(std::fabs(hit_a.vt_albedo.x - raster_a.albedo.x) < 2.0e-2f &&
              std::fabs(hit_a.vt_albedo.y - raster_a.albedo.y) < 2.0e-2f &&
              std::fabs(hit_a.vt_albedo.z - raster_a.albedo.z) < 2.0e-2f,
          "vt-rt: chart 0 traced shading agrees with its G-buffer shading");
    CHECK(std::fabs(hit_b.vt_albedo.x - raster_b.albedo.x) < 2.0e-2f &&
              std::fabs(hit_b.vt_albedo.y - raster_b.albedo.y) < 2.0e-2f &&
              std::fabs(hit_b.vt_albedo.z - raster_b.albedo.z) < 2.0e-2f,
          "vt-rt: chart 1 traced shading agrees with its G-buffer shading");
    // Decisive: the two charts are different pages of the same variant, so
    // agreement above is addressing, not a constant.
    CHECK(std::fabs(hit_a.vt_albedo.x - hit_b.vt_albedo.x) > 0.3f ||
              std::fabs(hit_a.vt_albedo.z - hit_b.vt_albedo.z) > 0.3f,
          "vt-rt: the two charts trace to their own distinct pages");

    // --- (2) cone-mip monotonicity ------------------------------------------
    // Same surface point, same spread, four increasing distances. Footprint =
    // spread * t, so the requested mip must never decrease and must strictly
    // increase across the 16x distance sweep.
    const float kSweepSpread = 5.0e-4f;
    const float distances[4] = {1.0f, 5.0f, 13.0f, 29.0f};   // z of the origin
    float mips[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float cones[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; ++i) {
        frame_with_probe(true, {-0.4f, 0.0f, distances[i]}, forward, 0.0f,
                         kSweepSpread);
        CHECK(probe_hit.valid && probe_hit.vt_applied,
              "vt-rt: the cone sweep ray hit a VT page");
        mips[i] = probe_hit.vt_desired_mip;
        cones[i] = probe_hit.cone_width;
    }
    std::printf("vt-rt cone sweep: t=%.1f..%.1f mips=%.0f %.0f %.0f %.0f "
                "cones=%.5f %.5f %.5f %.5f\n",
                distances[0] - kQuadZ, distances[3] - kQuadZ, mips[0], mips[1],
                mips[2], mips[3], cones[0], cones[1], cones[2], cones[3]);
    for (int i = 1; i < 4; ++i) {
        CHECK(cones[i] > cones[i - 1],
              "vt-rt: the cone footprint grows with hit distance");
        CHECK(mips[i] >= mips[i - 1],
              "vt-rt: a farther hit never selects a finer mip");
    }
    CHECK(mips[3] > mips[0],
          "vt-rt: a 16x farther hit selects a strictly coarser mip");
    CHECK(mips[0] == 0.0f,
          "vt-rt: the nearest hit still resolves the finest mip");

    // Perf sanity. These are EMA-smoothed GPU zone timings for a 2-quad
    // fixture, so they are a "nothing pathological happened" gate, not a
    // flight-path benchmark: at this scene scale the RT zone is dominated by
    // fixed dispatch overhead and the VT sample is far below the noise floor.
    for (int i = 0; i < 4; ++i)
        frame_with_probe(false, {}, {}, 0.0f, 0.0f);
    if (renderer.gpu_timers_supported()) {
        std::printf("vt-rt gpu zones (ms): total=%.3f gbuffer=%.3f rt=%.3f "
                    "vt=%.3f\n",
                    renderer.gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneTotal),
                    renderer.gpu_zone_ms(
                        viewer::VkSceneRenderer::kGpuZoneGBuffer),
                    renderer.gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneRt),
                    renderer.gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneVt));
    }
}

void run_tileset_slot_load(matter::VulkanDevice& vulkan) {
    constexpr uint32_t width = 160;
    constexpr uint32_t height = 160;
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error),
          error.empty() ? "tileset: renderer init" : error.c_str());

    // Synthetic 4x4-tile atlas, tile_px = 8 (32x32). Constant channels so the
    // expected G-buffer values are exact regardless of Wang layer selection:
    //   albedo (40, 80, 120), normal flat (128, 128),
    //   ORM occlusion=255 / roughness=100 / metallic=20, height = 0.
    const int tile_px = 8;
    const int atlas_px = tile_px * 4;
    const size_t pixel_count = static_cast<size_t>(atlas_px) * atlas_px;
    std::vector<uint8_t> albedo(pixel_count * 3);
    std::vector<uint8_t> normal(pixel_count * 2);
    std::vector<uint8_t> orm(pixel_count * 3);
    std::vector<uint16_t> height_px(pixel_count, 0);
    for (size_t i = 0; i < pixel_count; ++i) {
        albedo[i * 3 + 0] = 40;
        albedo[i * 3 + 1] = 80;
        albedo[i * 3 + 2] = 120;
        normal[i * 2 + 0] = 128;
        normal[i * 2 + 1] = 128;
        orm[i * 3 + 0] = 255;
        orm[i * 3 + 1] = 100;
        orm[i * 3 + 2] = 20;
    }
    tileset::GTexHeader gtex_header{};
    gtex_header.tile_size_m = 2.0f;
    gtex_header.texels_per_meter = 4;
    gtex_header.height_min = 0.0f;
    gtex_header.height_max = 0.1f;
    gtex_header.content_hash = 0x7113537u;
    const char* temp_dir = std::getenv("TEMP");
    const std::string gtex_path =
        std::string(temp_dir ? temp_dir : ".") + "\\me3_smoke_tileset.gtex";
    CHECK(tileset::save_gtex(gtex_path, gtex_header, atlas_px, atlas_px,
                             albedo.data(), normal.data(), orm.data(),
                             height_px.data(), error),
          error.empty() ? "tileset: save synthetic .gtex" : error.c_str());

    // Fail-closed negatives: bad slot, missing file. Neither may poison the
    // renderer or leave descriptors half-written.
    // One past the last valid slot. Derived from tileset::kMaxTilesetSlots so
    // this stays a genuine out-of-range probe when the slot count changes
    // (it went 4 -> 8 with the BC-compressed slices); a hardcoded 4 would
    // silently turn into a real load and then assert the wrong thing.
    CHECK(!renderer.load_tileset_slot(tileset::kMaxTilesetSlots, gtex_path,
                                      error) && !error.empty(),
          "tileset: slot kMaxTilesetSlots out of range fails closed");
    CHECK(!renderer.load_tileset_slot(-1, gtex_path, error) && !error.empty(),
          "tileset: slot -1 out of range fails closed");
    CHECK(!renderer.load_tileset_slot(0, gtex_path + ".missing", error) &&
              !error.empty(),
          "tileset: missing .gtex path fails closed");

    CHECK(renderer.load_tileset_slot(0, gtex_path, error),
          error.empty() ? "tileset: load synthetic .gtex into slot 0"
                        : error.c_str());

    // Material 7 carries detail slot 0 (packed as slot + 1 = 1) with a flat
    // base color deliberately different from the atlas so a pass could not
    // come from the untextured path.
    std::vector<MaterialGpuRecord> materials(9);
    materials[7].base_roughness[0] = 0.9f;
    materials[7].base_roughness[1] = 0.1f;
    materials[7].base_roughness[2] = 0.1f;
    materials[7].base_roughness[3] = 0.9f;
    materials[7].metal_opacity_spec_coat[0] = 0.0f;
    materials[7].metal_opacity_spec_coat[1] = 1.0f;
    materials[7].scattering_shape[3] = 1.0f;
    materials[7].flags_misc[1] = 1u;  // detailSlot 0 + 1, no macro slot
    CHECK(renderer.update_materials(materials, 1, 1, error),
          error.empty() ? "tileset: stage materials" : error.c_str());

    viewer::VkScenePart triangle = known_raster_triangle(970);
    if (triangle.indices.empty()) triangle.indices = {0, 1, 2};
    CHECK(renderer.ensure_part(triangle, error) >= 0,
          error.empty() ? "tileset: ensure ground triangle" : error.c_str());
    const matter::Mat4f identity = identity_matrix();
    CHECK(renderer.update_instances({{970, identity, 42}}, error),
          error.empty() ? "tileset: upload instance" : error.c_str());

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices frame{};
    CHECK(viewer::build_frame_matrices(camera, width, height, frame, error),
          error.empty() ? "tileset: build frame matrices" : error.c_str());
    CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error) &&
              renderer.render_gbuffer_and_composite(width, height, error),
          error.empty() ? "tileset: render textured frame" : error.c_str());

    viewer::VkRasterPixel center{};
    CHECK(renderer.readback_raster_pixel(width / 2, height / 2, center, error),
          error.empty() ? "tileset: read center pixel" : error.c_str());
    CHECK(close4(center.albedo,
                 {40.0f / 255.0f, 80.0f / 255.0f, 120.0f / 255.0f, 1.0f},
                 8e-3f),
          "tileset: G-buffer albedo is the Wang-sampled atlas color");
    CHECK(close4(center.orm,
                 {100.0f / 255.0f, 20.0f / 255.0f, 0.5f, 1.0f}, 8e-3f),
          "tileset: G-buffer ORM is texture roughness/metallic with baked AO");
    CHECK(center.normal.y > 0.99f,
          "tileset: flat tangent normal stays aligned with the geometric +Y");
    CHECK(center.material_index == 7u,
          "tileset: material id channel is untouched by the tileset branch");

    // Replacement path: loading the same slot again must swap cleanly.
    CHECK(renderer.load_tileset_slot(0, gtex_path, error),
          error.empty() ? "tileset: reload slot 0 (replacement path)"
                        : error.c_str());
    CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error) &&
              renderer.render_gbuffer_and_composite(width, height, error),
          error.empty() ? "tileset: render after reload" : error.c_str());
    viewer::VkRasterPixel reloaded{};
    CHECK(renderer.readback_raster_pixel(width / 2, height / 2, reloaded,
                                         error) &&
              close4(reloaded.albedo,
                     {40.0f / 255.0f, 80.0f / 255.0f, 120.0f / 255.0f, 1.0f},
                     8e-3f),
          "tileset: reloaded slot still samples the atlas");

    // Unload: material still requests slot 0, so the shader samples the
    // zero-filled dummies (fail-closed black ground, never garbage/crash).
    renderer.unload_tileset_slot(0);
    CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error) &&
              renderer.render_gbuffer_and_composite(width, height, error),
          error.empty() ? "tileset: render after unload" : error.c_str());
    viewer::VkRasterPixel unloaded{};
    CHECK(renderer.readback_raster_pixel(width / 2, height / 2, unloaded,
                                         error) &&
              close4(unloaded.albedo, {0.0f, 0.0f, 0.0f, 1.0f}, 8e-3f),
          "tileset: unloaded slot falls back to zero-filled dummy layers");

    vulkan.wait_idle();
    std::remove(gtex_path.c_str());
}

void run_native_multilod_rt_mapping(matter::VulkanDevice& vulkan) {
    if (!vulkan.ray_tracing_available()) return;
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    std::vector<MaterialGpuRecord> materials(2);
    for (auto& material : materials) {
        material.metal_opacity_spec_coat[1] = 1.0f;
        material.scattering_shape[3] = 1.0f;
    }
    materials[0].base_roughness[0] = 0.8f;
    materials[1].base_roughness[1] = 0.8f;
    viewer::VkScenePart part{};
    part.part_hash = 0x4d554c54494c4f44ull;
    part.clusters = {
        {{-2.0f, -1.0f, -3.0f}, {0.0f, 1.0f, -1.0f}, 20.0f,
         {{0, 3, 1.0f}, {3, 3, 0.0f}}},
        {{0.0f, -1.0f, -3.0f}, {2.0f, 1.0f, -1.0f}, 1.0f,
         {{6, 3, 1.0f}, {9, 3, 0.0f}}},
    };
    const matter::Float3 normal{0.0f, 0.0f, 1.0f};
    const matter::Float4 tint{1.0f, 1.0f, 1.0f, 0.0f};
    const matter::Float4 surface{0.5f, 0.0f, 1.0f, 1.0f};
    const auto vertex = [&](float x, float y, float z, uint32_t material) {
        return viewer::VkRasterVertex{{x, y, z}, normal, tint, surface,
                                      material, {}};
    };
    part.vertices = {
        vertex(-1.8f, -0.8f, -2.0f, 0), vertex(-0.2f, -0.8f, -2.0f, 0),
        vertex(-1.0f, 0.8f, -2.0f, 0),
        vertex(-1.8f, -0.8f, -4.0f, 1), vertex(-0.2f, -0.8f, -4.0f, 1),
        vertex(-1.0f, 0.8f, -4.0f, 1),
        vertex(0.2f, -0.8f, -2.0f, 0), vertex(1.8f, -0.8f, -2.0f, 0),
        vertex(1.0f, 0.8f, -2.0f, 0),
        vertex(0.2f, -0.8f, -4.0f, 1), vertex(1.8f, -0.8f, -4.0f, 1),
        vertex(1.0f, 0.8f, -4.0f, 1),
    };
    // The cluster LODs above address indices, not vertices ({0,3} {3,3} {6,3}
    // {9,3}); identity indices give each LOD its own triangle so the per-LOD
    // BLAS records below are distinct and the LOD1 range really starts at 9.
    part.indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    CHECK(renderer.update_materials(materials, 1, 1, error) &&
              renderer.ensure_part(part, error) >= 0 &&
              renderer.update_instances({{part.part_hash, identity_matrix()}},
                                        error),
          error.empty() ? "prepare native multi-LOD RT mapping fixture"
                        : error.c_str());
    matter::VulkanRayTracingSettings rt{};
    rt.enabled = true;
    renderer.set_ray_tracing_settings(rt);
    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 10.0f};
    camera.target = {0.0f, 0.0f, -2.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.0f;
    camera.near_plane = 0.1f;
    camera.far_plane = 30.0f;
    viewer::FrameMatrices matrices{};
    CHECK(viewer::build_frame_matrices(camera, 320, 200, matrices, error),
          error.empty() ? "build multi-LOD RT matrices" : error.c_str());
    auto render = [&](uint64_t attempt,
                      std::vector<viewer::RtGeometryDebugRecord>&
                          records,
                      uint32_t& builds) {
        viewer::TemporalFrame temporal{};
        temporal.current_unjittered = matrices;
        temporal.previous_unjittered = matrices;
        temporal.current_jittered = matrices;
        temporal.previous_jittered = matrices;
        temporal.internal_extent = {320, 200};
        temporal.output_extent = {320, 200};
        temporal.attempt_token = attempt;
        renderer.set_temporal_frame(temporal);
        matter::VulkanFrame frame{};
        const bool began = vulkan.begin_frame(frame, error);
        const bool recorded = began &&
            renderer.prepare_frame(frame, matrices, camera.position, 1.0f,
                                   error) &&
            renderer.record_cull_and_render(frame, matrices, camera.position,
                                            1.0f, error) &&
            renderer.record_composite_to_swapchain(frame, error);
        records = renderer.test_last_rt_geometry_records();
        builds = renderer.test_last_rt_blas_build_count();
        const bool submitted = recorded && vulkan.end_frame(frame, error);
        renderer.finish_ray_tracing_frame(frame.serial, submitted);
        vulkan.wait_idle();
        return submitted;
    };
    std::vector<viewer::RtGeometryDebugRecord> first;
    uint32_t first_builds = 0;
    CHECK(render(1, first, first_builds),
          error.empty() ? "record first multi-LOD RT frame" : error.c_str());
    const VkDeviceAddress base =
        renderer.test_rt_geometry_address(part.part_hash);
    CHECK(first.size() == 2 && first_builds == 2 &&
              first[0].cluster_index == 0 && first[0].lod_index == 0 &&
              first[0].custom_index == 0 && first[0].first_index == 0 &&
              first[0].vertex_address == base && first[0].built_this_frame &&
              first[1].cluster_index == 1 && first[1].lod_index == 1 &&
              first[1].custom_index == 1 && first[1].first_index == 9 &&
              // Task 5: vertex_address is always the part base (no per-LOD
              // vertex offset); index_address carries the per-LOD variation.
              first[1].vertex_address == base &&
              first[1].built_this_frame &&
              first[0].blas_address != first[1].blas_address,
          "native TLAS uses dense custom indices and indexed BLAS; vertex_address always part base");
    std::vector<viewer::RtGeometryDebugRecord> reused;
    uint32_t reuse_builds = UINT32_MAX;
    CHECK(render(2, reused, reuse_builds) && reused.size() == 2 &&
              reuse_builds == 0 &&
              reused[0].blas_address == first[0].blas_address &&
              reused[1].blas_address == first[1].blas_address,
          "selected per-LOD BLAS records are reused without rebuilding");
    materials[1].metal_opacity_spec_coat[1] = 0.25f;
    CHECK(renderer.update_materials(materials, 2, 2, error),
          error.empty() ? "change only selected LOD1 opacity class"
                        : error.c_str());
    std::vector<viewer::RtGeometryDebugRecord> replaced;
    uint32_t replacement_builds = UINT32_MAX;
    CHECK(render(3, replaced, replacement_builds) && replaced.size() == 2 &&
              replacement_builds == 1 &&
              replaced[0].blas_address == first[0].blas_address &&
              replaced[0].opaque &&
              replaced[1].blas_address != first[1].blas_address &&
              !replaced[1].opaque,
          "per-LOD opacity change replaces only the affected selected BLAS");
}

// ---------------------------------------------------------------------------
// Regression test: rt_lod.first_index stays part-local after release_part
// compacts index_staging_.  Registers two parts with non-trivial index layouts,
// releases the first, and asserts that the survivor's rt_lod.first_index equals
// the original part-local value (not corrupted by the compaction rebase of
// part.index_start).
// ---------------------------------------------------------------------------
void run_rt_lod_compaction_invariant(matter::VulkanDevice& vulkan) {
    if (!vulkan.ray_tracing_available()) return;
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error), error.empty() ? "init" : error.c_str());

    const matter::Float3 norm{0.0f, 0.0f, 1.0f};
    const matter::Float4 tint{1.0f, 1.0f, 1.0f, 0.0f};
    const matter::Float4 surf{0.5f, 0.0f, 1.0f, 1.0f};
    auto v = [&](float x, float y, float z) {
        return viewer::VkRasterVertex{{x, y, z}, norm, tint, surf, 0u, {}};
    };

    // Part A: 2 vertices-per-cluster-lod layout, 6 indices (first_index=0/3).
    viewer::VkScenePart partA{};
    partA.part_hash = 0xAAAA0001ULL;
    partA.clusters = {{{-1.f, -1.f, -3.f}, {1.f, 1.f, -1.f}, 2.0f, {{0u, 3u, 1.0f}, {3u, 3u, 0.0f}}}};
    partA.vertices = {v(-1.f,0.f,-2.f), v(1.f,0.f,-2.f), v(0.f,1.f,-2.f),
                      v(-1.f,0.f,-3.f), v(1.f,0.f,-3.f), v(0.f,1.f,-3.f)};
    partA.indices  = {0u,1u,2u, 3u,4u,5u};

    // Part B: single cluster, single LOD, first_index=0 (part-local).
    viewer::VkScenePart partB{};
    partB.part_hash = 0xBBBB0002ULL;
    partB.clusters = {{{-2.f, -2.f, -5.f}, {2.f, 2.f, -3.f}, 3.0f, {{0u, 3u, 1.0f}}}};
    partB.vertices = {v(-2.f,0.f,-4.f), v(2.f,0.f,-4.f), v(0.f,2.f,-4.f)};
    partB.indices  = {0u,1u,2u};

    // Capture B's expected part-local first_index BEFORE registration.
    // cluster[0].lods[0].first_index in VkScenePart = 0.
    const uint32_t expected_b_lod0_first_index = 0u;

    std::vector<MaterialGpuRecord> materials(1);
    materials[0].metal_opacity_spec_coat[1] = 1.0f;
    materials[0].scattering_shape[3] = 1.0f;
    CHECK(renderer.update_materials(materials, 1, 1, error) &&
              renderer.ensure_part(partA, error) >= 0 &&
              renderer.ensure_part(partB, error) >= 0,
          error.empty() ? "register two indexed parts" : error.c_str());

    // Before release: verify B's rt_lod.first_index is already part-local.
    const uint32_t before_release =
        renderer.test_rt_lod_first_index(partB.part_hash, 0);
    CHECK(before_release == expected_b_lod0_first_index,
          "partB rt_lod[0].first_index is part-local before release_part(A)");

    // Release A — this compacts index_staging_ and rebases part.index_start.
    renderer.release_part(partA.part_hash);

    // After compaction: B's rt_lod.first_index must still equal the part-local
    // value (0), NOT the old global frame corrupted by the now-stale index_start.
    const uint32_t after_release =
        renderer.test_rt_lod_first_index(partB.part_hash, 0);
    CHECK(after_release == expected_b_lod0_first_index,
          "partB rt_lod[0].first_index unchanged (part-local) after release_part(A) compaction");
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Helper: build a horizontal triangle part used by the two-triangle RT fixture
// ---------------------------------------------------------------------------
static viewer::VkScenePart rt_horizontal_part(uint64_t hash, float y,
                                               float radius,
                                               matter::Float3 normal,
                                               uint32_t material_index,
                                               float baked_ao) {
    const float front_z = radius > 5.0f ? 10.0f : -1.0f;
    const float back_z  = radius > 5.0f ? -30.0f : -3.5f;
    viewer::VkScenePart part = fixed_part(
        hash, {-radius, y - 0.01f, back_z}, {radius, y + 0.01f, front_z}, 0);
    const matter::Float4 albedo{1.0f, 1.0f, 1.0f, 0.0f};
    const matter::Float4 orm{0.5f, 0.0f, baked_ao, 1.0f};
    part.vertices = {{{-radius, y, front_z}, normal, albedo, orm,
                      material_index, {}},
                     {{radius, y, front_z}, normal, albedo, orm,
                      material_index, {}},
                     {{0.0f, y, back_z}, normal, albedo, orm,
                      material_index, {}}};
    // fixed_part() describes the LOD as an index range, so the part needs the
    // indices to back it: the BLAS is built from them and the raster lane draws
    // indexed. Without these the geometry is empty and every ray misses.
    part.indices = {0, 1, 2};
    return part;
}

// ---------------------------------------------------------------------------
// Context struct holding shared mutable state across run_native_ray_tracing_path
// sub-scenarios.  All members are references/values that live in the driver
// function's stack frame and are passed by pointer into each scenario function.
// ---------------------------------------------------------------------------
struct RtPathContext {
    matter::VulkanDevice&                                  vulkan;
    const matter::VulkanRayTracingProperties&              properties;
    std::string&                                           error;
    viewer::VkSceneRenderer&                               renderer;
    viewer::VkSceneLighting&                               lighting;
    matter::VulkanRayTracingSettings&                      enabled;
    matter::VulkanGiSettings&                              gi;
    viewer::FrameMatrices&                                 matrices;
    matter::CameraDesc&                                    camera;
    viewer::TemporalFrame&                                 gi_temporal;
    std::vector<MaterialGpuRecord>&                        gi_materials;
    // Populated by rt_scenario_first_frame_and_blas_lifecycle, consumed later.
    uint32_t& retry_x;
    uint32_t& retry_y;
    // Populated by rt_scenario_secondary_sun_visibility, consumed by later scenarios.
    bool&     receiver_seen;
    float&    minimum_visibility;
    float&    maximum_visibility;
    float&    receiver_min_visibility;
    float&    receiver_max_visibility;
};

// ---------------------------------------------------------------------------
// Scenario: secondary surface-query rays and SBT layout
// ---------------------------------------------------------------------------
static void rt_scenario_surface_query(
        matter::VulkanDevice& vulkan,
        const matter::VulkanRayTracingProperties& properties,
        std::string& error) {
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
        // Indexed BLAS: the pinned second part needs its own index list, or the
        // secondary ray has no geometry to hit.
        second.indices = {0, 1, 2};
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

// ---------------------------------------------------------------------------
// Scenario: BLAS input geometry stays pinned while the vertex buffer grows
// ---------------------------------------------------------------------------
static void rt_scenario_blas_pinning(
        matter::VulkanDevice& vulkan,
        std::string& error) {
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
}

// ---------------------------------------------------------------------------
// Scenario: per-material visibility classification and reclassification
// ---------------------------------------------------------------------------
static void rt_scenario_visibility_classification(
        matter::VulkanDevice& vulkan,
        std::string& error) {
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
}

// ---------------------------------------------------------------------------
// Scenario: two-triangle shadow contract (disabled vs enabled RT)
// Setup helper shared by subsequent scenarios.
// ---------------------------------------------------------------------------
static void rt_scenario_shadow_contract(RtPathContext& ctx) {
    viewer::VkSceneRenderer& renderer = ctx.renderer;
    viewer::VkSceneLighting& lighting = ctx.lighting;
    matter::VulkanRayTracingSettings& enabled        = ctx.enabled;
    viewer::FrameMatrices& matrices                  = ctx.matrices;
    matter::CameraDesc& camera                       = ctx.camera;
    viewer::TemporalFrame& gi_temporal               = ctx.gi_temporal;
    matter::VulkanGiSettings& gi                     = ctx.gi;
    std::vector<MaterialGpuRecord>& gi_materials     = ctx.gi_materials;
    std::string& error                               = ctx.error;
    gi_materials.assign(3, MaterialGpuRecord{});
    // Material 2 is a pure occluder for the secondary-sun blocker: opaque (so
    // it lands in the 0x01 TLAS layer the visibility rays cull against) but
    // black, so it reflects nothing. It has to be black because
    // rt_surface_common.glsl shades two-sided -- `if (!front_face)
    // surface.normal = -surface.normal` -- so a down-facing plane still
    // presents an up-facing, sun-lit surface to a bounce ray arriving from
    // below. Reusing the red floor material here made the blocker contribute
    // almost exactly the sun-lit bounce the floor had lost, which hid the
    // occlusion the check is about.
    gi_materials[2].metal_opacity_spec_coat[1] = 1.0f;
    gi_materials[2].scattering_shape[3] = 1.0f;
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
              renderer.ensure_part(rt_horizontal_part(920, -1.0f, 20.0f,
                                              {0.0f, 1.0f, 0.0f}, 0, 1.0f),
                                   error) >= 0 &&
              renderer.ensure_part(rt_horizontal_part(921, 0.0f, 0.55f,
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
    enabled = {};
    enabled.enabled = true;
    enabled.max_distance = 100.0f;
    enabled.bias = 0.001f;
    enabled.samples = 4;
    enabled.debug_view = true;
    renderer.set_ray_tracing_settings(enabled);
    gi = {};
    gi.samples_per_pixel = 16;
    gi.trace_scale = 0.5f;
    renderer.set_gi_settings(gi);
    CHECK(renderer.test_gi_samples_per_pixel() == 1u,
          "RT-active GI clamps authored sample count to one continuation");
    lighting = {};
    lighting.sun_direction = {0.0f, -1.0f, 0.0f};
    renderer.set_lighting(lighting);
    const float open = renderer.test_shadow_visibility_for_ray(false);
    const float blocked = renderer.test_shadow_visibility_for_ray(true);
    CHECK(open == 1.0f && std::isfinite(blocked) && blocked < 1.0f,
          "two-triangle shadow contract is deterministic for open and blocked rays");

    camera = {};
    camera.position = {0.0f, 1.5f, 1.0f};
    camera.target = {0.0f, -0.75f, -2.2f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    matrices = {};
    CHECK(viewer::build_frame_matrices(camera, 320, 200, matrices, error),
          error.empty() ? "build native RT frame matrices" : error.c_str());
    gi_temporal = {};
    gi_temporal.current_unjittered = matrices;
    gi_temporal.previous_unjittered = matrices;
    gi_temporal.current_jittered = matrices;
    gi_temporal.previous_jittered = matrices;
    gi_temporal.internal_extent = {320, 200};
    gi_temporal.output_extent = {320, 200};
    gi_temporal.attempt_token = 101;
    gi_temporal.presented_frame_index = 7;
    renderer.set_temporal_frame(gi_temporal);
}

// ---------------------------------------------------------------------------
// Scenario: first native RT frame, BLAS candidate lifecycle, and GI determinism
// ---------------------------------------------------------------------------
static void rt_scenario_first_frame_and_blas_lifecycle(
        RtPathContext& ctx, matter::VulkanFrame& frame) {
    matter::VulkanDevice& vulkan      = ctx.vulkan;
    viewer::VkSceneRenderer& renderer = ctx.renderer;
    const matter::VulkanRayTracingProperties& properties = ctx.properties;
    viewer::FrameMatrices& matrices   = ctx.matrices;
    matter::CameraDesc& camera        = ctx.camera;
    viewer::TemporalFrame& gi_temporal = ctx.gi_temporal;
    std::string& error                = ctx.error;
    uint32_t& retry_x                 = ctx.retry_x;
    uint32_t& retry_y                 = ctx.retry_y;

    const uint64_t immediate_before = matter::immediate_submit_count();
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
        retry_x = 0;
        retry_y = 0;
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

        viewer::GiTemporalGpuFixture specular_temporal{};
        specular_temporal.signal_mode = 1u;
        specular_temporal.previous_history_length = 100u;
        specular_temporal.raw_aux = {2.0f, 0.02f, 0.0f};
        specular_temporal.previous_aux = specular_temporal.raw_aux;
        specular_temporal.raw = {0.05f, 0.6f, 0.1f, 1.0f};
        specular_temporal.previous_radiance = specular_temporal.raw;
        specular_temporal.previous_moments = {0.45f, 0.21f, 0.0f};
        viewer::GiTemporalGpuResult specular_temporal_result{};
        CHECK(renderer.test_dispatch_gi_temporal_fixture(
                  specular_temporal, specular_temporal_result, error) &&
                  specular_temporal_result.history_length == 4u &&
                  specular_temporal_result.rejection_bits == 0u &&
                  close4(specular_temporal_result.radiance,
                         specular_temporal.raw, 0.003f),
              error.empty()
                  ? "low-roughness specular uses a four-frame history without diffuse contamination"
                  : error.c_str());
        auto rough_specular_temporal = specular_temporal;
        rough_specular_temporal.raw_aux.y = 1.0f;
        rough_specular_temporal.previous_aux.y = 1.0f;
        CHECK(renderer.test_dispatch_gi_temporal_fixture(
                  rough_specular_temporal, specular_temporal_result, error) &&
                  specular_temporal_result.history_length == 16u,
              error.empty()
                  ? "rough specular extends history to sixteen frames"
                  : error.c_str());
        auto disoccluded_specular = specular_temporal;
        disoccluded_specular.raw_aux.x = 8.0f;
        CHECK(renderer.test_dispatch_gi_temporal_fixture(
                  disoccluded_specular, specular_temporal_result, error) &&
                  specular_temporal_result.history_length == 1u &&
                  specular_temporal_result.rejection_bits ==
                      VULKAN_GI_REJECT_HIT_DISTANCE,
              error.empty()
                  ? "GPU temporal shader agrees with CPU hit-distance rejection bit"
                  : error.c_str());
}

// ---------------------------------------------------------------------------
// Scenario: real RT/TLAS froxel bundle replacement and failed allocation
// ---------------------------------------------------------------------------
static void rt_scenario_froxel_resize(
        matter::VulkanDevice& vulkan, viewer::VkSceneRenderer& renderer,
        viewer::FrameMatrices& matrices, matter::CameraDesc& camera,
        std::string& error) {

    // This must run after the native first-frame scenario has committed a
    // traceable TLAS. The production record path then executes density,
    // scatter, integration, swap, descriptor rewrite, and composite sample.
    const matter::FroxelGridDimensions expected[] = {
        {160, 90, 128}, {80, 45, 64}, {320, 180, 256},
        {240, 135, 192}, {160, 90, 128}};
    const matter::FroxelXyScale xy[] = {
        matter::FroxelXyScale::X1_0, matter::FroxelXyScale::X0_5,
        matter::FroxelXyScale::X2_0, matter::FroxelXyScale::X1_5,
        matter::FroxelXyScale::X1_0};
    const matter::FroxelDepthSlices depth[] = {
        matter::FroxelDepthSlices::D128, matter::FroxelDepthSlices::D64,
        matter::FroxelDepthSlices::D256, matter::FroxelDepthSlices::D192,
        matter::FroxelDepthSlices::D128};
    matter::FogSettings fog{};
    matter::VulkanVolumetricsSettings settings{};
    settings.enabled = true;
    std::ifstream renderer_source("../MatterEngine3/src/render/vk_scene_renderer.cpp",
                                  std::ios::binary);
    const std::string renderer_implementation(
        (std::istreambuf_iterator<char>(renderer_source)),
        std::istreambuf_iterator<char>());
    const size_t froxel_prepare = renderer_implementation.find(
        "volumetrics_->prepare_froxel_bundle(frame.frame_slot, error)");
    const size_t composite_update = renderer_implementation.find(
        "update_composite_descriptor(selected)", froxel_prepare);
    CHECK(froxel_prepare != std::string::npos && composite_update != std::string::npos &&
              froxel_prepare < composite_update,
          "froxel swap precedes same-frame composite descriptor selection");
    const size_t composite_function = renderer_implementation.find(
        "void VkSceneRenderer::update_composite_descriptor");
    const size_t composite_end = renderer_implementation.find(
        "bool VkSceneRenderer::update_environment_descriptor", composite_function);
    const std::string composite_selection = composite_function == std::string::npos ||
        composite_end == std::string::npos ? std::string{} :
        renderer_implementation.substr(composite_function,
                                       composite_end - composite_function);
    CHECK(composite_selection.find("ray_tracing_settings_.enabled") !=
              std::string::npos &&
              composite_selection.find("vulkan_->ray_tracing_available()") !=
              std::string::npos &&
              composite_selection.find("!rt_instances_.empty()") !=
              std::string::npos &&
              composite_selection.find("rt_effective_observed()") ==
              std::string::npos,
          "composite selects live froxels from pre-record RT eligibility, not the reset per-frame observation");
    std::ifstream editor_props_source("src/editor_props.cpp",
                                      std::ios::binary);
    const std::string editor_props_implementation(
        (std::istreambuf_iterator<char>(editor_props_source)),
        std::istreambuf_iterator<char>());
    CHECK(editor_props_implementation.find(
              "\"Cloud shadow transmittance\"") != std::string::npos &&
              editor_props_implementation.find(
                  ".enums(kVolDebugLabels, 6)") != std::string::npos,
          "cloud-shadow debug label is reachable as the sixth session enum value");
    uint64_t previous_generation = renderer.volumetrics_resource_generation();
    VkImageView previous_view = renderer.volumetrics_integrated_view();
    bool seen_frame_slots[2] = {};
    for (size_t index = 0; index < std::size(expected); ++index) {
        settings.froxel_xy_scale = xy[index];
        settings.froxel_depth_slices = depth[index];
        renderer.set_volumetrics_settings(settings, fog);
        matter::VulkanFrame frame{};
        const bool prepared = vulkan.begin_frame(frame, error) &&
            renderer.prepare_frame(frame, matrices, camera.position, 1.0f,
                                   error);
        if (prepared && frame.frame_slot < 2) seen_frame_slots[frame.frame_slot] = true;
        const bool rendered = prepared &&
            renderer.record_cull_and_render(frame, matrices, camera.position,
                                            1.0f, error) &&
            renderer.record_composite_to_swapchain(frame, error) &&
            vulkan.end_frame(frame, error);
        renderer.finish_ray_tracing_frame(frame.serial, rendered);
        CHECK(rendered, error.empty() ? "record RT froxel resize frame"
                                      : error.c_str());
        CHECK(index == 0 || !renderer.volumetrics_last_scatter_history_was_valid_for_test(),
              "froxel replacement invalidates temporal history before scatter");
        const auto active = renderer.volumetrics_dimensions();
        const auto dispatch = renderer.volumetrics_last_dispatch_grid();
        const VkImageView active_view = renderer.volumetrics_integrated_view();
        std::printf("froxel RT trace: step=%zu requested=%ux%ux%u active=%ux%ux%u gen=%llu validation=%u\n",
                    index, expected[index].width, expected[index].height,
                    expected[index].depth, active.width, active.height,
                    active.depth,
                    static_cast<unsigned long long>(
                        renderer.volumetrics_resource_generation()),
                    vulkan.validation_error_count());
        CHECK(active.width == expected[index].width &&
                  active.height == expected[index].height &&
                  active.depth == expected[index].depth &&
                  renderer.volumetrics_effective_xy_scale() == xy[index] &&
                  renderer.volumetrics_effective_depth_slices() == depth[index] &&
                  dispatch.density_x == (active.width + 3) / 4 &&
                  dispatch.density_y == (active.height + 3) / 4 &&
                  dispatch.integrate_x == (active.width + 7) / 8 &&
                  dispatch.integrate_y == (active.height + 7) / 8 &&
                  active_view != VK_NULL_HANDLE &&
                  (index == 0 || active_view != previous_view) &&
                  renderer.volumetrics_resource_generation() >=
                      previous_generation + (index == 0 ? 0u : 1u),
              "RT froxel resize swaps descriptor view, effective enums, and ceil-covered dispatch grid");
        previous_generation = renderer.volumetrics_resource_generation();
        previous_view = active_view;
    }
    CHECK(seen_frame_slots[0] && seen_frame_slots[1],
          "froxel resize recycles both in-flight frame slots without per-resize idle waits");
    vulkan.wait_idle();
    viewer::VkRasterPixel pixel{};
    CHECK(renderer.readback_raster_pixel(4, 4, pixel, error) &&
              std::isfinite(pixel.hdr.x) && std::isfinite(pixel.hdr.y) &&
              std::isfinite(pixel.hdr.z),
          error.empty() ? "RT resized froxel composite remains finite"
                        : error.c_str());

    const auto before_failure = renderer.volumetrics_dimensions();
    const uint64_t generation_before_failure =
        renderer.volumetrics_resource_generation();
    const VkImageView view_before_failure = renderer.volumetrics_integrated_view();
    renderer.set_fail_next_froxel_bundle_descriptor_allocation_for_test(true);
    settings.froxel_xy_scale = matter::FroxelXyScale::X2_0;
    settings.froxel_depth_slices = matter::FroxelDepthSlices::D256;
    renderer.set_volumetrics_settings(settings, fog);
    matter::VulkanFrame failed{};
    const bool rendered = vulkan.begin_frame(failed, error) &&
        renderer.prepare_frame(failed, matrices, camera.position, 1.0f,
                               error) &&
        renderer.record_cull_and_render(failed, matrices, camera.position,
                                        1.0f, error) &&
        renderer.record_composite_to_swapchain(failed, error) &&
        vulkan.end_frame(failed, error);
    renderer.finish_ray_tracing_frame(failed.serial, rendered);
    CHECK(rendered, error.empty() ? "record RT injected froxel descriptor allocation failure"
                                  : error.c_str());
    vulkan.wait_idle();
    const auto after_failure = renderer.volumetrics_dimensions();
    CHECK(renderer.volumetrics_allocation_rejected() &&
              after_failure.width == before_failure.width &&
              after_failure.height == before_failure.height &&
              after_failure.depth == before_failure.depth &&
              renderer.volumetrics_integrated_view() == view_before_failure &&
              renderer.volumetrics_resource_generation() ==
                  generation_before_failure,
          "RT injected descriptor allocation failure retains live view, dimensions, and generation");
}

struct Task9CloudPoint { float x, y, z; };

static uint32_t task9_cloud_hash3i(int32_t ix, int32_t iy, int32_t iz,
                                   uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(ix) * 374761393u +
                 static_cast<uint32_t>(iy) * 3266489917u +
                 static_cast<uint32_t>(iz) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float task9_cloud_rand01(int32_t ix, int32_t iy, int32_t iz,
                                uint32_t seed) {
    return static_cast<float>(task9_cloud_hash3i(ix, iy, iz, seed) & 0xffffffu) /
           16777216.0f;
}

static float task9_cloud_smooth5(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float task9_cloud_value_noise3(float x, float y, float z,
                                      uint32_t seed) {
    const float fx0 = std::floor(x), fy0 = std::floor(y), fz0 = std::floor(z);
    const int32_t ix = static_cast<int32_t>(fx0);
    const int32_t iy = static_cast<int32_t>(fy0);
    const int32_t iz = static_cast<int32_t>(fz0);
    const float fx = x - fx0, fy = y - fy0, fz = z - fz0;
    const float c000 = task9_cloud_rand01(ix,     iy,     iz,     seed);
    const float c100 = task9_cloud_rand01(ix + 1, iy,     iz,     seed);
    const float c010 = task9_cloud_rand01(ix,     iy + 1, iz,     seed);
    const float c110 = task9_cloud_rand01(ix + 1, iy + 1, iz,     seed);
    const float c001 = task9_cloud_rand01(ix,     iy,     iz + 1, seed);
    const float c101 = task9_cloud_rand01(ix + 1, iy,     iz + 1, seed);
    const float c011 = task9_cloud_rand01(ix,     iy + 1, iz + 1, seed);
    const float c111 = task9_cloud_rand01(ix + 1, iy + 1, iz + 1, seed);
    const float u = task9_cloud_smooth5(fx), v = task9_cloud_smooth5(fy);
    const float w = task9_cloud_smooth5(fz);
    const float x00 = c000 + (c100 - c000) * u;
    const float x10 = c010 + (c110 - c010) * u;
    const float x01 = c001 + (c101 - c001) * u;
    const float x11 = c011 + (c111 - c011) * u;
    const float y0 = x00 + (x10 - x00) * v;
    const float y1 = x01 + (x11 - x01) * v;
    return y0 + (y1 - y0) * w;
}

static float task9_cloud_fbm3(Task9CloudPoint p, uint32_t seed, int octaves,
                              float gain, float lacunarity) {
    float amplitude = 1.0f, sum = 0.0f, normalization = 0.0f, frequency = 1.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        float noise = task9_cloud_value_noise3(
            p.x * frequency, p.y * frequency, p.z * frequency,
            seed + static_cast<uint32_t>(octave) * 131u);
        noise = noise * 2.0f - 1.0f;
        sum += noise * amplitude;
        normalization += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return sum / normalization;
}

static float task9_cloud_smoothstep(float edge0, float edge1, float value) {
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float task9_cloud_density_cpu(const matter::GpuCloudLayer& layer,
                                     Task9CloudPoint world_pos,
                                     float time_seconds) {
    const float lo = layer.min_height, hi = layer.max_height;
    if (!(hi > lo) || world_pos.y <= lo || world_pos.y >= hi) return 0.0f;
    const float thickness = hi - lo;
    const float f_lo = std::clamp(layer.falloff_min, 0.0f, thickness);
    const float f_hi = std::clamp(layer.falloff_max, 0.0f, thickness);
    const float rise = f_lo > 0.0f
        ? task9_cloud_smoothstep(lo, lo + f_lo, world_pos.y) : 1.0f;
    const float fall = f_hi > 0.0f
        ? 1.0f - task9_cloud_smoothstep(hi - f_hi, hi, world_pos.y) : 1.0f;
    const float profile = std::min(rise, fall);
    if (profile <= 0.0f || layer.coverage <= 0.0f) return 0.0f;
    const Task9CloudPoint p{
        (world_pos.x + layer.wind[0] * time_seconds) * layer.noise_scale,
        (world_pos.y + layer.wind[1] * time_seconds) * layer.noise_scale,
        (world_pos.z + layer.wind[2] * time_seconds) * layer.noise_scale};
    const float normalized = task9_cloud_fbm3(
        p, static_cast<uint32_t>(layer.seed), static_cast<int>(layer.octaves),
        layer.gain, layer.lacunarity) * 0.5f + 0.5f;
    const float threshold = 1.0f - layer.coverage;
    const float shape = task9_cloud_smoothstep(
        threshold - matter::kCloudCoverageEdge,
        threshold + matter::kCloudCoverageEdge, normalized);
    return profile * layer.max_density * shape;
}

static Task9CloudPoint task9_froxel_world_position(
    const viewer::FrameMatrices& matrices, uint32_t x, uint32_t y, uint32_t z) {
    constexpr float froxel_near = 0.1f;
    constexpr float froxel_far = 3000.0f;
    constexpr float width = 160.0f, height = 90.0f, slices = 128.0f;
    const float depth = froxel_near * std::pow(
        froxel_far / froxel_near, (static_cast<float>(z) + 0.5f) / slices);
    const float camera_near = matrices.view_to_clip.m[11] /
                              (matrices.view_to_clip.m[10] + 1.0f);
    const float camera_far = matrices.view_to_clip.m[11] / matrices.view_to_clip.m[10];
    const float ndc_z = camera_near * (camera_far - depth) /
                        ((camera_far - camera_near) * depth);
    const matter::Float3 world = viewer::unproject_ndc(
        matrices.clip_to_world,
        {(static_cast<float>(x) + 0.5f) / width * 2.0f - 1.0f,
         1.0f - (static_cast<float>(y) + 0.5f) / height * 2.0f,
         ndc_z});
    return {world.x, world.y, world.z};
}

// Isolated real-device lane for Task 8. It deliberately creates a new
// renderer before the broad legacy RT scenarios: their intentional descriptor
// stress must not obscure a resize validation failure.
static void run_rt_froxel_resize_smoke(matter::VulkanDevice& vulkan) {
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error), error.empty() ? "initialize isolated froxel RT renderer"
                                              : error.c_str());
    CHECK(renderer.ensure_part(known_raster_triangle(998), error) >= 0 &&
              renderer.update_instances({{998, identity_matrix()}}, error),
          error.empty() ? "prepare isolated froxel RT geometry" : error.c_str());
    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.5f, 2.0f};
    camera.target = {0.0f, 0.5f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.0f;
    viewer::FrameMatrices matrices{};
    CHECK(viewer::build_frame_matrices(camera, 160, 100, matrices, error),
          error.empty() ? "build isolated froxel RT matrices" : error.c_str());
    matter::VulkanRayTracingSettings rt{};
    rt.enabled = true;
    renderer.set_ray_tracing_settings(rt);
    const auto warm_rt_tlas = [&]() {
        matter::VulkanFrame frame{};
        const bool recorded = vulkan.begin_frame(frame, error) &&
            renderer.prepare_frame(frame, matrices, camera.position, 1.0f,
                                   error) &&
            renderer.record_cull_and_render(frame, matrices, camera.position,
                                            1.0f, error) &&
            renderer.record_composite_to_swapchain(frame, error) &&
            vulkan.end_frame(frame, error);
        renderer.finish_ray_tracing_frame(frame.serial, recorded);
        vulkan.wait_idle();
        return recorded;
    };
    CHECK(warm_rt_tlas() && warm_rt_tlas() && renderer.rt_effective_observed(),
          error.empty() ? "commit isolated traceable TLAS before froxel resize"
                        : error.c_str());

    // Task 10: allocate the clear-only sun-space clipmaps through the real
    // renderer frame lifecycle before any Task 11 density generation exists.
    // The observable contract is resource ownership, descriptor publication,
    // fallback, failure cleanup, and retirement -- every sample remains clear.
    matter::FogSettings clear_fog{};
    matter::VulkanVolumetricsSettings clear_volumetrics{};
    clear_volumetrics.enabled = true;
    matter::CloudShadowSettings improved_shadows{};
    matter::apply_volumetric_quality_preset(
        matter::VolumetricQualityPreset::Improved, clear_volumetrics,
        improved_shadows);
    renderer.set_volumetrics_settings(clear_volumetrics, clear_fog,
                                      improved_shadows);
    CHECK(warm_rt_tlas(),
          error.empty() ? "allocate clear Task 10 cloud-shadow clipmaps"
                        : error.c_str());
    const auto improved_levels =
        matter::resolve_cloud_shadow_levels(improved_shadows);
    bool all_task10_images_match = renderer.cloud_shadows_active() &&
        renderer.cloud_shadow_persistent_bytes() ==
            matter::estimate_cloud_shadow_bytes(improved_shadows);
    for (uint32_t level = 0; level < 2; ++level) {
        const auto active_desc = renderer.cloud_shadow_level_desc(level);
        all_task10_images_match = all_task10_images_match &&
            active_desc.width == improved_levels[level].width &&
            active_desc.height == improved_levels[level].height &&
            active_desc.depth == improved_levels[level].depth &&
            renderer.cloud_shadow_density_format(level) ==
                VK_FORMAT_R16_SFLOAT &&
            renderer.cloud_shadow_density_extent(level).width ==
                improved_levels[level].width &&
            renderer.cloud_shadow_density_extent(level).height ==
                improved_levels[level].height &&
            renderer.cloud_shadow_density_extent(level).depth ==
                improved_levels[level].depth &&
            renderer.cloud_shadow_density_view(level) != VK_NULL_HANDLE;
        for (uint32_t ping = 0; ping < 2; ++ping) {
            const auto extent =
                renderer.cloud_shadow_cumulative_extent(level, ping);
            all_task10_images_match = all_task10_images_match &&
                renderer.cloud_shadow_cumulative_format(level, ping) ==
                    VK_FORMAT_R16_SFLOAT &&
                extent.width == improved_levels[level].width &&
                extent.height == improved_levels[level].height &&
                extent.depth == improved_levels[level].depth &&
                renderer.cloud_shadow_cumulative_view(level, ping) !=
                    VK_NULL_HANDLE &&
                renderer.cloud_shadow_environment_image_is_clear_for_test(
                    level * 2u + ping, error);
        }
    }
    CHECK(all_task10_images_match,
          "Improved owns exactly one density and two R16F cumulative images per resolved level");
    CHECK(renderer.cloud_shadow_active_ping(0) < 2u &&
              renderer.cloud_shadow_active_ping(1) < 2u &&
              renderer.cloud_shadow_environment_bindings_match_for_test() &&
              renderer.cloud_shadow_environment_state_for_test(0) == 1.0f,
          "both ping indices and the current set-1 cloud-shadow views are valid");

    matter::CloudShadowSettings disabled_shadows = improved_shadows;
    disabled_shadows.enabled = false;
    renderer.set_volumetrics_settings(clear_volumetrics, clear_fog,
                                      disabled_shadows);
    CHECK(warm_rt_tlas(),
          error.empty() ? "bind Task 10 emergency cloud-shadow images"
                        : error.c_str());
    bool disabled_is_clear = !renderer.cloud_shadows_active() &&
        renderer.cloud_shadow_persistent_bytes() == 0u &&
        renderer.cloud_shadow_environment_state_for_test(0) == 0.0f;
    for (uint32_t binding = 0; binding < 4; ++binding) {
        const auto extent =
            renderer.cloud_shadow_environment_extent_for_test(binding);
        disabled_is_clear = disabled_is_clear &&
            renderer.cloud_shadow_environment_view_for_test(binding) !=
                VK_NULL_HANDLE &&
            renderer.cloud_shadow_environment_image_is_clear_for_test(
                binding, error) &&
            extent.width == 1u && extent.height == 1u && extent.depth == 1u;
    }
    CHECK(disabled_is_clear,
          "disabled mode publishes cloud_state.x zero and initialized 1x1x1 clear bindings");

    // Re-establish a live pair, then fail High after partial candidate image
    // allocation. The current safe slot must switch to emergency descriptors;
    // no failed-candidate lifetime may escape into a live set.
    renderer.set_volumetrics_settings(clear_volumetrics, clear_fog,
                                      improved_shadows);
    CHECK(warm_rt_tlas(),
          error.empty() ? "restore Improved clipmaps before failure injection"
                        : error.c_str());
    matter::CloudShadowSettings high_shadows{};
    matter::VulkanVolumetricsSettings high_volumetrics{};
    matter::apply_volumetric_quality_preset(
        matter::VolumetricQualityPreset::High, high_volumetrics,
        high_shadows);
    renderer.set_fail_next_cloud_shadow_bundle_creation_for_test(true);
    renderer.set_volumetrics_settings(high_volumetrics, clear_fog,
                                      high_shadows);
    CHECK(warm_rt_tlas(),
          error.empty() ? "renderer survives partial High cloud-shadow allocation failure"
                        : error.c_str());
    const std::string cloud_failure = renderer.cloud_shadow_allocation_error();
    bool failure_uses_clear = !renderer.cloud_shadows_active() &&
        renderer.cloud_shadow_environment_state_for_test(0) == 0.0f &&
        renderer.cloud_shadow_failed_candidate_destroyed_for_test();
    for (uint32_t binding = 0; binding < 4; ++binding) {
        const auto extent =
            renderer.cloud_shadow_environment_extent_for_test(binding);
        failure_uses_clear = failure_uses_clear &&
            renderer.cloud_shadow_environment_image_is_clear_for_test(
                binding, error) &&
            extent.width == 1u && extent.height == 1u && extent.depth == 1u;
    }
    CHECK(failure_uses_clear &&
              cloud_failure.find("512x512x32") != std::string::npos &&
              cloud_failure.find("256x256x24") != std::string::npos &&
              cloud_failure.find("MiB") != std::string::npos,
          "partial High failure destroys candidates, binds emergency clear, and names both levels plus MiB");

    renderer.set_volumetrics_settings(clear_volumetrics, clear_fog,
                                      improved_shadows);
    CHECK(warm_rt_tlas(),
          error.empty() ? "recreate Improved clipmaps after rejected High request"
                        : error.c_str());
    renderer.set_volumetrics_settings(clear_volumetrics, clear_fog,
                                      disabled_shadows);
    CHECK(warm_rt_tlas() && warm_rt_tlas() &&
              renderer.cloud_shadow_retired_bundle_count_for_test() == 0u,
          error.empty() ? "retire cloud-shadow bundle after both frame slots complete"
                        : error.c_str());

    // Task 11 numerical path: use a separate small real module so the test
    // can inject an analytical density without weakening authored cloud math.
    viewer::VkCloudShadows task11_clouds;
    CHECK(task11_clouds.init(vulkan, error),
          error.empty() ? "initialize analytical Task 11 cloud-shadow module"
                        : error.c_str());
    matter::CloudShadowSettings task11_settings{};
    task11_settings.enabled = true;
    task11_settings.near_resolution = 0;
    task11_settings.near_depth_slices = 0;
    task11_settings.near_coverage_m = 160.0f;
    task11_settings.far_resolution = 0;
    task11_settings.far_depth_slices = 0;
    task11_settings.far_coverage_m = 160.0f;
    task11_settings.update_fraction = 1.0f;
    task11_clouds.request_settings(task11_settings);
    task11_clouds.set_density_override_for_test(0.02f, -1, 0.02f, true);
    const matter::Float3 task11_camera{0.0f, 0.0f, 0.0f};
    const matter::Float3 task11_daylight{0.0f, -1.0f, 0.0f};
    CHECK(task11_clouds.generate_for_test(
              0, task11_camera, task11_daylight, 0.0f, error),
          error.empty() ? "generate analytical constant cloud-shadow slab"
                        : error.c_str());
    const uint32_t slab_ping = task11_clouds.level(0).active_index;
    bool slab_matches = task11_clouds.last_generation_dispatch_count_for_test() == 6u;
    float previous_tau = std::numeric_limits<float>::infinity();
    for (uint32_t z = 0; z < 16; ++z) {
        float tau = 0.0f;
        uint16_t raw = 0;
        slab_matches = slab_matches &&
            task11_clouds.readback_cumulative_voxel_for_test(
                0, slab_ping, 64, 64, z, tau, raw, error) &&
            raw != 0u && std::isfinite(tau) && tau <= previous_tau &&
            std::fabs(tau - static_cast<float>(16u - z) * 0.2f) < 0.005f &&
            std::isfinite(std::exp(-tau)) && std::exp(-tau) >= 0.0f &&
            std::exp(-tau) <= 1.0f;
        previous_tau = tau;
    }
    CHECK(slab_matches,
          error.empty() ? "R16F GPU prefix matches the constant analytical slab"
                        : error.c_str());

    task11_clouds.set_density_override_for_test(0.02f, 2, 0.02f, true);
    CHECK(task11_clouds.generate_for_test(
              1, task11_camera, task11_daylight, 1.0f, error),
          error.empty() ? "generate slab with one non-finite density slice"
                        : error.c_str());
    const uint32_t nan_ping = task11_clouds.level(0).active_index;
    float tau_before_nan = 0.0f, tau_at_nan = 0.0f, tau_after_nan = 0.0f;
    uint16_t raw_before_nan = 0, raw_at_nan = 0, raw_after_nan = 0;
    CHECK(task11_clouds.readback_cumulative_voxel_for_test(
              0, nan_ping, 64, 64, 1, tau_before_nan, raw_before_nan, error) &&
              task11_clouds.readback_cumulative_voxel_for_test(
                  0, nan_ping, 64, 64, 2, tau_at_nan, raw_at_nan, error) &&
              task11_clouds.readback_cumulative_voxel_for_test(
                  0, nan_ping, 64, 64, 3, tau_after_nan, raw_after_nan, error) &&
              std::fabs(tau_before_nan - 2.8f) < 0.005f &&
              std::fabs(tau_at_nan - 2.6f) < 0.005f &&
              std::fabs(tau_after_nan - 2.6f) < 0.005f,
          error.empty() ? "GPU prefix treats one NaN density sample as clear"
                        : error.c_str());

    task11_clouds.set_density_layers_for_test(12, 0.03f, 3, 0.02f,
                                              true);
    CHECK(task11_clouds.generate_for_test(
              0, task11_camera, task11_daylight, 1.5f, error),
          error.empty() ? "generate two analytically separated cloud layers"
                        : error.c_str());
    const uint32_t layers_ping = task11_clouds.level(0).active_index;
    float above_tau = 0.0f, between_tau = 0.0f, below_tau = 0.0f;
    uint16_t above_raw = 0, between_raw = 0, below_raw = 0;
    CHECK(task11_clouds.readback_cumulative_voxel_for_test(
              0, layers_ping, 64, 64, 15, above_tau, above_raw, error) &&
              task11_clouds.readback_cumulative_voxel_for_test(
                  0, layers_ping, 64, 64, 8, between_tau, between_raw, error) &&
              task11_clouds.readback_cumulative_voxel_for_test(
                  0, layers_ping, 64, 64, 0, below_tau, below_raw, error) &&
              std::fabs(above_tau) < 0.005f &&
              std::fabs(between_tau - 0.3f) < 0.005f &&
              std::fabs(below_tau - 0.5f) < 0.005f,
          error.empty() ? "GPU receivers above, between, and below separated layers see only sunward extinction"
                        : error.c_str());

    // Quarter scheduling changes a selected tile while a non-selected tile
    // retains the reprojected value. The phase helper independently locates
    // representative columns for frame phase 1.
    task11_settings.update_fraction = 0.25f;
    task11_clouds.request_settings(task11_settings);
    task11_clouds.set_density_override_for_test(0.01f, -1, 0.01f, true);
    CHECK(task11_clouds.generate_for_test(
              0, task11_camera, task11_daylight, 2.0f, error),
          error.empty() ? "seed Task 11 quarter-scheduler history"
                        : error.c_str());
    std::array<uint32_t, 2> selected_column{0, 0};
    std::array<uint32_t, 2> retained_column{0, 0};
    bool found_selected = false, found_retained = false;
    const uint32_t quarter_phase = task11_clouds.frame_index_for_test();
    for (uint32_t y = 0; y < 128 && (!found_selected || !found_retained); y += 8)
        for (uint32_t x = 0; x < 128 && (!found_selected || !found_retained); x += 8) {
            const bool selected = viewer::cloud_shadow_column_selected(
                true, false, {x, y}, 0, quarter_phase, 0.25f);
            if (selected && !found_selected) {
                selected_column = {x, y}; found_selected = true;
            } else if (!selected && !found_retained) {
                retained_column = {x, y}; found_retained = true;
            }
        }
    task11_clouds.set_density_override_for_test(0.02f, -1, 0.02f, false);
    CHECK(task11_clouds.generate_for_test(
              1, task11_camera, task11_daylight, 3.0f, error),
          error.empty() ? "advance one rotating Task 11 quarter"
                        : error.c_str());
    const uint32_t quarter_ping = task11_clouds.level(0).active_index;
    float selected_tau = 0.0f, retained_tau = 0.0f;
    uint16_t selected_raw = 0, retained_raw = 0;
    CHECK(found_selected && found_retained &&
              task11_clouds.readback_cumulative_voxel_for_test(
                  0, quarter_ping, selected_column[0], selected_column[1], 0,
                  selected_tau, selected_raw, error) &&
              task11_clouds.readback_cumulative_voxel_for_test(
                  0, quarter_ping, retained_column[0], retained_column[1], 0,
                  retained_tau, retained_raw, error) &&
              std::fabs(selected_tau - 3.2f) < 0.005f &&
              std::fabs(retained_tau - 1.6f) < 0.005f,
          error.empty() ? "GPU updates one deterministic quarter and retains reprojected tiles"
                        : error.c_str());

    // Seed a tile-unique checker, then move exactly one snapped voxel. Pick a
    // retained current tile whose previous-world source crosses an 8x8 tile
    // boundary and has a different raw value than the wrong equal-index copy.
    task11_settings.update_fraction = 1.0f;
    task11_clouds.request_settings(task11_settings);
    task11_clouds.set_density_override_for_test(0.01f, -1, 0.03f, true);
    CHECK(task11_clouds.generate_for_test(
              0, task11_camera, task11_daylight, 3.5f, error),
          error.empty() ? "seed checker history for world-position reprojection"
                        : error.c_str());
    const uint32_t checker_ping = task11_clouds.level(0).active_index;
    const auto before_move = task11_clouds.level(0).current_frame;
    matter::Float3 task11_lateral{
        before_move.uvw_to_world.m[0] / task11_settings.near_coverage_m,
        before_move.uvw_to_world.m[4] / task11_settings.near_coverage_m,
        before_move.uvw_to_world.m[8] / task11_settings.near_coverage_m};
    const matter::Float3 moved_camera{
        task11_lateral.x * before_move.voxel_xy_m,
        task11_lateral.y * before_move.voxel_xy_m,
        task11_lateral.z * before_move.voxel_xy_m};
    task11_settings.update_fraction = 0.25f;
    task11_clouds.request_settings(task11_settings);
    const uint32_t move_phase = task11_clouds.frame_index_for_test();
    std::array<uint32_t, 2> overlap_column{0, 0};
    uint32_t source_x = 0;
    uint16_t source_raw = 0, equal_index_raw = 0;
    bool found_shifted_checker = false;
    for (uint32_t y = 0; y < 128 && !found_shifted_checker; y += 8) {
        for (uint32_t x = 7; x < 127 && !found_shifted_checker; x += 8) {
            if (viewer::cloud_shadow_column_selected(
                    true, false, {x, y}, 0, move_phase, 0.25f)) continue;
            const auto moved_frame = matter::make_cloud_shadow_frame(
                task11_clouds.level(0).desc, moved_camera, task11_daylight);
            const auto source_uvw = viewer::cloud_shadow_previous_uvw_for_voxel(
                moved_frame, before_move, task11_clouds.level(0).desc,
                x, y, 0);
            const uint32_t candidate_source_x = static_cast<uint32_t>(
                source_uvw.x * task11_clouds.level(0).desc.width);
            float ignored_tau = 0.0f;
            uint16_t candidate_source_raw = 0, candidate_equal_raw = 0;
            if (candidate_source_x < 128 && candidate_source_x != x &&
                task11_clouds.readback_cumulative_voxel_for_test(
                    0, checker_ping, candidate_source_x, y, 0,
                    ignored_tau, candidate_source_raw, error) &&
                task11_clouds.readback_cumulative_voxel_for_test(
                    0, checker_ping, x, y, 0,
                    ignored_tau, candidate_equal_raw, error) &&
                candidate_source_raw != candidate_equal_raw) {
                overlap_column = {x, y};
                source_x = candidate_source_x;
                source_raw = candidate_source_raw;
                equal_index_raw = candidate_equal_raw;
                found_shifted_checker = true;
            }
        }
    }
    task11_clouds.set_density_override_for_test(0.02f, -1, 0.04f, false);
    CHECK(task11_clouds.generate_for_test(
              0, moved_camera, task11_daylight, 4.0f, error),
          error.empty() ? "reproject one snapped voxel and refresh exposed border"
                        : error.c_str());
    const uint32_t moved_ping = task11_clouds.level(0).active_index;
    float overlap_tau = 0.0f, border_tau = 0.0f;
    uint16_t overlap_raw = 0, border_raw = 0;
    const float border_sigma =
        (viewer::cloud_shadow_tile_hash(127, 64, 0) & 1u) == 0u
            ? 0.02f : 0.04f;
    CHECK(found_shifted_checker && source_x != overlap_column[0] &&
              source_raw != equal_index_raw &&
              task11_clouds.readback_cumulative_voxel_for_test(
                  0, moved_ping, overlap_column[0], overlap_column[1], 0,
                  overlap_tau, overlap_raw, error) &&
              task11_clouds.readback_cumulative_voxel_for_test(
                  0, moved_ping, 127, 64, 0, border_tau, border_raw, error) &&
              overlap_raw == source_raw &&
              std::fabs(border_tau - 16.0f * border_sigma * 10.0f) < 0.01f,
          error.empty() ? "shifted world-position checker history survives while only the exposed border refreshes"
                        : error.c_str());

    // A non-finite value in otherwise valid history must invalidate and
    // refresh its entire column immediately, independent of rotating phase.
    std::array<uint32_t, 2> nan_history_column{0, 0};
    bool found_nan_history_column = false;
    const uint32_t nan_phase = task11_clouds.frame_index_for_test();
    for (uint32_t y = 0; y < 128 && !found_nan_history_column; y += 8)
        for (uint32_t x = 0; x < 128 && !found_nan_history_column; x += 8)
            if (!viewer::cloud_shadow_column_selected(
                    true, false, {x, y}, 0, nan_phase, 0.25f)) {
                nan_history_column = {x, y};
                found_nan_history_column = true;
            }
    CHECK(found_nan_history_column &&
              task11_clouds.write_cumulative_raw_for_test(
                  0, moved_ping, nan_history_column[0],
                  nan_history_column[1], 8, 0x7e00u, error),
          error.empty() ? "inject non-finite cumulative history into a non-rotating column"
                        : error.c_str());
    task11_clouds.set_density_override_for_test(0.05f, -1, 0.05f, false);
    CHECK(task11_clouds.generate_for_test(
              1, moved_camera, task11_daylight, 4.5f, error),
          error.empty() ? "refresh non-finite reprojected history"
                        : error.c_str());
    float repaired_tau = 0.0f;
    uint16_t repaired_raw = 0;
    CHECK(task11_clouds.readback_cumulative_voxel_for_test(
              0, task11_clouds.level(0).active_index,
              nan_history_column[0], nan_history_column[1], 0,
              repaired_tau, repaired_raw, error) &&
              std::isfinite(repaired_tau) &&
              std::fabs(repaired_tau - 8.0f) < 0.01f,
          error.empty() ? "non-finite history triggers immediate full-column regeneration instead of a clear hole"
                        : error.c_str());

    // Exercise production packed cloud density: fixed camera and frame phase,
    // advancing cloud time may modify exactly the rotating quarter only.
    matter::FogSettings animated_fog{};
    animated_fog.cloud_count = 1;
    animated_fog.clouds[0].enabled = true;
    animated_fog.clouds[0].min_height = -10000.0f;
    animated_fog.clouds[0].max_height = 10000.0f;
    animated_fog.clouds[0].max_density = 0.02f;
    animated_fog.clouds[0].coverage = 0.5f;
    animated_fog.clouds[0].noise_scale = 0.01f;
    animated_fog.clouds[0].wind[0] = 25.0f;
    animated_fog.clouds[0].octaves = 2;
    task11_clouds.request_cloud_layers(animated_fog);
    task11_clouds.clear_density_override_for_test(true);
    task11_settings.update_fraction = 1.0f;
    task11_clouds.request_settings(task11_settings);
    CHECK(task11_clouds.generate_for_test(
              0, task11_camera, task11_daylight, 0.0f, error),
          error.empty() ? "seed production packed-cloud history"
                        : error.c_str());
    const uint32_t production_seed_ping =
        task11_clouds.level(0).active_index;
    std::array<uint16_t, 256> production_before{};
    uint32_t tile_sample = 0;
    for (uint32_t y = 4; y < 128; y += 8)
        for (uint32_t x = 4; x < 128; x += 8) {
            float ignored_tau = 0.0f;
            task11_clouds.readback_cumulative_voxel_for_test(
                0, production_seed_ping, x, y, 0, ignored_tau,
                production_before[tile_sample++], error);
        }
    task11_settings.update_fraction = 0.25f;
    task11_clouds.request_settings(task11_settings);
    const uint32_t production_phase = task11_clouds.frame_index_for_test();
    CHECK(task11_clouds.generate_for_test(
              1, task11_camera, task11_daylight, 10.0f, error),
          error.empty() ? "advance cloud time for one production rotating quarter"
                        : error.c_str());
    bool nonselected_unchanged = true, selected_changed = false;
    tile_sample = 0;
    for (uint32_t y = 4; y < 128; y += 8)
        for (uint32_t x = 4; x < 128; x += 8) {
            float ignored_tau = 0.0f;
            uint16_t after_raw = 0;
            const bool read = task11_clouds.readback_cumulative_voxel_for_test(
                0, task11_clouds.level(0).active_index, x, y, 0,
                ignored_tau, after_raw, error);
            const bool selected = viewer::cloud_shadow_column_selected(
                true, false, {x, y}, 0, production_phase, 0.25f);
            nonselected_unchanged = nonselected_unchanged && read &&
                (selected || after_raw == production_before[tile_sample]);
            selected_changed = selected_changed ||
                (read && selected && after_raw != production_before[tile_sample]);
            ++tile_sample;
        }
    CHECK(nonselected_unchanged && selected_changed,
          error.empty() ? "cloud time advances production density only in the deterministic rotating quarter"
                        : error.c_str());

    animated_fog.clouds[0].max_density = 0.03f;
    task11_clouds.request_cloud_layers(animated_fog);
    CHECK(task11_clouds.prepare_frame(
              0, task11_camera, task11_daylight, 0.53f, error) &&
              !task11_clouds.level(0).history_valid &&
              !task11_clouds.level(1).history_valid,
          error.empty() ? "packed cloud authoring invalidates both generated levels"
                        : error.c_str());
    CHECK(task11_clouds.generate_for_test(
              0, task11_camera, task11_daylight, 11.0f, error),
          error.empty() ? "reseed after packed authoring invalidation"
                        : error.c_str());
    task11_settings.near_coverage_m = 180.0f;
    task11_clouds.request_settings(task11_settings);
    CHECK(task11_clouds.prepare_frame(
              1, task11_camera, task11_daylight, 0.53f, error) &&
              !task11_clouds.level(0).history_valid &&
              !task11_clouds.level(1).history_valid,
          error.empty() ? "dimension or coverage changes invalidate both generated levels"
                        : error.c_str());
    CHECK(task11_clouds.generate_for_test(
              1, task11_camera, task11_daylight, 12.0f, error),
          error.empty() ? "reseed resized cloud-shadow levels"
                        : error.c_str());
    const float deg = 3.14159265358979323846f / 180.0f;
    const matter::Float3 within_two{
        0.0f, -std::cos(1.9f * deg), std::sin(1.9f * deg)};
    const matter::Float3 beyond_two{
        0.0f, -std::cos(4.1f * deg), std::sin(4.1f * deg)};
    CHECK(task11_clouds.prepare_frame(
              0, task11_camera, within_two, 0.53f, error) &&
              task11_clouds.level(0).history_valid &&
              task11_clouds.level(1).history_valid,
          error.empty() ? "a two-degree-or-smaller sun edit preserves both histories"
                        : error.c_str());
    CHECK(task11_clouds.prepare_frame(
              0, task11_camera, beyond_two, 0.53f, error) &&
              !task11_clouds.level(0).history_valid &&
              !task11_clouds.level(1).history_valid,
          error.empty() ? "a greater-than-two-degree sun edit invalidates both histories"
                        : error.c_str());

    CHECK(task11_clouds.generate_for_test(
              1, task11_camera, {0.0f, 0.1f, 1.0f}, 5.0f, error) &&
              task11_clouds.last_generation_dispatch_count_for_test() == 0u &&
              task11_clouds.environment_block()[32] == 0.0f &&
              task11_clouds.environment_image_is_clear_for_test(0, error),
          error.empty() ? "below-horizon sun publishes clear without generation dispatches"
                        : error.c_str());
    task11_clouds.destroy();

    // Task 9: exercise the production bundle swap, density dispatch, and
    // readback path with a cloud deck that is constant over every froxel this
    // fixture can see.  Ground fog and emitters stay clear, so enhanced
    // media.rgb has only the cloud's documented 0.99 scattering term.
    matter::FogSettings cloud_fog{};
    cloud_fog.cloud_count = 1;
    cloud_fog.clouds[0].enabled = true;
    cloud_fog.clouds[0].min_height = -10000.0f;
    cloud_fog.clouds[0].max_height = 10000.0f;
    cloud_fog.clouds[0].max_density = 0.02f;
    cloud_fog.clouds[0].coverage = 1.0f;
    cloud_fog.clouds[0].falloff_min = 0.0f;
    cloud_fog.clouds[0].falloff_max = 0.0f;
    cloud_fog.clouds[0].octaves = 1;
    cloud_fog.color[0] = 0.0f;
    cloud_fog.color[1] = 0.0f;
    cloud_fog.color[2] = 0.0f;
    matter::VulkanVolumetricsSettings current_clouds{};
    current_clouds.enabled = true;
    current_clouds.local_sun_march_steps = 0;
    current_clouds.multiple_scattering_orders = 1;
    current_clouds.multiple_scattering_strength = 0.0f;
    current_clouds.powder_strength = 0.0f;
    renderer.set_volumetrics_settings(current_clouds, cloud_fog);
    CHECK(warm_rt_tlas(), error.empty() ? "render Current-cost cloud density"
                                        : error.c_str());
    CHECK(renderer.volumetrics_grid_rgba16f_volume_count_for_test() == 4u &&
              !renderer.volumetrics_cloud_density_allocated_for_test() &&
              renderer.volumetrics_cloud_density_dimensions_for_test().width == 1u &&
              renderer.volumetrics_cloud_density_dimensions_for_test().height == 1u &&
              renderer.volumetrics_cloud_density_dimensions_for_test().depth == 1u &&
              renderer.volumetrics_grid_bytes_for_test() == 58982400u,
          "Current cost owns four RGBA16F grids and only a non-accounted R16F dummy");

    // Exercise the composite cloud-density debug view on the Current path as
    // well: its stable dummy stays GENERAL because it is also the unused
    // storage-image binding of the Current specialization.
    current_clouds.vol_debug_view = 4.0f;
    renderer.set_volumetrics_settings(current_clouds, cloud_fog);
    CHECK(warm_rt_tlas(), error.empty() ? "render Current cloud-density debug"
                                        : error.c_str());
    CHECK(vulkan.validation_error_count() == 0,
          "Current cloud-density debug samples the stable GENERAL dummy without validation errors");
    current_clouds.vol_debug_view = 0.0f;

    // Custom-state predicate coverage: strength is a weighting control, not
    // an enhanced feature by itself, while cloud shadows consume the shared
    // density even when every volumetric enhanced dial remains neutral.
    matter::CloudShadowSettings no_cloud_shadows{};
    no_cloud_shadows.enabled = false;
    matter::VulkanVolumetricsSettings strength_only = current_clouds;
    strength_only.multiple_scattering_strength = 0.85f;
    renderer.set_volumetrics_settings(strength_only, cloud_fog,
                                      no_cloud_shadows);
    CHECK(warm_rt_tlas(), error.empty() ? "render strength-only custom cloud state"
                                        : error.c_str());
    CHECK(!renderer.volumetrics_cloud_density_allocated_for_test() &&
              renderer.volumetrics_grid_bytes_for_test() == 58982400u,
          "multiple-scattering strength alone keeps the Current R16F dummy");

    matter::CloudShadowSettings shadows_only{};
    shadows_only.enabled = true;
    renderer.set_volumetrics_settings(current_clouds, cloud_fog, shadows_only);
    CHECK(warm_rt_tlas(), error.empty() ? "render shadows-only custom cloud state"
                                        : error.c_str());
    CHECK(renderer.volumetrics_cloud_density_allocated_for_test() &&
              renderer.volumetrics_grid_bytes_for_test() == 62668800u,
          "cloud shadows alone allocate the enhanced R16F density grid");
    const uint32_t task11_initial_ping =
        renderer.cloud_shadow_active_ping(0);
    CHECK(!renderer.cloud_shadow_environment_image_is_clear_for_test(
              task11_initial_ping, error),
          error.empty()
              ? "authored cloud extinction generates nonzero sun-space optical depth"
              : error.c_str());

    matter::VulkanFrame flush_failure_frame{};
    CHECK(vulkan.begin_frame(flush_failure_frame, error) &&
              renderer.prepare_frame(flush_failure_frame, matrices,
                                     camera.position, 1.0f, error),
          error.empty() ? "begin acquired slot for EnvironmentBlock flush failure"
                        : error.c_str());
    const uint32_t ping_before_flush_failure[2]{
        renderer.cloud_shadow_active_ping(0),
        renderer.cloud_shadow_active_ping(1)};
    float state_before_flush_failure[4]{};
    VkImageView view_before_flush_failure[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        state_before_flush_failure[i] =
            renderer.cloud_shadow_environment_state_for_test(i);
        view_before_flush_failure[i] =
            renderer.cloud_shadow_environment_view_for_test(i);
    }
    renderer.set_fail_next_environment_flush_for_test(true);
    const bool flush_failure_recorded = renderer.record_cull_and_render(
        flush_failure_frame, matrices, camera.position, 1.0f, error);
    const std::string flush_failure_error = error;
    bool publication_unchanged = !flush_failure_recorded &&
        renderer.cloud_shadow_active_ping(0) == ping_before_flush_failure[0] &&
        renderer.cloud_shadow_active_ping(1) == ping_before_flush_failure[1];
    for (uint32_t i = 0; i < 4; ++i) {
        publication_unchanged = publication_unchanged &&
            renderer.cloud_shadow_environment_state_for_test(i) ==
                state_before_flush_failure[i] &&
            renderer.cloud_shadow_environment_view_for_test(i) ==
                view_before_flush_failure[i];
    }
    CHECK(publication_unchanged &&
              flush_failure_error.find("EnvironmentBlock flush failure") !=
                  std::string::npos,
          "failed acquired-slot EnvironmentBlock flush keeps the previous published ping transactionally");
    _putenv_s("MATTER_VK_TEST_END_FRAME_FAULT", "record");
    std::string abort_error;
    CHECK(!vulkan.end_frame(flush_failure_frame, abort_error),
          "abort the intentionally incomplete flush-failure frame");
    _putenv_s("MATTER_VK_TEST_END_FRAME_FAULT", "");
    renderer.finish_ray_tracing_frame(flush_failure_frame.serial, false);
    vulkan.wait_idle();
    error.clear();
    CHECK(warm_rt_tlas(),
          error.empty() ? "regenerate and publish after transactional flush failure"
                        : error.c_str());
    renderer.set_volumetrics_settings(current_clouds, cloud_fog,
                                      no_cloud_shadows);
    CHECK(warm_rt_tlas() &&
              !renderer.volumetrics_cloud_density_allocated_for_test(),
          error.empty() ? "shadows-only density retires safely when disabled"
                        : error.c_str());

    matter::VulkanVolumetricsSettings improved_clouds = current_clouds;
    improved_clouds.local_sun_march_steps = 1;
    renderer.set_volumetrics_settings(improved_clouds, cloud_fog);
    CHECK(warm_rt_tlas(), error.empty() ? "render Improved cloud density"
                                        : error.c_str());
    matter::Float4 cloud_media{};
    float cloud_density = 0.0f;
    constexpr uint32_t task9_x = 80u, task9_y = 45u, task9_z = 64u;
    const Task9CloudPoint task9_world =
        task9_froxel_world_position(matrices, task9_x, task9_y, task9_z);
    matter::GpuCloudLayer task9_packed{};
    matter::pack_cloud_layer(cloud_fog.clouds[0], 0, task9_packed);
    const float task9_cpu_density =
        task9_cloud_density_cpu(task9_packed, task9_world, 1.0f);
    CHECK(renderer.volumetrics_cloud_density_allocated_for_test() &&
              renderer.volumetrics_cloud_density_dimensions_for_test().width == 160u &&
              renderer.volumetrics_cloud_density_dimensions_for_test().height == 90u &&
              renderer.volumetrics_cloud_density_dimensions_for_test().depth == 128u &&
              renderer.volumetrics_grid_rgba16f_volume_count_for_test() == 4u &&
              renderer.volumetrics_grid_bytes_for_test() == 62668800u &&
              renderer.readback_volumetrics_density_voxel_for_test(
                  task9_x, task9_y, task9_z, cloud_media, cloud_density, error) &&
              std::isfinite(cloud_density) && cloud_density > 0.0f &&
              std::fabs(cloud_density - task9_cpu_density) < 2e-4f &&
              std::fabs(cloud_media.w - cloud_density) < 2e-4f &&
              std::fabs(cloud_media.x - 0.99f * cloud_density) < 2e-4f &&
              std::fabs(cloud_media.y - 0.99f * cloud_density) < 2e-4f &&
              std::fabs(cloud_media.z - 0.99f * cloud_density) < 2e-4f,
          error.empty() ? "Improved cloud grid records extinction and near-white scattering"
                        : error.c_str());

    // Fixed-voxel semantic coverage uses the same binary32 hash/value-noise/
    // FBM order as the shader oracle above.  These cases catch a missing
    // coverage/height branch and a seed/index packing regression, rather than
    // merely proving that some cloud voxel is nonzero.
    matter::FogSettings semantic_fog = cloud_fog;
    semantic_fog.clouds[0].coverage = 0.0f;
    renderer.set_volumetrics_settings(improved_clouds, semantic_fog);
    CHECK(warm_rt_tlas(), error.empty() ? "render coverage-zero cloud fixture"
                                        : error.c_str());
    matter::Float4 semantic_media{};
    float semantic_density = -1.0f;
    CHECK(renderer.readback_volumetrics_density_voxel_for_test(
              task9_x, task9_y, task9_z, semantic_media, semantic_density, error) &&
              semantic_density == 0.0f && semantic_media.w == 0.0f,
          error.empty() ? "coverage zero clears the fixed GPU cloud voxel"
                        : error.c_str());

    semantic_fog.clouds[0].coverage = 1.0f;
    semantic_fog.clouds[0].min_height = 100.0f;
    semantic_fog.clouds[0].max_height = 300.0f;
    renderer.set_volumetrics_settings(improved_clouds, semantic_fog);
    CHECK(warm_rt_tlas(), error.empty() ? "render outside-height cloud fixture"
                                        : error.c_str());
    CHECK(renderer.readback_volumetrics_density_voxel_for_test(
              task9_x, task9_y, task9_z, semantic_media, semantic_density, error) &&
              semantic_density == 0.0f && semantic_media.w == 0.0f,
          error.empty() ? "outside-height fixed GPU cloud voxel is clear"
                        : error.c_str());

    matter::CloudLayer shaped = cloud_fog.clouds[0];
    shaped.min_height = -100.0f;
    shaped.max_height = 100.0f;
    shaped.coverage = 0.58f;
    shaped.noise_scale = 0.017f;
    shaped.octaves = 4;
    shaped.lacunarity = 2.03f;
    shaped.gain = 0.5f;
    semantic_fog.cloud_count = 1;
    semantic_fog.clouds[0] = shaped;
    renderer.set_volumetrics_settings(improved_clouds, semantic_fog);
    CHECK(warm_rt_tlas(), error.empty() ? "render seed-zero shaped cloud fixture"
                                        : error.c_str());
    float seed_zero_density = 0.0f;
    matter::GpuCloudLayer seed_zero_layer{};
    matter::pack_cloud_layer(shaped, 0, seed_zero_layer);
    const float seed_zero_cpu = task9_cloud_density_cpu(
        seed_zero_layer, task9_world, 1.0f);
    CHECK(renderer.readback_volumetrics_density_voxel_for_test(
              task9_x, task9_y, task9_z, semantic_media, seed_zero_density, error) &&
              std::isfinite(seed_zero_density) && seed_zero_density >= 0.0f &&
              std::fabs(seed_zero_density - seed_zero_cpu) < 2e-4f,
          error.empty() ? "seed-zero fixed R16 voxel matches the CPU cloud evaluator"
                        : error.c_str());

    semantic_fog.cloud_count = 2;
    semantic_fog.clouds[0] = shaped;
    semantic_fog.clouds[0].max_density = 0.0f;
    semantic_fog.clouds[1] = shaped;
    renderer.set_volumetrics_settings(improved_clouds, semantic_fog);
    CHECK(warm_rt_tlas(), error.empty() ? "render seed-one shaped cloud fixture"
                                        : error.c_str());
    float seed_one_density = 0.0f;
    matter::GpuCloudLayer seed_one_layer{};
    matter::pack_cloud_layer(shaped, 1, seed_one_layer);
    const float seed_one_cpu = task9_cloud_density_cpu(
        seed_one_layer, task9_world, 1.0f);
    CHECK(renderer.readback_volumetrics_density_voxel_for_test(
              task9_x, task9_y, task9_z, semantic_media, seed_one_density, error) &&
              std::isfinite(seed_one_density) && seed_one_density >= 0.0f &&
              std::fabs(seed_one_density - seed_one_cpu) < 2e-4f &&
              std::fabs(seed_one_density - seed_zero_density) > 2e-4f,
          error.empty() ? "derived seeds decorrelate fixed GPU cloud samples"
                        : error.c_str());

    renderer.set_volumetrics_settings(improved_clouds, cloud_fog);
    CHECK(warm_rt_tlas(), error.empty() ? "restore constant cloud fixture"
                                        : error.c_str());

    // RED/GREEN guard for the enhanced composition: establish a nonzero fog
    // baseline, then add the same constant cloud.  Only the cloud delta may
    // contribute near-white scattering; multiplying fog_albedo by total
    // extinction would double-count it here.
    matter::FogSettings fog_plus_cloud = cloud_fog;
    fog_plus_cloud.density = 0.004f;
    fog_plus_cloud.floor = -10000.0f;
    fog_plus_cloud.falloff = 100000.0f;
    fog_plus_cloud.color[0] = 0.21f;
    fog_plus_cloud.color[1] = 0.37f;
    fog_plus_cloud.color[2] = 0.58f;
    // Keep the one-layer specialization stable; zero density makes the
    // baseline cloud-free without relying on a pipeline-count swap.
    fog_plus_cloud.clouds[0].max_density = 0.0f;
    renderer.set_volumetrics_settings(improved_clouds, fog_plus_cloud);
    CHECK(warm_rt_tlas() && warm_rt_tlas(), error.empty() ? "render nonzero fog baseline" : error.c_str());
    matter::Float4 fog_media{};
    float no_cloud_density = 0.0f;
    CHECK(renderer.readback_volumetrics_density_voxel_for_test(
              80u, 45u, 64u, fog_media, no_cloud_density, error),
          error.empty() ? "read nonzero fog baseline" : error.c_str());
    fog_plus_cloud.clouds[0].max_density = cloud_fog.clouds[0].max_density;
    renderer.set_volumetrics_settings(improved_clouds, fog_plus_cloud);
    CHECK(warm_rt_tlas() && warm_rt_tlas(), error.empty() ? "render nonzero fog plus cloud" : error.c_str());
    matter::Float4 combined_media{};
    float combined_cloud_density = 0.0f;
    const bool combined_read = renderer.readback_volumetrics_density_voxel_for_test(
              80u, 45u, 64u, combined_media, combined_cloud_density, error);
    if (combined_read) std::fprintf(stderr,
        "task9 fog/cloud: base=(%.6f,%.6f,%.6f,%.6f) combined=(%.6f,%.6f,%.6f,%.6f) cloud=%.6f\n",
        fog_media.x, fog_media.y, fog_media.z, fog_media.w, combined_media.x,
        combined_media.y, combined_media.z, combined_media.w, combined_cloud_density);
    CHECK(combined_read &&
              std::fabs((combined_media.w - fog_media.w) - combined_cloud_density) < 2e-4f &&
              std::fabs((combined_media.x - fog_media.x) - 0.99f * combined_cloud_density) < 2e-4f &&
              std::fabs((combined_media.y - fog_media.y) - 0.99f * combined_cloud_density) < 2e-4f &&
              std::fabs((combined_media.z - fog_media.z) - 0.99f * combined_cloud_density) < 2e-4f,
          error.empty() ? "enhanced cloud delta preserves nonzero fog and adds only 0.99 cloud scattering"
                        : error.c_str());

    // Task 12 RED/GREEN: exercise the production scatter and integration
    // pipelines with fixed physical direct/ambient inputs. Four frames cover
    // the complete 2x2 Bayer schedule after every lighting-history reset.
    viewer::VkSceneLighting task12_lighting{};
    task12_lighting.sun_direction = {0.0f, -1.0f, 0.0f};
    task12_lighting.sun_intensity = 1.0f;
    task12_lighting.atmosphere_sources.authored_irradiance_chroma_rgb =
        {0.0f, 0.0f, 0.0f};
    task12_lighting.atmosphere_sources.live_sun_tint_rgb =
        {1.0f, 1.0f, 1.0f};
    task12_lighting.atmosphere_sources.sun_multiplier = 1.0f;
    renderer.set_lighting(task12_lighting);
    matter::CloudShadowSettings task12_no_shadows{};
    task12_no_shadows.enabled = false;
    matter::VulkanVolumetricsSettings task12_settings = improved_clouds;
    task12_settings.temporal_blend = 0.0f;
    task12_settings.local_sun_march_steps = 8;
    task12_settings.local_sun_march_distance_m = 250.0f;
    task12_settings.multiple_scattering_strength = 0.55f;
    task12_settings.powder_strength = 0.0f;
    matter::FogSettings task12_cloud = cloud_fog;
    task12_cloud.clouds[0].max_density = 0.004f;
    const auto task12_capture = [&](int orders, float strength,
                                    const matter::FogSettings& fixture,
                                    matter::Float4& sample) {
        task12_settings.multiple_scattering_orders = orders;
        task12_settings.multiple_scattering_strength = strength;
        renderer.set_volumetrics_settings(task12_settings, fixture,
                                          task12_no_shadows);
        bool rendered = true;
        for (int frame = 0; frame < 4; ++frame)
            rendered = rendered && warm_rt_tlas();
        return rendered &&
            renderer.readback_volumetrics_integrated_voxel_for_test(
                80u, 45u, 100u, sample, error);
    };
    const auto luminance = [](const matter::Float4& value) {
        return value.x * 0.2126f + value.y * 0.7152f + value.z * 0.0722f;
    };
    matter::Float4 task12_orders[4]{};
    const uint64_t task12_enhanced_generation =
        renderer.volumetrics_resource_generation();
    bool task12_orders_read = true;
    for (int order = 1; order <= 4; ++order)
        task12_orders_read = task12_orders_read &&
            task12_capture(order, 0.55f, task12_cloud,
                           task12_orders[order - 1]);
    const float task12_luma[4]{luminance(task12_orders[0]),
                               luminance(task12_orders[1]),
                               luminance(task12_orders[2]),
                               luminance(task12_orders[3])};
    std::fprintf(stderr,
                 "task12 orders: %.7f %.7f %.7f %.7f generation=%llu\n",
                 task12_luma[0], task12_luma[1], task12_luma[2],
                 task12_luma[3],
                 static_cast<unsigned long long>(
                     renderer.volumetrics_resource_generation()));
    CHECK(task12_orders_read && std::isfinite(task12_luma[0]) &&
              task12_luma[0] > 0.0f &&
              task12_luma[1] > task12_luma[0] &&
              task12_luma[2] > task12_luma[1] &&
              task12_luma[3] > task12_luma[2] &&
              renderer.volumetrics_resource_generation() ==
                  task12_enhanced_generation,
          error.empty()
              ? "real enhanced scatter monotonically brightens bounded orders one through four without resource recreation"
              : error.c_str());

    matter::Float4 task12_order1_zero{}, task12_order1_full{};
    const bool task12_strength_read =
        task12_capture(1, 0.0f, task12_cloud, task12_order1_zero) &&
        task12_capture(1, 1.0f, task12_cloud, task12_order1_full);
    CHECK(task12_strength_read &&
              std::fabs(luminance(task12_order1_zero) -
                        luminance(task12_order1_full)) < 2.0e-4f,
          error.empty()
              ? "real order-one cloud lighting is independent of multiple-scattering strength"
              : error.c_str());

    matter::FogSettings task12_fog_only{};
    task12_fog_only.density = 0.004f;
    task12_fog_only.floor = -10000.0f;
    task12_fog_only.falloff = 100000.0f;
    task12_fog_only.color[0] = 0.21f;
    task12_fog_only.color[1] = 0.37f;
    task12_fog_only.color[2] = 0.58f;
    matter::Float4 task12_fog_order1{}, task12_fog_order4{};
    const bool task12_fog_read =
        task12_capture(1, 0.55f, task12_fog_only, task12_fog_order1) &&
        task12_capture(4, 0.55f, task12_fog_only, task12_fog_order4);
    matter::Float4 task12_fog_media{};
    float task12_fog_cloud_density = -1.0f;
    const bool task12_fog_density_read =
        renderer.readback_volumetrics_density_voxel_for_test(
            80u, 45u, 64u, task12_fog_media,
            task12_fog_cloud_density, error);
    CHECK(task12_fog_read && task12_fog_density_read &&
              task12_fog_cloud_density == 0.0f &&
              task12_fog_media.w > 0.0f &&
              luminance(task12_fog_order1) > 0.0f &&
              std::fabs(task12_fog_order1.x - task12_fog_order4.x) < 2.0e-4f &&
              std::fabs(task12_fog_order1.y - task12_fog_order4.y) < 2.0e-4f &&
              std::fabs(task12_fog_order1.z - task12_fog_order4.z) < 2.0e-4f,
          error.empty()
              ? "real FogLab-style no-cloud fixture retains haze with a zero cloud channel independent of cloud orders"
              : error.c_str());

    // Review fix: run the production enhanced scatter against a seeded,
    // nonuniform cumulative-tau texture. The near clipmap has 2 m slices;
    // one sunward and one receiverward slab each contribute tau=2. The center
    // local march contributes 0.625/m * 4 m = 2.5, so endpoint composition
    // must expose 4.5. Sampling the coarse field at the start would expose
    // 6.5 and fail independently of phase, sun RGB, or half precision.
    // Mostly camera-ward: the near sample exits through the real near plane
    // before reaching the existing smoke-scene triangle, while the center
    // sample still has enough depth for the complete four-metre march.
    const matter::Float3 task12_to_sun{0.1f, 0.25f, 0.9630680142f};
    task12_lighting.sun_direction = {-task12_to_sun.x, -task12_to_sun.y,
                                     -task12_to_sun.z};
    renderer.set_lighting(task12_lighting);
    matter::FogSettings task12_seeded_cloud = cloud_fog;
    task12_seeded_cloud.clouds[0].max_density = 0.625f;
    matter::VulkanVolumetricsSettings task12_tau_settings = improved_clouds;
    task12_tau_settings.temporal_blend = 0.0f;
    task12_tau_settings.local_sun_march_distance_m = 4.0f;
    task12_tau_settings.multiple_scattering_orders = 2;
    task12_tau_settings.multiple_scattering_strength = 0.0f;
    task12_tau_settings.powder_strength = 0.0f;
    matter::CloudShadowSettings task12_seeded_shadows{};
    task12_seeded_shadows.enabled = true;
    task12_seeded_shadows.near_resolution = 0;
    task12_seeded_shadows.near_depth_slices = 0;
    task12_seeded_shadows.near_coverage_m = 32.0f;
    task12_seeded_shadows.far_resolution = 0;
    task12_seeded_shadows.far_depth_slices = 0;
    task12_seeded_shadows.far_coverage_m = 64.0f;
    task12_seeded_shadows.filter_scale = 0.0f;
    task12_seeded_shadows.update_fraction = 1.0f;
    struct Task12TauPoint { uint32_t x, y, z; };
    const Task12TauPoint task12_tau_points[]{
        {80u, 45u, 50u},   // center, full four-metre local march
        {80u, 45u, 30u},   // just beyond the 1 m camera near boundary
        {159u, 45u, 50u},  // lateral edge, endpoint leaves the froxel grid
    };
    viewer::FroxelCameraReference task12_camera{};
    task12_camera.eye = viewer::volumetric_camera_eye(matrices.world_to_view);
    task12_camera.forward = {-matrices.world_to_view.m[8],
                             -matrices.world_to_view.m[9],
                             -matrices.world_to_view.m[10]};
    task12_camera.right = {matrices.world_to_view.m[0],
                           matrices.world_to_view.m[1],
                           matrices.world_to_view.m[2]};
    task12_camera.up = {matrices.world_to_view.m[4],
                        matrices.world_to_view.m[5],
                        matrices.world_to_view.m[6]};
    task12_camera.tan_half_fov = 1.0f / matrices.view_to_clip.m[5];
    task12_camera.aspect_ratio = matrices.view_to_clip.m[5] /
                                 matrices.view_to_clip.m[0];
    task12_camera.near_plane = matrices.view_to_clip.m[11] /
        (matrices.view_to_clip.m[10] + 1.0f);
    Task9CloudPoint task12_tau_world[3]{};
    float task12_expected_local[3]{};
    for (size_t index = 0; index < std::size(task12_tau_points); ++index) {
        const auto& point = task12_tau_points[index];
        task12_tau_world[index] = task9_froxel_world_position(
            matrices, point.x, point.y, point.z);
        const matter::Float3 world{task12_tau_world[index].x,
                                   task12_tau_world[index].y,
                                   task12_tau_world[index].z};
        const float exit_m = viewer::froxel_ray_exit_distance_reference(
            task12_camera, world, task12_to_sun);
        task12_expected_local[index] = 0.625f * std::min(4.0f, exit_m);
    }
    CHECK(task12_expected_local[0] > 2.49f &&
              task12_expected_local[1] < 1.0f &&
              task12_expected_local[2] < 0.1f,
          "center marches fully while near and lateral-edge endpoints leave the camera froxel grid");

    const auto task12_shadow_frame = matter::make_cloud_shadow_frame(
        matter::resolve_cloud_shadow_levels(task12_seeded_shadows)[0],
        camera.position, task12_lighting.sun_direction);
    const auto& center_world = task12_tau_world[0];
    const auto& shadow_matrix = task12_shadow_frame.world_to_uvw.m;
    const matter::Float3 center_uvw{
        shadow_matrix[0] * center_world.x +
            shadow_matrix[1] * center_world.y +
            shadow_matrix[2] * center_world.z + shadow_matrix[3],
        shadow_matrix[4] * center_world.x +
            shadow_matrix[5] * center_world.y +
            shadow_matrix[6] * center_world.z + shadow_matrix[7],
        shadow_matrix[8] * center_world.x +
            shadow_matrix[9] * center_world.y +
            shadow_matrix[10] * center_world.z + shadow_matrix[11]};
    const uint32_t receiver_slice = static_cast<uint32_t>(std::clamp(
        static_cast<int>(std::floor(center_uvw.z * 16.0f - 0.5f)) + 1,
        1, 14));
    renderer.set_cloud_shadow_density_layers_for_test(
        15u, 1.0f, receiver_slice, 1.0f, true);

    const auto capture_task12_scatter = [&](int local_steps,
                                             const matter::CloudShadowSettings& shadows,
                                             matter::Float4 samples[3]) {
        task12_tau_settings.local_sun_march_steps = local_steps;
        renderer.set_volumetrics_settings(task12_tau_settings,
                                          task12_seeded_cloud, shadows);
        bool rendered = true;
        for (int frame = 0; frame < 4; ++frame)
            rendered = rendered && warm_rt_tlas();
        for (size_t index = 0; index < std::size(task12_tau_points); ++index) {
            const auto& point = task12_tau_points[index];
            rendered = rendered &&
                renderer.readback_volumetrics_scatter_voxel_for_test(
                    point.x, point.y, point.z, samples[index], error);
        }
        return rendered;
    };
    matter::Float4 task12_tau_clear[3]{}, task12_tau_local[3]{},
                   task12_tau_shadowed[3]{};
    const bool task12_tau_read =
        capture_task12_scatter(0, task12_no_shadows, task12_tau_clear) &&
        capture_task12_scatter(8, task12_no_shadows, task12_tau_local) &&
        capture_task12_scatter(8, task12_seeded_shadows,
                               task12_tau_shadowed);
    float task12_inferred_local[3]{}, task12_inferred_total[3]{};
    bool task12_tau_finite[3]{task12_tau_read, task12_tau_read,
                              task12_tau_read};
    for (size_t index = 0; index < std::size(task12_tau_points); ++index) {
        const float clear = luminance(task12_tau_clear[index]);
        const float local = luminance(task12_tau_local[index]);
        const float shadowed = luminance(task12_tau_shadowed[index]);
        task12_tau_finite[index] = task12_tau_finite[index] && clear > 0.0f &&
            local > 0.0f && shadowed > 0.0f;
        task12_inferred_local[index] = -std::log(local / clear);
        task12_inferred_total[index] = -std::log(shadowed / clear);
    }
    std::fprintf(stderr,
                 "task12 seeded tau: center local=%.4f total=%.4f near=%.4f/%.4f alpha=%.4f edge=%.4f/%.4f receiver=%u\n",
                 task12_inferred_local[0], task12_inferred_total[0],
                 task12_inferred_local[1], task12_inferred_total[1],
                 task12_tau_shadowed[1].w,
                 task12_inferred_local[2], task12_inferred_total[2],
                 receiver_slice);
    CHECK(task12_tau_finite[0] &&
              std::fabs(task12_inferred_local[0] - 2.5f) < 0.15f &&
              std::fabs(task12_inferred_total[0] - 4.5f) < 0.2f &&
              std::fabs(task12_inferred_total[0] - 6.5f) > 1.0f,
          error.empty()
              ? "real seeded cumulative tau composes local 2.5 with endpoint 2 as 4.5 not start-overlapped 6.5"
              : error.c_str());
    CHECK(task12_tau_finite[1] &&
              task12_inferred_total[1] - task12_inferred_local[1] > 1.5f &&
              std::isfinite(task12_tau_clear[1].w) &&
              std::isfinite(task12_tau_local[1].w) &&
              std::isfinite(task12_tau_shadowed[1].w) &&
              task12_tau_clear[1].w > 0.6f &&
              std::fabs(task12_tau_local[1].w - task12_tau_clear[1].w) < 1e-3f &&
              std::fabs(task12_tau_shadowed[1].w - task12_tau_clear[1].w) < 1e-3f,
          error.empty()
              ? "near-camera production froxel remains populated across seeded shadow captures"
              : error.c_str());
    CHECK(task12_tau_finite[2] &&
              task12_inferred_total[2] - task12_inferred_local[2] > 1.5f,
          error.empty()
              ? "out-of-frustum lateral endpoint retains seeded coarse remainder"
              : error.c_str());

    // Task 13 RED/GREEN: the same seeded cumulative field must attenuate only
    // enhanced low-fog direct sun. Fog has no cloud-density channel here, so
    // this cannot pass by changing Task 12's cloud self-lighting term.
    const auto capture_task13_fog = [&] (
        const viewer::VkSceneLighting& fixture_lighting,
        const matter::CloudShadowSettings& shadows,
        matter::Float4& sample) {
        renderer.set_lighting(fixture_lighting);
        renderer.set_volumetrics_settings(task12_tau_settings,
                                          task12_fog_only, shadows);
        bool rendered = true;
        for (int frame = 0; frame < 4; ++frame)
            rendered = rendered && warm_rt_tlas();
        const auto& point = task12_tau_points[0];
        return rendered &&
            renderer.readback_volumetrics_scatter_voxel_for_test(
                point.x, point.y, point.z, sample, error);
    };
    matter::Float4 task13_fog_clear{}, task13_fog_shadowed{};
    const bool task13_fog_direct_read =
        capture_task13_fog(task12_lighting, task12_no_shadows,
                           task13_fog_clear) &&
        capture_task13_fog(task12_lighting, task12_seeded_shadows,
                           task13_fog_shadowed);
    const float task13_fog_clear_luma = luminance(task13_fog_clear);
    const float task13_fog_shadowed_luma = luminance(task13_fog_shadowed);
    CHECK(task13_fog_direct_read && task13_fog_clear_luma > 1.0e-5f &&
              task13_fog_shadowed_luma < task13_fog_clear_luma * 0.35f,
          error.empty()
              ? "overhead cumulative cloud slab attenuates enhanced low-fog direct sun"
              : error.c_str());

    viewer::VkSceneLighting task13_ambient_lighting = task12_lighting;
    task13_ambient_lighting.sun_intensity = 0.0f;
    task13_ambient_lighting.atmosphere_sources.authored_irradiance_chroma_rgb =
        {0.45f, 0.45f, 0.45f};
    matter::Float4 task13_fog_ambient_clear{}, task13_fog_ambient_shadowed{};
    const bool task13_fog_ambient_read =
        capture_task13_fog(task13_ambient_lighting, task12_no_shadows,
                           task13_fog_ambient_clear) &&
        capture_task13_fog(task13_ambient_lighting, task12_seeded_shadows,
                           task13_fog_ambient_shadowed);
    CHECK(task13_fog_ambient_read && luminance(task13_fog_ambient_clear) > 0.0f &&
              close4(task13_fog_ambient_clear, task13_fog_ambient_shadowed,
                     2.0e-4f),
          "enhanced fog cloud attenuation leaves evaluated SH ambient unchanged");

    renderer.set_cloud_shadow_density_layers_for_test(
        2u, 1.0f, 1u, 0.0f, true);
    matter::Float4 task13_fog_above{};
    const bool task13_fog_above_read = capture_task13_fog(
        task12_lighting, task12_seeded_shadows, task13_fog_above);
    CHECK(task13_fog_above_read &&
              std::fabs(luminance(task13_fog_above) -
                        task13_fog_clear_luma) <
                  task13_fog_clear_luma * 0.05f,
          "enhanced fog receiver above the cloud slab remains clear");
    matter::Float4 task13_fog_disabled{};
    const bool task13_fog_disabled_read = capture_task13_fog(
        task12_lighting, task12_no_shadows, task13_fog_disabled);
    CHECK(task13_fog_disabled_read &&
              std::fabs(luminance(task13_fog_disabled) -
                        task13_fog_clear_luma) <
                  task13_fog_clear_luma * 0.05f,
          "disabled cloud shadows restore enhanced low-fog direct sun");
    renderer.set_lighting(task12_lighting);
    renderer.clear_cloud_shadow_density_override_for_test(true);

    // Restore the Task 9 constant fog+cloud fixture expected by the following
    // stale-low-slice regression; Task 12's no-cloud isolation must not leak
    // into an older fixture.
    renderer.set_volumetrics_settings(improved_clouds, fog_plus_cloud);

    // Regression: the same enhanced bundle first writes a positive low-z
    // voxel with a permissive camera near, then must clear it when that slice
    // becomes invalid under a higher near plane.  Reallocation would hide the
    // stale-write bug, so only matrices change between the two dispatches.
    const viewer::FrameMatrices base_matrices = matrices;
    camera.near_plane = 0.01f;
    CHECK(viewer::build_frame_matrices(camera, 160, 100, matrices, error) &&
              warm_rt_tlas(),
          error.empty() ? "write permissive-near enhanced cloud voxel"
                        : error.c_str());
    matter::Float4 near_media{};
    float near_density = 0.0f;
    CHECK(renderer.readback_volumetrics_density_voxel_for_test(
              task9_x, task9_y, 0u, near_media, near_density, error) &&
              near_density > 0.0f,
          error.empty() ? "permissive near writes positive low-z enhanced density"
                        : error.c_str());
    camera.near_plane = 1.0f;
    matrices = base_matrices;
    CHECK(warm_rt_tlas(), error.empty() ? "clear high-near enhanced cloud voxel"
                                        : error.c_str());
    CHECK(renderer.readback_volumetrics_density_voxel_for_test(
              task9_x, task9_y, 0u, near_media, near_density, error) &&
              near_density == 0.0f && near_media.w == 0.0f,
          error.empty() ? "high near clears stale enhanced cloud density in-place"
                        : error.c_str());

    renderer.set_volumetrics_settings(current_clouds, cloud_fog);
    CHECK(warm_rt_tlas(), error.empty() ? "retire Improved cloud density"
                                        : error.c_str());
    CHECK(!renderer.volumetrics_cloud_density_allocated_for_test() &&
              renderer.volumetrics_grid_bytes_for_test() == 58982400u &&
              vulkan.validation_error_count() == 0,
          "Current-cost toggle retires cloud density without validation errors");
    rt_scenario_froxel_resize(vulkan, renderer, matrices, camera, error);
    CHECK(vulkan.validation_error_count() == 0,
          "isolated RT froxel resize has no Vulkan validation errors");
}

// ---------------------------------------------------------------------------
// Scenario: GPU A-trous denoising fixture (variance reduction, boundary, constant)
// ---------------------------------------------------------------------------
static void rt_scenario_atrous_denoising(RtPathContext& ctx) {
    viewer::VkSceneRenderer& renderer = ctx.renderer;
    std::string& error                = ctx.error;

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
}

// ---------------------------------------------------------------------------
// Scenario: GI history resets from RT/DLSS/lighting transitions and composite proof
// ---------------------------------------------------------------------------
static void rt_scenario_gi_history_resets(RtPathContext& ctx) {
    matter::VulkanDevice& vulkan      = ctx.vulkan;
    viewer::VkSceneRenderer& renderer = ctx.renderer;
    viewer::VkSceneLighting& lighting = ctx.lighting;
    matter::VulkanRayTracingSettings& enabled = ctx.enabled;
    viewer::FrameMatrices& matrices           = ctx.matrices;
    matter::CameraDesc& camera                = ctx.camera;
    viewer::TemporalFrame& gi_temporal        = ctx.gi_temporal;
    std::vector<MaterialGpuRecord>& gi_materials = ctx.gi_materials;
    uint32_t& retry_x                         = ctx.retry_x;
    uint32_t& retry_y                         = ctx.retry_y;
    std::string& error                        = ctx.error;
        const auto render_temporal_control = [&](uint64_t attempt_token,
                                                 bool reset = false,
                                                 bool presented = true) {
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
            renderer.finish_ray_tracing_frame(control.serial,
                                               rendered && presented);
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
        const uint64_t lighting_reset_baseline =
            renderer.test_gi_history_reset_count();
        viewer::VkSceneLighting changed_source = lighting;
        changed_source.atmosphere_sources.authored_irradiance_chroma_rgb.x *=
            0.5f;
        renderer.set_lighting(changed_source);
        CHECK(render_temporal_control(247, false, false) &&
                  renderer.test_gi_history_reset_count() ==
                      lighting_reset_baseline,
              error.empty() ? "failed presentation retains pending lighting reset"
                            : error.c_str());
        CHECK(render_temporal_control(248) &&
                  renderer.test_gi_history_reset_count() ==
                      lighting_reset_baseline + 1u,
              error.empty() ? "source lighting change resets GI history once"
                            : error.c_str());
        CHECK(render_temporal_control(249) &&
                  renderer.test_gi_history_reset_count() ==
                      lighting_reset_baseline + 1u,
              error.empty() ? "stable source lighting does not repeat GI reset"
                            : error.c_str());
        std::printf("lighting reset counts: before=%llu changed=%llu stable=%llu\n",
                    static_cast<unsigned long long>(lighting_reset_baseline),
                    static_cast<unsigned long long>(lighting_reset_baseline + 1u),
                    static_cast<unsigned long long>(
                        renderer.test_gi_history_reset_count()));
        renderer.set_display_exposure(1.0f);
        CHECK(render_temporal_control(250) &&
                  renderer.test_gi_history_reset_count() ==
                      lighting_reset_baseline + 1u,
              error.empty() ? "exposure-only change preserves GI history"
                            : error.c_str());
        const uint64_t world_transition_reset_baseline =
            renderer.test_gi_history_reset_count();
        renderer.set_lighting(lighting);
        CHECK(render_temporal_control(251, true) &&
                  renderer.test_gi_history_reset_count() ==
                      world_transition_reset_baseline + 1u,
              error.empty()
                  ? "world transition plus lighting reset invalidates history once"
                  : error.c_str());
        CHECK(render_temporal_control(252) &&
                  renderer.test_gi_history_reset_count() ==
                      world_transition_reset_baseline + 1u,
              error.empty()
                  ? "first post-transition frame does not double-reset history"
                  : error.c_str());
        matter::VulkanRayTracingSettings non_debug_rt = enabled;
        non_debug_rt.debug_view = false;
        renderer.set_ray_tracing_settings(non_debug_rt);
        CHECK(render_temporal_control(253),
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
        const auto atmosphere_status =
            renderer.test_resolved_atmosphere_status();
        const matter::Float3 normal{composite_pixel.normal.x,
                                    composite_pixel.normal.y,
                                    composite_pixel.normal.z};
        const auto sh_basis = [&](size_t index) {
            if (index == 0) return 0.282095f;
            if (index == 1) return 0.488603f * normal.y;
            if (index == 2) return 0.488603f * normal.z;
            if (index == 3) return 0.488603f * normal.x;
            if (index == 4) return 1.092548f * normal.x * normal.y;
            if (index == 5) return 1.092548f * normal.y * normal.z;
            if (index == 6)
                return 0.315392f * (3.0f * normal.z * normal.z - 1.0f);
            if (index == 7) return 1.092548f * normal.x * normal.z;
            return 0.546274f *
                   (normal.x * normal.x - normal.y * normal.y);
        };
        matter::Float3 sky_irradiance{};
        for (size_t index = 0; index < 9; ++index) {
            const float band = index == 0 ? 3.14159265359f
                               : index < 4 ? 2.0f * 3.14159265359f / 3.0f
                                           : 3.14159265359f / 4.0f;
            const float scale = sh_basis(index) * band;
            sky_irradiance.x += atmosphere_status.irradiance_sh[index].x * scale;
            sky_irradiance.y += atmosphere_status.irradiance_sh[index].y * scale;
            sky_irradiance.z += atmosphere_status.irradiance_sh[index].z * scale;
        }
        sky_irradiance.x = std::max(0.0f, sky_irradiance.x) *
                           atmosphere_status.sky_irradiance_modifier_rgb.x;
        sky_irradiance.y = std::max(0.0f, sky_irradiance.y) *
                           atmosphere_status.sky_irradiance_modifier_rgb.y;
        sky_irradiance.z = std::max(0.0f, sky_irradiance.z) *
                           atmosphere_status.sky_irradiance_modifier_rgb.z;
        const matter::Float3 expected_composite{
            composite_pixel.albedo.x * diffuse_scale * sky_irradiance.x *
                    composite_pixel.orm.z +
                composite_pixel.albedo.x * diffuse_scale * sun_base *
                    atmosphere_status.direct_world_sun_rgb.x *
                    composite_pixel.visibility.x +
                composite_pixel.albedo.x * emission_strength +
                composite_pixel.accumulated_diffuse.x +
                composite_pixel.accumulated_specular.x,
            composite_pixel.albedo.y * diffuse_scale * sky_irradiance.y *
                    composite_pixel.orm.z +
                composite_pixel.albedo.y * diffuse_scale * sun_base *
                    atmosphere_status.direct_world_sun_rgb.y *
                    composite_pixel.visibility.y +
                composite_pixel.albedo.y * emission_strength +
                composite_pixel.accumulated_diffuse.y +
                composite_pixel.accumulated_specular.y,
            composite_pixel.albedo.z * diffuse_scale * sky_irradiance.z *
                    composite_pixel.orm.z +
                composite_pixel.albedo.z * diffuse_scale * sun_base *
                    atmosphere_status.direct_world_sun_rgb.z *
                    composite_pixel.visibility.z +
                composite_pixel.albedo.z * emission_strength +
                composite_pixel.accumulated_diffuse.z +
                composite_pixel.accumulated_specular.z};
        CHECK(renderer.test_composite_uses_gi_temporal() &&
                  std::fabs(composite_pixel.hdr.x - expected_composite.x) < 0.04f &&
                  std::fabs(composite_pixel.hdr.y - expected_composite.y) < 0.04f &&
                  std::fabs(composite_pixel.hdr.z - expected_composite.z) < 0.04f,
              "GPU composite equation samples accumulated temporal radiance");
        auto emissive_materials = gi_materials;
        emissive_materials[0].emission_strength[3] = 4.0f;
        // rt_lighting.rgen multiplies by the authored emission *color*; supply
        // it the way material_registry.c would for a legacy emissive material
        // (normalized to albedo), or both bounces multiply by zero.
        emissive_materials[0].emission_strength[0] =
            emissive_materials[0].base_roughness[0];
        emissive_materials[0].emission_strength[1] =
            emissive_materials[0].base_roughness[1];
        emissive_materials[0].emission_strength[2] =
            emissive_materials[0].base_roughness[2];
        CHECK(renderer.update_materials(emissive_materials, 2, 1, error),
              error.empty() ? "install isolated emissive source"
                            : error.c_str());
        uint32_t emitter_x = 0;
        uint32_t emitter_y = 0;
        bool emitter_seen = false;
        for (uint32_t y = 10; y < 200 && !emitter_seen; y += 10) {
            for (uint32_t x = 10; x < 320; x += 10) {
                viewer::VkRasterPixel pixel{};
                if (renderer.readback_raster_pixel(x, y, pixel, error) &&
                    pixel.material_index == 0u) {
                    emitter_x = x;
                    emitter_y = y;
                    emitter_seen = true;
                    break;
                }
            }
        }
        const auto render_emission_probe = [&](float multiplier,
                                               uint64_t attempt_token,
                                               viewer::VkRasterPixel& emitter,
                                               viewer::VkRasterPixel& receiver) {
            viewer::VkSceneLighting isolated = lighting;
            isolated.authored_sun_rgb = {};
            isolated.atmosphere_sources.authored_display_sky_chroma_rgb = {};
            isolated.atmosphere_sources.authored_irradiance_chroma_rgb = {};
            isolated.emission_multiplier = multiplier;
            renderer.set_lighting(isolated);
            const bool rendered = render_temporal_control(attempt_token);
            return rendered && emitter_seen &&
                renderer.readback_raster_pixel(emitter_x, emitter_y, emitter,
                                               error) &&
                renderer.readback_raster_pixel(retry_x, retry_y, receiver,
                                               error);
        };
        viewer::VkRasterPixel zero_emission{};
        viewer::VkRasterPixel zero_receiver{};
        viewer::VkRasterPixel half_emission{};
        viewer::VkRasterPixel half_receiver{};
        viewer::VkRasterPixel full_emission{};
        viewer::VkRasterPixel full_receiver{};
        const bool emission_probes_ok =
            render_emission_probe(0.0f, 253, zero_emission, zero_receiver) &&
            render_emission_probe(0.5f, 254, half_emission, half_receiver) &&
            render_emission_probe(1.0f, 255, full_emission, full_receiver);
        const auto luminance = [](matter::Float4 value) {
            return 0.2126f * value.x + 0.7152f * value.y + 0.0722f * value.z;
        };
        const auto relative_error = [](float actual, float expected) {
            return std::fabs(actual - expected) /
                std::max(std::fabs(expected), 1e-5f);
        };
        const float primary_half = luminance(half_emission.hdr);
        const float primary_full = luminance(full_emission.hdr);
        const float secondary_half = luminance(half_receiver.raw_diffuse);
        const float secondary_full = luminance(full_receiver.raw_diffuse);
        std::printf("emission ratios: primary=%.3f secondary=%.3f\n",
                    primary_half > 0.0f ? primary_full / primary_half : 0.0f,
                    secondary_half > 0.0f ? secondary_full / secondary_half
                                          : 0.0f);
        CHECK(emission_probes_ok &&
                  luminance(zero_emission.hdr) < primary_half &&
                  relative_error(primary_full, 2.0f * primary_half) < 0.05f,
              "primary emission follows authored multiplier");
        CHECK(emission_probes_ok &&
                  luminance(zero_receiver.raw_diffuse) < secondary_half &&
                  relative_error(secondary_full, 2.0f * secondary_half) < 0.10f,
              "secondary emissive bounce follows authored multiplier");
        CHECK(renderer.update_materials(gi_materials, 3, 1, error),
              error.empty() ? "restore non-emissive RT material fixture"
                            : error.c_str());
        renderer.set_lighting(lighting);
        renderer.set_ray_tracing_settings(enabled);
}

// ---------------------------------------------------------------------------
// Scenario: secondary sun visibility (unblocked vs blocked secondary-hit ray)
// ---------------------------------------------------------------------------
static void rt_scenario_secondary_sun_visibility(RtPathContext& ctx) {
    matter::VulkanDevice& vulkan      = ctx.vulkan;
    viewer::VkSceneRenderer& renderer = ctx.renderer;
    viewer::VkSceneLighting& lighting = ctx.lighting;
    viewer::FrameMatrices& matrices           = ctx.matrices;
    matter::CameraDesc& camera                = ctx.camera;
    viewer::TemporalFrame& gi_temporal        = ctx.gi_temporal;
    uint32_t& retry_x                         = ctx.retry_x;
    uint32_t& retry_y                         = ctx.retry_y;
    std::string& error                        = ctx.error;
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
        viewer::VkScenePart sun_blocker = rt_horizontal_part(
            924, 2.0f, 20.0f, {0.0f, -1.0f, 0.0f}, 2, 1.0f);
        CHECK(renderer.ensure_part(sun_blocker, error) >= 0 &&
                  renderer.update_instances({{920, identity_matrix()},
                                             {921, identity_matrix()},
                                             {924, identity_matrix()}},
                                            error),
              error.empty() ? "add secondary-sun blocker"
                            : error.c_str());
        matter::Float4 blocked_sun_zero{};
        matter::Float4 blocked_sun_high{};
        const bool blocked_probes_ok =
            render_sun_probe(0.0f, 301, blocked_sun_zero) &&
            render_sun_probe(100.0f, 302, blocked_sun_high);
        const float blocked_sun_delta =
            rgb_delta(blocked_sun_zero, blocked_sun_high);
        std::printf("secondary sun: blocked_delta=%.6f unblocked_delta=%.6f "
                    "| bz=%.5f %.5f %.5f bh=%.5f %.5f %.5f "
                    "uz=%.5f %.5f %.5f uh=%.5f %.5f %.5f\n",
                    blocked_sun_delta, unblocked_sun_delta,
                    blocked_sun_zero.x, blocked_sun_zero.y, blocked_sun_zero.z,
                    blocked_sun_high.x, blocked_sun_high.y, blocked_sun_high.z,
                    unblocked_sun_zero.x, unblocked_sun_zero.y,
                    unblocked_sun_zero.z, unblocked_sun_high.x,
                    unblocked_sun_high.y, unblocked_sun_high.z);
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
        float& minimum_visibility     = ctx.minimum_visibility;
        float& maximum_visibility     = ctx.maximum_visibility;
        bool&  receiver_seen          = ctx.receiver_seen;
        float& receiver_min_visibility = ctx.receiver_min_visibility;
        float& receiver_max_visibility = ctx.receiver_max_visibility;
        minimum_visibility    = 1.0f;
        maximum_visibility    = 0.0f;
        receiver_seen         = false;
        receiver_min_visibility = 1.0f;
        receiver_max_visibility = 0.0f;
        bool visibility_reads_ok = true;
        bool debug_output_matches = true;
        matter::Float4 strongest_receiver_raw{};
        matter::Float4 strongest_receiver_specular{};
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
                // Covered pixels only. composite.frag's normal-length miss test
                // returns the sky before every debug branch (the vol_debug_view
                // views sit behind the same early-out), so the debug view is a
                // surface visualization by construction; an uncovered pixel
                // shows sky, not the cleared visibility texture.
                if (pixel.material_index != UINT32_MAX)
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
}

// ---------------------------------------------------------------------------
// Scenario: GGX mirror, rough-metal, specular tint, and clearcoat lobes
// ---------------------------------------------------------------------------
static void rt_scenario_mirror_specular(RtPathContext& ctx) {
    matter::VulkanDevice& vulkan      = ctx.vulkan;
    viewer::VkSceneRenderer& renderer = ctx.renderer;
    matter::VulkanRayTracingSettings& enabled        = ctx.enabled;
    viewer::FrameMatrices& matrices                  = ctx.matrices;
    matter::CameraDesc& camera                       = ctx.camera;
    viewer::TemporalFrame& gi_temporal               = ctx.gi_temporal;
    std::vector<MaterialGpuRecord>& gi_materials     = ctx.gi_materials;
    std::string& error                               = ctx.error;
    const auto render_temporal_control = [&](uint64_t attempt_token,
                                             bool reset = false,
                                             bool presented = true) {
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
        renderer.finish_ray_tracing_frame(control.serial,
                                           rendered && presented);
        return rendered;
    };
        renderer.release_part(921);
        gi_materials[1].metal_opacity_spec_coat[0] = 1.0f;
        gi_materials[1].metal_opacity_spec_coat[3] = 0.0f;
        gi_materials[1].base_roughness[3] = 0.02f;
        gi_materials[1].specular_tint_coat_roughness[0] = 1.0f;
        gi_materials[1].specular_tint_coat_roughness[1] = 1.0f;
        gi_materials[1].specular_tint_coat_roughness[2] = 1.0f;
        CHECK(renderer.ensure_part(rt_horizontal_part(925, 0.0f, 0.55f,
                                              {0.0f, -0.3162278f, 0.9486833f},
                                              1, 1.0f), error) >= 0 &&
                  renderer.update_materials(gi_materials, 10, 1, error) &&
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
        // Published alongside the pair below (the pair's shape is relied on by
        // the tint/clearcoat checks further down): how many receiver pixels the
        // scan sampled, and their mean max-channel specular energy. The lit
        // count alone cannot describe a lobe on this fixture -- see the
        // roughness check below.
        uint32_t receiver_total = 0;
        double receiver_mean_energy = 0.0;
        const auto specular_coverage = [&]() {
            uint32_t count = 0;
            uint32_t total = 0;
            double energy_sum = 0.0;
            matter::Float4 peak{};
            for (uint32_t sy = 20; sy < 200; sy += 10)
                for (uint32_t sx = 20; sx < 320; sx += 10) {
                    viewer::VkRasterPixel pixel{};
                    if (!renderer.readback_raster_pixel(sx, sy, pixel, error) ||
                        pixel.material_index != 1u)
                        continue;
                    ++total;
                    const float energy = std::max(pixel.raw_specular.x,
                        std::max(pixel.raw_specular.y, pixel.raw_specular.z));
                    energy_sum += energy;
                    if (energy > 0.001f) ++count;
                    if (energy > std::max(peak.x, std::max(peak.y, peak.z)))
                        peak = pixel.raw_specular;
                }
            receiver_total = total;
            receiver_mean_energy = total ? energy_sum / total : 0.0;
            return std::pair<uint32_t, matter::Float4>{count, peak};
        };
        const auto peak_energy = [](const matter::Float4& value) {
            return std::max(value.x, std::max(value.y, value.z));
        };
        const auto mirror_stats = specular_coverage();
        const uint32_t mirror_receiver_total = receiver_total;
        const double mirror_mean = receiver_mean_energy;
        const float mirror_peak = peak_energy(mirror_stats.second);
        gi_materials[1].base_roughness[3] = 0.65f;
        CHECK(renderer.update_materials(gi_materials, 11, 1, error) &&
                  render_temporal_control(305),
              error.empty() ? "render rough-metal broadening fixture"
                            : error.c_str());
        const auto rough_metal_stats = specular_coverage();
        const double rough_mean = receiver_mean_energy;
        const float rough_peak = peak_energy(rough_metal_stats.second);
        std::printf("specular lobe: mirror lit=%u/%u peak=%.6f mean=%.6f "
                    "p/m=%.4f | rough lit=%u/%u peak=%.6f mean=%.6f p/m=%.4f\n",
                    mirror_stats.first, mirror_receiver_total, mirror_peak,
                    mirror_mean,
                    mirror_mean > 0.0 ? mirror_peak / mirror_mean : -1.0,
                    rough_metal_stats.first, receiver_total, rough_peak,
                    rough_mean,
                    rough_mean > 0.0 ? rough_peak / rough_mean : -1.0);
        // Broadening cannot be read off the lit-pixel count here: the mirror
        // already lights the receiver's whole sampled footprint (8/8), so the
        // count is saturated and can only fall. It asserted
        // rough_lit >= mirror_lit, which no widening could ever satisfy.
        //
        // What this fixture can show, and what a broken GGX normalization would
        // break, is the pair of invariants below: reflected energy over the
        // receiver is conserved as roughness goes 0.02 -> 0.65, while the
        // radiance stops being uniform across it. The sharp lobe reflects the
        // same patch of the target from every receiver pixel (peak == mean,
        // p/m == 1.0); the wide lobe integrates a different slice per pixel, so
        // per-pixel radiance scatters (p/m ~ 2.6) even though the mean holds.
        CHECK(std::isfinite(rough_metal_stats.second.x) &&
                  std::isfinite(rough_metal_stats.second.y) &&
                  std::isfinite(rough_metal_stats.second.z) &&
                  // the wide lobe must not go dark over the receiver
                  rough_metal_stats.first * 2u >= mirror_receiver_total &&
                  // energy conserved, not amplified or swallowed
                  rough_mean > mirror_mean * 0.5 &&
                  rough_mean < mirror_mean * 1.5 &&
                  // sharp lobe uniform across the receiver, wide lobe not
                  mirror_peak <= mirror_mean * 1.05 &&
                  rough_peak > rough_mean * 1.5,
              "rough metal spreads the reflected lobe and conserves finite energy");
        gi_materials[1].metal_opacity_spec_coat[0] = 0.0f;
        gi_materials[1].base_roughness[3] = 0.35f;
        CHECK(renderer.update_materials(gi_materials, 12, 1, error) &&
                  render_temporal_control(306),
              error.empty() ? "render untinted dielectric baseline"
                            : error.c_str());
        const auto untinted_dielectric_stats = specular_coverage();
        gi_materials[1].specular_tint_coat_roughness[0] = 0.01f;
        gi_materials[1].specular_tint_coat_roughness[1] = 1.0f;
        gi_materials[1].specular_tint_coat_roughness[2] = 0.01f;
        CHECK(renderer.update_materials(gi_materials, 13, 1, error) &&
                  render_temporal_control(307),
              error.empty() ? "render tinted dielectric fixture"
                            : error.c_str());
        const auto tinted_stats = specular_coverage();
        const float mirror_red_green = untinted_dielectric_stats.second.x /
            std::max(untinted_dielectric_stats.second.y, 1e-6f);
        const float tinted_red_green = tinted_stats.second.x /
            std::max(tinted_stats.second.y, 1e-6f);
        CHECK(tinted_stats.first > 0u && tinted_stats.second.y > 0.0f &&
                  tinted_red_green < mirror_red_green,
              "dielectric GGX uses authored specular tint");
        gi_materials[1].specular_tint_coat_roughness[0] = 1.0f;
        gi_materials[1].specular_tint_coat_roughness[1] = 1.0f;
        gi_materials[1].specular_tint_coat_roughness[2] = 1.0f;
        gi_materials[1].base_roughness[3] = 0.8f;
        CHECK(renderer.update_materials(gi_materials, 14, 1, error) &&
                  render_temporal_control(308),
              error.empty() ? "render rough dielectric F0 fixture"
                            : error.c_str());
        const auto rough_dielectric_stats = specular_coverage();
        CHECK(rough_dielectric_stats.first > 0u &&
                  std::isfinite(rough_dielectric_stats.second.x) &&
                  std::fabs(rough_dielectric_stats.second.w - 0.04f) < 0.002f,
              "rough dielectric retains numeric nonmetal F0 near 0.04");
        gi_materials[1].specular_tint_coat_roughness[0] = 1.0f;
        gi_materials[1].specular_tint_coat_roughness[1] = 0.05f;
        gi_materials[1].specular_tint_coat_roughness[2] = 0.05f;
        gi_materials[1].metal_opacity_spec_coat[3] = 0.0f;
        CHECK(renderer.update_materials(gi_materials, 15, 1, error) &&
                  render_temporal_control(309),
              error.empty() ? "render clearcoat-off red-base fixture"
                            : error.c_str());
        const auto coat_off_stats = specular_coverage();
        uint32_t coat_off_base_samples = 0;
        uint32_t coat_off_coat_samples = 0;
        CHECK(renderer.test_readback_reflection_sample_counts(
                  coat_off_base_samples, coat_off_coat_samples, error) &&
                  coat_off_base_samples > 0u && coat_off_coat_samples == 0u,
              error.empty()
                  ? "clearcoat zero launches no GPU coat samples"
                  : error.c_str());
        gi_materials[1].metal_opacity_spec_coat[3] = 1.0f;
        gi_materials[1].specular_tint_coat_roughness[3] = 0.08f;
        CHECK(renderer.update_materials(gi_materials, 16, 1, error) &&
                  render_temporal_control(310),
              error.empty() ? "render clearcoat second-lobe fixture"
                            : error.c_str());
        const auto coat_stats = specular_coverage();
        uint32_t coat_on_base_samples = 0;
        uint32_t coat_on_coat_samples = 0;
        CHECK(renderer.test_readback_reflection_sample_counts(
                  coat_on_base_samples, coat_on_coat_samples, error) &&
                  coat_on_base_samples > 0u && coat_on_coat_samples > 0u,
              error.empty()
                  ? "clearcoat one launches both normalized GPU lobes"
                  : error.c_str());
        const float coat_off_blue_ratio = coat_off_stats.second.z /
            std::max(coat_off_stats.second.x, 1e-6f);
        const float coat_on_blue_ratio = coat_stats.second.z /
            std::max(coat_stats.second.x, 1e-6f);
        CHECK(coat_stats.first > 0u &&
                  coat_stats.second.x > 0.0f && coat_stats.second.y > 0.0f &&
                  coat_stats.second.z > 0.0f &&
                  coat_on_blue_ratio > coat_off_blue_ratio,
              "clearcoat adds a distinct untinted dielectric highlight over the tinted base");
        const uint64_t fallback_reset_baseline =
            renderer.test_gi_history_reset_count();
        matter::VulkanRayTracingSettings rt_disabled = enabled;
        rt_disabled.enabled = false;
        renderer.set_ray_tracing_settings(rt_disabled);
        CHECK(render_temporal_control(320),
              error.empty() ? "render disabled-RT stale-reflection fixture"
                            : error.c_str());
        const auto disabled_specular = specular_coverage();
        CHECK(disabled_specular.first == 0u &&
                  disabled_specular.second.x == 0.0f &&
                  disabled_specular.second.y == 0.0f &&
                  disabled_specular.second.z == 0.0f,
              "RT disable clears raw and filtered reflection signals");
        renderer.set_ray_tracing_settings(enabled);
        CHECK(render_temporal_control(321) &&
                  renderer.test_gi_history_reset_count() ==
                      fallback_reset_baseline + 1u,
              error.empty() ? "RT re-enable resets reflection history once"
                            : error.c_str());
        renderer.test_force_rt_unavailable(true);
        CHECK(render_temporal_control(322),
              error.empty() ? "render forced-RT-unavailable fallback"
                            : error.c_str());
        const auto unavailable_specular = specular_coverage();
        CHECK(unavailable_specular.first == 0u,
              "RT unavailable clears prior filtered reflection signal");
        renderer.test_force_rt_unavailable(false);
        CHECK(renderer.update_instances({}, error) &&
                  render_temporal_control(323),
              error.empty() ? "render empty-instance reflection fallback"
                            : error.c_str());
        viewer::VkRasterPixel empty_specular{};
        CHECK(renderer.readback_raster_pixel(160, 100, empty_specular, error) &&
                  close4(empty_specular.raw_specular, {}, 1e-6f) &&
                  close4(empty_specular.accumulated_specular, {}, 1e-6f),
              error.empty()
                  ? "empty RT scene exposes zero raw and accumulated reflection"
                  : error.c_str());
        CHECK(renderer.update_instances(
                  {{920, identity_matrix()}, {925, identity_matrix()}}, error) &&
                  render_temporal_control(324) &&
                  renderer.test_gi_history_reset_count() ==
                      fallback_reset_baseline + 2u,
              error.empty()
                  ? "restoring RT instances resets stale reflection history once"
                  : error.c_str());
}

// ---------------------------------------------------------------------------
// Scenario: baked-AO-zero GI suppression and GI disable with active RT
// ---------------------------------------------------------------------------
static void rt_scenario_baked_ao_and_gi_disable(RtPathContext& ctx) {
    matter::VulkanDevice& vulkan      = ctx.vulkan;
    viewer::VkSceneRenderer& renderer = ctx.renderer;
    viewer::FrameMatrices& matrices   = ctx.matrices;
    matter::CameraDesc& camera        = ctx.camera;
    matter::VulkanGiSettings& gi      = ctx.gi;
    bool&  receiver_seen              = ctx.receiver_seen;
    float& receiver_min_visibility    = ctx.receiver_min_visibility;
    float& receiver_max_visibility    = ctx.receiver_max_visibility;
    float& minimum_visibility         = ctx.minimum_visibility;
    float& maximum_visibility         = ctx.maximum_visibility;
    std::string& error                = ctx.error;
        renderer.release_part(925);
        CHECK(renderer.ensure_part(rt_horizontal_part(921, 0.0f, 0.55f,
                                              {0.0f, -1.0f, 0.0f}, 1, 1.0f),
                                   error) >= 0 &&
                  renderer.update_instances(
                      {{920, identity_matrix()}, {921, identity_matrix()}},
                      error),
              error.empty() ? "restore diffuse receiver after mirror fixture"
                            : error.c_str());
        renderer.release_part(921);
        CHECK(renderer.ensure_part(rt_horizontal_part(922, 0.0f, 0.55f,
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
        CHECK(renderer.ensure_part(rt_horizontal_part(923, 0.0f, 0.55f,
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

// ---------------------------------------------------------------------------
// Driver: run all native RT path scenarios in original order
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// RT PBR Phase 1 — microfacet (frosted) transmission + correctness fixes.
// MATTER_VK_SMOKE_MODE=rt-transmission.
//
// Fixture: a two-quad glass slab (front face at z=-2, back face at z=-2.2)
// seen by a 90-degree camera at the origin, with a large wall at z=-6 behind
// it. The wall is authored either as alternating 0.6 m white/dark stripes
// (blur measurements) or as a uniform white field (energy measurements) by
// re-authoring the dark stripe material -- same geometry, different table.
//
// Covered gates:
//   * blur grows monotonically with authored glass roughness (accumulated
//     transmission stddev across a stripe row strictly decreases);
//   * transmitted energy at the slab is within 2% of the smooth slab
//     (uniform wall, where hit radiance is direction-independent, so the
//     comparison isolates the perturbation math);
//   * smooth glass byte-parity: roughness 0.0 and 0.019 (both below the 0.02
//     sampling threshold) produce bit-identical raw transmission, and the
//     denoiser chain passes smooth pixels through bit-exactly
//     (accumulated == raw);
//   * determinism: equal presented_frame_index => bit-identical raw signal;
//   * the composite fallback guard: a zeroed absorptionColor through the
//     coverage < 0.01 branch is non-black and matches a white absorption
//     (the black-glass regression test that never existed);
//   * alpha-tested occluders in the walk: with the two-mask walk (default) a
//     failing alpha-test card between slab and wall is skipped; with
//     MATTER_RT_WALK_ALPHA_TEST=0 it silhouettes the refraction (legacy),
//     and the RT gpu-zone cost of both is printed for the ~5% budget check.
// ---------------------------------------------------------------------------
static float rt_trans_luminance(const matter::Float4& v) {
    return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z;
}

static viewer::VkScenePart rt_trans_quad(uint64_t hash, float x0, float x1,
                                         float y0, float y1, float z,
                                         float nz, uint32_t material) {
    viewer::VkScenePart part = fixed_part(
        hash, {x0, y0, z - 0.01f}, {x1, y1, z + 0.01f}, 0);
    const matter::Float3 normal{0.0f, 0.0f, nz};
    const matter::Float4 tint{1.0f, 1.0f, 1.0f, 0.0f};
    const matter::Float4 surf{0.0f, 0.0f, 1.0f, 1.0f};
    part.vertices = {
        {{x0, y0, z}, normal, tint, surf, material, {}},
        {{x1, y0, z}, normal, tint, surf, material, {}},
        {{x1, y1, z}, normal, tint, surf, material, {}},
        {{x0, y1, z}, normal, tint, surf, material, {}}};
    // Winding decides gl_HitKindEXT: +z quads face the camera (front hits for
    // camera rays), -z quads face away so the refraction walk's forward ray
    // reaches them as BACKFACES -- the exit-refraction event under test.
    if (nz > 0.0f)
        part.indices = {0, 1, 2, 0, 2, 3};
    else
        part.indices = {0, 2, 1, 0, 3, 2};
    part.clusters[0].lods[0] = {0, 6, 0.0f};
    return part;
}

void run_rt_transmission_path(matter::VulkanDevice& vulkan) {
    if (!vulkan.ray_tracing_available()) {
        std::printf("rt-transmission: ray tracing unavailable, skipping\n");
        return;
    }
    constexpr uint32_t width = 320;
    constexpr uint32_t height = 200;
    constexpr uint32_t kGlass = 1;
    constexpr uint32_t kWhite = 2;
    constexpr uint32_t kDark = 3;
    constexpr uint32_t kFoliage = 4;
    constexpr uint32_t kMaterialAlphaTested = 1u << 2u;
    std::string error;

    std::vector<MaterialGpuRecord> materials;
    uint64_t shading_revision = 1;
    const auto author = [&](float glass_roughness, float dark_albedo,
                            matter::Float3 glass_absorption) {
        materials.assign(5, MaterialGpuRecord{});
        materials[0].metal_opacity_spec_coat[1] = 1.0f;
        materials[0].scattering_shape[3] = 1.0f;
        materials[kGlass].base_roughness[0] = 1.0f;
        materials[kGlass].base_roughness[1] = 1.0f;
        materials[kGlass].base_roughness[2] = 1.0f;
        materials[kGlass].base_roughness[3] = glass_roughness;
        materials[kGlass].metal_opacity_spec_coat[1] = 1.0f;
        materials[kGlass].transmission[0] = 1.0f;   // transmission
        materials[kGlass].transmission[1] = 1.5f;   // ior
        materials[kGlass].transmission[2] = 0.2f;   // thickness fallback
        materials[kGlass].transmission[3] = 0.0f;   // absorption distance
        materials[kGlass].absorption_pad[0] = glass_absorption.x;
        materials[kGlass].absorption_pad[1] = glass_absorption.y;
        materials[kGlass].absorption_pad[2] = glass_absorption.z;
        materials[kWhite].base_roughness[0] = 0.85f;
        materials[kWhite].base_roughness[1] = 0.85f;
        materials[kWhite].base_roughness[2] = 0.85f;
        materials[kWhite].base_roughness[3] = 0.6f;
        materials[kWhite].metal_opacity_spec_coat[1] = 1.0f;
        materials[kWhite].scattering_shape[3] = 1.0f;
        materials[kDark] = materials[kWhite];
        materials[kDark].base_roughness[0] = dark_albedo;
        materials[kDark].base_roughness[1] = dark_albedo;
        materials[kDark].base_roughness[2] = dark_albedo;
        materials[kFoliage].base_roughness[0] = 0.8f;
        materials[kFoliage].base_roughness[1] = 0.05f;
        materials[kFoliage].base_roughness[2] = 0.05f;
        materials[kFoliage].base_roughness[3] = 0.5f;
        materials[kFoliage].metal_opacity_spec_coat[1] = 0.3f;  // opacity
        materials[kFoliage].scattering_shape[2] = 0.5f;         // cutoff
        materials[kFoliage].scattering_shape[3] = 1.0f;
        materials[kFoliage].flags_misc[0] = kMaterialAlphaTested;
    };

    // Shared scene builder so the legacy-walk renderer sees the same world.
    const auto build_scene = [&](viewer::VkSceneRenderer& renderer,
                                 bool with_foliage) {
        CHECK(renderer.ensure_part(rt_trans_quad(7801, -2.4f, 2.4f, -1.4f,
                                                 1.4f, -2.0f, 1.0f, kGlass),
                                   error) >= 0,
              error.empty() ? "rt-transmission: slab front" : error.c_str());
        CHECK(renderer.ensure_part(rt_trans_quad(7802, -2.4f, 2.4f, -1.4f,
                                                 1.4f, -2.2f, -1.0f, kGlass),
                                   error) >= 0,
              error.empty() ? "rt-transmission: slab back" : error.c_str());
        // Striped wall: 80 stripes of 0.6 m across x in [-24, 24].
        viewer::VkScenePart wall = fixed_part(
            7803, {-24.0f, -16.0f, -6.01f}, {24.0f, 16.0f, -5.99f}, 0);
        wall.vertices.clear();
        wall.indices.clear();
        const matter::Float3 wall_normal{0.0f, 0.0f, 1.0f};
        const matter::Float4 tint{1.0f, 1.0f, 1.0f, 0.0f};
        const matter::Float4 surf{0.0f, 0.0f, 1.0f, 1.0f};
        for (int stripe = 0; stripe < 80; ++stripe) {
            const float x0 = -24.0f + 0.6f * static_cast<float>(stripe);
            const float x1 = x0 + 0.6f;
            const uint32_t material = (stripe & 1) ? kDark : kWhite;
            const uint32_t base = static_cast<uint32_t>(wall.vertices.size());
            wall.vertices.push_back(
                {{x0, -16.0f, -6.0f}, wall_normal, tint, surf, material, {}});
            wall.vertices.push_back(
                {{x1, -16.0f, -6.0f}, wall_normal, tint, surf, material, {}});
            wall.vertices.push_back(
                {{x1, 16.0f, -6.0f}, wall_normal, tint, surf, material, {}});
            wall.vertices.push_back(
                {{x0, 16.0f, -6.0f}, wall_normal, tint, surf, material, {}});
            for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
                wall.indices.push_back(base + index);
        }
        wall.clusters[0].lods[0] = {
            0, static_cast<uint32_t>(wall.indices.size()), 0.0f};
        CHECK(renderer.ensure_part(wall, error) >= 0,
              error.empty() ? "rt-transmission: striped wall" : error.c_str());
        std::vector<viewer::VkSceneInstance> instances = {
            {7801, identity_matrix()},
            {7802, identity_matrix()},
            {7803, identity_matrix()}};
        if (with_foliage) {
            // Alpha-tested card between slab and wall: opacity 0.3 < cutoff
            // 0.5, so a correct alpha test skips it entirely.
            CHECK(renderer.ensure_part(rt_trans_quad(7804, -1.2f, 1.2f, -1.0f,
                                                     1.0f, -4.0f, 1.0f,
                                                     kFoliage),
                                       error) >= 0,
                  error.empty() ? "rt-transmission: foliage card"
                                : error.c_str());
            instances.push_back({7804, identity_matrix()});
        }
        CHECK(renderer.update_instances(instances, error),
              error.empty() ? "rt-transmission: upload instances"
                            : error.c_str());
    };

    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 100.0f;
    viewer::FrameMatrices matrices{};
    CHECK(viewer::build_frame_matrices(camera, width, height, matrices, error),
          error.empty() ? "rt-transmission: matrices" : error.c_str());
    viewer::VkSceneLighting lighting{};
    lighting.sun_direction = {0.2f, -0.4f, -0.9f};
    matter::VulkanRayTracingSettings rt_enabled{};
    rt_enabled.enabled = true;
    rt_enabled.max_distance = 100.0f;
    rt_enabled.bias = 0.001f;
    rt_enabled.samples = 1;
    matter::VulkanGiSettings gi{};
    gi.enabled = 1;
    gi.max_bounces = 1;
    gi.samples_per_pixel = 1;
    gi.trace_scale = 1.0f;

    viewer::TemporalFrame temporal{};
    temporal.current_unjittered = matrices;
    temporal.previous_unjittered = matrices;
    temporal.current_jittered = matrices;
    temporal.previous_jittered = matrices;
    temporal.internal_extent = {width, height};
    temporal.output_extent = {width, height};
    uint64_t attempt_token = 5000;
    uint64_t presented_index = 1;

    const auto configure = [&](viewer::VkSceneRenderer& renderer) {
        renderer.set_lighting(lighting);
        renderer.set_ray_tracing_settings(rt_enabled);
        renderer.set_gi_settings(gi);
    };
    const auto render_frame = [&](viewer::VkSceneRenderer& renderer,
                                  bool reset, uint64_t frame_index) {
        temporal.reset = reset;
        temporal.attempt_token = ++attempt_token;
        temporal.presented_frame_index = frame_index;
        renderer.set_temporal_frame(temporal);
        matter::VulkanFrame frame{};
        std::string local;
        const bool ok = vulkan.begin_frame(frame, local) &&
            renderer.prepare_frame(frame, matrices, camera.position, 1.0f,
                                   local) &&
            renderer.record_cull_and_render(frame, matrices, camera.position,
                                            1.0f, local) &&
            renderer.record_composite_to_swapchain(frame, local) &&
            vulkan.end_frame(frame, local);
        renderer.finish_ray_tracing_frame(frame.serial, ok);
        CHECK(ok, local.empty() ? "rt-transmission: render frame"
                                : local.c_str());
    };
    // Slab pixels: screen x in [40, 280], y in [30, 170]; sample the center
    // row well inside the slab.
    constexpr uint32_t kRowY = 100;
    constexpr uint32_t kRowX0 = 64;
    constexpr uint32_t kRowX1 = 256;
    const auto sample_row = [&](viewer::VkSceneRenderer& renderer,
                                uint32_t step, bool accumulated) {
        std::vector<matter::Float4> row;
        for (uint32_t x = kRowX0; x <= kRowX1; x += step) {
            viewer::VkRasterPixel pixel{};
            CHECK(renderer.readback_raster_pixel(x, kRowY, pixel, error),
                  error.empty() ? "rt-transmission: row readback"
                                : error.c_str());
            CHECK(pixel.material_index == kGlass,
                  "rt-transmission: row probe lands on the glass slab");
            row.push_back(accumulated ? pixel.accumulated_transmission
                                      : pixel.raw_transmission);
        }
        return row;
    };
    // Stripe contrast: mean luminance over pixels whose straight-through ray
    // lands on a white stripe minus the mean over dark-stripe pixels. Class
    // means average pixel noise across ~half the row each, so the metric
    // tracks BLUR (cross-stripe mixing) instead of residual 1-spp noise --
    // a plain row stddev reads the noise floor as "contrast" at high
    // roughness. Pixels within 0.12 m of a stripe boundary are skipped
    // (refraction offset through the 0.2 m slab is a few centimetres).
    const auto stripe_contrast = [&](const std::vector<matter::Float4>& row,
                                     uint32_t step) {
        double sum[2] = {0.0, 0.0};
        int count[2] = {0, 0};
        for (size_t p = 0; p < row.size(); ++p) {
            const uint32_t x = kRowX0 + static_cast<uint32_t>(p) * step;
            const double ndc_x =
                (static_cast<double>(x) + 0.5) / width * 2.0 - 1.0;
            // 90-degree vfov, 1.6 aspect: at the wall depth (6 m) the ray's
            // lateral world offset is ndc_x * 1.6 * 6.
            const double wall_x = ndc_x * 1.6 * 6.0;
            const double stripe_pos = (wall_x + 24.0) / 0.6;
            const double in_stripe = stripe_pos - std::floor(stripe_pos);
            if (in_stripe < 0.2 || in_stripe > 0.8) continue;
            const int parity = static_cast<int>(std::floor(stripe_pos)) & 1;
            sum[parity] += rt_trans_luminance(row[p]);
            ++count[parity];
        }
        CHECK(count[0] > 4 && count[1] > 4,
              "rt-transmission: both stripe classes are sampled");
        return sum[0] / std::max(count[0], 1) -
               sum[1] / std::max(count[1], 1);
    };
    const auto mean_luminance = [](const std::vector<matter::Float4>& row) {
        double mean = 0.0;
        for (const auto& v : row) mean += rt_trans_luminance(v);
        return mean / static_cast<double>(row.size());
    };

    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error),
          error.empty() ? "rt-transmission: renderer init" : error.c_str());
    author(0.0f, 0.05f, {1.0f, 1.0f, 1.0f});
    CHECK(renderer.update_materials(materials, shading_revision++, 1, error),
          error.empty() ? "rt-transmission: stage materials" : error.c_str());
    build_scene(renderer, false);
    configure(renderer);

    // --- (1) blur grows monotonically with roughness (striped wall) --------
    // The ladder tops out at 0.5, double the spec's authored frosted range
    // (GlacialIce: 0.05-0.25). Beyond ~0.6 the documented grazing-TIR
    // fallback -- a sampled microfacet whose refract() fails keeps the
    // GEOMETRIC direction, bias accepted over a re-sample loop -- re-adds a
    // sharp image fraction, so stripe contrast stops being a pure blur
    // measure out there (measured: contrast 0.018 at r=0.45 vs 0.060 at
    // r=0.8 from exactly that sharp fallback fraction).
    const float ladder[4] = {0.0f, 0.1f, 0.25f, 0.5f};
    double contrast[4] = {0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < 4; ++i) {
        author(ladder[i], 0.05f, {1.0f, 1.0f, 1.0f});
        CHECK(renderer.update_materials(materials, shading_revision++, 1,
                                        error),
              error.empty() ? "rt-transmission: ladder materials"
                            : error.c_str());
        render_frame(renderer, true, presented_index++);
        for (int f = 0; f < 15; ++f)
            render_frame(renderer, false, presented_index++);
        // Average the denoised row over four more frames so the residual
        // 1-spp noise cannot mask the contrast ordering between adjacent
        // roughness rungs.
        std::vector<matter::Float4> averaged;
        for (int f = 0; f < 4; ++f) {
            render_frame(renderer, false, presented_index++);
            vulkan.wait_idle();
            const auto row = sample_row(renderer, 4, true);
            if (averaged.empty()) averaged.assign(row.size(), {});
            for (size_t p = 0; p < row.size(); ++p) {
                averaged[p].x += row[p].x * 0.25f;
                averaged[p].y += row[p].y * 0.25f;
                averaged[p].z += row[p].z * 0.25f;
                averaged[p].w += row[p].w * 0.25f;
            }
        }
        contrast[i] = stripe_contrast(averaged, 4);
        // Aux-lane contract at one rough rung: (hit_t, roughness).
        if (i == 2) {
            viewer::VkRasterPixel pixel{};
            CHECK(renderer.readback_raster_pixel(160, kRowY, pixel, error),
                  error.c_str());
            CHECK(std::fabs(pixel.transmission_aux.y - ladder[i]) < 0.01f,
                  "rt-transmission: aux carries the authored roughness");
            CHECK(pixel.transmission_aux.x > 1.0f &&
                      pixel.transmission_aux.x < 50.0f,
                  "rt-transmission: aux carries a finite hit distance");
            CHECK(pixel.raw_transmission.w > 0.9f &&
                      pixel.accumulated_transmission.w > 0.9f,
                  "rt-transmission: coverage rides alpha through the denoiser");
        }
    }
    std::printf("rt-transmission blur: stripe contrast(r=%.2f..%.2f) = "
                "%.5f %.5f %.5f %.5f\n",
                ladder[0], ladder[3], contrast[0], contrast[1], contrast[2],
                contrast[3]);
    for (int i = 1; i < 4; ++i)
        CHECK(contrast[i] < contrast[i - 1],
              "rt-transmission: blur grows monotonically with roughness");
    CHECK(contrast[3] < 0.25 * contrast[0],
          "rt-transmission: the roughest slab collapses stripe contrast");

    // --- (2) transmitted energy within 2% of the smooth slab (uniform wall).
    // hit_radiance is direction-independent, so any perturbed exit ray sees
    // the same wall radiance; a deviation here is an energy bug in the
    // perturbation itself, not Monte Carlo noise.
    double energy[4] = {0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < 4; ++i) {
        author(ladder[i], 0.85f, {1.0f, 1.0f, 1.0f});
        CHECK(renderer.update_materials(materials, shading_revision++, 1,
                                        error),
              error.empty() ? "rt-transmission: energy materials"
                            : error.c_str());
        render_frame(renderer, true, presented_index++);
        double sum = 0.0;
        int frames = 0;
        for (int f = 0; f < 4; ++f) {
            render_frame(renderer, false, presented_index++);
            vulkan.wait_idle();
            sum += mean_luminance(sample_row(renderer, 16, false));
            ++frames;
        }
        energy[i] = sum / frames;
    }
    std::printf("rt-transmission energy: mean(r=%.2f..%.2f) = "
                "%.5f %.5f %.5f %.5f\n",
                ladder[0], ladder[3], energy[0], energy[1], energy[2],
                energy[3]);
    // 2% integration-sanity bound across the ladder. (For reference: at
    // r = 0.8, well past the authored range, real extra TIR bounces from
    // steep sampled microfacets cost ~2.7% -- genuine frost transport, not
    // an estimator bug.)
    for (int i = 1; i < 4; ++i)
        CHECK(std::fabs(energy[i] - energy[0]) <= 0.02 * energy[0],
              "rt-transmission: rough transmission conserves energy within 2%");

    // --- (3) determinism + smooth byte-parity (uniform wall) ---------------
    const uint64_t fixed_index = presented_index + 100;
    author(0.0f, 0.85f, {1.0f, 1.0f, 1.0f});
    CHECK(renderer.update_materials(materials, shading_revision++, 1, error),
          error.c_str());
    render_frame(renderer, true, fixed_index);
    vulkan.wait_idle();
    const auto smooth_row = sample_row(renderer, 8, false);
    const auto smooth_accumulated = sample_row(renderer, 8, true);
    render_frame(renderer, true, fixed_index);
    vulkan.wait_idle();
    const auto repeat_row = sample_row(renderer, 8, false);
    bool deterministic = true;
    bool passthrough = true;
    for (size_t i = 0; i < smooth_row.size(); ++i) {
        deterministic = deterministic &&
            smooth_row[i].x == repeat_row[i].x &&
            smooth_row[i].y == repeat_row[i].y &&
            smooth_row[i].z == repeat_row[i].z &&
            smooth_row[i].w == repeat_row[i].w;
        passthrough = passthrough &&
            smooth_row[i].x == smooth_accumulated[i].x &&
            smooth_row[i].y == smooth_accumulated[i].y &&
            smooth_row[i].z == smooth_accumulated[i].z &&
            smooth_row[i].w == smooth_accumulated[i].w;
    }
    CHECK(deterministic,
          "rt-transmission: fixed frame index reproduces bit-identical rays");
    CHECK(passthrough,
          "rt-transmission: smooth pixels pass the denoiser bit-exactly");
    // Roughness below the 0.02 sampling threshold must not perturb anything:
    // no VNDF sample is drawn, so the whole lane stays bit-identical.
    author(0.019f, 0.85f, {1.0f, 1.0f, 1.0f});
    CHECK(renderer.update_materials(materials, shading_revision++, 1, error),
          error.c_str());
    render_frame(renderer, true, fixed_index);
    vulkan.wait_idle();
    const auto threshold_row = sample_row(renderer, 8, false);
    bool byte_identical = true;
    for (size_t i = 0; i < smooth_row.size(); ++i) {
        byte_identical = byte_identical &&
            smooth_row[i].x == threshold_row[i].x &&
            smooth_row[i].y == threshold_row[i].y &&
            smooth_row[i].z == threshold_row[i].z &&
            smooth_row[i].w == threshold_row[i].w;
    }
    CHECK(byte_identical,
          "rt-transmission: sub-threshold roughness is byte-identical to smooth");

    // --- (4) composite fallback guard (black-glass regression) -------------
    // RT off => the transmission lane is cleared, coverage < 0.01, and the
    // composite takes the material fallback branch. A zeroed absorptionColor
    // must behave exactly like a clear (white) one.
    matter::VulkanRayTracingSettings rt_disabled{};
    rt_disabled.enabled = false;
    renderer.set_ray_tracing_settings(rt_disabled);
    author(0.0f, 0.85f, {0.0f, 0.0f, 0.0f});
    CHECK(renderer.update_materials(materials, shading_revision++, 1, error),
          error.c_str());
    render_frame(renderer, true, presented_index++);
    vulkan.wait_idle();
    viewer::VkRasterPixel black_absorption{};
    CHECK(renderer.readback_raster_pixel(160, kRowY, black_absorption, error),
          error.c_str());
    author(0.0f, 0.85f, {1.0f, 1.0f, 1.0f});
    CHECK(renderer.update_materials(materials, shading_revision++, 1, error),
          error.c_str());
    render_frame(renderer, true, presented_index++);
    vulkan.wait_idle();
    viewer::VkRasterPixel white_absorption{};
    CHECK(renderer.readback_raster_pixel(160, kRowY, white_absorption, error),
          error.c_str());
    std::printf("rt-transmission fallback: black-absorption hdr=%.5f "
                "white-absorption hdr=%.5f\n",
                rt_trans_luminance(black_absorption.hdr),
                rt_trans_luminance(white_absorption.hdr));
    CHECK(rt_trans_luminance(black_absorption.hdr) > 0.02f,
          "rt-transmission: zeroed-absorption fallback is non-black");
    CHECK(std::fabs(rt_trans_luminance(black_absorption.hdr) -
                    rt_trans_luminance(white_absorption.hdr)) < 1e-3f,
          "rt-transmission: the fallback guard treats black absorption as clear");
    renderer.set_ray_tracing_settings(rt_enabled);

    // --- (5) alpha-tested occluders in the walk + cost --------------------
    // Default (two-mask) walk: the failing alpha-test card is skipped, so the
    // center pixel still transmits the gray wall.
    author(0.0f, 0.05f, {1.0f, 1.0f, 1.0f});
    CHECK(renderer.update_materials(materials, shading_revision++, 1, error),
          error.c_str());
    build_scene(renderer, true);
    render_frame(renderer, true, presented_index++);
    for (int f = 0; f < 19; ++f)
        render_frame(renderer, false, presented_index++);
    vulkan.wait_idle();
    viewer::VkRasterPixel masked_center{};
    CHECK(renderer.readback_raster_pixel(160, kRowY, masked_center, error),
          error.c_str());
    const float masked_rt_ms =
        renderer.gpu_timers_supported()
            ? renderer.gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneRt)
            : 0.0f;
    CHECK(masked_center.raw_transmission.x <
              2.0f * masked_center.raw_transmission.y + 0.05f,
          "rt-transmission: two-mask walk skips the failing alpha-test card");

    // Legacy walk (spec constant off): the same card silhouettes refraction
    // and its red albedo dominates the transmitted color.
    _putenv_s("MATTER_RT_WALK_ALPHA_TEST", "0");
    {
        viewer::VkSceneRenderer legacy(vulkan);
        CHECK(legacy.init(error),
              error.empty() ? "rt-transmission: legacy renderer init"
                            : error.c_str());
        CHECK(legacy.update_materials(materials, 1, 1, error),
              error.c_str());
        build_scene(legacy, true);
        configure(legacy);
        render_frame(legacy, true, presented_index++);
        for (int f = 0; f < 19; ++f)
            render_frame(legacy, false, presented_index++);
        vulkan.wait_idle();
        viewer::VkRasterPixel legacy_center{};
        CHECK(legacy.readback_raster_pixel(160, kRowY, legacy_center, error),
              error.c_str());
        const float legacy_rt_ms =
            legacy.gpu_timers_supported()
                ? legacy.gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneRt)
                : 0.0f;
        std::printf("rt-transmission alpha walk: masked=(%.4f %.4f %.4f) "
                    "legacy=(%.4f %.4f %.4f)\n",
                    masked_center.raw_transmission.x,
                    masked_center.raw_transmission.y,
                    masked_center.raw_transmission.z,
                    legacy_center.raw_transmission.x,
                    legacy_center.raw_transmission.y,
                    legacy_center.raw_transmission.z);
        std::printf("rt-transmission walk cost: two-mask rt=%.3f ms, "
                    "legacy rt=%.3f ms (%+.1f%%)\n",
                    masked_rt_ms, legacy_rt_ms,
                    legacy_rt_ms > 0.0f
                        ? (masked_rt_ms - legacy_rt_ms) / legacy_rt_ms * 100.0f
                        : 0.0f);
        CHECK(legacy_center.raw_transmission.x >
                  2.0f * legacy_center.raw_transmission.y,
              "rt-transmission: legacy walk silhouettes the red card (control)");
    }
    _putenv_s("MATTER_RT_WALK_ALPHA_TEST", "");
}

void run_native_ray_tracing_path(matter::VulkanDevice& vulkan) {
    run_native_multilod_rt_mapping(vulkan);
    run_rt_lod_compaction_invariant(vulkan);
    CHECK(vulkan.ray_tracing_available(),
          vulkan.ray_tracing_unavailable_reason().empty()
              ? "native ray tracing available"
              : vulkan.ray_tracing_unavailable_reason().c_str());
    if (!vulkan.ray_tracing_available()) return;
    run_rt_froxel_resize_smoke(vulkan);
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
    rt_scenario_surface_query(vulkan, properties, error);
    rt_scenario_blas_pinning(vulkan, error);
    rt_scenario_visibility_classification(vulkan, error);

    // Shared state for all renderer-based scenarios.
    viewer::VkSceneRenderer renderer(vulkan);
    // Initialize before any scenario samples immediate_submit_count(): one-time
    // renderer init submits immediately (tileset dummy array layers and the
    // volumetric placeholder must be transitioned before the first frame), and
    // prepare_frame() inits lazily, so a baseline taken just before the first
    // prepare_frame charges those to the frame and reads as a regression.
    CHECK(renderer.init(error),
          error.empty() ? "initialize RT renderer before immediate baselines"
                        : error.c_str());
    viewer::VkSceneLighting lighting{};
    matter::VulkanRayTracingSettings enabled{};
    matter::VulkanGiSettings gi{};
    viewer::FrameMatrices matrices{};
    matter::CameraDesc camera{};
    viewer::TemporalFrame gi_temporal{};
    std::vector<MaterialGpuRecord> gi_materials;
    uint32_t retry_x = 0;
    uint32_t retry_y = 0;
    bool     receiver_seen = false;
    float    minimum_visibility = 1.0f;
    float    maximum_visibility = 0.0f;
    float    receiver_min_visibility = 1.0f;
    float    receiver_max_visibility = 0.0f;

    RtPathContext ctx{vulkan, properties, error, renderer, lighting, enabled,
                      gi, matrices, camera, gi_temporal, gi_materials,
                      retry_x, retry_y, receiver_seen,
                      minimum_visibility, maximum_visibility,
                      receiver_min_visibility, receiver_max_visibility};

    rt_scenario_shadow_contract(ctx);

    matter::VulkanFrame frame{};
    CHECK(vulkan.begin_frame(frame, error),
          error.empty() ? "begin native RT frame" : error.c_str());
    if (frame.command_buffer != VK_NULL_HANDLE) {
        rt_scenario_first_frame_and_blas_lifecycle(ctx, frame);
        rt_scenario_atrous_denoising(ctx);
        rt_scenario_gi_history_resets(ctx);
        rt_scenario_secondary_sun_visibility(ctx);
        rt_scenario_mirror_specular(ctx);
        rt_scenario_baked_ao_and_gi_disable(ctx);
    }

}

void run_atmosphere_real_gpu_gate(matter::VulkanDevice& vulkan) {
    constexpr uint32_t width = 160;
    constexpr uint32_t height = 160;
    std::string error;

    viewer::VkSceneRenderer raster(vulkan);
    std::vector<MaterialGpuRecord> materials(8);
    materials[7].base_roughness[0] = 0.7f;
    materials[7].base_roughness[1] = 0.7f;
    materials[7].base_roughness[2] = 0.7f;
    materials[7].base_roughness[3] = 0.6f;
    materials[7].metal_opacity_spec_coat[1] = 1.0f;
    materials[7].scattering_shape[3] = 1.0f;
    const viewer::VkScenePart triangle = known_raster_triangle(990);
    CHECK(raster.update_materials(materials, 1, 1, error) &&
              raster.ensure_part(triangle, error) >= 0 &&
              raster.update_instances({{990, identity_matrix()}}, error),
          error.empty() ? "prepare atmosphere raster GPU gate"
                        : error.c_str());
    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices matrices{};
    CHECK(viewer::build_frame_matrices(camera, width, height, matrices,
                                       error) &&
              raster.dispatch_culling(matrices, camera.position, 1.0f,
                                      error),
          error.empty() ? "build atmosphere raster GPU gate matrices"
                        : error.c_str());
    for (size_t index = 0; index < kAtmosphereGpuElevations.size(); ++index) {
        const matter::Float3 to_sun =
            matter::atmosphere_to_sun_from_elevation_deg(
                kAtmosphereGpuElevations[index]);
        viewer::VkSceneLighting lighting{};
        lighting.sun_direction = {-to_sun.x, -to_sun.y, -to_sun.z};
        lighting.authored_sun_rgb = {1.0f, 1.0f, 1.0f};
        lighting.atmosphere_sources.authored_display_sky_chroma_rgb =
            {1.0f, 1.0f, 1.0f};
        lighting.atmosphere_sources.authored_irradiance_chroma_rgb =
            {1.0f, 1.0f, 1.0f};
        lighting.atmosphere_sources.sun_multiplier = 1.0f;
        lighting.atmosphere_sources.sky_multiplier = 1.0f;
        raster.set_lighting(lighting);
        viewer::VkRasterPixel direct_on{};
        viewer::VkRasterPixel direct_off{};
        CHECK(raster.render_gbuffer_and_composite(width, height, error) &&
                  raster.readback_raster_pixel(width / 2, height / 2,
                                               direct_on, error),
              error.empty() ? "render atmosphere raster direct-on frame"
                            : error.c_str());
        const auto status = raster.test_resolved_atmosphere_status();
        g_atmosphere_raster_direct_rgb[index] =
            status.direct_world_sun_rgb;
        const float published_luma =
            0.2126f * status.direct_world_sun_rgb.x +
            0.7152f * status.direct_world_sun_rgb.y +
            0.0722f * status.direct_world_sun_rgb.z;
        const float noon_luma = index == 0
                                    ? published_luma
                                    : 0.2126f * g_atmosphere_raster_direct_rgb[0].x +
                                          0.7152f * g_atmosphere_raster_direct_rgb[0].y +
                                          0.0722f * g_atmosphere_raster_direct_rgb[0].z;
        const float expected_published_ratio = kAtmosphereGpuRatios[index];
        CHECK(noon_luma > 0.0f &&
                  std::fabs(published_luma / noon_luma -
                            expected_published_ratio) <= 2.0e-3f,
              "raster published world-sun luminance ratio matches its exact elevation anchor");
        if (index == 0 || index == 2)
            std::printf("atmosphere published direct e=%.0f luma=%.9f rgb=%.9f %.9f %.9f\n",
                        kAtmosphereGpuElevations[index], published_luma,
                        status.direct_world_sun_rgb.x,
                        status.direct_world_sun_rgb.y,
                        status.direct_world_sun_rgb.z);
        CHECK(std::fabs(status.direct_world_ratio -
                        kAtmosphereGpuRatios[index]) <= 1.0e-6f,
              "raster publishes the exact direct-world elevation ratio");
        lighting.sun_intensity = 0.0f;
        raster.set_lighting(lighting);
        CHECK(raster.render_gbuffer_and_composite(width, height, error) &&
                  raster.readback_raster_pixel(width / 2, height / 2,
                                               direct_off, error),
              error.empty() ? "render atmosphere raster direct-off frame"
                            : error.c_str());
        if (index == 0 || index == 2) {
            const matter::Float3 receiver_direct{
                direct_on.hdr.x - direct_off.hdr.x,
                direct_on.hdr.y - direct_off.hdr.y,
                direct_on.hdr.z - direct_off.hdr.z};
            const float receiver_luma = 0.2126f * receiver_direct.x +
                                        0.7152f * receiver_direct.y +
                                        0.0722f * receiver_direct.z;
            std::printf("atmosphere receiver direct e=%.0f luma=%.9f rgb=%.9f %.9f %.9f\n",
                        kAtmosphereGpuElevations[index], receiver_luma,
                        receiver_direct.x, receiver_direct.y,
                        receiver_direct.z);
        }
        if (index >= 3) {
            CHECK(direct_on.hdr.x == direct_off.hdr.x &&
                      direct_on.hdr.y == direct_off.hdr.y &&
                      direct_on.hdr.z == direct_off.hdr.z,
                  "raster direct contribution is exactly zero at and below the horizon");
        }
        if (index == 4) {
            CHECK(direct_off.hdr.x > 1.0e-4f ||
                      direct_off.hdr.y > 1.0e-4f ||
                      direct_off.hdr.z > 1.0e-4f,
                  "-5 degree raster receiver remains positive from evaluated SH");
        }
    }
    g_atmosphere_raster_direct_valid = true;

    CHECK(vulkan.ray_tracing_available(),
          vulkan.ray_tracing_available()
              ? "native ray tracing available for atmosphere ratio gate"
              : vulkan.ray_tracing_unavailable_reason().c_str());
    if (!vulkan.ray_tracing_available()) return;
    viewer::VkSceneRenderer native(vulkan);
    CHECK(native.init(error),
          error.empty() ? "initialize atmosphere native-RT GPU gate"
                        : error.c_str());
    viewer::VkSceneLighting native_lighting{};
    matter::VulkanRayTracingSettings enabled{};
    matter::VulkanGiSettings gi{};
    viewer::FrameMatrices native_matrices{};
    matter::CameraDesc native_camera{};
    viewer::TemporalFrame temporal{};
    std::vector<MaterialGpuRecord> native_materials;
    uint32_t retry_x = 0, retry_y = 0;
    bool receiver_seen = false;
    float minimum_visibility = 1.0f, maximum_visibility = 0.0f;
    float receiver_min_visibility = 1.0f, receiver_max_visibility = 0.0f;
    const auto& properties = vulkan.ray_tracing_properties();
    RtPathContext ctx{vulkan, properties, error, native, native_lighting,
                      enabled, gi, native_matrices, native_camera, temporal,
                      native_materials, retry_x, retry_y, receiver_seen,
                      minimum_visibility, maximum_visibility,
                      receiver_min_visibility, receiver_max_visibility};
    rt_scenario_shadow_contract(ctx);
    constexpr float kNativeProbeSunIntensity = 100.0f;
    float native_noon_luma = 0.0f;
    for (size_t index = 0; index < kAtmosphereGpuElevations.size(); ++index) {
        const matter::Float3 to_sun =
            matter::atmosphere_to_sun_from_elevation_deg(
                kAtmosphereGpuElevations[index]);
        native_lighting.sun_direction = {-to_sun.x, -to_sun.y, -to_sun.z};
        native_lighting.sun_intensity = kNativeProbeSunIntensity;
        native_lighting.authored_sun_rgb = {1.0f, 1.0f, 1.0f};
        native_lighting.atmosphere_sources.authored_display_sky_chroma_rgb =
            {1.0f, 1.0f, 1.0f};
        native_lighting.atmosphere_sources.authored_irradiance_chroma_rgb =
            {1.0f, 1.0f, 1.0f};
        native_lighting.atmosphere_sources.sun_multiplier = 1.0f;
        native_lighting.atmosphere_sources.sky_multiplier = 1.0f;
        native.set_lighting(native_lighting);
        temporal.reset = true;
        temporal.attempt_token = 1000 + index;
        native.set_temporal_frame(temporal);
        matter::VulkanFrame frame{};
        const bool rendered =
            vulkan.begin_frame(frame, error) &&
            native.prepare_frame(frame, native_matrices,
                                 native_camera.position, 1.0f, error) &&
            native.record_cull_and_render(frame, native_matrices,
                                          native_camera.position, 1.0f,
                                          error) &&
            native.record_composite_to_swapchain(frame, error) &&
            vulkan.end_frame(frame, error);
        CHECK(rendered,
              error.empty() ? "render atmosphere native-RT ratio frame"
                            : error.c_str());
        if (!rendered) break;
        native.finish_ray_tracing_frame(frame.serial, true);
        vulkan.wait_idle();
        const auto status = native.test_resolved_atmosphere_status();
        viewer::EnvironmentLightingGpu environment_gpu{};
        CHECK(native.test_read_environment_lighting_gpu(
                  frame.frame_slot, environment_gpu) &&
                  environment_gpu.direct_world_sun_ratio[0] ==
                      status.direct_world_sun_rgb.x &&
                  environment_gpu.direct_world_sun_ratio[1] ==
                      status.direct_world_sun_rgb.y &&
                  environment_gpu.direct_world_sun_ratio[2] ==
                      status.direct_world_sun_rgb.z &&
                  environment_gpu.direct_world_sun_ratio[3] ==
                      kAtmosphereGpuRatios[index],
              "native RT frame uploads the exact RGB/ratio Environment UBO lane");
        viewer::VkRasterPixel native_direct_on{};
        uint32_t native_receiver_x = 0;
        uint32_t native_receiver_y = 0;
        bool native_receiver_found = false;
        for (uint32_t y = 20; y < 200 && !native_receiver_found; y += 20) {
            for (uint32_t x = 20; x < 320; x += 20) {
                viewer::VkRasterPixel pixel{};
                if (native.readback_raster_pixel(x, y, pixel, error) &&
                    pixel.material_index == 1u &&
                    std::isfinite(pixel.raw_diffuse.x) &&
                    std::isfinite(pixel.raw_diffuse.y) &&
                    std::isfinite(pixel.raw_diffuse.z)) {
                    native_direct_on = pixel;
                    native_receiver_x = x;
                    native_receiver_y = y;
                    if (index == 0) {
                        retry_x = x;
                        retry_y = y;
                    }
                    native_receiver_found = true;
                    break;
                }
            }
        }
        CHECK(native_receiver_found,
              "native RT direct-on frame exposes a finite material receiver");
        const matter::Float3 raster_rgb =
            g_atmosphere_raster_direct_rgb[index];
        const float native_luma =
            0.2126f * status.direct_world_sun_rgb.x +
            0.7152f * status.direct_world_sun_rgb.y +
            0.0722f * status.direct_world_sun_rgb.z;
        if (index == 0) native_noon_luma = native_luma;
        CHECK(native.rt_effective_observed() &&
                  native.rt_trace_dispatches_observed() > 0 &&
                  std::fabs(status.direct_world_ratio -
                            kAtmosphereGpuRatios[index]) <= 1.0e-6f,
              "native RT dispatch consumes the exact direct-world ratio");
        CHECK(native_noon_luma > 0.0f &&
                  std::fabs(native_luma / native_noon_luma -
                            kAtmosphereGpuRatios[index]) <= 2.0e-3f,
              "native RT published world-sun luminance ratio matches its exact elevation anchor");
        CHECK(std::fabs(status.direct_world_sun_rgb.x /
                            kNativeProbeSunIntensity - raster_rgb.x) <=
                      2.0e-3f &&
                  std::fabs(status.direct_world_sun_rgb.y /
                                kNativeProbeSunIntensity - raster_rgb.y) <=
                      2.0e-3f &&
                  std::fabs(status.direct_world_sun_rgb.z /
                                kNativeProbeSunIntensity - raster_rgb.z) <=
                      2.0e-3f,
              "raster and normalized native RT direct RGB agree channel-wise");
        if (index >= 3) {
            CHECK(status.direct_world_sun_rgb.x == 0.0f &&
                      status.direct_world_sun_rgb.y == 0.0f &&
                      status.direct_world_sun_rgb.z == 0.0f,
                  "native RT direct RGB is exactly zero at and below the horizon");
        }
        native_lighting.sun_intensity = 0.0f;
        native.set_lighting(native_lighting);
        temporal.reset = true;
        temporal.attempt_token = 3000 + index;
        native.set_temporal_frame(temporal);
        matter::VulkanFrame direct_off_frame{};
        const bool direct_off_rendered =
            vulkan.begin_frame(direct_off_frame, error) &&
            native.prepare_frame(direct_off_frame, native_matrices,
                                 native_camera.position, 1.0f, error) &&
            native.record_cull_and_render(direct_off_frame, native_matrices,
                                          native_camera.position, 1.0f,
                                          error) &&
            native.record_composite_to_swapchain(direct_off_frame, error) &&
            vulkan.end_frame(direct_off_frame, error);
        CHECK(direct_off_rendered,
              error.empty() ? "render atmosphere native-RT direct-off frame"
                            : error.c_str());
        viewer::VkRasterPixel native_direct_off{};
        if (direct_off_rendered) {
            native.finish_ray_tracing_frame(direct_off_frame.serial, true);
            vulkan.wait_idle();
            CHECK(native_receiver_found &&
                      native.readback_raster_pixel(
                          native_receiver_x, native_receiver_y,
                          native_direct_off, error) &&
                      native_direct_off.material_index == 1u,
                  "native RT direct-off frame reads the identical material receiver");
            const auto raw_luma = [](matter::Float4 value) {
                return 0.2126f * value.x + 0.7152f * value.y +
                       0.0722f * value.z;
            };
            const float direct_delta =
                raw_luma(native_direct_on.raw_diffuse) -
                raw_luma(native_direct_off.raw_diffuse);
            if (index < 3) {
                CHECK(direct_delta > 1.0e-5f,
                      "native RT raw-diffuse direct-on exceeds direct-off above the horizon");
            } else {
                CHECK(std::memcmp(&native_direct_on.raw_diffuse,
                                  &native_direct_off.raw_diffuse,
                                  sizeof(matter::Float4)) == 0,
                      "native RT raw-diffuse direct-on/off are identical at and below the horizon");
            }
            std::printf(
                "atmosphere native raw direct e=%.0f delta=%.9f "
                "on=%.6f/%.6f/%.6f off=%.6f/%.6f/%.6f\n",
                kAtmosphereGpuElevations[index], direct_delta,
                native_direct_on.raw_diffuse.x,
                native_direct_on.raw_diffuse.y,
                native_direct_on.raw_diffuse.z,
                native_direct_off.raw_diffuse.x,
                native_direct_off.raw_diffuse.y,
                native_direct_off.raw_diffuse.z);
        }
        if (index == 4) {
            bool positive_receiver = false;
            for (uint32_t y = 20; y < 200 && !positive_receiver; y += 20) {
                for (uint32_t x = 20; x < 320; x += 20) {
                    viewer::VkRasterPixel pixel{};
                    if (native.readback_raster_pixel(x, y, pixel, error) &&
                        pixel.material_index < 2u &&
                        (pixel.hdr.x > 1.0e-4f || pixel.hdr.y > 1.0e-4f ||
                         pixel.hdr.z > 1.0e-4f)) {
                        positive_receiver = true;
                        break;
                    }
                }
            }
            CHECK(positive_receiver,
                  "-5 degree native RT receiver/fog remains positive from evaluated SH");
        }
    }

    // Task 13 native-RT receiver gate runs after the atmosphere ratio loop so
    // no command buffer is open while cloud resources/descriptors transition.
    // The retained material-1 pixel is the same secondary-hit receiver used by
    // the established native fixture; raw_diffuse excludes raster direct sun.
    const auto task13_native_capture = [&](float sun_intensity,
                                           uint64_t attempt_token,
                                           matter::Float4& raw) {
        viewer::VkSceneLighting probe = native_lighting;
        probe.sun_direction = {0.0f, -1.0f, 0.0f};
        probe.sun_intensity = sun_intensity;
        probe.authored_sun_rgb = {1.0f, 1.0f, 1.0f};
        probe.atmosphere_sources.authored_display_sky_chroma_rgb =
            {1.0f, 1.0f, 1.0f};
        probe.atmosphere_sources.authored_irradiance_chroma_rgb =
            {0.35f, 0.35f, 0.35f};
        probe.atmosphere_sources.sun_multiplier = 1.0f;
        probe.atmosphere_sources.sky_multiplier = 1.0f;
        native.set_lighting(probe);
        temporal.reset = true;
        temporal.attempt_token = attempt_token;
        native.set_temporal_frame(temporal);
        matter::VulkanFrame frame{};
        const bool rendered = vulkan.begin_frame(frame, error) &&
            native.prepare_frame(frame, native_matrices,
                                 native_camera.position, 1.0f, error) &&
            native.record_cull_and_render(frame, native_matrices,
                                          native_camera.position, 1.0f,
                                          error) &&
            native.record_composite_to_swapchain(frame, error) &&
            vulkan.end_frame(frame, error);
        native.finish_ray_tracing_frame(frame.serial, rendered);
        if (!rendered) return false;
        vulkan.wait_idle();
        viewer::VkRasterPixel pixel{};
        const bool read = native.readback_raster_pixel(
            retry_x, retry_y, pixel, error);
        raw = pixel.raw_diffuse;
        return read && pixel.material_index == 1u;
    };
    const auto task13_raw_luma = [](matter::Float4 value) {
        return value.x * 0.2126f + value.y * 0.7152f + value.z * 0.0722f;
    };
    matter::VulkanVolumetricsSettings task13_rt_volumetrics{};
    task13_rt_volumetrics.enabled = false;
    matter::FogSettings task13_rt_deck{};
    task13_rt_deck.cloud_count = 1;
    task13_rt_deck.clouds[0].enabled = true;
    task13_rt_deck.clouds[0].min_height = 4.0f;
    task13_rt_deck.clouds[0].max_height = 5.0f;
    task13_rt_deck.clouds[0].coverage = 1.0f;
    task13_rt_deck.clouds[0].max_density = 0.0f;
    matter::CloudShadowSettings task13_rt_shadows{};
    task13_rt_shadows.enabled = true;
    task13_rt_shadows.near_resolution = 0;
    task13_rt_shadows.near_depth_slices = 0;
    task13_rt_shadows.near_coverage_m = 16.0f;
    task13_rt_shadows.far_resolution = 0;
    task13_rt_shadows.far_depth_slices = 0;
    task13_rt_shadows.far_coverage_m = 16.0f;
    task13_rt_shadows.filter_scale = 0.0f;
    task13_rt_shadows.update_fraction = 1.0f;
    matter::CloudShadowSettings task13_rt_disabled = task13_rt_shadows;
    task13_rt_disabled.enabled = false;
    native.set_volumetrics_settings(task13_rt_volumetrics, task13_rt_deck,
                                    task13_rt_disabled);
    matter::Float4 task13_rt_clear_zero{}, task13_rt_clear_high{};
    const bool task13_rt_clear_read =
        task13_native_capture(0.0f, 4000, task13_rt_clear_zero) &&
        task13_native_capture(100.0f, 4001, task13_rt_clear_high);
    const float task13_rt_clear_delta =
        task13_raw_luma(task13_rt_clear_high) -
        task13_raw_luma(task13_rt_clear_zero);

    native.set_volumetrics_settings(task13_rt_volumetrics, task13_rt_deck,
                                    task13_rt_shadows);
    native.set_cloud_shadow_density_layers_for_test(
        12u, 2.0f, 2u, 0.0f, true);
    matter::Float4 task13_rt_shadow_zero{}, task13_rt_shadow_high{};
    const bool task13_rt_shadow_read =
        task13_native_capture(0.0f, 4002, task13_rt_shadow_zero) &&
        task13_native_capture(100.0f, 4003, task13_rt_shadow_high);
    const float task13_rt_shadow_delta =
        task13_raw_luma(task13_rt_shadow_high) -
        task13_raw_luma(task13_rt_shadow_zero);
    CHECK(task13_rt_clear_read && task13_rt_shadow_read &&
              task13_rt_clear_delta > 0.05f &&
              task13_rt_shadow_delta < task13_rt_clear_delta * 0.35f,
          "overhead cloud slab attenuates native RT secondary-hit direct sun");
    CHECK(task13_rt_clear_read && task13_rt_shadow_read &&
              close4(task13_rt_clear_zero, task13_rt_shadow_zero, 2.0e-4f),
          "native RT cloud attenuation leaves secondary-hit SH ambient unchanged");

    native.set_cloud_shadow_density_layers_for_test(
        2u, 2.0f, 1u, 0.0f, true);
    matter::Float4 task13_rt_above_zero{}, task13_rt_above_high{};
    const bool task13_rt_above_read =
        task13_native_capture(0.0f, 4004, task13_rt_above_zero) &&
        task13_native_capture(100.0f, 4005, task13_rt_above_high);
    const float task13_rt_above_delta =
        task13_raw_luma(task13_rt_above_high) -
        task13_raw_luma(task13_rt_above_zero);
    CHECK(task13_rt_above_read &&
              std::fabs(task13_rt_above_delta - task13_rt_clear_delta) <
                  task13_rt_clear_delta * 0.05f,
          "native RT secondary receiver above the slab remains clear");
    native.set_volumetrics_settings(task13_rt_volumetrics, task13_rt_deck,
                                    task13_rt_disabled);
    matter::Float4 task13_rt_disabled_zero{}, task13_rt_disabled_high{};
    const bool task13_rt_disabled_read =
        task13_native_capture(0.0f, 4006, task13_rt_disabled_zero) &&
        task13_native_capture(100.0f, 4007, task13_rt_disabled_high);
    const float task13_rt_disabled_delta =
        task13_raw_luma(task13_rt_disabled_high) -
        task13_raw_luma(task13_rt_disabled_zero);
    CHECK(task13_rt_disabled_read &&
              std::fabs(task13_rt_disabled_delta - task13_rt_clear_delta) <
                  task13_rt_clear_delta * 0.05f,
          "disabled cloud shadows restore native RT secondary direct sun");
    native.clear_cloud_shadow_density_override_for_test(true);

    matter::VulkanVolumetricsSettings twilight_volume{};
    twilight_volume.enabled = true;
    twilight_volume.froxel_xy_scale = matter::FroxelXyScale::X0_5;
    twilight_volume.froxel_depth_slices = matter::FroxelDepthSlices::D64;
    matter::FogSettings twilight_fog{};
    twilight_fog.density = 0.01f;
    twilight_fog.floor = -10000.0f;
    twilight_fog.falloff = 100000.0f;
    twilight_fog.color[0] = 1.0f;
    twilight_fog.color[1] = 1.0f;
    twilight_fog.color[2] = 1.0f;
    native.set_volumetrics_settings(twilight_volume, twilight_fog);
    const matter::Float3 twilight_to_sun =
        matter::atmosphere_to_sun_from_elevation_deg(-5.0f);
    native_lighting.sun_direction = {-twilight_to_sun.x,
                                     -twilight_to_sun.y,
                                     -twilight_to_sun.z};
    native_lighting.sun_intensity = 1.0f;
    native.set_lighting(native_lighting);
    temporal.reset = true;
    temporal.attempt_token = 2000;
    native.set_temporal_frame(temporal);
    matter::VulkanFrame fog_frame{};
    const bool fog_rendered =
        vulkan.begin_frame(fog_frame, error) &&
        native.prepare_frame(fog_frame, native_matrices,
                             native_camera.position, 1.0f, error) &&
        native.record_cull_and_render(fog_frame, native_matrices,
                                      native_camera.position, 1.0f, error) &&
        native.record_composite_to_swapchain(fog_frame, error) &&
        vulkan.end_frame(fog_frame, error);
    CHECK(fog_rendered,
          error.empty() ? "render -5 degree native-RT twilight fog frame"
                        : error.c_str());
    if (fog_rendered) {
        native.finish_ray_tracing_frame(fog_frame.serial, true);
        vulkan.wait_idle();
        const auto dimensions = native.volumetrics_dimensions();
        bool positive_fog = false;
        for (uint32_t y = dimensions.height / 2; y < dimensions.height / 2 + 2;
             ++y) {
            for (uint32_t x = dimensions.width / 2;
                 x < dimensions.width / 2 + 2; ++x) {
                matter::Float4 integrated{};
                if (native.readback_volumetrics_integrated_voxel_for_test(
                        x, y, dimensions.depth - 1, integrated, error) &&
                    std::isfinite(integrated.x) &&
                    std::isfinite(integrated.y) &&
                    std::isfinite(integrated.z) &&
                    (integrated.x > 1.0e-4f || integrated.y > 1.0e-4f ||
                     integrated.z > 1.0e-4f)) {
                    positive_fog = true;
                }
            }
        }
        CHECK(positive_fog,
              "-5 degree volumetric fog remains positive from evaluated SH");
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
    CHECK(renderer.raster_vertex_count() == 0 &&
              renderer.raster_index_count() == 0,
          "reset clears CPU vertex and index staging");
    CHECK(renderer.init(error) && prepare_scene() &&
              renderer.raster_attachments().hdr.image == VK_NULL_HANDLE &&
              renderer.render_gbuffer_and_composite(width, height, error) &&
              renderer.raster_attachments().hdr.image != VK_NULL_HANDLE,
          error.empty() ? "reset and reinit restore raster attachments only after render"
                        : error.c_str());
    CHECK(renderer.raster_vertex_count() == 3 &&
              renderer.raster_index_count() == 3,
          "re-registered scene stages exactly one triangle, no stale tail");
}

viewer::VkScenePart fixed_part(uint64_t hash, matter::Float3 minimum,
                               matter::Float3 maximum, uint32_t first_index) {
    viewer::VkSceneCluster cluster{};
    cluster.aabb_min = minimum;
    cluster.aabb_max = maximum;
    const float dx = maximum.x - minimum.x;
    const float dy = maximum.y - minimum.y;
    const float dz = maximum.z - minimum.z;
    cluster.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    cluster.lods.push_back({first_index, 3, 0.0f});
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
        command.index_count = scene.parts[i].clusters[0].lods[0].index_count;
        command.first_index = scene.parts[i].clusters[0].lods[0].first_index;
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
    // Initialize explicitly so the immediate-submit baseline below measures the
    // per-frame path only. One-time renderer init legitimately submits
    // immediately -- the tileset dummy array layers and the volumetric
    // placeholder texture must reach SHADER_READ_ONLY_OPTIMAL before the first
    // frame -- and prepare_frame() lazily inits, which would otherwise charge
    // those three submits to the first frame and read as a frame-path
    // regression.
    CHECK(renderer.init(error),
          error.empty() ? "initialize renderer before immediate-submit baseline"
                        : error.c_str());
    const matter::Mat4f identity = identity_matrix();
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
    // Atmosphere LUT initialization legitimately uses immediate submissions
    // on the first prepared frame. Warm that independent one-shot before this
    // test establishes the steady-state frame-upload baseline.
    CHECK(prepare(scene.frame),
          error.empty() ? "warm atmosphere before frame-upload baseline"
                        : error.c_str());
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

// Streaming-append contract (issues/bfb5f13e): registering a part into a live
// scene must reach the GPU as a tail append into the existing static buffers,
// not a recreate + O(world) rewrite. Full re-uploads are legal only when a
// buffer outgrew its capacity — capacity doubles, so over a streaming load
// they are O(log N) while appends carry the steady state.
void run_static_append_upload_tests(matter::VulkanDevice& vulkan) {
    std::string error;
    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.target = {0.0f, 0.0f, -1.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 1.57079632679f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    viewer::FrameMatrices frame{};
    CHECK(viewer::build_frame_matrices(camera, 320, 320, frame, error),
          error.empty() ? "build streamed-append matrices" : error.c_str());

    // Even parts sit in front of the camera, odd parts behind it. The final
    // emitted/culled split therefore depends on every appended cluster's AABB
    // bytes actually reaching the GPU: an append that left zeroes would put
    // that cluster's degenerate box at the (visible) origin and skew the count.
    const auto streamed_part = [](uint64_t hash, float z_center) {
        viewer::VkScenePart part = fixed_part(
            hash, {-0.25f, -0.25f, z_center - 0.25f},
            {0.25f, 0.25f, z_center + 0.25f}, 0);
        const matter::Float3 normal{0.0f, 1.0f, 0.0f};
        const matter::Float4 tint{0.9f, 0.1f, 0.3f, 0.0f};
        part.vertices = {
            {{-0.25f, -0.25f, z_center}, normal, tint,
             {0.0f, 0.0f, 0.0f, 1.0f}, 7u, {}},
            {{0.25f, -0.25f, z_center}, normal, tint,
             {0.0f, 0.0f, 0.0f, 1.0f}, 7u, {}},
            {{0.0f, 0.25f, z_center}, normal, tint,
             {0.0f, 0.0f, 0.0f, 1.0f}, 7u, {}},
        };
        part.indices = {0, 1, 2};
        return part;
    };

    viewer::VkSceneRenderer renderer(vulkan);
    CHECK(renderer.init(error),
          error.empty() ? "init streamed-append renderer" : error.c_str());
    const matter::Mat4f identity = identity_matrix();
    std::vector<viewer::VkSceneInstance> instances;
    constexpr int kStreamedParts = 16;
    uint64_t fulls = 0;
    uint64_t appends = 0;
    for (int i = 0; i < kStreamedParts; ++i) {
        const uint64_t hash = 2000 + static_cast<uint64_t>(i);
        const float z_center = (i % 2 == 0) ? -2.0f : 2.0f;
        CHECK(renderer.ensure_part(streamed_part(hash, z_center), error) >= 0,
              error.empty() ? "register streamed part" : error.c_str());
        instances.push_back({hash, identity});
        CHECK(renderer.update_instances(instances, error),
              error.empty() ? "upload streamed instances" : error.c_str());
        const VkDeviceSize cluster_capacity = renderer.cluster_buffer_size();
        const VkDeviceSize vertex_capacity =
            renderer.raster_vertex_buffer_size();
        const VkDeviceSize index_capacity = renderer.raster_index_buffer_size();
        const viewer::VkSceneUploadCounters before = renderer.upload_counters();
        CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error),
              error.empty() ? "dispatch streamed cull" : error.c_str());
        const viewer::VkSceneUploadCounters after = renderer.upload_counters();
        const uint64_t full_delta =
            after.static_full_uploads - before.static_full_uploads;
        const uint64_t append_delta =
            after.static_append_uploads - before.static_append_uploads;
        CHECK(full_delta + append_delta == 1,
              "each streamed registration performs exactly one static upload");
        const bool capacity_grew =
            renderer.cluster_buffer_size() > cluster_capacity ||
            renderer.raster_vertex_buffer_size() > vertex_capacity ||
            renderer.raster_index_buffer_size() > index_capacity;
        // The first upload of a renderer's life is a full by construction:
        // static_upload_dirty_ starts at kFull because nothing has been
        // uploaded yet. The static buffers are now RESERVED at init(), so that
        // seeding full no longer coincides with a growth and has to be excused
        // explicitly instead of being inferred from capacity_grew.
        const bool seeding_upload = before.static_full_uploads == 0;
        CHECK(full_delta == 0 || capacity_grew || seeding_upload,
              "full static re-uploads happen only to seed the buffers, or when "
              "one must grow");
        CHECK(capacity_grew || seeding_upload || append_delta == 1,
              "a registration that fits existing capacity appends in place");
        fulls += full_delta;
        appends += append_delta;
    }
    // With the buffers reserved at init(), a fixture this size never crosses a
    // capacity boundary, so exactly ONE full upload should occur -- the seeding
    // one -- and every subsequent registration appends. This is the assertion
    // that fails if the reservation regresses: before it, the same fixture
    // reported 6 appends / 10 fulls as each buffer climbed the doubling ladder.
    CHECK(appends >= 4, "append path carries the streaming steady state");
    CHECK(fulls <= 1,
          "reserved static buffers take a full upload only to seed");
    std::printf("streamed static uploads: %llu appends / %llu fulls\n",
                static_cast<unsigned long long>(appends),
                static_cast<unsigned long long>(fulls));
    viewer::VkCullStats stats{};
    CHECK(renderer.cull_stats(stats, error),
          error.empty() ? "read streamed cull stats" : error.c_str());
    CHECK(stats.emitted == kStreamedParts / 2,
          "appended front clusters are emitted from GPU cluster data");
    CHECK(stats.frustum_culled == kStreamedParts / 2,
          "appended behind clusters are culled from GPU cluster data");
}

void run_display_transform_tests(matter::VulkanDevice& vulkan) {
    std::string error;
    viewer::VkSceneRenderer renderer(vulkan);
    const matter::Mat4f identity = identity_matrix();
    const viewer::VkScenePart part = known_raster_triangle(971);
    CHECK(renderer.ensure_part(part, error) >= 0 &&
              renderer.update_instances({{971, identity}}, error),
          error.empty() ? "stage native display-transform scene"
                        : error.c_str());
    const FixedCullScene scene = make_fixed_cull_scene();
    const float hdr_values[] = {0.0f, 1.0f, 5.0f, 65504.0f};
    const float exposure_values[] = {-2.0f, 0.0f, 2.0f};
    float previous_response[3] = {-1.0f, -1.0f, -1.0f};
    for (float hdr_value : hdr_values) {
        for (size_t exposure_index = 0;
             exposure_index < std::size(exposure_values);
             ++exposure_index) {
            matter::VulkanFrame frame{};
            CHECK(vulkan.begin_frame(frame, error),
                  error.empty() ? "begin native display-transform frame"
                                : error.c_str());
            if (frame.command_buffer == VK_NULL_HANDLE) return;
            renderer.set_display_exposure(exposure_values[exposure_index]);
            CHECK(renderer.prepare_frame(frame, scene.frame, scene.eye, 1.0f,
                                         error) &&
                      renderer.record_cull_and_render(
                          frame, scene.frame, scene.eye, 1.0f, error) &&
                      renderer.test_record_hdr_constant(
                          frame, {hdr_value, hdr_value, hdr_value}, error) &&
                      renderer.record_composite_to_swapchain(frame, error),
                  error.empty() ? "record native display transform"
                                : error.c_str());
            std::vector<uint8_t> rgba;
            CHECK(vulkan.readback_swapchain_rgba8(frame, rgba, error),
                  error.empty() ? "queue native display-transform readback"
                                : error.c_str());
            CHECK(vulkan.end_frame(frame, error),
                  error.empty() ? "submit native display-transform frame"
                                : error.c_str());
            const matter::Float3 expected = aces_reference(
                {hdr_value, hdr_value, hdr_value},
                exposure_values[exposure_index]);
            const int expected_code =
                display_unorm_code(expected.x, frame.swapchain_format);
            const bool finite_unit = std::isfinite(expected.x) &&
                                     expected.x >= 0.0f && expected.x <= 1.0f;
            const bool matches = rgba.size() >= 4 &&
                                 std::abs(static_cast<int>(rgba[0]) -
                                          expected_code) <= 2 &&
                                 std::abs(static_cast<int>(rgba[1]) -
                                          expected_code) <= 2 &&
                                 std::abs(static_cast<int>(rgba[2]) -
                                          expected_code) <= 2;
            if (!matches && rgba.size() >= 4) {
                std::printf("display curve mismatch: hdr=%.1f ev=%.1f actual=%u,%u,%u expected=%d\n",
                            hdr_value, exposure_values[exposure_index],
                            rgba[0], rgba[1], rgba[2], expected_code);
            }
            CHECK(finite_unit && matches,
                  "native HDR display follows finite ACES reference curve");
            CHECK(expected.x + 1e-6f >= previous_response[exposure_index],
                  "native ACES response is monotonic at each exposure");
            previous_response[exposure_index] = expected.x;
        }
    }
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
    bool dlss_input_is_linear_hdr = false;
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
                std::fabs(constants.jitter_offset.x) < 0.5f &&
                std::fabs(constants.jitter_offset.y) < 0.5f &&
                constants.motion_vector_scale.x ==
                    -1.0f / constants.internal_extent.width &&
                constants.motion_vector_scale.y ==
                    -1.0f / constants.internal_extent.height &&
                resources.hdr.image != resources.depth.image &&
                resources.hdr.image != resources.velocity.image &&
                resources.hdr.image != resources.output.image;
            dlss_input_is_linear_hdr =
                resources.hdr.image == renderer.raster_attachments().hdr.image &&
                resources.hdr.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            const VkClearColorValue clear{{5.0f, 0.5f, 1.0f, 1.0f}};
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
              dlss_output_evaluated && dlss_input_is_linear_hdr &&
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
    // Adjacent parts now COALESCE into one multi-draw (record_raster), so the
    // recorded count is no longer one-per-part. The property this test has
    // always been about survives and is asserted directly: commands are grouped
    // into far fewer calls than there are commands, and the calls cover the
    // command span in order without gaps or overlaps.
    const auto ranges = renderer.test_recorded_draw_ranges();
    uint32_t recorded_commands = 0;
    bool ordered_and_disjoint = !ranges.empty();
    for (size_t i = 0; i < ranges.size(); ++i) {
        recorded_commands += ranges[i].command_count;
        ordered_and_disjoint = ordered_and_disjoint &&
                               ranges[i].command_count != 0;
        if (i != 0)
            ordered_and_disjoint =
                ordered_and_disjoint &&
                ranges[i].first_command == ranges[i - 1].first_command +
                                               ranges[i - 1].command_count;
    }
    CHECK(!ranges.empty() && ranges.size() <= 2,
          "two active parts record at most one indirect call each, fewer when "
          "their command spans are adjacent");
    CHECK(recorded_commands > ranges.size(),
          "cluster LOD commands are grouped instead of submitted individually");
    CHECK(ordered_and_disjoint,
          "recorded indirect ranges are ordered, non-empty and non-overlapping");
    CHECK(vulkan.end_frame(frame, error),
          error.empty() ? "submit asynchronous Vulkan record frame"
                        : error.c_str());
    const matter::Float3 expected_dlss =
        aces_reference({5.0f, 0.5f, 1.0f}, -2.0f);
    CHECK(dlss_composite_rgba.size() >= 4 &&
              std::abs(static_cast<int>(dlss_composite_rgba[0]) -
                       display_unorm_code(expected_dlss.x,
                                          frame.swapchain_format)) <= 2 &&
              std::abs(static_cast<int>(dlss_composite_rgba[1]) -
                       display_unorm_code(expected_dlss.y,
                                          frame.swapchain_format)) <= 2 &&
              std::abs(static_cast<int>(dlss_composite_rgba[2]) -
                       display_unorm_code(expected_dlss.z,
                                          frame.swapchain_format)) <= 2,
          "fake DLSS HDR output is tone mapped after evaluation");
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
    // Coalescing merges the two parts' adjacent spans into one run, which is
    // then split by maxDrawIndirectCount (3 here) -- so ranges can no longer be
    // bucketed by part_slot, and part_slot on a merged range names only the
    // run's first part. The invariant that actually matters is stronger and is
    // asserted directly: every recorded call respects the device cap, and the
    // calls together cover BOTH parts' commands, [kVkMaxLod, 3*kVkMaxLod),
    // contiguously -- no gap, no overlap, nothing dropped.
    const auto capped_ranges = renderer.test_recorded_draw_ranges();
    bool capped_and_contiguous = !capped_ranges.empty();
    const uint32_t coverage_start =
        capped_ranges.empty() ? UINT32_MAX : capped_ranges.front().first_command;
    uint32_t cursor = coverage_start;
    for (const auto& range : capped_ranges) {
        capped_and_contiguous = capped_and_contiguous &&
                                range.command_count != 0 &&
                                range.command_count <= 3 &&
                                range.first_command == cursor;
        cursor = range.first_command + range.command_count;
    }
    // Both parts' commands occupy [0, 2 * kVkMaxLod): the pre-coalescing
    // version of this test accumulated per part_slot and ended with
    // first_offset == kVkMaxLod, second_offset == 2 * kVkMaxLod, i.e. one
    // kVkMaxLod-sized block each, back to back from zero.
    CHECK(capped_and_contiguous && coverage_start == 0 &&
              cursor == 2 * viewer::kVkMaxLod,
          "capped grouped ranges cover both active parts contiguously");
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

    viewer::VkSceneRenderer bounds_renderer(vulkan);
    CHECK(bounds_renderer.init(error) &&
              bounds_renderer.ensure_part(scene.parts[0], error) >= 0 &&
              bounds_renderer.update_instances({scene.instances[0]}, error),
          error.empty() ? "prepare animation-bounds descriptor recovery scene"
                        : error.c_str());
    viewer::VkAnimationBoundsAsset bounds_asset{};
    bounds_asset.asset_key = 0x424f554eu;
    bounds_asset.conservative_asset_bound =
        {{-2.0f, -2.0f, -2.0f}, {2.0f, 2.0f, 2.0f}};
    bounds_asset.clusters = {
        {0, 0, {{0, {{-1.0f, -1.0f, -1.0f},
                      {1.0f, 1.0f, 1.0f}}}}},
        {1, 0, {{0, {{-0.5f, -0.5f, -0.5f},
                      {0.5f, 0.5f, 0.5f}}}}},
    };
    viewer::VkSkinJoint bounds_joint{};
    bounds_joint.position.elements[0] = bounds_joint.position.elements[5] =
        bounds_joint.position.elements[10] =
        bounds_joint.position.elements[15] = 1.0f;
    bounds_joint.normal = bounds_joint.position;
    viewer::VkSkinPose bounds_pose{};
    bounds_pose.current = {bounds_joint};
    bounds_pose.previous = {bounds_joint};
    CHECK(bounds_renderer.register_animation_bounds_asset(bounds_asset) &&
              bounds_renderer.update_animation_bounds(
                  0, 1, bounds_asset.asset_key, bounds_pose, false),
          "stage two animated records that grow the frame bounds buffer");
    const uint32_t bounds_validation_before = vulkan.validation_error_count();
    matter::VulkanFrame bounds_frame{};
    CHECK(vulkan.begin_frame(bounds_frame, error),
          error.empty() ? "begin animation-bounds upload-fault frame"
                        : error.c_str());
    bounds_renderer.set_test_animation_bounds_upload_failure_once();
    CHECK(!bounds_renderer.prepare_frame(
              bounds_frame, scene.frame, scene.eye, 1.0f, error) &&
              error.find("forced animation bounds upload failure") !=
                  std::string::npos,
          "fault after animation-bounds growth leaves a refreshable descriptor");
    CHECK(bounds_renderer.prepare_frame(
              bounds_frame, scene.frame, scene.eye, 1.0f, error) &&
              bounds_renderer.record_cull_and_render(
                  bounds_frame, scene.frame, scene.eye, 1.0f, error) &&
              vulkan.end_frame(bounds_frame, error),
          error.empty() ? "same-slot retry recovers grown bounds descriptor"
                        : error.c_str());
    vulkan.wait_idle();
    std::vector<viewer::DrawCommand> recovered_commands;
    CHECK(bounds_renderer.readback_commands(recovered_commands, error) &&
              !recovered_commands.empty(),
          "recovered descriptor survives submission and command readback");
    CHECK(vulkan.validation_error_count() == bounds_validation_before,
          "bounds grow/fault/recovery emits no descriptor validation error");
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
    // Free-range recycling: settle the freed range past the in-flight window
    // so re-registration reuses it instead of growing the tail (bounded
    // storage across an evict/republish cycle replaces eager compaction).
    for (int settle = 0; settle < 4; ++settle) {
        CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f, error),
              error.empty() ? "settle freed cluster range" : error.c_str());
    }
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
    // The CLUSTER buffer is reserved at init() and deliberately does not grow
    // for a fixture this size -- that is the reservation working, so it is
    // asserted stable rather than growing. The command and transform buffers
    // carry no reservation and still exercise the reallocation path, which is
    // what this check is actually about.
    CHECK(stable_cluster_bytes >= initial_cluster_bytes &&
              stable_command_bytes > initial_command_bytes &&
              stable_transform_bytes > initial_transform_bytes,
          "scene buffers grow safely across reallocations");

    for (int cycle = 0; cycle < 4; ++cycle) {
        renderer.release_part(77);
        CHECK(renderer.cluster_count() == 4,
              "release keeps uploaded cluster count coherent until dispatch");
        // Settle the freed range so the re-add reuses it — part 77 occupies
        // the same cluster range every cycle (stable bucket indices below).
        for (int settle = 0; settle < 4; ++settle) {
            CHECK(renderer.dispatch_culling(frame, camera.position, 1.0f,
                                            error),
                  error.empty() ? "settle churn range" : error.c_str());
        }
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
        // Range reuse puts part 77 back into cluster slot 0 every cycle (it
        // was registered first and always reuses its own freed range), so the
        // mixed part's clusters sit at 1..3: buckets shift from the eager-
        // compaction era's {1, 9, 19, 27} (77 last) to {0, 10, 18, 28}.
        const uint32_t expected_buckets[] = {0, 10, 18, 28};
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
                        bucket == 0 ? near_instance.object_to_world
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

    // Verify that ensure_part rejects a part whose index buffer references a
    // vertex beyond the end of the vertex array (brief mandate: real ensure_part
    // rejection exercised on the full renderer, not a mirrored local loop).
    {
        viewer::VkScenePart bad_part{};
        bad_part.part_hash = 0xBAD0Cu;
        bad_part.vertices.resize(3);
        bad_part.indices = {0u, 1u, 99u};  // index 99 is out of range
        viewer::VkSceneCluster bad_cluster{};
        bad_cluster.aabb_min = {-1.0f, -1.0f, -1.0f};
        bad_cluster.aabb_max = {1.0f, 1.0f, 1.0f};
        bad_cluster.radius = 1.7f;
        bad_cluster.lods.push_back({0u, 3u, 0.0f});
        bad_part.clusters.push_back(bad_cluster);
        std::string rejection_error;
        const int result = renderer.ensure_part(bad_part, rejection_error);
        CHECK(result < 0 && !rejection_error.empty(),
              "ensure_part rejects out-of-range index with non-empty error");
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
        // The appended part may or may not fit the live buffers' capacity;
        // force the full recreate path so the replacement fault below is
        // reachable deterministically.
        renderer.test_force_full_static_upload();
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

viewer::VkSkinMatrix skin_identity_matrix() {
    viewer::VkSkinMatrix result{};
    result.elements[0] = result.elements[5] = result.elements[10] =
        result.elements[15] = 1.0f;
    return result;
}

viewer::VkSkinMatrix skin_z_rotation(float radians, float sx = 1.0f,
                                     float sy = 1.0f) {
    viewer::VkSkinMatrix result = skin_identity_matrix();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    result.elements[0] = c * sx;
    result.elements[1] = s * sx;
    result.elements[4] = -s * sy;
    result.elements[5] = c * sy;
    return result;
}

void check_skin_fixture_against_cpu(
    const viewer::VkAnimationSkinGpuFixture& fixture,
    const viewer::VkAnimationSkinGpuResult& result, const char* label) {
    for (const viewer::VkSkinWorkItem& work : fixture.work) {
        const uint32_t palette_count =
            work.flags >> viewer::kVkSkinPaletteCountShift;
        for (uint32_t vertex = 0; vertex != work.vertex_count; ++vertex) {
            viewer::VkSkinVertex expected{};
            const bool cpu_ok = viewer::vk_skin_vertex_cpu(
                fixture.source[work.source_vertex + vertex],
                fixture.influences[work.influence + vertex],
                fixture.current_palette.data() + work.palette,
                fixture.previous_palette.data() + work.palette,
                palette_count, expected);
            const viewer::VkSkinVertex& actual =
                result.current[work.output_current + vertex];
            const viewer::VkSkinVertex& previous =
                result.previous[work.output_previous + vertex];
            if (!cpu_ok) {
                CHECK(actual.position[0] == 0.0f && previous.position[0] == 0.0f,
                      label);
                continue;
            }
            bool parity = true;
            for (uint32_t component = 0; component != 3; ++component) {
                parity = parity &&
                    std::fabs(actual.position[component] -
                              expected.position[component]) <= 1e-5f &&
                    std::fabs(previous.position[component] -
                              expected.previous_position[component]) <= 1e-5f &&
                    std::fabs(actual.normal[component] -
                              expected.normal[component]) <= 1e-4f;
            }
            if (!parity)
                std::printf("%s: GPU current=(%.5f %.5f %.5f) prior=(%.5f %.5f %.5f) expected=(%.5f %.5f %.5f) prior=(%.5f %.5f %.5f)\n",
                            label, actual.position[0], actual.position[1],
                            actual.position[2], previous.position[0],
                            previous.position[1], previous.position[2],
                            expected.position[0], expected.position[1],
                            expected.position[2], expected.previous_position[0],
                            expected.previous_position[1],
                            expected.previous_position[2]);
            CHECK(parity, label);
        }
    }
}

void run_animation_skin_gpu_readback_tests(matter::VulkanDevice& vulkan) {
    viewer::VkSceneRenderer renderer(vulkan);
    renderer.test_force_rt_unavailable(true);
    std::string error;
    const bool initialized = renderer.init(error);
    CHECK(initialized, error.empty() ? "initialize skin GPU fixture renderer"
                                      : error.c_str());
    if (!initialized) return;

    for (const uint32_t count : {1u, 63u, 64u, 65u}) {
        viewer::VkAnimationSkinGpuFixture fixture{};
        fixture.source.resize(count);
        fixture.influences.resize(count);
        for (uint32_t index = 0; index != count; ++index) {
            fixture.source[index].position[0] = static_cast<float>(index) * 0.25f;
            fixture.source[index].position[1] = 1.0f;
            fixture.source[index].normal[0] = 1.0f;
            fixture.source[index].tint[2] = 0.75f;
            fixture.source[index].surface[3] = 1.0f;
            fixture.source[index].material_index = 7;
            fixture.influences[index].weight[0] = 65535;
        }
        viewer::VkSkinJoint current{};
        current.position = skin_identity_matrix();
        current.normal = skin_identity_matrix();
        current.position.elements[12] = 2.0f;
        viewer::VkSkinJoint previous = current;
        previous.position.elements[12] = -3.0f;
        fixture.current_palette.push_back(current);
        fixture.previous_palette.push_back(previous);
        fixture.work.push_back({0, 0, count, 0, 0, 0, 0,
                                1u << viewer::kVkSkinPaletteCountShift});
        viewer::VkAnimationSkinGpuResult result;
        error.clear();
        const bool dispatched =
            renderer.test_dispatch_animation_skin_fixture(fixture, result, error);
        CHECK(dispatched, error.empty() ? "dispatch skin GPU fixture" : error.c_str());
        if (dispatched) check_skin_fixture_against_cpu(
            fixture, result, "GPU skin identity/current-previous parity");
    }

    viewer::VkAnimationSkinGpuFixture blend{};
    blend.source.resize(1);
    blend.source[0].position[0] = 1.0f;
    blend.source[0].normal[0] = 1.0f;
    blend.influences.resize(1);
    blend.influences[0].joint[0] = 0;
    blend.influences[0].joint[1] = 1;
    blend.influences[0].weight[0] = 32768;
    blend.influences[0].weight[1] = 32767;
    viewer::VkSkinJoint joint0{};
    joint0.position = skin_z_rotation(0.4f, 2.0f, 0.5f);
    joint0.normal = skin_z_rotation(0.4f, 0.5f, 2.0f);
    viewer::VkSkinJoint joint1{};
    joint1.position = skin_z_rotation(-0.7f, 0.25f, 3.0f);
    joint1.normal = skin_z_rotation(-0.7f, 4.0f, 1.0f / 3.0f);
    blend.current_palette = {joint0, joint1};
    joint0.position.elements[12] = -1.0f;
    joint1.position.elements[12] = 3.0f;
    blend.previous_palette = {joint0, joint1};
    blend.work.push_back({0, 0, 1, 0, 0, 0, 0,
                          2u << viewer::kVkSkinPaletteCountShift});
    viewer::VkAnimationSkinGpuResult blend_result;
    error.clear();
    const bool blend_dispatched = renderer.test_dispatch_animation_skin_fixture(
        blend, blend_result, error);
    CHECK(blend_dispatched, error.empty() ? "dispatch rotated blended skin GPU fixture"
                                          : error.c_str());
    if (blend_dispatched) check_skin_fixture_against_cpu(
        blend, blend_result, "GPU skin rotation blend and nonuniform normal parity");

    viewer::VkAnimationSkinGpuFixture invalid = blend;
    invalid.influences[0].joint[0] = 2;
    viewer::VkAnimationSkinGpuResult invalid_result;
    error.clear();
    const bool invalid_dispatched = renderer.test_dispatch_animation_skin_fixture(
        invalid, invalid_result, error);
    CHECK(invalid_dispatched, error.empty() ? "dispatch invalid-joint guard fixture"
                                            : error.c_str());
    if (invalid_dispatched) check_skin_fixture_against_cpu(
        invalid, invalid_result, "GPU invalid joint guard leaves output unwritten");
    CHECK(vulkan.validation_error_count() == 0,
          "animation skin fixture emits no Vulkan validation errors");
}

void run_animation_skin_record_fault_matrix(matter::VulkanDevice& vulkan) {
    const FixedCullScene scene = make_fixed_cull_scene();
    const auto run_case = [&](bool allocation_failure, uint32_t fail_point) {
        constexpr uint64_t part_hash = 0x534b4641u;
        constexpr uint64_t asset_key = 0x534b4642u;
        viewer::VkSceneRenderer renderer(vulkan);
        renderer.test_skip_volumetrics(true);
        std::string error;
        viewer::VkScenePart part = known_raster_triangle(part_hash);
        part.indices = {0, 1, 2};
        const bool initialized = renderer.init(error);
        CHECK(initialized,
              error.empty() ? "initialize animation skin fault renderer"
                            : error.c_str());
        if (!initialized) return;
        const bool scene_ready = renderer.ensure_part(part, error) >= 0 &&
            renderer.update_instances({{part_hash, identity_matrix()}}, error);
        CHECK(scene_ready,
              error.empty() ? "prepare animation skin fault scene" : error.c_str());
        if (!scene_ready) return;

        uint32_t source_vertex = 0, vertex_count = 0;
        uint32_t first_index = 0, index_count = 0;
        CHECK(renderer.part_raster_range(part_hash, source_vertex, vertex_count,
                                         first_index, index_count) &&
                  vertex_count == 3 && index_count == 3,
              "fault fixture resolves exact renderer-global skin ranges");
        std::vector<viewer::VkSkinInfluence> influences(vertex_count);
        for (auto& influence : influences) influence.weight[0] = 65535;
        viewer::VkAnimationBoundsAsset bounds{};
        bounds.asset_key = asset_key;
        bounds.conservative_asset_bound =
            {{-0.75f, -0.75f, -2.0f}, {0.75f, 1.5f, -2.0f}};
        bounds.clusters.push_back(
            {0, 0, {{0, bounds.conservative_asset_bound}}});
        CHECK(renderer.register_animation_skin_asset(asset_key, influences) &&
                  renderer.register_animation_bounds_asset(bounds),
              "fault fixture registers skin influences and animated bounds");

        const auto submission = [&](uint32_t slot, uint32_t generation) {
            viewer::VkSkinSubmission value{};
            value.asset_key = asset_key;
            value.source_vertex = source_vertex;
            value.vertex_count = vertex_count;
            value.first_index = first_index;
            value.index_count = index_count;
            value.instance_slot = slot;
            value.instance_generation = generation;
            value.cluster = 0;
            value.lod = 0;
            viewer::VkSkinJoint joint{};
            joint.position = skin_identity_matrix();
            joint.normal = skin_identity_matrix();
            value.pose.current = {joint};
            value.pose.previous = {joint};
            value.history_valid = true;
            return value;
        };
        const auto bind_dynamic = [&](uint32_t slot, uint32_t generation,
                                      uint64_t serial) {
            matter::render::DynamicSlotChange change{};
            change.kind = matter::render::DynamicSlotChangeKind::Bind;
            change.slot_index = slot;
            change.slot_generation = generation;
            change.part_hash = part_hash;
            change.object_to_world = identity_matrix();
            change.previous_object_to_world = identity_matrix();
            return renderer.update_dynamic_instances(&change, 1, serial, error);
        };

        matter::VulkanFrame seed{};
        CHECK(vulkan.begin_frame(seed, error) && bind_dynamic(0, 11, seed.serial) &&
                  renderer.begin_animation_skinning_frame(seed.frame_slot, 0) &&
                  renderer.submit_visible_animation_skinning(
                      seed.frame_slot, {submission(0, 11)}, scene.frame,
                      scene.eye, 1.0f) &&
                  renderer.prepare_frame(seed, scene.frame, scene.eye, 1.0f, error) &&
                  renderer.record_cull_and_render(seed, scene.frame, scene.eye,
                                                  1.0f, error) &&
                  renderer.finish_animation_skinning_frame(seed.frame_slot,
                                                           seed.serial) &&
                  renderer.record_composite_to_swapchain(seed, error) &&
                  vulkan.end_frame(seed, error),
              error.empty() ? "seed sealed animation skin retained pose"
                            : error.c_str());
        vulkan.wait_idle();

        matter::VulkanFrame failed{};
        const auto& diagnostics = renderer.animation_skinning();
        const auto before_runtime = renderer.animation_runtime_stats();
        CHECK(diagnostics.gpu_failure_count() == 0 &&
                  diagnostics.gpu_allocation_failure_count() == 0 &&
                  diagnostics.gpu_upload_failure_count() == 0 &&
                  diagnostics.fallback_count() == 0 &&
                  before_runtime.fallback_count == 0 &&
                  before_runtime.last_complete_fallback_count == 0 &&
                  before_runtime.bind_pose_fallback_count == 0,
              "each real record fault case starts with reset GPU diagnostics");
        CHECK(vulkan.begin_frame(failed, error) &&
                  bind_dynamic(1, 22, failed.serial) &&
                  renderer.begin_animation_skinning_frame(failed.frame_slot,
                                                          seed.serial) &&
                  renderer.submit_visible_animation_skinning(
                      failed.frame_slot,
                      {submission(0, 11), submission(1, 22)}, scene.frame,
                      scene.eye, 1.0f),
              error.empty() ? "publish unsealed skin fault queue" : error.c_str());
        renderer.set_test_animation_skin_failure(
            allocation_failure ? fail_point : std::numeric_limits<uint32_t>::max(),
            allocation_failure ? std::numeric_limits<uint32_t>::max() : fail_point);
        CHECK(renderer.prepare_frame(failed, scene.frame, scene.eye, 1.0f, error) &&
                  renderer.record_cull_and_render(
                      failed, scene.frame, scene.eye, 1.0f, error),
              error.empty() ? "skin allocation/upload fault continues frame record"
                            : error.c_str());
        const auto& degraded = renderer.animation_skinning().frame(failed.frame_slot);
        const auto& fallbacks = degraded.fallbacks;
        const bool retained = std::any_of(
            fallbacks.begin(), fallbacks.end(), [](const viewer::VkSkinFallback& value) {
                return value.instance_slot == 0 &&
                       value.mode == viewer::VkSkinFallbackMode::LastCompletePose;
            });
        const bool bound = std::any_of(
            fallbacks.begin(), fallbacks.end(), [](const viewer::VkSkinFallback& value) {
                return value.instance_slot == 1 &&
                       value.mode == viewer::VkSkinFallbackMode::BindPose;
            });
        const uint32_t retained_count = static_cast<uint32_t>(std::count_if(
            fallbacks.begin(), fallbacks.end(), [](const viewer::VkSkinFallback& value) {
                return value.mode == viewer::VkSkinFallbackMode::LastCompletePose;
            }));
        const uint32_t bind_count = static_cast<uint32_t>(std::count_if(
            fallbacks.begin(), fallbacks.end(), [](const viewer::VkSkinFallback& value) {
                return value.mode == viewer::VkSkinFallbackMode::BindPose;
            }));
        CHECK(degraded.work_items.empty() && fallbacks.size() == 2 &&
                  retained && bound && retained_count == 1 && bind_count == 1 &&
                  degraded.raster_draws.size() == 1 &&
                  degraded.raster_draws[0].output_frame_slot == seed.frame_slot,
              "real record fault atomically keeps retained pose and bind/static peer");
        const auto degraded_bounds = renderer.animation_bounds().gpu_records();
        const bool retained_is_conservative = std::any_of(
            degraded_bounds.begin(), degraded_bounds.end(),
            [](const viewer::VkAnimationBoundsGpuRecord& value) {
                return value.instance_slot == 0 &&
                       (value.flags &
                        viewer::kVkAnimationBoundsOcclusionEnabled) == 0;
            });
        const bool bind_is_conservative = std::any_of(
            degraded_bounds.begin(), degraded_bounds.end(),
            [](const viewer::VkAnimationBoundsGpuRecord& value) {
                return value.instance_slot == 1 &&
                       (value.flags &
                        viewer::kVkAnimationBoundsOcclusionEnabled) == 0;
            });
        CHECK(degraded_bounds.size() == 2 && retained_is_conservative &&
                  bind_is_conservative,
              "record fault keeps conservative bounds for retained and bind fallback ownership");
        CHECK(degraded.gpu_failure ==
                  (allocation_failure ? viewer::VkSkinGpuFailureReason::Allocation
                                      : viewer::VkSkinGpuFailureReason::Upload),
              "real record fault preserves its precise transaction reason");
        const auto runtime = renderer.animation_runtime_stats();
        const size_t invalid_submission = static_cast<size_t>(
            matter::animation::AnimationFallbackReason::InvalidSkinSubmission);
        CHECK(diagnostics.gpu_failure_count() == 1 &&
                  diagnostics.gpu_allocation_failure_count() ==
                      (allocation_failure ? 1u : 0u) &&
                  diagnostics.gpu_upload_failure_count() ==
                      (allocation_failure ? 0u : 1u),
              "real record fault increments exactly one precise GPU diagnostic");
        CHECK(diagnostics.fallback_count() == 2 &&
                  runtime.fallback_count == 2 &&
                  runtime.last_complete_fallback_count == 1 &&
                  runtime.bind_pose_fallback_count == 1 &&
                  runtime.fallbacks[invalid_submission] == 2,
              "real record fault surfaces one retained and one bind contribution");
        CHECK(renderer.finish_animation_skinning_frame(failed.frame_slot,
                                                       failed.serial) &&
                  renderer.record_composite_to_swapchain(failed, error) &&
                  vulkan.end_frame(failed, error),
              error.empty() ? "degraded skin frame seals and presents"
                            : error.c_str());
        vulkan.wait_idle();
        std::vector<viewer::DrawCommand> degraded_commands;
        // Three instances share this cluster's command: the static scene
        // instance from update_instances() above, the slot-1 bind-pose
        // fallback, and the slot-0 retained pose. Only the retained one owns an
        // explicit skin raster draw, so cull.comp's uses_skin_raster removes
        // exactly it and the static command carries the other two.
        //
        // This asserted 1 for as long as the suite was dark, which was the
        // clobbered-AABB bug in cull.comp's dynamic_cluster_union reading as a
        // pass: the static instance was silently frustum-culled, so the one
        // surviving draw looked like correct exclusion of the retained pose.
        CHECK(renderer.readback_commands(degraded_commands, error) &&
                  !degraded_commands.empty() &&
                  degraded_commands[0].instance_count == 2,
              "retained explicit raster does not double-draw through the static indirect command");
        const auto sealed_runtime = renderer.animation_runtime_stats();
        CHECK(diagnostics.gpu_failure_count() == 1 &&
                  diagnostics.gpu_allocation_failure_count() ==
                      (allocation_failure ? 1u : 0u) &&
                  diagnostics.gpu_upload_failure_count() ==
                      (allocation_failure ? 0u : 1u) &&
                  diagnostics.fallback_count() == 2 &&
                  sealed_runtime.fallback_count == 2 &&
                  sealed_runtime.last_complete_fallback_count == 1 &&
                  sealed_runtime.bind_pose_fallback_count == 1 &&
                  sealed_runtime.fallbacks[invalid_submission] == 2,
              "sealing a degraded frame does not double-count its diagnostics");
        CHECK(renderer.begin_animation_skinning_frame(seed.frame_slot,
                                                      failed.serial),
              "retained dependency releases after degraded consumer fence completes");
        if (!allocation_failure && fail_point == 0) {
            renderer.set_test_animation_skin_failure(
                std::numeric_limits<uint32_t>::max(),
                std::numeric_limits<uint32_t>::max());
            matter::VulkanFrame retry{};
            bool retried_same_slot = false;
            for (uint32_t attempt = 0; attempt != 4 && !retried_same_slot;
                 ++attempt) {
                CHECK(vulkan.begin_frame(retry, error),
                      error.empty() ? "begin grown skin descriptor retry"
                                    : error.c_str());
                if (retry.command_buffer == VK_NULL_HANDLE) break;
                if (retry.frame_slot != failed.frame_slot) {
                    CHECK(vulkan.end_frame(retry, error),
                          error.empty() ? "advance to failed skin frame slot"
                                        : error.c_str());
                    continue;
                }
                retried_same_slot =
                    renderer.begin_animation_skinning_frame(
                        retry.frame_slot, failed.serial) &&
                    renderer.submit_visible_animation_skinning(
                        retry.frame_slot,
                        {submission(0, 11), submission(1, 22)}, scene.frame,
                        scene.eye, 1.0f) &&
                    renderer.prepare_frame(
                        retry, scene.frame, scene.eye, 1.0f, error) &&
                    renderer.record_cull_and_render(
                        retry, scene.frame, scene.eye, 1.0f, error) &&
                    renderer.finish_animation_skinning_frame(
                        retry.frame_slot, retry.serial) &&
                    renderer.record_composite_to_swapchain(retry, error) &&
                    vulkan.end_frame(retry, error);
                CHECK(retried_same_slot,
                      error.empty()
                          ? "same-slot retry records grown skin descriptors"
                          : error.c_str());
            }
            vulkan.wait_idle();
            std::vector<viewer::VkSkinVertex> recovered_skin;
            CHECK(retried_same_slot &&
                      renderer.test_readback_animation_skin_output(
                          failed.frame_slot, vertex_count * 2,
                          recovered_skin, error) &&
                      recovered_skin.size() == vertex_count * 2 &&
                      std::isfinite(recovered_skin[0].position[0]) &&
                      recovered_skin[0].material_index ==
                          part.vertices[0].material_index &&
                      vulkan.validation_error_count() == 0,
                  error.empty()
                      ? "grown skin upload fault retries with valid readback and no device errors"
                      : error.c_str());
        }
    };

    for (uint32_t point = 0; point != 7; ++point) run_case(true, point);
    for (uint32_t point = 0; point != 5; ++point) run_case(false, point);
    CHECK(vulkan.validation_error_count() == 0,
          "animation skin allocation/upload fault matrix emits no validation errors");
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
    test_atmosphere_acceptance_fifo_parser_and_present_sequencer();
    const char* startup_smoke_mode = std::getenv("MATTER_VK_SMOKE_MODE");
    const bool animation_skin_only = startup_smoke_mode &&
        (std::string(startup_smoke_mode) == "animation-skin" ||
         std::string(startup_smoke_mode) == "animation-skin-faults");
    if (!animation_skin_only) {
        test_vulkan_lighting_override_contract();
        test_viewer_lighting_controls();
        run_vulkan_gi_math_tests();
        run_raster_mesh_material_contract_tests();
        run_rt_lod_payload_contract_tests();
        run_ray_tracing_capability_contract_tests();
        run_vulkan_instance_cache_tests();
        run_vulkan_temporal_tests();
        run_vulkan_gi_temporal_sequence_tests();
        run_streamline_bridge_fallback_tests();
        run_dlss_bridge_contract_tests();
    }
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
        (std::string(requested_smoke_mode) == "rt-unavailable" ||
         // WP-H: the RT-unavailable arm of the tier-2 enrichment gate. Ray
         // tracing is a device property, so the only honest way to test the
         // "no enricher" path is to bring the device up without it.
         std::string(requested_smoke_mode) == "vt-enrich-nort")) {
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
        if (!requested_smoke_mode ||
            std::string(requested_smoke_mode) == "default")
            run_display_transform_tests(*vulkan);
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
            run_static_append_upload_tests(*vulkan);
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
        if (smoke_mode && std::string(smoke_mode) == "animation-skin-faults") {
            run_animation_skin_record_fault_matrix(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "animation-skin") {
            run_animation_skin_gpu_readback_tests(*vulkan);
            run_animation_skin_record_fault_matrix(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "vt") {
            run_vt_path(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "vt-surfaces") {
            run_vt_surfaces_path(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && (std::string(smoke_mode) == "vt-enrich" ||
                           std::string(smoke_mode) == "vt-enrich-nort")) {
            run_vt_enrich_path(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "vt-rt") {
            run_vt_rt_path(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "tileset") {
            run_tileset_slot_load(*vulkan);
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
        if (smoke_mode && std::string(smoke_mode) == "froxel-resize") {
            run_rt_froxel_resize_smoke(*vulkan);
            std::printf("validation errors: %u\n",
                        vulkan->validation_error_count());
            vulkan->wait_idle();
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "rt-transmission") {
            run_rt_transmission_path(*vulkan);
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
        if (smoke_mode && std::string(smoke_mode) == "handle-diag-vulkan") {
            run_vulkan_only_handle_diagnostic(*vulkan);
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }
        if (smoke_mode && std::string(smoke_mode) == "atmosphere") {
            test_atmosphere_lighting_control_sanitization();
            test_atmosphere_irradiance_dispatch_contract();
            run_atmosphere_lut_smoke(*vulkan);
            run_atmosphere_presentation_sampling_tests(*vulkan);
            run_atmosphere_irradiance_last_valid_test(*vulkan);
            run_atmosphere_transaction_failure_tests(*vulkan);
            run_atmosphere_real_gpu_gate(*vulkan);
            std::printf("validation errors: %u\n", vulkan->validation_error_count());
            finish_vulkan_test(vulkan);
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return check_summary();
        }

        uint32_t retained_probe_destroyed = 0;
        run_frame_upload_tests(*vulkan);
        run_static_append_upload_tests(*vulkan);
        run_frame_record_tests(*vulkan);
        run_frame_resource_recovery_tests(*vulkan);
        run_tileset_slot_load(*vulkan);
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
