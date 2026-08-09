// cloud_layer_tests.cpp — the CPU side of the cloud density profile
// (matter/cloud_layers.h) and the prefix/compaction invariant the shader
// specialization depends on.
//
// What this suite is FOR. cloud_height_profile() has a GLSL twin in
// shaders_vk/vol_density.comp, and the two must agree; the GLSL side cannot be
// unit-tested without a GPU, so this pins the contract the shader is written
// against. If someone changes the profile here without changing the shader (or
// the reverse), these assertions are what should have caught it — read them as
// the specification, not as coverage.
//
// The shape under test:
//
//   max_density  |        ,----------------,
//                |       /                  .
//              0 +-----'                     '-------
//                     min                   max
//                       |<->|           |<->|
//                    falloff_min     falloff_max

#include "check.h"
#include "matter/cloud_layers.h"
#include "matter/world_definition.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

namespace {

struct CpuCloudPoint { float x, y, z; };
struct CpuCloudDensity { float coarse, full; };

CpuCloudDensity evaluate_old_cloud_density_for_test(
    const matter::GpuCloudLayer& layer, CpuCloudPoint world_pos,
    float time_seconds);
CpuCloudDensity evaluate_new_cloud_density_for_test(
    const matter::GpuCloudLayer& layer, CpuCloudPoint world_pos,
    float time_seconds);

float task9_clamp01(float value) {
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

float task9_smoothstep(float edge0, float edge1, float value) {
    const float t = task9_clamp01((value - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

uint32_t task9_hash3i(int32_t ix, int32_t iy, int32_t iz, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(ix) * 374761393u +
                 static_cast<uint32_t>(iy) * 3266489917u +
                 static_cast<uint32_t>(iz) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float task9_rand01_3(int32_t ix, int32_t iy, int32_t iz, uint32_t seed) {
    return static_cast<float>(task9_hash3i(ix, iy, iz, seed) & 0xffffffu) /
           16777216.0f;
}

float task9_smooth5(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float task9_value_noise3(float x, float y, float z, uint32_t seed) {
    const float fx0 = std::floor(x), fy0 = std::floor(y), fz0 = std::floor(z);
    const int32_t ix = static_cast<int32_t>(fx0);
    const int32_t iy = static_cast<int32_t>(fy0);
    const int32_t iz = static_cast<int32_t>(fz0);
    const float fx = x - fx0, fy = y - fy0, fz = z - fz0;
    const float c000 = task9_rand01_3(ix,     iy,     iz,     seed);
    const float c100 = task9_rand01_3(ix + 1, iy,     iz,     seed);
    const float c010 = task9_rand01_3(ix,     iy + 1, iz,     seed);
    const float c110 = task9_rand01_3(ix + 1, iy + 1, iz,     seed);
    const float c001 = task9_rand01_3(ix,     iy,     iz + 1, seed);
    const float c101 = task9_rand01_3(ix + 1, iy,     iz + 1, seed);
    const float c011 = task9_rand01_3(ix,     iy + 1, iz + 1, seed);
    const float c111 = task9_rand01_3(ix + 1, iy + 1, iz + 1, seed);
    const float u = task9_smooth5(fx), v = task9_smooth5(fy), w = task9_smooth5(fz);
    const float x00 = c000 + (c100 - c000) * u;
    const float x10 = c010 + (c110 - c010) * u;
    const float x01 = c001 + (c101 - c001) * u;
    const float x11 = c011 + (c111 - c011) * u;
    const float y0 = x00 + (x10 - x00) * v;
    const float y1 = x01 + (x11 - x01) * v;
    return y0 + (y1 - y0) * w;
}

float task9_fbm3(CpuCloudPoint p, uint32_t seed, int octaves, float gain,
                 float lacunarity) {
    float amplitude = 1.0f, sum = 0.0f, normalization = 0.0f, frequency = 1.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        float noise = task9_value_noise3(p.x * frequency, p.y * frequency,
                                         p.z * frequency,
                                         seed + static_cast<uint32_t>(octave) * 131u);
        noise = noise * 2.0f - 1.0f;
        sum += noise * amplitude;
        normalization += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return sum / normalization;
}

float task9_height_profile(const matter::GpuCloudLayer& layer, float y) {
    const float lo = layer.min_height, hi = layer.max_height;
    if (!(hi > lo) || y <= lo || y >= hi) return 0.0f;
    const float thickness = hi - lo;
    const float f_lo = std::clamp(layer.falloff_min, 0.0f, thickness);
    const float f_hi = std::clamp(layer.falloff_max, 0.0f, thickness);
    const float rise = f_lo > 0.0f ? task9_smoothstep(lo, lo + f_lo, y) : 1.0f;
    const float fall = f_hi > 0.0f ? 1.0f - task9_smoothstep(hi - f_hi, hi, y) : 1.0f;
    return rise < fall ? rise : fall;
}

float task9_cloud_fbm(const matter::GpuCloudLayer& layer, CpuCloudPoint p,
                      int octaves) {
    return task9_fbm3(p, static_cast<uint32_t>(layer.seed), octaves,
                      layer.gain, layer.lacunarity) * 0.5f + 0.5f;
}

CpuCloudDensity evaluate_old_cloud_density_for_test(
    const matter::GpuCloudLayer& layer, CpuCloudPoint world_pos,
    float time_seconds) {
    const float profile = task9_height_profile(layer, world_pos.y);
    if (profile <= 0.0f || layer.coverage <= 0.0f) return {};
    const CpuCloudPoint p{
        (world_pos.x + layer.wind[0] * time_seconds) * layer.noise_scale,
        (world_pos.y + layer.wind[1] * time_seconds) * layer.noise_scale,
        (world_pos.z + layer.wind[2] * time_seconds) * layer.noise_scale};
    const float threshold = 1.0f - layer.coverage;
    const float shape = task9_smoothstep(
        threshold - matter::kCloudCoverageEdge,
        threshold + matter::kCloudCoverageEdge,
        task9_cloud_fbm(layer, p, static_cast<int>(layer.octaves)));
    return {0.0f, profile * layer.max_density * shape};
}

CpuCloudDensity evaluate_new_cloud_density_for_test(
    const matter::GpuCloudLayer& layer, CpuCloudPoint world_pos,
    float time_seconds) {
    CpuCloudDensity result{};
    const float profile = task9_height_profile(layer, world_pos.y);
    if (profile <= 0.0f) return result;
    const CpuCloudPoint p{
        (world_pos.x + layer.wind[0] * time_seconds) * layer.noise_scale,
        (world_pos.y + layer.wind[1] * time_seconds) * layer.noise_scale,
        (world_pos.z + layer.wind[2] * time_seconds) * layer.noise_scale};
    float coverage = layer.coverage;
    if (layer.weather_influence > 0.0f) {
        const float weather = task9_cloud_fbm(
            layer, {world_pos.x * layer.weather_scale,
                    world_pos.z * layer.weather_scale, 0.0f}, 2);
        coverage = task9_clamp01(coverage + (weather - 0.5f) * 2.0f *
                                layer.weather_influence);
    }
    if (coverage <= 0.0f) return result;
    const float threshold = 1.0f - coverage;
    const int authored_octaves = static_cast<int>(layer.octaves);
    float full_shape = task9_smoothstep(
        threshold - matter::kCloudCoverageEdge,
        threshold + matter::kCloudCoverageEdge,
        task9_cloud_fbm(layer, p, authored_octaves));
    const float coarse_shape = task9_smoothstep(
        threshold - matter::kCloudCoverageEdge,
        threshold + matter::kCloudCoverageEdge,
        task9_cloud_fbm(layer, p, authored_octaves < 2 ? authored_octaves : 2));
    result.coarse = profile * layer.max_density * coarse_shape;
    if (layer.shape_bias != 0.0f)
        full_shape = task9_clamp01(full_shape + layer.shape_bias);
    result.full = profile * layer.max_density * full_shape;
    if (layer.detail_erosion > 0.0f) {
        const float detail01 = task9_cloud_fbm(
            layer, {world_pos.x * layer.detail_scale,
                    world_pos.y * layer.detail_scale,
                    world_pos.z * layer.detail_scale}, 3);
        const float detail = task9_smoothstep(0.2f, 0.8f, detail01);
        result.full *= 1.0f + (detail - 1.0f) * layer.detail_erosion;
    }
    return result;
}

bool nearly_equal(float a, float b, float tol = 1e-4f) {
    const float d = a - b;
    return (d < 0.0f ? -d : d) <= tol;
}

matter::CloudLayer make_layer(float min_h, float max_h, float density,
                              float f_min, float f_max) {
    matter::CloudLayer layer;
    layer.enabled = true;
    layer.min_height = min_h;
    layer.max_height = max_h;
    layer.max_density = density;
    layer.falloff_min = f_min;
    layer.falloff_max = f_max;
    return layer;
}

// The containment property that makes layers composable at all: a deck
// contributes NOTHING outside its own bounds. This is the whole difference
// from the single `height_layer` it replaces, which was solid all the way to
// the bottom of the world.
void test_density_is_zero_outside_the_layer() {
    const matter::CloudLayer layer = make_layer(100.0f, 200.0f, 0.5f, 10.0f, 20.0f);

    CHECK(nearly_equal(matter::cloud_height_profile(layer, -1000.0f), 0.0f),
          "zero far below the layer");
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 0.0f), 0.0f),
          "zero at the ground");
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 99.9f), 0.0f),
          "zero just below min_height");
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 100.0f), 0.0f),
          "zero exactly at min_height");
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 200.0f), 0.0f),
          "zero exactly at max_height");
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 200.1f), 0.0f),
          "zero just above max_height");
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 5000.0f), 0.0f),
          "zero far above the layer");
}

// max_density must be a density the deck REACHES, not one it approaches. That
// is why the two shoulders combine with min() rather than multiplying: the
// product would dip below 1 everywhere and make the authored number a lie.
void test_density_peaks_at_max_density_inside() {
    const matter::CloudLayer layer = make_layer(100.0f, 200.0f, 0.5f, 10.0f, 20.0f);

    // Plateau: above min+falloff_min (110) and below max-falloff_max (180).
    for (float y : {111.0f, 130.0f, 150.0f, 170.0f, 179.0f}) {
        CHECK(nearly_equal(matter::cloud_height_profile(layer, y), 1.0f),
              "profile is exactly 1 on the plateau");
    }
    CHECK(nearly_equal(layer.max_density *
                           matter::cloud_height_profile(layer, 150.0f),
                       0.5f),
          "extinction on the plateau equals max_density");
}

// The two shoulders are independent, which is the point of having two.
void test_falloffs_are_independent() {
    const matter::CloudLayer layer = make_layer(100.0f, 200.0f, 1.0f, 10.0f, 50.0f);

    // Bottom shoulder spans 100..110, so its midpoint is 105.
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 105.0f), 0.5f),
          "bottom shoulder is half height at its midpoint");
    // Top shoulder spans 150..200, so its midpoint is 175 — a completely
    // different width from the bottom's.
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 175.0f), 0.5f),
          "top shoulder is half height at its own midpoint");
    // And the plateau between them is still a plateau.
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 130.0f), 1.0f),
          "asymmetric shoulders still leave a full-strength plateau");

    // Monotonic up through the bottom shoulder and down through the top.
    float prev = -1.0f;
    for (float y = 100.5f; y < 110.0f; y += 0.5f) {
        const float v = matter::cloud_height_profile(layer, y);
        CHECK(v >= prev, "bottom shoulder rises monotonically");
        prev = v;
    }
    prev = 2.0f;
    for (float y = 150.5f; y < 200.0f; y += 0.5f) {
        const float v = matter::cloud_height_profile(layer, y);
        CHECK(v <= prev, "top shoulder falls monotonically");
        prev = v;
    }
}

// A hard-edged deck: falloff 0 on both sides is a box, not a degenerate case.
void test_zero_falloff_is_a_hard_edge() {
    const matter::CloudLayer layer = make_layer(100.0f, 200.0f, 1.0f, 0.0f, 0.0f);
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 100.001f), 1.0f),
          "full density immediately above a hard bottom edge");
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 199.999f), 1.0f),
          "full density immediately below a hard top edge");
    CHECK(nearly_equal(matter::cloud_height_profile(layer, 100.0f), 0.0f),
          "still zero AT the bound");
}

// Over-large shoulders must degrade to "soft all the way across" rather than
// inverting the profile or producing something above 1.
void test_oversized_falloffs_stay_bounded() {
    const matter::CloudLayer layer = make_layer(100.0f, 200.0f, 1.0f, 500.0f, 500.0f);
    for (float y = 100.5f; y < 200.0f; y += 2.5f) {
        const float v = matter::cloud_height_profile(layer, y);
        CHECK(v >= 0.0f && v <= 1.0f,
              "profile stays in [0,1] with shoulders wider than the layer");
    }
    CHECK(matter::cloud_height_profile(layer, 150.0f) > 0.0f,
          "the middle of an all-shoulder layer is still non-zero");
}

// A disabled layer, and a layer whose bounds are inverted or degenerate,
// contribute nothing anywhere.
void test_inert_layers() {
    matter::CloudLayer off = make_layer(100.0f, 200.0f, 1.0f, 0.0f, 0.0f);
    off.enabled = false;
    CHECK(nearly_equal(matter::cloud_height_profile(off, 150.0f), 0.0f),
          "a disabled layer contributes nothing");

    const matter::CloudLayer inverted = make_layer(200.0f, 100.0f, 1.0f, 0.0f, 0.0f);
    CHECK(nearly_equal(matter::cloud_height_profile(inverted, 150.0f), 0.0f),
          "max below min contributes nothing");

    const matter::CloudLayer flat = make_layer(100.0f, 100.0f, 1.0f, 0.0f, 0.0f);
    CHECK(nearly_equal(matter::cloud_height_profile(flat, 100.0f), 0.0f),
          "a zero-thickness layer contributes nothing");
}

// THE compositing rule: overlapping decks SUM their extinction. max() would
// make a thin veil vanish inside a thick base instead of thickening it, which
// is the behaviour that makes a layered sky worth having.
void test_two_overlapping_layers_sum() {
    // 100..200 at 0.30, and 150..250 at 0.20. They share 150..200.
    const matter::CloudLayer low = make_layer(100.0f, 200.0f, 0.30f, 0.0f, 0.0f);
    const matter::CloudLayer high = make_layer(150.0f, 250.0f, 0.20f, 0.0f, 0.0f);

    auto total = [&](float y) {
        return low.max_density * matter::cloud_height_profile(low, y) +
               high.max_density * matter::cloud_height_profile(high, y);
    };

    CHECK(nearly_equal(total(120.0f), 0.30f), "below the overlap: low only");
    CHECK(nearly_equal(total(175.0f), 0.50f),
          "inside the overlap: the two densities SUM (0.30 + 0.20)");
    CHECK(nearly_equal(total(225.0f), 0.20f), "above the overlap: high only");
    CHECK(nearly_equal(total(50.0f), 0.0f), "below both: nothing");
    CHECK(nearly_equal(total(300.0f), 0.0f), "above both: nothing");
    CHECK(total(175.0f) > total(120.0f) && total(175.0f) > total(225.0f),
          "the overlap is denser than either layer alone - this is what "
          "distinguishes summing from max()");
}

// Two SEPARATED decks with clear air between them — the configuration the old
// single layer could not express at all, because it filled everything below
// its own minimum.
void test_two_separated_layers_leave_clear_air() {
    const matter::CloudLayer low = make_layer(100.0f, 150.0f, 0.4f, 0.0f, 0.0f);
    const matter::CloudLayer high = make_layer(300.0f, 400.0f, 0.1f, 0.0f, 0.0f);

    auto total = [&](float y) {
        return low.max_density * matter::cloud_height_profile(low, y) +
               high.max_density * matter::cloud_height_profile(high, y);
    };

    CHECK(nearly_equal(total(125.0f), 0.4f), "lower deck at its own height");
    CHECK(nearly_equal(total(200.0f), 0.0f),
          "clear air BETWEEN the decks - the whole point of a bounded layer");
    CHECK(nearly_equal(total(250.0f), 0.0f), "still clear just below the upper deck");
    CHECK(nearly_equal(total(350.0f), 0.1f), "upper deck at its own height");
    CHECK(nearly_equal(total(50.0f), 0.0f),
          "clear air BELOW the lower deck - the old profile was solid here");
}

// ---------------------------------------------------------------------------
// The prefix invariant
//
// vol_density.comp is specialized on the layer COUNT and reads entries
// [0, count). A hole in the middle of the array would therefore make the
// shader read a dead entry, or read layer 1's parameters as layer 0's. These
// pin the two functions that prevent it.
// ---------------------------------------------------------------------------

void test_active_cloud_count_stops_at_the_first_hole() {
    matter::FogSettings fog;
    fog.cloud_count = 3;
    fog.clouds[0] = make_layer(100.0f, 200.0f, 0.1f, 0.0f, 0.0f);
    fog.clouds[1] = make_layer(300.0f, 400.0f, 0.1f, 0.0f, 0.0f);
    fog.clouds[2] = make_layer(500.0f, 600.0f, 0.1f, 0.0f, 0.0f);
    CHECK(matter::active_cloud_count(fog) == 3, "three contiguous layers");

    fog.clouds[1].enabled = false;
    CHECK(matter::active_cloud_count(fog) == 1,
          "a disabled middle layer truncates the count rather than being "
          "skipped - the shader reads a prefix");

    fog.clouds[1].enabled = true;
    fog.clouds[1].max_height = fog.clouds[1].min_height;
    CHECK(matter::active_cloud_count(fog) == 1,
          "a degenerate middle layer truncates too");

    fog.cloud_count = 99;
    fog.clouds[1] = make_layer(300.0f, 400.0f, 0.1f, 0.0f, 0.0f);
    fog.clouds[3] = make_layer(700.0f, 800.0f, 0.1f, 0.0f, 0.0f);
    CHECK(matter::active_cloud_count(fog) == 4,
          "the count is clamped to kMaxCloudLayers even when cloud_count lies");
}

// Persistence across a restart (editor-cloud-deck-cannot-be-enabled).
//
// Every write path that sets a layer WITHOUT running compaction leaves
// `cloud_count` stale: the World-scope property file loaded in
// EditorProps::on_world_connected, MATTER_CLOUD_LAYER<i>, and the FIFO
// `set render.clouds.layerN_enabled`. None of them compact. While
// active_cloud_count clamped its scan to that field, a deck enabled through
// any of them rendered nothing.
//
// The 0-deck case below is the one the user actually hit: LightingGarden's
// script authors no clouds, so the loader compacts to cloud_count == 0. A deck
// enabled in the panel worked, autosaved to the world file, and was then
// invisible on the next launch — until some unrelated cloud field was nudged
// and tripped the panel's compaction, which made it reappear. Intermittent
// resurrection is a far worse bug to chase than the original bounce.
void test_active_cloud_count_ignores_a_stale_cloud_count() {
    matter::FogSettings fog;
    // Exactly the post-load state for a world whose script authors no decks
    // and whose property override file enables one.
    fog.cloud_count = 0;
    fog.clouds[0] = make_layer(150.0f, 250.0f, 0.02f, 0.0f, 0.0f);
    CHECK(matter::active_cloud_count(fog) == 1,
          "a layer enabled by an override file is live even though the world "
          "script left cloud_count at 0 and nothing has compacted since");

    // The same hazard one deck up: a world authoring one deck, an override
    // file adding a second.
    fog.cloud_count = 1;
    fog.clouds[1] = make_layer(400.0f, 600.0f, 0.03f, 0.0f, 0.0f);
    CHECK(matter::active_cloud_count(fog) == 2,
          "an override may add a deck beyond the count the world compacted to");

    // And the field must not be able to over-report either: a stale count
    // ahead of the layers is still bounded by the prefix scan, so a lying
    // cloud_count cannot make the shader read a dead entry.
    matter::FogSettings empty;
    empty.cloud_count = 4;
    CHECK(matter::active_cloud_count(empty) == 0,
          "a cloud_count ahead of the layers still yields no live decks");
}

void test_compact_clouds_closes_holes() {
    matter::FogSettings fog;
    fog.cloud_count = 3;
    fog.clouds[0] = make_layer(100.0f, 200.0f, 0.1f, 0.0f, 0.0f);
    fog.clouds[1] = make_layer(300.0f, 400.0f, 0.2f, 0.0f, 0.0f);
    fog.clouds[2] = make_layer(500.0f, 600.0f, 0.3f, 0.0f, 0.0f);

    // Switching off layer 0 is the ordinary editor gesture, and the one that
    // would otherwise render layer 1's parameters as layer 0's.
    fog.clouds[0].enabled = false;
    matter::compact_clouds(fog);

    CHECK(fog.cloud_count == 2, "compaction leaves two live layers");
    CHECK(nearly_equal(fog.clouds[0].min_height, 300.0f) &&
              nearly_equal(fog.clouds[0].max_density, 0.2f),
          "the survivor slid down into slot 0 intact");
    CHECK(nearly_equal(fog.clouds[1].min_height, 500.0f),
          "and the one behind it followed");
    // STABLE PARTITION, not a wipe (editor-cloud-deck-cannot-be-enabled):
    // the disabled layer moves to the tail with its parameters intact rather
    // than being reset to CloudLayer{}. Slot 3 was never touched and was
    // already a CloudLayer{} default, so it stays that way — this is not
    // "still zeroed", it is "never had anything to preserve".
    CHECK(!fog.clouds[2].enabled, "the disabled layer moved to the tail");
    CHECK(nearly_equal(fog.clouds[2].min_height, 100.0f) &&
              nearly_equal(fog.clouds[2].max_height, 200.0f) &&
              nearly_equal(fog.clouds[2].max_density, 0.1f),
          "its parameters survive the move intact, not zeroed");
    CHECK(!fog.clouds[3].enabled, "the never-authored slot stays off");
    CHECK(matter::active_cloud_count(fog) == 2,
          "the compacted array satisfies the prefix invariant");
}

// Fault B (editor-cloud-deck-cannot-be-enabled): switching a deck off must
// PARK it, not destroy it. A round trip off -> compact -> back on must
// reproduce the original authored deck exactly, not a degenerate default.
void test_disabled_layer_survives_compaction_and_reenable() {
    matter::FogSettings fog;
    fog.cloud_count = 2;
    fog.clouds[0] = make_layer(130.0f, 210.0f, 0.02f, 6.0f, 45.0f);
    fog.clouds[1] = make_layer(420.0f, 520.0f, 0.009f, 30.0f, 30.0f);

    // Switch deck 0 off - the ordinary editor gesture (property_editor.cpp
    // calls compact_clouds on every edit).
    fog.clouds[0].enabled = false;
    matter::compact_clouds(fog);

    CHECK(fog.cloud_count == 1, "only the still-enabled deck is live");
    CHECK(nearly_equal(fog.clouds[0].min_height, 420.0f),
          "the survivor slid to the front");

    // The parked deck's authored values must still be sitting in the tail,
    // not wiped - this is the whole fix for the data-loss half of the bug.
    CHECK(!fog.clouds[1].enabled, "the parked slot reads as off");
    CHECK(nearly_equal(fog.clouds[1].min_height, 130.0f) &&
              nearly_equal(fog.clouds[1].max_height, 210.0f) &&
              nearly_equal(fog.clouds[1].max_density, 0.02f) &&
              nearly_equal(fog.clouds[1].falloff_min, 6.0f) &&
              nearly_equal(fog.clouds[1].falloff_max, 45.0f),
          "the disabled deck's parameters are parked intact");

    // Switching it back on must bring those authored values with it.
    fog.clouds[1].enabled = true;
    matter::compact_clouds(fog);

    CHECK(fog.cloud_count == 2, "both decks are live again");
    bool found_original = false;
    for (int i = 0; i < 2; ++i) {
        if (nearly_equal(fog.clouds[i].min_height, 130.0f) &&
            nearly_equal(fog.clouds[i].max_height, 210.0f) &&
            nearly_equal(fog.clouds[i].max_density, 0.02f))
            found_original = true;
    }
    CHECK(found_original,
          "the re-enabled deck's original authored values are back, not a "
          "degenerate CloudLayer{} default");
}

// Fault A (editor-cloud-deck-cannot-be-enabled): a fresh/degenerate layer -
// CloudLayer{} defaults, exactly what a world authoring no clouds hands the
// editor - must become live the instant it is seeded and enabled, which is
// the panel's actual sequence: tick enabled -> seed_default_cloud_layer ->
// compact_clouds.
void test_seed_default_cloud_layer_makes_a_degenerate_layer_live() {
    matter::FogSettings fog;
    // All four slots are CloudLayer{} defaults here.
    fog.clouds[0].enabled = true;  // the panel checkbox click
    CHECK(matter::active_cloud_count(fog) == 0,
          "still degenerate before seeding - the bounce the issue reports");

    matter::seed_default_cloud_layer(fog.clouds[0]);
    matter::compact_clouds(fog);

    CHECK(matter::active_cloud_count(fog) == 1,
          "the seeded layer is now counted as live");
    CHECK(fog.clouds[0].enabled, "seeding does not touch enabled itself");
    CHECK(fog.clouds[0].max_height > fog.clouds[0].min_height,
          "seeding fixes the degenerate height pair");
    CHECK(fog.clouds[0].max_density > 0.0f,
          "seeding fixes max_density too - a well-formed but zero-density "
          "deck would still render nothing");
    // Altitude must sit inside the froxel volume (VOL_FROXEL_FAR in
    // vol_common.glsl, 3000 m of view-space depth from the camera) or the
    // seeded deck is invisible and looks exactly like this bug again.
    CHECK(fog.clouds[0].max_height < 3000.0f,
          "the seeded deck sits inside the froxel volume's far range");

    // A no-op on an already well-formed layer - the helper must not stomp an
    // authored deck just because it happened to be re-enabled.
    matter::CloudLayer authored =
        make_layer(420.0f, 520.0f, 0.009f, 30.0f, 30.0f);
    matter::seed_default_cloud_layer(authored);
    CHECK(nearly_equal(authored.min_height, 420.0f) &&
              nearly_equal(authored.max_height, 520.0f) &&
              nearly_equal(authored.max_density, 0.009f),
          "seeding a well-formed layer is a no-op");
}

// The GPU mirror must stay exactly 96 bytes and must carry every authored
// field. A silent layout drift here shows up as garbage clouds, not a crash.
void test_gpu_packing_round_trip() {
    CHECK(sizeof(matter::GpuCloudLayer) == 96,
          "GpuCloudLayer is 96 bytes - vol_density.comp's SSBO stride");

    matter::CloudLayer in = make_layer(120.0f, 260.0f, 0.045f, 8.0f, 40.0f);
    in.noise_scale = 0.0016f;
    in.octaves = 3;
    in.lacunarity = 2.03f;
    in.gain = 0.5f;
    in.coverage = 0.62f;
    in.wind[0] = 1.5f;
    in.wind[1] = 0.0f;
    in.wind[2] = -0.25f;
    in.weather_scale = 0.00025f;
    in.weather_influence = 0.6f;
    in.detail_scale = 0.012f;
    in.detail_erosion = 0.35f;
    in.shape_bias = -0.1f;

    matter::GpuCloudLayer out{};
    matter::pack_cloud_layer(in, 2, out);

    CHECK(nearly_equal(out.min_height, 120.0f) &&
              nearly_equal(out.max_height, 260.0f) &&
              nearly_equal(out.falloff_min, 8.0f) &&
              nearly_equal(out.falloff_max, 40.0f),
          "bounds and shoulders packed");
    CHECK(nearly_equal(out.max_density, 0.045f) &&
              nearly_equal(out.noise_scale, 0.0016f) &&
              nearly_equal(out.coverage, 0.62f),
          "density, scale and coverage packed");
    CHECK(nearly_equal(out.lacunarity, 2.03f) &&
              nearly_equal(out.gain, 0.5f) &&
              nearly_equal(out.octaves, 3.0f),
          "harmonics packed");
    CHECK(nearly_equal(out.wind[0], 1.5f) && nearly_equal(out.wind[1], 0.0f) &&
              nearly_equal(out.wind[2], -0.25f),
          "per-layer wind packed");
    CHECK(nearly_equal(out.seed,
                       static_cast<float>(matter::cloud_layer_seed(2))),
          "the seed is derived from the layer index");
    CHECK(!nearly_equal(static_cast<float>(matter::cloud_layer_seed(0)),
                        static_cast<float>(matter::cloud_layer_seed(1))),
          "identical layers at different indices get different noise");
    CHECK(nearly_equal(out.weather_scale, 0.00025f) &&
              nearly_equal(out.weather_influence, 0.6f) &&
              nearly_equal(out.detail_scale, 0.012f) &&
              nearly_equal(out.detail_erosion, 0.35f) &&
              nearly_equal(out.shape_bias, -0.1f),
          "weather and detail controls pack without loss");
}

// This is the ABI boundary for the cloud SSBO. The density shader may not use
// the new controls until Task 9, but changing C++ stride without extending its
// GLSL mirror would make every following element read at the wrong offset.
void test_gpu_cloud_layer_shader_layout_contract() {
    std::ifstream input("../shaders_vk/vol_common.glsl", std::ios::binary);
    CHECK(static_cast<bool>(input), "cloud GLSL source is available to pin the SSBO contract");
    const std::string source((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    CHECK(source.find("GpuCloudLayer in matter/cloud_layers.h (96 bytes std430)") !=
              std::string::npos,
          "GLSL mirror declares the 96-byte cloud-layer stride");
    CHECK(source.find("vec4 weather_scale_influence_detail_scale_detail_erosion;") !=
              std::string::npos,
          "GLSL appends weatherScale, weatherInfluence, detailScale, detailErosion as one vec4");
    CHECK(source.find("vec4 shape_bias_padding;") != std::string::npos,
          "GLSL appends shapeBias plus three zero padding lanes as one vec4");
}

// The octave ceiling is a frame-cost guard, not a preference: octaves
// multiply the per-froxel work directly across 1.84M froxels.
void test_sanitize_clamps_the_cost_dials() {
    matter::CloudLayer layer = make_layer(100.0f, 200.0f, 1.0f, 0.0f, 0.0f);
    layer.octaves = 64;
    layer.coverage = 3.0f;
    layer.gain = -1.0f;
    layer.noise_scale = 0.0f;
    layer.weather_scale = 0.0f;
    layer.weather_influence = -1.0f;
    layer.detail_scale = -1.0f;
    layer.detail_erosion = 2.0f;
    layer.shape_bias = -2.0f;
    matter::sanitize_cloud_layer(layer);

    CHECK(layer.octaves == matter::kMaxCloudOctaves,
          "octaves clamped to the ceiling");
    CHECK(nearly_equal(layer.coverage, 1.0f), "coverage clamped to [0,1]");
    CHECK(nearly_equal(layer.gain, 0.0f), "gain clamped to [0,1]");
    CHECK(layer.noise_scale > 0.0f, "a zero noise scale is replaced");
    CHECK(layer.weather_scale > 0.0f && layer.detail_scale > 0.0f,
          "non-positive weather and detail scales use safe defaults");
    CHECK(nearly_equal(layer.weather_influence, 0.0f) &&
              nearly_equal(layer.detail_erosion, 1.0f) &&
              nearly_equal(layer.shape_bias, -1.0f),
          "weather influence, erosion and shape bias are bounded");

    matter::CloudLayer degenerate = make_layer(200.0f, 100.0f, 1.0f, 0.0f, 0.0f);
    matter::sanitize_cloud_layer(degenerate);
    CHECK(!degenerate.enabled, "an inverted layer is switched off, not kept");
}

void test_task9_shared_density_and_optional_r16f_contract() {
    std::ifstream density("../shaders_vk/cloud_density.glsl", std::ios::binary);
    const std::string shared((std::istreambuf_iterator<char>(density)),
                             std::istreambuf_iterator<char>());
    std::ifstream shader("../shaders_vk/vol_density.comp", std::ios::binary);
    const std::string volume((std::istreambuf_iterator<char>(shader)),
                             std::istreambuf_iterator<char>());
    std::ifstream host("../src/render/vk_volumetrics.cpp", std::ios::binary);
    const std::string vk((std::istreambuf_iterator<char>(host)),
                         std::istreambuf_iterator<char>());
    std::ifstream composite_file("../shaders_vk/composite.frag", std::ios::binary);
    const std::string composite((std::istreambuf_iterator<char>(composite_file)),
                                std::istreambuf_iterator<char>());
    std::ifstream harness_file("../tools/atmosphere_cloud_shots.sh", std::ios::binary);
    const std::string harness((std::istreambuf_iterator<char>(harness_file)),
                              std::istreambuf_iterator<char>());
    CHECK(shared.find("CloudDensitySample evaluate_cloud_density") != std::string::npos &&
              volume.find("ENHANCED_CLOUDS") != std::string::npos &&
              volume.find("vol_cloud_density") != std::string::npos &&
              vk.find("VK_FORMAT_R16_SFLOAT") != std::string::npos &&
              vk.find("density_pipelines_[count][enhanced]") != std::string::npos &&
              vk.find("cloud_density_dummy_") != std::string::npos,
          "Task 9 shares cloud density and only allocates R16F extinction for enhanced clouds");
    const size_t cloud_debug = composite.find("Cloud density must precede every GBuffer/sky early-out");
    const size_t sky_early_out = composite.find("if (normal_length_squared <= 1e-20)");
    CHECK(cloud_debug != std::string::npos && sky_early_out != std::string::npos &&
              cloud_debug < sky_early_out &&
              composite.find("for (int z = 0; z < depth_slices; ++z)") != std::string::npos &&
              volume.find("fog_albedo * non_cloud_extinction + vec3(0.99) * cloud_extinction") !=
                  std::string::npos,
          "Task 9 debug covers full sky rays and enhanced scattering excludes cloud double-counting");
    CHECK(harness.find("historical Task7 comparison is diagnostic only") != std::string::npos &&
              harness.find("current-repeat.png\" --max-diff-pct 10.0") != std::string::npos,
          "Task 9 capture gates same-process static repeat while retaining Task7 as a diagnostic");
    CHECK(shared.find("if (L.weather_scale_influence_detail_scale_detail_erosion.y > 0.0)") !=
              std::string::npos &&
              shared.find("if (L.shape_bias_padding.x != 0.0)") !=
                  std::string::npos &&
              shared.find("result.full = profile * L.max_density * full_shape;") !=
                  std::string::npos &&
              shared.find("if (L.weather_scale_influence_detail_scale_detail_erosion.w > 0.0)") !=
                  std::string::npos,
          "neutral density preserves the authored full-octave multiplication before optional controls");
    CHECK(vk.find("enhanced_clouds_requested_ =\n"
                  "        matter::enhanced_cloud_lighting(vol, shadows);") !=
              std::string::npos,
          "Task 9 runtime allocation uses the canonical enhanced-cloud predicate");
}

matter::GpuCloudLayer task9_numerical_layer(int index = 0) {
    matter::CloudLayer layer = make_layer(100.0f, 300.0f, 0.0375f, 20.0f, 30.0f);
    layer.noise_scale = 0.00425f;
    layer.coverage = 0.58f;
    layer.lacunarity = 2.03f;
    layer.gain = 0.5f;
    layer.octaves = 4;
    layer.wind[0] = 1.25f;
    layer.wind[1] = -0.1f;
    layer.wind[2] = -0.75f;
    layer.weather_scale = 0.0013f;
    layer.detail_scale = 0.017f;
    matter::GpuCloudLayer gpu{};
    matter::pack_cloud_layer(layer, index, gpu);
    return gpu;
}

void test_task9_neutral_density_numerical_parity() {
    const matter::GpuCloudLayer layer = task9_numerical_layer();
    constexpr std::array<CpuCloudPoint, 5> positions{{
        {13.25f, 125.0f, -44.75f}, {240.5f, 150.0f, 91.0f},
        {-333.0f, 200.0f, 612.0f}, {999.25f, 250.0f, -721.5f},
        {-75.5f, 285.0f, 18.25f}}};
    for (CpuCloudPoint p : positions) {
        const CpuCloudDensity old_density =
            evaluate_old_cloud_density_for_test(layer, p, 17.25f);
        const CpuCloudDensity new_density =
            evaluate_new_cloud_density_for_test(layer, p, 17.25f);
        CHECK(std::fabs(old_density.full - new_density.full) <= 1e-6f,
              "neutral Task 9 full density preserves the authored evaluator");
        CHECK(std::isfinite(new_density.coarse) &&
                  std::isfinite(new_density.full) &&
                  new_density.coarse >= 0.0f && new_density.full >= 0.0f,
              "coarse and full numerical cloud density stay finite and nonnegative");
    }
}

void test_task9_density_semantics() {
    const CpuCloudPoint inside{240.5f, 200.0f, 91.0f};
    matter::GpuCloudLayer layer = task9_numerical_layer();
    CHECK(evaluate_new_cloud_density_for_test(layer, {inside.x, 100.0f, inside.z}, 0.0f).full == 0.0f &&
              evaluate_new_cloud_density_for_test(layer, {inside.x, 300.0f, inside.z}, 0.0f).full == 0.0f,
          "full density is zero at and outside the cloud height bounds");

    layer.coverage = 0.0f;
    const CpuCloudDensity clear = evaluate_new_cloud_density_for_test(layer, inside, 0.0f);
    layer.coverage = 1.0f;
    const CpuCloudDensity filled = evaluate_new_cloud_density_for_test(layer, inside, 0.0f);
    CHECK(clear.coarse == 0.0f && clear.full == 0.0f,
          "coverage zero clears coarse and full density");
    CHECK(filled.coarse > 0.0f && filled.full > 0.0f &&
              std::fabs(filled.coarse - layer.max_density) <= 1e-6f &&
              std::fabs(filled.full - layer.max_density) <= 1e-6f,
          "coverage one produces bounded maximally filled cloud density");
    layer.coverage = 0.0f;
    layer.detail_erosion = 1.0f;
    const CpuCloudDensity clear_eroded =
        evaluate_new_cloud_density_for_test(layer, inside, 0.0f);
    CHECK(clear_eroded.coarse == 0.0f && clear_eroded.full == 0.0f,
          "detail erosion cannot create density in a clear base body");

    layer = task9_numerical_layer();
    layer.weather_influence = 0.8f;
    const CpuCloudDensity weather_a = evaluate_new_cloud_density_for_test(layer, inside, 11.0f);
    const CpuCloudDensity weather_b = evaluate_new_cloud_density_for_test(layer, inside, 11.0f);
    CHECK(weather_a.coarse == weather_b.coarse && weather_a.full == weather_b.full &&
              weather_a.coarse >= 0.0f && weather_a.coarse <= layer.max_density &&
              weather_a.full >= 0.0f && weather_a.full <= layer.max_density,
          "weather modulation is deterministic and density-bounded");

    layer.weather_influence = 0.0f;
    layer.detail_erosion = 0.0f;
    const CpuCloudDensity uneroded = evaluate_new_cloud_density_for_test(layer, inside, 11.0f);
    layer.detail_erosion = 1.0f;
    const CpuCloudDensity eroded = evaluate_new_cloud_density_for_test(layer, inside, 11.0f);
    CHECK(eroded.full <= uneroded.full + 1e-7f &&
              (uneroded.full != 0.0f || eroded.full == 0.0f),
          "detail erosion never creates or increases cloud density");
}

void test_task9_derived_seeds_decorrelate_density_samples() {
    const matter::GpuCloudLayer first = task9_numerical_layer(0);
    const matter::GpuCloudLayer second = task9_numerical_layer(1);
    bool differs = false;
    for (CpuCloudPoint p : std::array<CpuCloudPoint, 4>{{
             {13.0f, 180.0f, 29.0f}, {171.0f, 200.0f, -85.0f},
             {-241.0f, 220.0f, 377.0f}, {790.0f, 250.0f, -633.0f}}}) {
        const float a = evaluate_new_cloud_density_for_test(first, p, 3.0f).full;
        const float b = evaluate_new_cloud_density_for_test(second, p, 3.0f).full;
        differs = differs || std::fabs(a - b) > 1e-6f;
    }
    CHECK(differs, "derived cloud-layer seeds decorrelate otherwise identical samples");
}

}  // namespace

int main() {
    test_density_is_zero_outside_the_layer();
    test_density_peaks_at_max_density_inside();
    test_falloffs_are_independent();
    test_zero_falloff_is_a_hard_edge();
    test_oversized_falloffs_stay_bounded();
    test_inert_layers();
    test_two_overlapping_layers_sum();
    test_two_separated_layers_leave_clear_air();
    test_active_cloud_count_stops_at_the_first_hole();
    test_active_cloud_count_ignores_a_stale_cloud_count();
    test_compact_clouds_closes_holes();
    test_disabled_layer_survives_compaction_and_reenable();
    test_seed_default_cloud_layer_makes_a_degenerate_layer_live();
    test_gpu_packing_round_trip();
    test_gpu_cloud_layer_shader_layout_contract();
    test_sanitize_clamps_the_cost_dials();
    test_task9_shared_density_and_optional_r16f_contract();
    test_task9_neutral_density_numerical_parity();
    test_task9_density_semantics();
    test_task9_derived_seeds_decorrelate_density_samples();
    return check_summary();
}
