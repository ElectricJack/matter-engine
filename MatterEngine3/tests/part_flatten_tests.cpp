// Bake-time subtree flattening + error-bounded LOD ladder tests.
// Harness convention mirrors composition_tests.cpp (CHECK + failures counter).
//
// Fixtures: synthetic parent/child .part v2 files written into a temp cache dir
// (parts/<hash>.part), then flatten_part() merges them and we verify the flat
// artifact via load_v2.
#include "../include/part_flatten.h"
#include "../include/part_asset_v2.h"
#include "../include/lod_bake.h"
#include "../../MatterSurfaceLib/include/blas_manager.hpp"
#include "../../MatterSurfaceLib/include/tlas_manager.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static const char* kCacheRoot = "/tmp/part_flatten_tests_cache";

static const uint64_t kChildHash  = 0x1111000011110000ull;
static const uint64_t kParentHash = 0x2222000022220000ull;

// ---------------------------------------------------------------- fixtures --

static Tri make_tri(float3 a, float3 b, float3 c) {
    Tri t; t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
    t.centroid = make_float3((a.x+b.x+c.x)/3, (a.y+b.y+c.y)/3, (a.z+b.z+c.z)/3);
    return t;
}

static TriEx make_triex(int material_id) {
    TriEx ex;
    std::memset(&ex, 0, sizeof(TriEx));
    ex.materialId = material_id;
    ex.tint = make_float4(1, 1, 1, 0);
    ex.ao0 = ex.ao1 = ex.ao2 = 1.0f;
    ex.N0 = ex.N1 = ex.N2 = make_float3(0, 1, 0);
    return ex;
}

// Unit quad in the XY plane at z=0 (2 tris), all triangles material `mat`.
static std::vector<Tri> quad_tris() {
    std::vector<Tri> out;
    out.push_back(make_tri(make_float3(0,0,0), make_float3(1,0,0), make_float3(1,1,0)));
    out.push_back(make_tri(make_float3(0,0,0), make_float3(1,1,0), make_float3(0,1,0)));
    return out;
}

// Save a synthetic part: `lod_tri_sets[i]` becomes LOD level i (one BLAS entry
// per level, mirroring the real baker). All triangles carry material `mat`.
static bool save_fixture(uint64_t hash, int mat,
                         const std::vector<std::vector<Tri>>& lod_tri_sets,
                         const std::vector<part_asset::ChildInstance>& children) {
    BLASManager blas;
    TLASManager tlas(16);
    part_asset::LodLevels lods;
    for (size_t lvl = 0; lvl < lod_tri_sets.size(); ++lvl) {
        std::vector<Tri> tris = lod_tri_sets[lvl];
        std::vector<TriEx> ex(tris.size(), make_triex(mat));
        BLASHandle h = blas.register_triangles(tris.data(), (int)tris.size(), ex.data());
        uint32_t idx = UINT32_MAX;
        const auto& entries = blas.get_entries();
        for (size_t k = 0; k < entries.size(); ++k)
            if (entries[k]->handle == h) { idx = (uint32_t)k; break; }
        if (idx == UINT32_MAX) return false;
        part_asset::LodLevel L;
        L.screen_size_threshold = (lvl + 1 < lod_tri_sets.size()) ? 100.0f / (float)(lvl+1) : 0.0f;
        L.blas_indices.push_back(idx);
        lods.push_back(std::move(L));
    }
    const std::string path = std::string(kCacheRoot) + "/" + part_asset::cache_path_resolved(hash);
    return part_asset::save_v2(path, blas, tlas,
                               children.empty() ? nullptr : children.data(),
                               children.size(), lods, hash);
}

static void set_translate(float m[16], float x, float y, float z) {
    for (int i = 0; i < 16; ++i) m[i] = 0;
    m[0] = m[5] = m[10] = m[15] = 1;
    m[3] = x; m[7] = y; m[11] = z;
}

// Write the parent (quad at origin, material 3) with two instances of the child
// (quad, material 7) at +10x and +20x. Child carries TWO LOD levels (full quad +
// a single-tri coarse level) so the flatten must pick only level 0.
static bool write_fixtures() {
    mkdir(kCacheRoot, 0755);
    mkdir((std::string(kCacheRoot) + "/parts").c_str(), 0755);

    std::vector<Tri> quad = quad_tris();
    std::vector<Tri> coarse(quad.begin(), quad.begin() + 1);   // 1 tri "LOD1"
    if (!save_fixture(kChildHash, 7, {quad, coarse}, {})) return false;

    std::vector<part_asset::ChildInstance> children(2);
    children[0].child_resolved_hash = kChildHash;
    set_translate(children[0].transform, 10, 0, 0);
    children[1].child_resolved_hash = kChildHash;
    set_translate(children[1].transform, 20, 0, 0);
    return save_fixture(kParentHash, 3, {quad}, children);
}

static std::string flat_path() {
    return std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(kParentHash);
}

static bool read_bytes(const std::string& path, std::vector<char>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// ------------------------------------------------------------------- tests --

static void test_flatten_merge() {
    std::remove(flat_path().c_str());
    part_flatten::FlattenResult res =
        part_flatten::flatten_part(kCacheRoot, kParentHash);
    CHECK(res.ok, "flatten_part ok");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); return; }
    // Parent quad (2) + 2 child instances x LOD0 quad (2) = 6. The child's
    // coarse LOD entry (1 tri) must NOT leak into the merge.
    CHECK(res.full_tris == 6, "merged level-0 tri count = parent + 2x child LOD0");

    BLASManager blas; TLASManager tlas(16);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    bool loaded = part_asset::load_v2(flat_path(), kParentHash, blas, tlas, children, lods);
    CHECK(loaded, "flat artifact loads as v2");
    if (!loaded) return;
    CHECK(children.empty(), "flat artifact has an empty child table");
    CHECK(lods.size() == res.levels, "stored LOD count matches result");
    CHECK(!lods.empty() && lods[0].blas_indices.size() == 1, "level 0 = one BLAS entry");

    const auto& e0 = *blas.get_entries()[lods[0].blas_indices[0]];
    CHECK(e0.triangles.size() == 6, "level-0 entry holds all 6 merged tris");
    CHECK(e0.tri_extra.size() == 6, "TriEx table parallel to triangles");

    // Child placement: a vertex at local (1,1,0) under translate(20,0,0) must
    // appear at world (21,1,0).
    bool found = false;
    for (const Tri& t : e0.triangles) {
        const float3* vs[3] = { &t.vertex0, &t.vertex1, &t.vertex2 };
        for (const float3* v : vs)
            if (std::fabs(v->x - 21) < 1e-5f && std::fabs(v->y - 1) < 1e-5f &&
                std::fabs(v->z) < 1e-5f) found = true;
    }
    CHECK(found, "child vertex lands at placement-transformed position");

    // Materials: both the parent's (3) and the child's (7) survive, no others.
    std::set<int> mats;
    for (const TriEx& ex : e0.tri_extra) mats.insert(ex.materialId);
    CHECK(mats.count(3) == 1 && mats.count(7) == 1 && mats.size() == 2,
          "parent + child materialIds preserved through the merge");

    // Thresholds finest-to-coarsest, last level open-ended (0).
    for (size_t i = 0; i + 1 < lods.size(); ++i)
        CHECK(lods[i].screen_size_threshold > lods[i+1].screen_size_threshold,
              "thresholds strictly decreasing");
    CHECK(lods.back().screen_size_threshold == 0.0f, "coarsest threshold is 0");
}

static void test_flatten_deterministic() {
    std::remove(flat_path().c_str());
    part_flatten::FlattenResult a = part_flatten::flatten_part(kCacheRoot, kParentHash);
    std::vector<char> bytes_a;
    CHECK(a.ok && read_bytes(flat_path(), bytes_a), "first flatten written");

    std::remove(flat_path().c_str());
    part_flatten::FlattenResult b = part_flatten::flatten_part(kCacheRoot, kParentHash);
    std::vector<char> bytes_b;
    CHECK(b.ok && read_bytes(flat_path(), bytes_b), "second flatten written");

    CHECK(bytes_a == bytes_b, "re-flatten is byte-identical (deterministic)");
}

static void test_flatten_missing_part() {
    part_flatten::FlattenResult res =
        part_flatten::flatten_part(kCacheRoot, 0xDEADull);
    CHECK(!res.ok, "flatten of a missing part fails");
    CHECK(!res.error.empty(), "failure carries an error message");
}

// Dense UV sphere of radius 1 centered at origin.
static std::vector<Tri> sphere_tris(int segs, int rings) {
    auto pt = [&](int s, int r) {
        float u = 2.0f * 3.14159265f * s / segs;
        float v = 3.14159265f * r / rings;
        return make_float3(std::sin(v)*std::cos(u), std::cos(v), std::sin(v)*std::sin(u));
    };
    std::vector<Tri> out;
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segs; ++s) {
            float3 a = pt(s, r), b = pt(s+1, r), c = pt(s+1, r+1), d = pt(s, r+1);
            if (r > 0)         out.push_back(make_tri(a, b, c));
            if (r + 1 < rings) out.push_back(make_tri(a, c, d));
        }
    return out;
}

static void test_error_bound_calibration() {
    std::vector<Tri> sphere = sphere_tris(48, 24);   // ~2.2k tris, radius 1
    const float eps_list[] = {0.01f, 0.05f, 0.2f};
    size_t prev = sphere.size();
    for (float eps : eps_list) {
        std::vector<Tri> dec = lod_bake::decimate_to_error(sphere, eps);
        CHECK(!dec.empty(), "decimate_to_error produced output");
        CHECK(dec.size() < prev, "growing epsilon strictly shrinks the mesh");
        prev = dec.size();

        // Every output vertex must stay near the unit sphere: deviation bounded
        // by a small multiple of eps (calibrates the eps^2 QEM cost mapping).
        float worst = 0;
        for (const Tri& t : dec) {
            const float3* vs[3] = { &t.vertex0, &t.vertex1, &t.vertex2 };
            for (const float3* v : vs) {
                float rad = std::sqrt(v->x*v->x + v->y*v->y + v->z*v->z);
                worst = std::fmax(worst, std::fabs(rad - 1.0f));
            }
        }
        char msg[128];
        std::snprintf(msg, sizeof msg,
                      "eps=%.3f: vertex deviation %.4f within 4*eps", eps, worst);
        CHECK(worst <= 4.0f * eps, msg);
    }
}

static void test_reproject_two_materials() {
    // Closed two-material mesh: unit sphere, left hemisphere (x<0) material 1,
    // right material 2. (Closed on purpose: QEM error-only mode erodes OPEN
    // outlines for free — coplanar boundary collapses cost 0 — so an open sheet
    // is not a valid decimation fixture.)
    std::vector<Tri> tris = sphere_tris(48, 24);
    std::vector<TriEx> triex;
    triex.reserve(tris.size());
    for (const Tri& t : tris) triex.push_back(make_triex(t.centroid.x < 0 ? 1 : 2));

    std::vector<Tri> dec = lod_bake::decimate_to_error(tris, 0.05f);
    CHECK(!dec.empty() && dec.size() < tris.size(), "sphere decimated");
    std::vector<TriEx> ex = lod_bake::reproject_triex(dec, tris, triex);
    CHECK(ex.size() == dec.size(), "reprojected TriEx parallel to output tris");

    std::set<int> mats;
    for (const TriEx& e : ex) mats.insert(e.materialId);
    CHECK(mats.count(1) == 1 && mats.count(2) == 1, "both materials survive decimation");
    CHECK(mats.size() == 2, "no phantom materials introduced");
}

int main() {
    if (!write_fixtures()) {
        printf("FAIL: could not write fixture parts under %s\n", kCacheRoot);
        return 1;
    }
    test_flatten_merge();
    test_flatten_deterministic();
    test_flatten_missing_part();
    test_error_bound_calibration();
    test_reproject_two_materials();

    if (failures == 0) { printf("part_flatten_tests: ALL PASS\n"); return 0; }
    printf("part_flatten_tests: %d FAILURE(S)\n", failures);
    return 1;
}
