// SP-4 Composition-to-world tests: LOD bake, world flatten, sector grid, LOD select.
// Harness convention mirrors MatterSurfaceLib/tests/part_asset_tests.cpp.
#include "../include/lod_bake.h"
#include "../include/part_asset_v2.h"
#include "../../MatterSurfaceLib/include/blas_manager.hpp"
#include "../../MatterSurfaceLib/include/tlas_manager.hpp"
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

static void test_lod_roundtrip_v2() {
    std::vector<Tri> tris = grid_tris(32);
    BLASManager blas; TLASManager tlas(64);
    lod_bake::LodLevels lods = lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, blas);
    const char* path = "/tmp/sp4_lod_roundtrip.part";
    uint64_t rh = 0xABCDEF1234567890ull;
    bool saved = part_asset::save_v2(path, blas, tlas, nullptr, 0, lods, rh);
    CHECK(saved, "save_v2 ok");
    BLASManager blas2; TLASManager tlas2(64);
    std::vector<part_asset::ChildInstance> children_out;
    part_asset::LodLevels lods_out;
    bool loaded = part_asset::load_v2(path, rh, blas2, tlas2, children_out, lods_out);
    CHECK(loaded, "load_v2 ok");
    CHECK(lods_out.size() == lods.size(), "lod level count round-trips");
    for (size_t i = 0; i < lods.size(); ++i) {
        CHECK(lods_out[i].screen_size_threshold == lods[i].screen_size_threshold, "threshold round-trips");
        // SP-4 aliases SP-1's LodLevel directly: both use blas_indices.
        CHECK(lods_out[i].blas_indices == lods[i].blas_indices, "blas indices round-trip");
    }
}

static void test_lod_roundtrip_degenerate() {
    BLASManager blas; TLASManager tlas(8);
    const char* path = "/tmp/sp4_lod_empty.part";
    part_asset::LodLevels empty;
    CHECK(part_asset::save_v2(path, blas, tlas, nullptr, 0, empty, 7), "save empty lods");
    BLASManager b2; TLASManager t2(8);
    std::vector<part_asset::ChildInstance> c; part_asset::LodLevels out;
    CHECK(part_asset::load_v2(path, 7, b2, t2, c, out), "load empty lods");
    CHECK(out.empty(), "empty lods round-trip");
}

int main() {
    test_decimate_one_level();
    test_bake_three_levels();
    test_lod_roundtrip_v2();
    test_lod_roundtrip_degenerate();
    printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
