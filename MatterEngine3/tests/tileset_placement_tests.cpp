#include "tileset_placement.h"
#include <cmath>
#include <cstdio>
#include <set>

#include "check.h"

using namespace tileset;

static PlacementDomain interior_dom() {
    PlacementDomain d{ 0.15f, 1.85f, 0.15f, 1.85f, {}, 0.0f };
    return d;   // 2.0 m tile, 0.15 strip margin
}

static PlacementDomain strip_dom() {
    // vertical strip: across [-0.15, 0.15), along [0, 2.0), corners cleared at both ends
    PlacementDomain d{ -0.15f, 0.15f, 0.0f, 2.0f,
                       { {0.0f, 0.0f}, {0.0f, 2.0f} }, 0.08f };
    return d;
}

static void test_determinism() {
    for (int kind = 0; kind <= 2; ++kind) {
        auto a = scatter((PlacementKind)kind, interior_dom(), 40.0f, 1234);
        auto b = scatter((PlacementKind)kind, interior_dom(), 40.0f, 1234);
        CHECK(a.size() == b.size(), "placement: same-seed same count");
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i].x == b[i].x && a[i].z == b[i].z;
        CHECK(same, "placement: same-seed bit-identical points");
        auto c = scatter((PlacementKind)kind, interior_dom(), 40.0f, 1235);
        bool differs = c.size() != a.size();
        for (size_t i = 0; !differs && i < a.size() && i < c.size(); ++i)
            differs = a[i].x != c[i].x || a[i].z != c[i].z;
        CHECK(differs, "placement: different seed differs");
    }
}

static void test_bounds_and_clearance() {
    for (int kind = 0; kind <= 2; ++kind) {
        auto pts = scatter((PlacementKind)kind, strip_dom(), 60.0f, 99);
        CHECK(!pts.empty(), "placement: strip domain produces points");
        bool in = true, clear = true;
        for (const auto& p : pts) {
            if (p.x < -0.15f || p.x >= 0.15f || p.z < 0.0f || p.z >= 2.0f) in = false;
            for (const auto& d : strip_dom().clear_disks) {
                float dx = p.x - d.x, dz = p.z - d.z;
                if (std::sqrt(dx*dx + dz*dz) < 0.08f) clear = false;
            }
        }
        CHECK(in,    "placement: all points inside domain rect");
        CHECK(clear, "placement: corner disks respected");
    }
}

static void test_density() {
    // interior area = 1.7*1.7 = 2.89 m^2; density 40 => ~115 expected
    auto pts = scatter(PlacementKind::Uniform, interior_dom(), 40.0f, 7);
    CHECK(pts.size() >= 100 && pts.size() <= 132, "placement: uniform count ~= density*area");
    auto pp = scatter(PlacementKind::Poisson, interior_dom(), 40.0f, 7);
    CHECK(pp.size() >= 70, "placement: poisson reaches >=60% of target at moderate density");
    // poisson min-distance holds: r = 0.7/sqrt(density)
    float r = 0.7f / std::sqrt(40.0f);
    bool ok = true;
    for (size_t i = 0; i < pp.size() && ok; ++i)
        for (size_t j = i + 1; j < pp.size(); ++j) {
            float dx = pp[i].x - pp[j].x, dz = pp[i].z - pp[j].z;
            if (std::sqrt(dx*dx + dz*dz) < r * 0.999f) { ok = false; break; }
        }
    CHECK(ok, "placement: poisson min distance holds");
}

static void test_cluster_shape() {
    auto pts = scatter(PlacementKind::Cluster, interior_dom(), 60.0f, 117);
    CHECK(pts.size() >= 20, "placement: cluster produces points");
    // Clustered: mean nearest-neighbour distance well under uniform expectation.
    // Uniform mean NN ~ 0.5/sqrt(density) = 0.0645; clusters should be < 80% of that.
    float sum = 0.0f;
    for (size_t i = 0; i < pts.size(); ++i) {
        float best = 1e9f;
        for (size_t j = 0; j < pts.size(); ++j) {
            if (i == j) continue;
            float dx = pts[i].x - pts[j].x, dz = pts[i].z - pts[j].z;
            best = std::fmin(best, std::sqrt(dx*dx + dz*dz));
        }
        sum += best;
    }
    float mean_nn = sum / (float)pts.size();
    CHECK(mean_nn < 0.8f * (0.5f / std::sqrt(60.0f)), "placement: cluster mean-NN below uniform");
}

static void test_seed_fold() {
    CHECK(placement_seed(1, 0, 0) != placement_seed(1, 0, 1), "placement: domain id folds");
    CHECK(placement_seed(1, 0, 0) != placement_seed(1, 1, 0), "placement: layer index folds");
    CHECK(placement_seed(1, 0, 0) != placement_seed(2, 0, 0), "placement: master folds");
    CHECK(placement_seed(5, 3, 9) == placement_seed(5, 3, 9), "placement: fold is pure");
}

int main() {
    printf("== tileset_placement_tests ==\n");
    test_determinism();
    test_bounds_and_clearance();
    test_density();
    test_cluster_shape();
    test_seed_fold();
    if (g_failures == 0) printf("PASSED (0 failures)\n");
    else                 printf("FAILED (%d failures)\n", g_failures);
    return g_failures;
}
