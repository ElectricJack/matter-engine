// SP-4 Composition-to-world tests: LOD bake, world flatten, sector grid, LOD select.
// Harness convention mirrors MatterSurfaceLib/tests/part_asset_tests.cpp.
#include "../include/lod_bake.h"
#include "../../MatterSurfaceLib/include/blas_manager.hpp"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

// Build a flat NxN quad grid (2 tris per cell) as a Tri vector in [0,1]^2.
static std::vector<Tri> grid_tris(int n) {
    std::vector<Tri> out;
    float step = 1.0f / n;
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        float x0 = x*step, y0 = y*step, x1 = x0+step, y1 = y0+step;
        Tri a; a.vertex0 = make_float3(x0,y0,0); a.vertex1 = make_float3(x1,y0,0);
        a.vertex2 = make_float3(x1,y1,0); a.centroid = make_float3((x0+2*x1)/3,(2*y0+y1)/3,0);
        Tri b; b.vertex0 = make_float3(x0,y0,0); b.vertex1 = make_float3(x1,y1,0);
        b.vertex2 = make_float3(x0,y1,0); b.centroid = make_float3((2*x0+x1)/3,(y0+2*y1)/3,0);
        out.push_back(a); out.push_back(b);
    }
    return out;
}

static void test_decimate_one_level() {
    std::vector<Tri> tris = grid_tris(16);           // 512 tris
    std::vector<Tri> half = lod_bake::decimate_tris(tris, 0.5f);
    CHECK(!half.empty(), "decimate produced output");
    CHECK(half.size() < tris.size(), "decimate reduced tri count");
}

static void test_bake_three_levels() {
    std::vector<Tri> tris = grid_tris(32);           // 2048 tris (LOD0)
    BLASManager blas;
    lod_bake::BakeTargets t;                         // defaults: {1.0, 0.1, 0.01}
    lod_bake::LodLevels lods = lod_bake::bake_lods(tris, t, blas);

    CHECK(lods.size() == 3, "three LOD levels");
    // Each level registered exactly one BLAS (single-material part).
    CHECK(lods[0].blas_indices.size() == 1, "lod0 one blas");
    CHECK(lods[2].blas_indices.size() == 1, "lod2 one blas");
    // Tri counts strictly decrease LOD0 -> LOD2.
    auto tri_count = [&](uint32_t bi) {
        return blas.get_entries()[bi]->triangles.size();
    };
    size_t c0 = tri_count(lods[0].blas_indices[0]);
    size_t c1 = tri_count(lods[1].blas_indices[0]);
    size_t c2 = tri_count(lods[2].blas_indices[0]);
    CHECK(c0 > c1 && c1 > c2, "monotonically decreasing tri counts");
    CHECK(c0 == tris.size(), "lod0 is full geometry");
    // Thresholds: LOD0 largest (nearest), LOD2 smallest (farthest).
    CHECK(lods[0].screen_size_threshold > lods[1].screen_size_threshold, "thr0 > thr1");
    CHECK(lods[1].screen_size_threshold > lods[2].screen_size_threshold, "thr1 > thr2");
}

int main() {
    test_decimate_one_level();
    test_bake_three_levels();
    printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
