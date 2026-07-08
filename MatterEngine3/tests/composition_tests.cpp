// SP-4 Composition-to-world tests: LOD bake, world flatten, sector grid, LOD select.
// Harness convention mirrors MatterSurfaceLib/tests/part_asset_tests.cpp.
#include "lod_bake.h"
#include "world_flatten.h"
#include "sector_grid.h"
#include "lod_select.h"
#include "part_asset_v2.h"
#include "../../MatterSurfaceLib/include/blas_manager.hpp"
#include "../../MatterSurfaceLib/include/tlas_manager.hpp"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>

#include "check.h"

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

// Identity-ish translate matrix, row-major float[16].
static void set_translate(float m[16], float x, float y, float z) {
    for (int i=0;i<16;++i) m[i]=0; m[0]=m[5]=m[10]=m[15]=1; m[3]=x; m[7]=y; m[11]=z;
}

static void test_flatten_n_times_m() {
    using namespace world_flatten;
    // root=1, mid=2 (the N children part), leaf=3 (the M grandchildren part).
    PartGraph g;
    ChildInstance c;
    for (int i = 0; i < 2; ++i) { c.child_resolved_hash = 2; set_translate(c.transform, (float)(i*100),0,0); g[1].push_back(c); }
    for (int j = 0; j < 3; ++j) { c.child_resolved_hash = 3; set_translate(c.transform, 0,(float)(j*10),0); g[2].push_back(c); }
    g[3];   // leaf part: present, no children

    FlattenLimits lim; lim.max_depth = 8; lim.max_instances = 100;
    std::vector<FlatInstance> flat;
    std::string err;
    bool ok = flatten(g, /*root=*/1, lim, flat, err);
    CHECK(ok, "flatten ok");
    CHECK(flat.size() == 2*3, "N*M = 6 leaf instances");
    bool found = false;
    for (auto& f : flat) {
        if (f.resolved_hash == 3 && f.world.cell[3]==100 && f.world.cell[7]==20 && f.world.cell[11]==0) found = true;
    }
    CHECK(found, "composed translate (100,20,0) present");
}

static void test_dedup_preserved() {
    using namespace world_flatten;
    PartGraph g; ChildInstance c; c.child_resolved_hash = 9;
    for (int i = 0; i < 5; ++i) { set_translate(c.transform,(float)i,0,0); g[1].push_back(c); }
    g[9];   // leaf
    FlattenLimits lim; std::vector<FlatInstance> flat; std::string err;
    CHECK(flatten(g, 1, lim, flat, err), "dedup flatten ok");
    CHECK(flat.size() == 5, "instance count grows to 5");
    std::vector<uint64_t> uniq;
    for (auto& f : flat) if (std::find(uniq.begin(),uniq.end(),f.resolved_hash)==uniq.end()) uniq.push_back(f.resolved_hash);
    CHECK(uniq.size() == 1, "one unique geometry hash despite 5 instances");
}

static void test_depth_guard() {
    using namespace world_flatten;
    PartGraph g; ChildInstance c;
    set_translate(c.transform,0,0,0);
    for (uint64_t h = 1; h <= 4; ++h) { c.child_resolved_hash = h+1; g[h].push_back(c); }
    g[5];   // leaf at depth 4
    FlattenLimits lim; lim.max_depth = 2;     // too shallow for a depth-4 chain
    std::vector<FlatInstance> flat; std::string err;
    CHECK(!flatten(g, 1, lim, flat, err), "depth guard fires");
    CHECK(err.find("max_depth") != std::string::npos, "depth error message");
    CHECK(err.find("part") != std::string::npos, "depth error names offending part");
}

static void test_budget_guard() {
    using namespace world_flatten;
    PartGraph g; ChildInstance c; c.child_resolved_hash = 9;
    for (int i = 0; i < 50; ++i) { set_translate(c.transform,(float)i,0,0); g[1].push_back(c); }
    g[9];
    FlattenLimits lim; lim.max_instances = 10;  // 50 leaves > budget
    std::vector<FlatInstance> flat; std::string err;
    CHECK(!flatten(g, 1, lim, flat, err), "budget guard fires");
    CHECK(err.find("max_instances") != std::string::npos, "budget error message");
}

static void test_sector_binning() {
    using namespace sector_grid;
    SectorGrid grid(10.0f);     // 10-unit pitch, origin at 0
    SectorCoord a = grid.sector_of(make_float3(5,5,5));
    CHECK(a.x==0 && a.y==0 && a.z==0, "interior point sector (0,0,0)");
    SectorCoord b = grid.sector_of(make_float3(15,5,-5));
    CHECK(b.x==1 && b.y==0 && b.z==-1, "point sector (1,0,-1)");
    SectorCoord c = grid.sector_of(make_float3(-1,0,0));
    CHECK(c.x==-1, "negative coord uses floor not truncation");
    SectorCoord bd = grid.sector_of(make_float3(10.0f,0,0));
    CHECK(bd.x==1, "boundary point belongs to upper cell deterministically");
}

static void test_bin_instances() {
    using namespace sector_grid;
    using world_flatten::FlatInstance;
    SectorGrid grid(10.0f);
    std::vector<FlatInstance> flat(3);
    flat[0].resolved_hash=1; flat[0].world = mat4::Translate(make_float3(1,1,1));
    flat[1].resolved_hash=1; flat[1].world = mat4::Translate(make_float3(2,2,2));   // same sector as [0]
    flat[2].resolved_hash=2; flat[2].world = mat4::Translate(make_float3(25,0,0));  // sector (2,0,0)
    Sectors s = bin_instances(flat, grid);
    CHECK(s.size() == 2, "two distinct occupied sectors");
    SectorCoord k0{0,0,0};
    CHECK(s[k0].size() == 2, "two instances in sector (0,0,0)");
}

static void test_lod_selection_and_escalation() {
    using namespace lod_select;
    std::vector<float> thresholds = {0.20f, 0.05f, 0.0125f};
    float r = 1.0f; float3 c = make_float3(0,0,0);

    int far_lvl = select_level(projected_size(c, r, make_float3(0,0,100)), thresholds);
    CHECK(far_lvl == 2, "far camera -> coarsest level 2");
    int mid_lvl = select_level(projected_size(c, r, make_float3(0,0,10)), thresholds);
    CHECK(mid_lvl == 1, "mid camera -> level 1");
    int near_lvl = select_level(projected_size(c, r, make_float3(0,0,2)), thresholds);
    CHECK(near_lvl == 0, "near camera -> finest level 0");
    CHECK(near_lvl <= mid_lvl && mid_lvl <= far_lvl, "moving closer escalates (lower index)");
}

static void test_sector_uses_closest_instance() {
    using namespace lod_select;
    using namespace sector_grid;
    using world_flatten::FlatInstance;
    std::vector<FlatInstance> flat(2);
    flat[0].resolved_hash=1; flat[0].world = mat4::Translate(make_float3(0,0,0));   // closest to z=3 cam
    flat[1].resolved_hash=1; flat[1].world = mat4::Translate(make_float3(0,0,8));
    SectorGrid grid(100.0f);                 // both land in sector (0,0,0)
    Sectors sec = bin_instances(flat, grid);
    PartLodTable parts;
    parts[1] = PartLod{ /*radius=*/1.0f, /*thresholds=*/{0.20f,0.05f,0.0125f} };
    float3 cam = make_float3(0,0,3);
    auto chosen = select_sector_lods(sec, parts, cam);
    SectorCoord k{0,0,0};
    CHECK(chosen[k].at(1) == 0, "sector LOD for part 1 driven by closest instance -> level 0");
}

static void test_variation_lod_independence() {
    // Two variations of "the same part kind" are two distinct resolved_hashes
    // (varA=100, varB=200) whose geometry is the SAME shape, so their baked LOD
    // level sets must be identical, and selection at a given distance identical.
    std::vector<Tri> tris = grid_tris(32);
    BLASManager blasA, blasB;
    lod_bake::LodLevels lodsA = lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, blasA);
    lod_bake::LodLevels lodsB = lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, blasB);
    CHECK(lodsA.size() == lodsB.size(), "variations have same level count");
    bool same_thr = true, same_tris = true;
    for (size_t i = 0; i < lodsA.size(); ++i) {
        if (lodsA[i].screen_size_threshold != lodsB[i].screen_size_threshold) same_thr = false;
        size_t ca = blasA.get_entries()[lodsA[i].blas_indices[0]]->triangles.size();
        size_t cb = blasB.get_entries()[lodsB[i].blas_indices[0]]->triangles.size();
        if (ca != cb) same_tris = false;
    }
    CHECK(same_thr, "variations share identical thresholds");
    CHECK(same_tris, "variations share identical per-level tri counts");

    using namespace lod_select; using namespace sector_grid; using world_flatten::FlatInstance;
    std::vector<float> thr; for (auto& L : lodsA) thr.push_back(L.screen_size_threshold);
    PartLodTable parts;
    parts[100] = PartLod{1.0f, thr};
    parts[200] = PartLod{1.0f, thr};
    std::vector<FlatInstance> flat(2);
    flat[0].resolved_hash=100; flat[0].world = mat4::Translate(make_float3(0,0,0));
    flat[1].resolved_hash=200; flat[1].world = mat4::Translate(make_float3(0,0,0));
    SectorGrid grid(100.0f);
    auto chosen = select_sector_lods(bin_instances(flat, grid), parts, make_float3(0,0,7));
    SectorCoord k{0,0,0};
    CHECK(chosen[k].at(100) == chosen[k].at(200), "two variations select identical LOD");
}

static void test_floor_cull_lod_select() {
    auto mk = [](uint64_t h, float x) {
        world_flatten::FlatInstance f;
        for (int i = 0; i < 16; ++i) f.world.cell[i] = 0.0f;
        f.world.cell[0] = f.world.cell[5] = f.world.cell[10] = f.world.cell[15] = 1.0f;
        f.world.cell[3] = x;
        f.resolved_hash = h;
        return f;
    };
    // 0xA: tiny part far away (0.05/100 = 0.0005 < floor)   -> culled (-1)
    // 0xB: large part, same far sector (10/100 = 0.1)        -> level 0
    // 0xC: tiny part near the camera (0.05/4 = 0.0125)       -> level 0
    std::vector<world_flatten::FlatInstance> flat = {
        mk(0xA, 100.0f), mk(0xB, 100.0f), mk(0xC, 4.0f)
    };
    sector_grid::SectorGrid grid(16.0f);
    sector_grid::Sectors sectors = sector_grid::bin_instances(flat, grid);
    lod_select::PartLodTable parts;
    parts[0xA] = { 0.05f, {0.0f} };
    parts[0xB] = { 10.0f, {0.0f} };
    parts[0xC] = { 0.05f, {0.0f} };
    float3 cam = make_float3(0, 0, 0);

    auto chosen = lod_select::select_sector_lods(sectors, parts, cam, 0.002f);
    int lodA = 99, lodB = 99, lodC = 99;
    for (const auto& sk : chosen)
        for (const auto& pl : sk.second) {
            if (pl.first == 0xA) lodA = pl.second;
            if (pl.first == 0xB) lodB = pl.second;
            if (pl.first == 0xC) lodC = pl.second;
        }
    CHECK(lodA == -1, "small far part floor-culled (level -1)");
    CHECK(lodB == 0,  "large far part not culled");
    CHECK(lodC == 0,  "small near part not culled");

    // Default arg (no floor) never emits -1: existing behavior preserved.
    auto chosen0 = lod_select::select_sector_lods(sectors, parts, cam);
    bool any_cull = false;
    for (const auto& sk : chosen0)
        for (const auto& pl : sk.second) if (pl.second < 0) any_cull = true;
    CHECK(!any_cull, "zero floor culls nothing (back-compat)");
}

int main() {
    test_decimate_one_level();
    test_bake_three_levels();
    test_lod_roundtrip_v2();
    test_lod_roundtrip_degenerate();
    test_flatten_n_times_m();
    test_dedup_preserved();
    test_depth_guard();
    test_budget_guard();
    test_sector_binning();
    test_bin_instances();
    test_lod_selection_and_escalation();
    test_sector_uses_closest_instance();
    test_variation_lod_independence();
    test_floor_cull_lod_select();
    printf(g_failures ? "FAILED (%d)\n" : "OK\n", g_failures);
    return g_failures ? 1 : 0;
}
