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

#include <cstdio>

namespace {

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
    CHECK(!fog.clouds[2].enabled && !fog.clouds[3].enabled,
          "vacated slots are cleared, not left stale");
    CHECK(matter::active_cloud_count(fog) == 2,
          "the compacted array satisfies the prefix invariant");
}

// The GPU mirror must stay exactly 64 bytes and must carry every authored
// field. A silent layout drift here shows up as garbage clouds, not a crash.
void test_gpu_packing_round_trip() {
    CHECK(sizeof(matter::GpuCloudLayer) == 64,
          "GpuCloudLayer is 64 bytes - vol_density.comp's SSBO stride");

    matter::CloudLayer in = make_layer(120.0f, 260.0f, 0.045f, 8.0f, 40.0f);
    in.noise_scale = 0.0016f;
    in.octaves = 3;
    in.lacunarity = 2.03f;
    in.gain = 0.5f;
    in.coverage = 0.62f;
    in.wind[0] = 1.5f;
    in.wind[1] = 0.0f;
    in.wind[2] = -0.25f;

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
}

// The octave ceiling is a frame-cost guard, not a preference: octaves
// multiply the per-froxel work directly across 1.84M froxels.
void test_sanitize_clamps_the_cost_dials() {
    matter::CloudLayer layer = make_layer(100.0f, 200.0f, 1.0f, 0.0f, 0.0f);
    layer.octaves = 64;
    layer.coverage = 3.0f;
    layer.gain = -1.0f;
    layer.noise_scale = 0.0f;
    matter::sanitize_cloud_layer(layer);

    CHECK(layer.octaves == matter::kMaxCloudOctaves,
          "octaves clamped to the ceiling");
    CHECK(nearly_equal(layer.coverage, 1.0f), "coverage clamped to [0,1]");
    CHECK(nearly_equal(layer.gain, 0.0f), "gain clamped to [0,1]");
    CHECK(layer.noise_scale > 0.0f, "a zero noise scale is replaced");

    matter::CloudLayer degenerate = make_layer(200.0f, 100.0f, 1.0f, 0.0f, 0.0f);
    matter::sanitize_cloud_layer(degenerate);
    CHECK(!degenerate.enabled, "an inverted layer is switched off, not kept");
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
    test_compact_clouds_closes_holes();
    test_gpu_packing_round_trip();
    test_sanitize_clamps_the_cost_dials();
    return check_summary();
}
