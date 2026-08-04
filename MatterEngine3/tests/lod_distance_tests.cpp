// lod_distance_tests.cpp — prove the distance form of LOD selection picks the
// same rung as the projected-size form it replaces.
//
// M1 is required to be visually inert, so the conversion in lod_distance.h has
// to be shown equivalent BEFORE any consumer depends on it. This sweeps a wide
// parameter space, compares the two rules point by point, and holds every
// disagreement to a much stronger standard than "rare": each one must sit
// within a hair of an actual switch boundary, which is the only place floating
// point can legitimately separate them.
//
// The guard is proven failable at the bottom (test_detects_a_wrong_conversion):
// a deliberately wrong conversion must be caught by the same comparison.

#include "../src/render/lod_distance.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::printf("FAIL: %s\n", (msg)); ++g_failures; }       \
    } while (0)

// The rule as cull.comp implements it today (and lod_select::select_level on
// the CPU): first rung whose threshold the projected size clears, else coarsest.
static int select_by_threshold(const std::vector<float>& thresholds,
                               float bound_radius, float distance_to_eye,
                               float instance_scale, float global_lod_scale) {
    const float projected_size =
        bound_radius * instance_scale / distance_to_eye * global_lod_scale;
    for (size_t i = 0; i < thresholds.size(); ++i)
        if (projected_size >= thresholds[i]) return (int)i;
    return (int)thresholds.size() - 1;
}

static std::vector<float> to_distances(const std::vector<float>& thresholds) {
    std::vector<float> d;
    d.reserve(thresholds.size());
    for (float t : thresholds) d.push_back(lod::normalized_switch_distance(t));
    return d;
}

// Deterministic LCG: this suite must not vary run to run.
static uint32_t g_rng = 0x1234567u;
static float frand(float lo, float hi) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((g_rng >> 8) * (1.0f / 16777216.0f));
}

// How close to a switch boundary a disagreement is allowed to be, relative.
// A camera this close to a boundary is crossing it; either answer is defensible.
static const float kBoundaryRel = 1e-4f;

static bool near_a_boundary(const std::vector<float>& distances,
                            float distance_to_eye, float reach) {
    for (float d : distances) {
        if (std::isinf(d)) continue;
        const float boundary = d * reach;
        if (boundary <= 0.0f) continue;
        if (std::fabs(distance_to_eye - boundary) <=
            kBoundaryRel * std::fmax(boundary, distance_to_eye))
            return true;
    }
    return false;
}

static void test_equivalence_sweep() {
    long long compared = 0, disagreed = 0, structural = 0;

    for (int trial = 0; trial < 200000; ++trial) {
        const int count = 1 + (int)(frand(0.0f, 6.99f));
        const float bound_radius = frand(0.01f, 200.0f);

        // Thresholds fine -> coarse: strictly DECREASING, as every producer in
        // the engine emits them.
        std::vector<float> thresholds;
        float t = frand(0.02f, 1.5f);
        for (int i = 0; i < count; ++i) { thresholds.push_back(t); t *= frand(0.15f, 0.9f); }

        // Occasionally hand the coarsest rung a zero threshold ("always
        // qualifies"), which is a real case the engine emits.
        if (frand(0.0f, 1.0f) < 0.1f) thresholds.back() = 0.0f;

        const std::vector<float> distances = to_distances(thresholds);

        for (int probe = 0; probe < 6; ++probe) {
            const float instance_scale   = frand(0.05f, 20.0f);
            const float global_lod_scale = frand(0.05f, 4.0f);
            const float distance_to_eye  = frand(0.05f, 5000.0f);

            const int a = select_by_threshold(thresholds, bound_radius,
                                              distance_to_eye, instance_scale,
                                              global_lod_scale);
            const int b = lod::select_rep(
                distances.data(), (int)distances.size(), distance_to_eye,
                lod::reach(bound_radius, instance_scale, global_lod_scale));
            ++compared;
            if (a != b) {
                ++disagreed;
                if (!near_a_boundary(distances, distance_to_eye,
                                     lod::reach(bound_radius, instance_scale, global_lod_scale))) {
                    ++structural;
                    if (structural <= 3)
                        std::printf("  structural disagreement: thr=%d r=%.4f d=%.4f "
                                    "s=%.4f G=%.4f -> threshold=%d distance=%d\n",
                                    count, bound_radius, distance_to_eye,
                                    instance_scale, global_lod_scale, a, b);
                }
            }
        }
    }

    std::printf("  compared %lld selections, %lld disagreed, %lld structural\n",
                compared, disagreed, structural);
    CHECK(structural == 0,
          "distance form selects identically to threshold form away from boundaries");
    // Disagreements at all should be vanishingly rare; a flood would mean the
    // boundary excuse is doing more work than it should.
    CHECK(disagreed * 10000 <= compared,
          "boundary-adjacent disagreements stay below 0.01% of comparisons");
}

static void test_known_answers() {
    // thresholds {0.5, 0.25, 0.1} -> normalized distances {2, 4, 10}; at
    // radius 2 (unit scale, unit dial) the real switches are {4, 8, 20} m.
    const std::vector<float> thresholds{0.5f, 0.25f, 0.1f};
    const std::vector<float> d = to_distances(thresholds);
    CHECK(std::fabs(d[0] -  2.0f) < 1e-5f, "normalized switch rung 0 == 2");
    CHECK(std::fabs(d[1] -  4.0f) < 1e-5f, "normalized switch rung 1 == 4");
    CHECK(std::fabs(d[2] - 10.0f) < 1e-5f, "normalized switch rung 2 == 10");
    const float R = lod::reach(2.0f, 1.0f, 1.0f);
    CHECK(std::fabs(d[0] * R -  4.0f) < 1e-4f, "switch metres rung 0 == 4");
    CHECK(std::fabs(d[2] * R - 20.0f) < 1e-4f, "switch metres rung 2 == 20");

    CHECK(lod::select_rep(d.data(), 3, 1.0f, lod::reach(2.0f, 1.0f, 1.0f)) == 0, "inside rung 0");
    CHECK(lod::select_rep(d.data(), 3, 4.0f, lod::reach(2.0f, 1.0f, 1.0f)) == 0, "exactly at rung 0 boundary stays fine");
    CHECK(lod::select_rep(d.data(), 3, 6.0f, lod::reach(2.0f, 1.0f, 1.0f)) == 1, "between rung 0 and 1");
    CHECK(lod::select_rep(d.data(), 3, 12.0f, lod::reach(2.0f, 1.0f, 1.0f)) == 2, "between rung 1 and 2");
    CHECK(lod::select_rep(d.data(), 3, 999.0f, lod::reach(2.0f, 1.0f, 1.0f)) == 2, "beyond all rungs clamps to coarsest");

    // The runtime dials multiply reach: doubling the scale doubles every switch.
    CHECK(lod::select_rep(d.data(), 3, 6.0f, lod::reach(2.0f, 2.0f, 1.0f)) == 0, "2x instance scale keeps rung 0 at 6");
    CHECK(lod::select_rep(d.data(), 3, 6.0f, lod::reach(2.0f, 1.0f, 2.0f)) == 0, "2x global scale keeps rung 0 at 6");

    // A zero threshold is an infinite switch distance: that rung always qualifies.
    const std::vector<float> open = to_distances({0.5f, 0.0f});
    CHECK(std::isinf(open[1]), "zero threshold -> infinite switch distance");
    CHECK(lod::select_rep(open.data(), 2, 1e9f, lod::reach(2.0f, 1.0f, 1.0f)) == 1,
          "open rung qualifies at any distance");

    // Degenerate inputs must not read memory or crash.
    CHECK(lod::select_rep(nullptr, 0, 1.0f, 1.0f) == 0, "null table selects rung 0");
    CHECK(lod::select_rep(d.data(), 0, 1.0f, 1.0f) == 0, "zero count selects rung 0");
}

// The reason radius lives in reach() rather than in the stored table: cull.comp
// computes a PER-INSTANCE radius for dynamic-bound clusters, a value that does
// not exist when the table is built. So one normalized table must stay correct
// under a radius it was never told about. Assert exactly that.
static void test_one_table_serves_any_radius() {
    const std::vector<float> thresholds{0.4f, 0.12f, 0.03f};
    const std::vector<float> d = to_distances(thresholds);   // radius-free

    long long mismatches = 0;
    for (int i = 0; i < 20000; ++i) {
        // A radius the table never saw, as a dynamic AABB union would produce.
        const float runtime_radius = frand(0.02f, 400.0f);
        const float s = frand(0.05f, 12.0f), G = frand(0.05f, 3.0f);
        const float dist = frand(0.05f, 4000.0f);

        const int a = select_by_threshold(thresholds, runtime_radius, dist, s, G);
        const int b = lod::select_rep(d.data(), 3, dist,
                                      lod::reach(runtime_radius, s, G));
        if (a != b && !near_a_boundary(d, dist, lod::reach(runtime_radius, s, G)))
            ++mismatches;
    }
    std::printf("  one table across 20000 unseen radii: %lld structural mismatches\n",
                mismatches);
    CHECK(mismatches == 0,
          "a single normalized table selects correctly for any runtime radius");
}

// FAILABILITY: the sweep above only means something if it would catch a wrong
// conversion. Feed it one (radius/threshold^2 instead of radius/threshold) and
// require that structural disagreements appear.
static void test_detects_a_wrong_conversion() {
    long long structural = 0;
    for (int trial = 0; trial < 2000; ++trial) {
        const float bound_radius = frand(0.5f, 50.0f);
        std::vector<float> thresholds;
        float t = frand(0.05f, 1.0f);
        for (int i = 0; i < 3; ++i) { thresholds.push_back(t); t *= 0.4f; }

        std::vector<float> wrong;
        for (float th : thresholds) wrong.push_back(1.0f / (th * th)); // WRONG

        for (int probe = 0; probe < 4; ++probe) {
            const float s = frand(0.5f, 4.0f), G = frand(0.5f, 2.0f);
            const float dist = frand(0.5f, 500.0f);
            const int a = select_by_threshold(thresholds, bound_radius, dist, s, G);
            const int b = lod::select_rep(wrong.data(), 3, dist,
                                          lod::reach(bound_radius, s, G));
            if (a != b && !near_a_boundary(wrong, dist, lod::reach(bound_radius, s, G))) ++structural;
        }
    }
    std::printf("  wrong-conversion probe produced %lld structural disagreements\n",
                structural);
    CHECK(structural > 0,
          "the equivalence check CAN fail (a wrong conversion is detected)");
}

int main() {
    std::printf("=== test_known_answers ===\n");            test_known_answers();
    std::printf("=== test_equivalence_sweep ===\n");         test_equivalence_sweep();
    std::printf("=== test_one_table_serves_any_radius ===\n"); test_one_table_serves_any_radius();
    std::printf("=== test_detects_a_wrong_conversion ===\n"); test_detects_a_wrong_conversion();

    if (g_failures) { std::printf("lod-distance FAILED (%d)\n", g_failures); return 1; }
    std::printf("ALL PASS\n");
    return 0;
}
