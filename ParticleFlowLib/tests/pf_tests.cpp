// ParticleFlowLib test binary. Plain assert() + printf, built with ASan+UBSan.
#include "particle_flow.h"
#include "pf_spatial_hash.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

using namespace pf;

static void test_rng_determinism() {
    Rng a(42), b(42), c(43);
    bool diverged = false;
    for (int i = 0; i < 1000; ++i) {
        uint64_t va = a.next_u64();
        assert(va == b.next_u64() && "same seed must produce identical stream");
        if (va != c.next_u64()) diverged = true;
    }
    assert(diverged && "different seeds must diverge");
    Rng d(7);
    for (int i = 0; i < 10000; ++i) {
        float u = d.next_unit();
        assert(u >= 0.0f && u < 1.0f);
        V3 s = d.unit_sphere();
        assert(std::fabs(length(s) - 1.0f) < 1e-4f);
    }
    printf("  rng determinism OK\n");
}

static void test_v3_math() {
    V3 v = normalize(V3{3, 0, 4});
    assert(std::fabs(v.x - 0.6f) < 1e-6f && std::fabs(v.z - 0.8f) < 1e-6f);
    V3 z = normalize(V3{0, 0, 0});           // zero-safe
    assert(z.x == 0 && z.y == 0 && z.z == 0);
    V3 c = cross(V3{1,0,0}, V3{0,1,0});
    assert(std::fabs(c.z - 1.0f) < 1e-6f);
    printf("  v3 math OK\n");
}

static void test_spatial_hash_vs_brute_force() {
    Rng rng(99);
    std::vector<V3> pts;
    for (int i = 0; i < 500; ++i)
        pts.push_back({rng.range(-5,5), rng.range(-5,5), rng.range(-5,5)});
    SpatialHash h(0.7f);
    for (uint32_t i = 0; i < pts.size(); ++i) h.insert(pts[i], i);
    assert(h.size() == pts.size());

    const V3 centers[] = {{0,0,0}, {2.5f,-1,3}, {-4.9f,4.9f,0}};
    const float radii[] = {0.3f, 1.1f, 4.0f};
    for (V3 q : centers) for (float r : radii) {
        std::vector<uint32_t> got;
        h.query(q, r, [&](uint32_t idx, V3, float d2) {
            assert(d2 <= r * r + 1e-5f);
            got.push_back(idx);
        });
        size_t expect = 0;
        for (uint32_t i = 0; i < pts.size(); ++i) {
            V3 d = pts[i] - q;
            if (dot(d, d) <= r * r) ++expect;
        }
        assert(got.size() == expect && "hash query must match brute force");
    }
    // Determinism: same query twice -> identical visit order.
    std::vector<uint32_t> o1, o2;
    h.query({0,0,0}, 3.0f, [&](uint32_t i, V3, float){ o1.push_back(i); });
    h.query({0,0,0}, 3.0f, [&](uint32_t i, V3, float){ o2.push_back(i); });
    assert(o1 == o2);
    printf("  spatial hash OK (%zu pts)\n", pts.size());
}

static void test_spatial_hash_no_duplicates() {
    // Regression test: injective packed-coordinate key must prevent
    // duplicate query results from cell collisions. Dense grid [-16,16]^3
    // at step 4 with cell_size 1.0 = 9^3 = 729 points.
    std::vector<V3> pts;
    for (int x = -16; x <= 16; x += 4) {
        for (int y = -16; y <= 16; y += 4) {
            for (int z = -16; z <= 16; z += 4) {
                pts.push_back({(float)x, (float)y, (float)z});
            }
        }
    }
    SpatialHash h(1.0f);
    for (uint32_t i = 0; i < pts.size(); ++i) h.insert(pts[i], i);
    assert(h.size() == pts.size());

    Rng rng(42);
    for (int q_iter = 0; q_iter < 50; ++q_iter) {
        V3 center{rng.range(-16, 16), rng.range(-16, 16), rng.range(-16, 16)};
        const float radius = 3.0f;

        // Collect query results.
        std::vector<uint32_t> got;
        h.query(center, radius, [&](uint32_t idx, V3, float d2) {
            got.push_back(idx);
        });

        // Verify count matches brute force.
        size_t expect_count = 0;
        for (uint32_t i = 0; i < pts.size(); ++i) {
            V3 d = pts[i] - center;
            if (dot(d, d) <= radius * radius) ++expect_count;
        }
        assert(got.size() == expect_count && "query count must match brute force");

        // Verify no duplicates: sort and check adjacent pairs.
        std::sort(got.begin(), got.end());
        for (size_t i = 1; i < got.size(); ++i) {
            assert(got[i] != got[i - 1] && "no point id can appear twice in query results");
        }
    }
    printf("  spatial hash no-duplicates OK (%zu pts, 50 queries)\n", pts.size());
}

int main() {
    printf("pf_tests:\n");
    test_rng_determinism();
    test_v3_math();
    test_spatial_hash_vs_brute_force();
    test_spatial_hash_no_duplicates();
    printf("pf_tests: ALL OK\n");
    return 0;
}
