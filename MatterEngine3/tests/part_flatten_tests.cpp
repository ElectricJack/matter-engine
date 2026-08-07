// Bake-time subtree flattening + error-bounded LOD ladder tests.
// Harness convention mirrors composition_tests.cpp (CHECK + failures counter).
//
// Fixtures: synthetic parent/child .part v2 files written into a temp cache dir
// (parts/<hash>.part), then flatten_part() merges them and we verify the flat
// artifact via load_v2.
#include "part_flatten.h"
#include "matter/lod_contract.h"   // kMaxSerializedLodLevels (the over-cap guard)
#include "impostor_bake.h"
#include "part_asset_v2.h"
#include "part_bundle.h"   // M4: sections, not sibling files
#include "lod_bake.h"
#include "bake_trace.h"        // Bake Lab task 1.5: flatten/LOD trace-shape tests
#include "bake_trace_names.h"  // kSpanFlatten, kSpanLod, kSpanLodRung
#include "matter/bake_observer.h"  // Bake Lab W3: per-rung bake observer seam
#include "part_cluster.h"
#include "render/lod_distance.h"   // M3: the ONE selection rule, asserted against
                                   // the authored ladder's own metres
#include "../../libs/MatterSurfaceLib/include/material_registry.h"
#include "../../libs/MatterSurfaceLib/include/blas_manager.hpp"
#include "../../libs/MatterSurfaceLib/include/tlas_manager.hpp"
#include "../../libs/MatterSurfaceLib/include/mesh_simplifier.hpp"
#include "../../libs/MatterSurfaceLib/include/mesh_indexed.hpp"
#include "../../libs/MatterSurfaceLib/include/mesh_transform.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "check.h"

namespace fs = std::filesystem;

static const std::string kCacheRootStorage =
    (fs::temp_directory_path() / "part_flatten_tests_cache").string();
static const char* kCacheRoot = kCacheRootStorage.c_str();

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

// Whole-file byte read; the determinism gates compare artifacts with it.
static std::vector<char> read_all_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<char>((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
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
    fs::create_directories(fs::path(kCacheRoot) / "parts");

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


// M4: "delete the flat artifact" is a section drop, not a file delete -- the
// bundle at that path also holds the compositional body the flatten reads.
static void drop_flat(const std::string& bundle_path, uint64_t hash) {
    part_bundle::remove_section(bundle_path, hash, part_bundle::kSectionFlat);
}
static void drop_plan(const std::string& bundle_path, uint64_t hash) {
    part_bundle::remove_section(bundle_path, hash, part_bundle::kSectionPlan);
}
static void drop_variants(const std::string& bundle_path, uint64_t hash) {
    part_bundle::remove_section(bundle_path, hash, part_bundle::kSectionVariants);
}

static bool read_bytes(const std::string& path, std::vector<char>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// ------------------------------------------------------------------- tests --

static void test_flatten_merge() {
    drop_flat(flat_path(), kParentHash);
    part_flatten::FlattenResult res =
        part_flatten::flatten_part(kCacheRoot, kParentHash);
    CHECK(res.ok, "flatten_part ok");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); return; }
    // Parent quad (2) + 2 child instances x LOD0 quad (2) = 6. The child's
    // coarse LOD entry (1 tri) must NOT leak into the merge.
    CHECK(res.full_tris == 6, "merged level-0 tri count = parent + 2x child LOD0");
    CHECK(res.clusters >= 1, "result has at least 1 cluster");

    // Task 11: flatten writes v3 (now v4); load_flat_v3 must succeed.
    uint32_t fv = part_asset::peek_format_version(flat_path());
    CHECK(fv == part_asset::kFormatVersionFlat, "flat artifact is current bake version");

    BLASManager blas; TLASManager tlas(16);
    std::vector<part_asset::FlatCluster> clusters_in;
    bool loaded = part_asset::load_flat_v3(flat_path(), kParentHash, blas, tlas, clusters_in);
    CHECK(loaded, "flat artifact loads as v3");
    if (!loaded) return;
    CHECK(!clusters_in.empty(), "v3 flat has at least 1 cluster");

    // The 6-tri merged mesh should produce >= 1 cluster with all 6 tris at level 0.
    // (6 << 16000 so it's just one cluster, level 0 = full 6 tris.)
    uint32_t total_l0_tris = 0;
    for (const auto& cl : clusters_in) {
        if (cl.lods.empty()) continue;
        for (uint32_t bi : cl.lods[0].blas_indices) {
            if (bi < blas.get_entries().size())
                total_l0_tris += (uint32_t)blas.get_entries()[bi]->triangles.size();
        }
    }
    CHECK(total_l0_tris == 6, "sum of cluster level-0 tris == 6 (all merged tris)");

    // Collect all triangles across cluster level-0 to verify placement and materials.
    std::vector<Tri> all_tris;
    std::vector<TriEx> all_triex;
    for (const auto& cl : clusters_in) {
        if (cl.lods.empty()) continue;
        for (uint32_t bi : cl.lods[0].blas_indices) {
            if (bi >= blas.get_entries().size()) continue;
            const auto& e = *blas.get_entries()[bi];
            all_tris.insert(all_tris.end(), e.triangles.begin(), e.triangles.end());
            all_triex.insert(all_triex.end(), e.tri_extra.begin(), e.tri_extra.end());
        }
    }
    CHECK(all_tris.size() == 6, "level-0 entry holds all 6 merged tris across clusters");
    CHECK(all_triex.size() == 6, "TriEx table parallel to triangles across clusters");

    // Child placement: a vertex at local (1,1,0) under translate(20,0,0) must
    // appear at world (21,1,0).
    bool found = false;
    for (const Tri& t : all_tris) {
        const float3* vs[3] = { &t.vertex0, &t.vertex1, &t.vertex2 };
        for (const float3* v : vs)
            if (std::fabs(v->x - 21) < 1e-5f && std::fabs(v->y - 1) < 1e-5f &&
                std::fabs(v->z) < 1e-5f) found = true;
    }
    CHECK(found, "child vertex lands at placement-transformed position");

    // Materials: both the parent's (3) and the child's (7) survive, no others.
    std::set<int> mats;
    for (const TriEx& ex : all_triex) mats.insert(ex.materialId);
    CHECK(mats.count(3) == 1 && mats.count(7) == 1 && mats.size() == 2,
          "parent + child materialIds preserved through the merge");

    // Thresholds: within each cluster, thresholds must be finest-to-coarsest.
    for (const auto& cl : clusters_in) {
        for (size_t i = 0; i + 1 < cl.lods.size(); ++i)
            CHECK(cl.lods[i].screen_size_threshold >= cl.lods[i+1].screen_size_threshold,
                  "per-cluster thresholds non-increasing");
        if (!cl.lods.empty())
            CHECK(cl.lods.back().screen_size_threshold == 0.0f, "coarsest cluster threshold is 0");
    }
}

static void test_flatten_deterministic() {
    drop_flat(flat_path(), kParentHash);
    part_flatten::FlattenResult a = part_flatten::flatten_part(kCacheRoot, kParentHash);
    std::vector<char> bytes_a;
    CHECK(a.ok && read_bytes(flat_path(), bytes_a), "first flatten written");
    CHECK(part_asset::peek_format_version(flat_path()) == part_asset::kFormatVersionFlat, "first flatten is current bake version");

    drop_flat(flat_path(), kParentHash);
    part_flatten::FlattenResult b = part_flatten::flatten_part(kCacheRoot, kParentHash);
    std::vector<char> bytes_b;
    CHECK(b.ok && read_bytes(flat_path(), bytes_b), "second flatten written (v3)");

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

// ---- Task 7 fixture helpers (defined after sphere_tris) ----

static const uint64_t kSmallSphereHash = 0xAAAA000011110001ull;
static const uint64_t kDenseSphereHash = 0xBBBB000022220002ull;

// Write a small sphere part (~400 tris) as a childless .part in the cache.
// Returns kSmallSphereHash on success, 0 on failure.
static uint64_t write_small_sphere_part() {
    // segs=20, rings=10 => ~20*10*2=400 tris (well under old min_tris=2000)
    std::vector<Tri> tris = sphere_tris(20, 10);
    if (!save_fixture(kSmallSphereHash, 5, {tris}, {})) return 0;
    return kSmallSphereHash;
}

// Write a dense sphere part (>=20k tris) as a childless .part in the cache.
// Returns kDenseSphereHash on success, 0 on failure.
static uint64_t write_dense_sphere_part() {
    // segs=120, rings=90 => ~120*90*2=21600 tris
    std::vector<Tri> tris = sphere_tris(120, 90);
    if (!save_fixture(kDenseSphereHash, 6, {tris}, {})) return 0;
    return kDenseSphereHash;
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

// Open heightfield grid over a known XZ footprint — the terrain-tile shape.
// Decimation must NOT erode the border: adjacent tiles are simplified
// independently, and any outline shrink opens visible sky cracks at the seams.
static void test_open_grid_border_preserved() {
    // 32x32 quad grid over [0,16]^2, gentle sine relief (plenty to decimate).
    const int N = 32;
    const float W = 16.0f;
    auto h = [](float x, float z) {
        return 0.6f * std::sin(x * 0.7f) * std::cos(z * 0.5f);
    };
    auto pt = [&](int i, int j) {
        float x = W * i / N, z = W * j / N;
        return make_float3(x, h(x, z), z);
    };
    std::vector<Tri> grid;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            float3 a = pt(i, j), b = pt(i+1, j), c = pt(i+1, j+1), d = pt(i, j+1);
            grid.push_back(make_tri(a, b, c));
            grid.push_back(make_tri(a, c, d));
        }

    // Coarse epsilon (~tile bound_radius / 4 in the real ladder).
    std::vector<Tri> dec = lod_bake::decimate_to_error(grid, 2.8f);
    CHECK(dec.size() < grid.size() / 2, "open grid actually decimates (>2x reduction)");

    float minx = 1e9f, maxx = -1e9f, minz = 1e9f, maxz = -1e9f;
    for (const Tri& t : dec) {
        const float3* vs[3] = { &t.vertex0, &t.vertex1, &t.vertex2 };
        for (const float3* v : vs) {
            minx = std::fmin(minx, v->x); maxx = std::fmax(maxx, v->x);
            minz = std::fmin(minz, v->z); maxz = std::fmax(maxz, v->z);
        }
    }
    char msg[160];
    std::snprintf(msg, sizeof msg,
                  "border preserved exactly: x=[%.4f,%.4f] z=[%.4f,%.4f] vs [0,16]^2",
                  minx, maxx, minz, maxz);
    CHECK(minx == 0.0f && maxx == W && minz == 0.0f && maxz == W, msg);
}

static void test_reproject_two_materials() {
    // Closed two-material mesh: unit sphere, left hemisphere (x<0) material 1,
    // right material 2.
    std::vector<Tri> tris = sphere_tris(48, 24);
    std::vector<TriEx> triex;
    triex.reserve(tris.size());
    for (const Tri& t : tris) triex.push_back(make_triex(t.centroid.x < 0 ? 1 : 2));

    std::vector<Tri> dec = lod_bake::decimate_to_error(tris, 0.05f);
    CHECK(!dec.empty() && dec.size() < tris.size(), "sphere decimated");
    // Task 8: reproject_triex moved to MSL; wrap the Tri/TriEx call site
    // through MeshIndexed until Task 11 refactors this test to speak
    // MeshIndexed natively.
    std::vector<TriEx> ex;
    {
        MeshIndexed src_m = from_tri(tris, &triex);
        MeshIndexed tgt_m = from_tri(dec, nullptr);
        ::reproject_triex(src_m, tgt_m);
        std::vector<Tri> tgt_tris_ignored;
        to_tri(tgt_m, tgt_tris_ignored, ex);
    }
    CHECK(ex.size() == dec.size(), "reprojected TriEx parallel to output tris");

    std::set<int> mats;
    for (const TriEx& e : ex) mats.insert(e.materialId);
    CHECK(mats.count(1) == 1 && mats.count(2) == 1, "both materials survive decimation");
    CHECK(mats.size() == 2, "no phantom materials introduced");
}

// Task 8: topological boundary-vertex lock.
// Build an open 8x8 bumped grid sheet (128 tris). Collect its boundary vertex
// positions (edges with incidence == 1 in the welded input topology). Call
// simplify_mesh directly with bounds=nullptr and lock_boundary=true and a
// huge max_error so interior collapses have every opportunity to fire. Assert:
//   1. Every recorded boundary position is present bit-identical in the output.
//   2. The output has fewer tris than the input (interior did decimate).
static void test_topological_boundary_lock() {
    printf("=== test_topological_boundary_lock ===\n");

    // Build indexed 8x8 grid on XZ plane, y = 0.05*sin(x)*cos(z) bump.
    const int N = 8;
    const float span = 1.0f;
    const int side = N + 1; // 9 verts per side
    std::vector<float> vpos;
    std::vector<unsigned short> idx;
    vpos.reserve(side * side * 3);
    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            float x = span * (float)i / (float)N;
            float z = span * (float)j / (float)N;
            float y = 0.05f * std::sin(x * 6.28f) * std::cos(z * 6.28f);
            vpos.push_back(x);
            vpos.push_back(y);
            vpos.push_back(z);
        }
    }
    auto vid = [&](int i, int j) -> unsigned short { return (unsigned short)(j * side + i); };
    for (int j = 0; j < N; ++j) {
        for (int i = 0; i < N; ++i) {
            idx.push_back(vid(i,   j));   idx.push_back(vid(i+1, j));   idx.push_back(vid(i+1, j+1));
            idx.push_back(vid(i,   j));   idx.push_back(vid(i+1, j+1)); idx.push_back(vid(i,   j+1));
        }
    }
    Mesh in = {0};
    in.vertexCount   = side * side;
    in.triangleCount = N * N * 2; // 128
    in.vertices = (float*)MemAlloc(sizeof(float) * vpos.size());
    for (size_t k = 0; k < vpos.size(); ++k) in.vertices[k] = vpos[k];
    in.indices = (unsigned short*)MemAlloc(sizeof(unsigned short) * idx.size());
    for (size_t k = 0; k < idx.size(); ++k) in.indices[k] = idx[k];

    // Collect topological boundary vertex positions from the input: edges with
    // incidence 1 contribute both endpoints to the boundary set.
    std::map<std::pair<int,int>, int> edge_count;
    for (int t = 0; t < in.triangleCount; ++t) {
        int a = in.indices[t*3+0], b = in.indices[t*3+1], c = in.indices[t*3+2];
        int pairs[3][2] = {{a,b},{b,c},{c,a}};
        for (auto& p : pairs) {
            int lo = p[0], hi = p[1];
            if (lo > hi) std::swap(lo, hi);
            edge_count[{lo, hi}]++;
        }
    }
    // Collect the float positions of boundary vertices (incidence != 2).
    std::set<int> boundary_vi;
    for (const auto& kv : edge_count) {
        if (kv.second != 2) {
            boundary_vi.insert(kv.first.first);
            boundary_vi.insert(kv.first.second);
        }
    }
    // Store bit-identical float triples for each boundary vertex.
    struct FP3 { float x, y, z; bool operator<(const FP3& o) const {
        if (x != o.x) return x < o.x; if (y != o.y) return y < o.y; return z < o.z;
    }};
    std::set<FP3> boundary_pos;
    for (int vi : boundary_vi) {
        boundary_pos.insert({in.vertices[vi*3+0], in.vertices[vi*3+1], in.vertices[vi*3+2]});
    }
    printf("  input: %d tris, %d boundary verts\n",
           in.triangleCount, (int)boundary_pos.size());

    // Call simplify_mesh directly: bounds=nullptr, lock_boundary=true, huge max_error.
    SimplifyOptions opts;
    opts.target_ratio  = 0.0f;   // clamps to targetTri=1, so error-stop drives everything
    opts.max_error     = 1e30f;  // never stop on cost alone
    opts.lock_boundary = true;
    Mesh out = simplify_mesh(in, opts, nullptr);

    printf("  output: %d tris, %d verts\n", out.triangleCount, out.vertexCount);

    // 1. Interior must have decimated.
    CHECK(out.triangleCount < in.triangleCount,
          "topological lock: interior decimated (output has fewer tris than 128)");

    // 2. Every boundary position appears bit-identical in the output vertex set.
    std::set<FP3> out_pos;
    for (int vi = 0; vi < out.vertexCount; ++vi) {
        out_pos.insert({out.vertices[vi*3+0], out.vertices[vi*3+1], out.vertices[vi*3+2]});
    }
    int missing = 0;
    for (const FP3& bp : boundary_pos) {
        if (out_pos.find(bp) == out_pos.end()) {
            printf("  MISSING boundary vertex (%.6f, %.6f, %.6f)\n", bp.x, bp.y, bp.z);
            ++missing;
        }
    }
    CHECK(missing == 0,
          "topological lock: all boundary vertex positions preserved bit-identical");

    MemFree(in.vertices); MemFree(in.indices);
    if (out.vertices) MemFree(out.vertices);
    if (out.indices)  MemFree(out.indices);
    if (out.normals)  MemFree(out.normals);
    printf(missing == 0 && out.triangleCount < in.triangleCount ? "PASSED\n" : "FAILED\n");
}

// ----------------------------------------------------------------- cluster tests --

// Build a synthetic 40,000-tri flat grid sheet.  Each tri gets a unique
// materialId == its original index so we can track reordering via TriEx.
static std::vector<Tri> grid_sheet_tris(int nx, int nz, float w, float d) {
    std::vector<Tri> out;
    out.reserve(nx * nz * 2);
    for (int j = 0; j < nz; ++j) {
        for (int i = 0; i < nx; ++i) {
            float x0 = w * i / nx, x1 = w * (i+1) / nx;
            float z0 = d * j / nz, z1 = d * (j+1) / nz;
            out.push_back(make_tri(make_float3(x0,0,z0), make_float3(x1,0,z0), make_float3(x1,0,z1)));
            out.push_back(make_tri(make_float3(x0,0,z0), make_float3(x1,0,z1), make_float3(x0,0,z1)));
        }
    }
    return out;
}

static void test_cluster_split_40k() {
    printf("=== test_cluster_split_40k ===\n");

    // 200x100 grid => 200*100*2 = 40,000 tris
    const int NX = 200, NZ = 100;
    std::vector<Tri> tris = grid_sheet_tris(NX, NZ, 200.0f, 100.0f);
    CHECK(tris.size() == 40000u, "grid sheet has 40000 tris");

    // Give each tri a unique materialId equal to its original index
    std::vector<TriEx> triex(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) {
        std::memset(&triex[i], 0, sizeof(TriEx));
        triex[i].materialId = (int)i;
        triex[i].tint = make_float4(1,1,1,0);
        triex[i].ao0 = triex[i].ao1 = triex[i].ao2 = 1.0f;
        triex[i].N0 = triex[i].N1 = triex[i].N2 = make_float3(0,1,0);
    }

    // Keep a copy of centroids keyed by original materialId for conservation check
    std::vector<float3> orig_centroids(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) orig_centroids[i] = tris[i].centroid;

    auto clusters = part_cluster::split_clusters(tris, triex, 16000);

    // 1. Every cluster's tri_count <= 16000
    bool all_le_target = true;
    for (const auto& c : clusters)
        if (c.tri_count > 16000) { all_le_target = false; break; }
    CHECK(all_le_target, "every cluster tri_count <= 16000");

    // 2. tri_count sum == 40000
    uint32_t total = 0;
    for (const auto& c : clusters) total += c.tri_count;
    CHECK(total == 40000u, "cluster tri_count sum == 40000");

    // 3. Contiguous non-overlapping ranges starting from 0
    bool contiguous = true;
    uint32_t next = 0;
    for (const auto& c : clusters) {
        if (c.first_tri != next) { contiguous = false; break; }
        next += c.tri_count;
    }
    CHECK(contiguous, "cluster ranges are contiguous and non-overlapping from 0");

    // 4. Every output triangle's 3 vertices inside its cluster AABB (+/- 1e-5)
    bool verts_in_aabb = true;
    for (const auto& c : clusters) {
        for (uint32_t j = c.first_tri; j < c.first_tri + c.tri_count; ++j) {
            const Tri& t = tris[j];
            const float3* vs[3] = {&t.vertex0, &t.vertex1, &t.vertex2};
            for (const float3* v : vs) {
                if (v->x < c.aabb_min[0]-1e-5f || v->x > c.aabb_max[0]+1e-5f ||
                    v->y < c.aabb_min[1]-1e-5f || v->y > c.aabb_max[1]+1e-5f ||
                    v->z < c.aabb_min[2]-1e-5f || v->z > c.aabb_max[2]+1e-5f) {
                    verts_in_aabb = false;
                }
            }
        }
    }
    CHECK(verts_in_aabb, "every output tri vertex is inside its cluster AABB (+/-1e-5)");

    // 5. Conservation: multiset of centroids before == after
    // Sort a copy of orig_centroids and the post-split centroids, compare
    auto cent_lt = [](const float3& a, const float3& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    };
    std::vector<float3> before_sorted = orig_centroids;
    std::sort(before_sorted.begin(), before_sorted.end(), cent_lt);
    std::vector<float3> after_sorted(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) after_sorted[i] = tris[i].centroid;
    std::sort(after_sorted.begin(), after_sorted.end(), cent_lt);
    bool conserved = (before_sorted.size() == after_sorted.size());
    for (size_t i = 0; i < before_sorted.size() && conserved; ++i) {
        if (before_sorted[i].x != after_sorted[i].x ||
            before_sorted[i].y != after_sorted[i].y ||
            before_sorted[i].z != after_sorted[i].z) conserved = false;
    }
    CHECK(conserved, "centroid multiset is conserved after cluster reorder");

    // 6. TriEx parallelism: for every j, triex[j].materialId identifies the original
    //    source tri whose centroid matches tris[j].centroid
    bool triex_ok = true;
    for (size_t j = 0; j < tris.size() && triex_ok; ++j) {
        int mid = triex[j].materialId;
        if (mid < 0 || (size_t)mid >= orig_centroids.size()) { triex_ok = false; break; }
        const float3& oc = orig_centroids[mid];
        const float3& tc = tris[j].centroid;
        if (oc.x != tc.x || oc.y != tc.y || oc.z != tc.z) triex_ok = false;
    }
    CHECK(triex_ok, "triex[j].materialId tracks its source tri centroid after reorder");

    printf("  clusters: %zu, total tris: %u\n", clusters.size(), total);
    printf(all_le_target && total==40000u && contiguous && verts_in_aabb && conserved && triex_ok
           ? "PASSED\n" : "FAILED\n");
}

static void test_cluster_split_small() {
    printf("=== test_cluster_split_small (100 tris => 1 cluster) ===\n");

    // 5x10 grid => 5*10*2 = 100 tris
    std::vector<Tri> tris = grid_sheet_tris(5, 10, 5.0f, 10.0f);
    CHECK(tris.size() == 100u, "small grid has 100 tris");
    std::vector<TriEx> triex; // empty triex is allowed

    auto clusters = part_cluster::split_clusters(tris, triex, 16000);

    CHECK(clusters.size() == 1u, "100-tri input => exactly 1 cluster");
    if (!clusters.empty()) {
        CHECK(clusters[0].first_tri == 0u, "single cluster starts at 0");
        CHECK(clusters[0].tri_count == 100u, "single cluster has all 100 tris");
    }
    printf(clusters.size()==1u ? "PASSED\n" : "FAILED\n");
}

static void test_cluster_split_deterministic() {
    printf("=== test_cluster_split_deterministic ===\n");

    // 200x100 grid => 40,000 tris
    std::vector<Tri> tris_a = grid_sheet_tris(200, 100, 200.0f, 100.0f);
    std::vector<TriEx> triex_a(tris_a.size());
    for (size_t i = 0; i < tris_a.size(); ++i) {
        std::memset(&triex_a[i], 0, sizeof(TriEx));
        triex_a[i].materialId = (int)i;
        triex_a[i].tint = make_float4(1,1,1,0);
        triex_a[i].ao0 = triex_a[i].ao1 = triex_a[i].ao2 = 1.0f;
        triex_a[i].N0 = triex_a[i].N1 = triex_a[i].N2 = make_float3(0,1,0);
    }

    std::vector<Tri> tris_b = tris_a;          // copy before mutation
    std::vector<TriEx> triex_b = triex_a;

    auto clusters_a = part_cluster::split_clusters(tris_a, triex_a, 16000);
    auto clusters_b = part_cluster::split_clusters(tris_b, triex_b, 16000);

    bool same_count = (clusters_a.size() == clusters_b.size());
    CHECK(same_count, "determinism: same cluster count on identical inputs");

    bool same_clusters = same_count;
    for (size_t i = 0; i < clusters_a.size() && same_clusters; ++i) {
        if (clusters_a[i].first_tri != clusters_b[i].first_tri ||
            clusters_a[i].tri_count != clusters_b[i].tri_count) {
            same_clusters = false;
        }
    }
    CHECK(same_clusters, "determinism: cluster tables are identical");

    bool same_tris = (tris_a.size() == tris_b.size());
    for (size_t i = 0; i < tris_a.size() && same_tris; ++i) {
        if (std::memcmp(&tris_a[i], &tris_b[i], sizeof(Tri)) != 0) same_tris = false;
    }
    CHECK(same_tris, "determinism: reordered tri arrays are identical (memcmp)");

    bool same_triex = (triex_a.size() == triex_b.size());
    for (size_t i = 0; i < triex_a.size() && same_triex; ++i) {
        if (triex_a[i].materialId != triex_b[i].materialId) same_triex = false;
    }
    CHECK(same_triex, "determinism: reordered triex arrays are identical");

    printf(same_count && same_clusters && same_tris && same_triex ? "PASSED\n" : "FAILED\n");
}

// ------------------------------------------------------------------ v3 tests --

// Build a minimal BLASManager with n synthetic meshes of 2 tris each.
// Returns the BLAS handles in order.
static std::vector<BLASHandle> make_blas_n(BLASManager& blas, int n, int base_mat = 0) {
    std::vector<BLASHandle> handles;
    for (int k = 0; k < n; ++k) {
        std::vector<Tri> tris;
        tris.push_back(make_tri(
            make_float3((float)k,     0, 0),
            make_float3((float)k+1.f, 0, 0),
            make_float3((float)k+1.f, 1, 0)));
        tris.push_back(make_tri(
            make_float3((float)k,     0, 0),
            make_float3((float)k+1.f, 1, 0),
            make_float3((float)k,     1, 0)));
        std::vector<TriEx> ex(tris.size(), make_triex(base_mat + k));
        handles.push_back(blas.register_triangles(tris.data(), (int)tris.size(), ex.data()));
    }
    return handles;
}

// Map BLASHandle -> index in blas entries vector.
static uint32_t blas_handle_index(const BLASManager& blas, BLASHandle h) {
    const auto& entries = blas.get_entries();
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i]->handle == h) return (uint32_t)i;
    return UINT32_MAX;
}

static void test_v3_round_trip() {
    printf("=== test_v3_round_trip ===\n");

    // Build BLASManager with 2 entries (4 tris total, 2 each).
    BLASManager blas_out;
    TLASManager tlas_out(16);
    auto handles = make_blas_n(blas_out, 2, 10);
    uint32_t idx0 = blas_handle_index(blas_out, handles[0]);
    uint32_t idx1 = blas_handle_index(blas_out, handles[1]);
    CHECK(idx0 != UINT32_MAX && idx1 != UINT32_MAX, "v3: blas handles map to indices");

    // Build 2 clusters with distinct AABBs and 2-level LOD ladders.
    std::vector<part_asset::FlatCluster> clusters_out(2);
    // Cluster 0: covers BLAS 0
    clusters_out[0].aabb_min[0] = 0.0f; clusters_out[0].aabb_min[1] = 0.0f; clusters_out[0].aabb_min[2] = -1.0f;
    clusters_out[0].aabb_max[0] = 1.0f; clusters_out[0].aabb_max[1] = 1.0f; clusters_out[0].aabb_max[2] =  1.0f;
    { part_asset::LodLevel l0, l1;
      l0.screen_size_threshold = 200.0f; l0.blas_indices.push_back(idx0);
      l1.screen_size_threshold =   0.0f; l1.blas_indices.push_back(idx0);
      clusters_out[0].lods.push_back(std::move(l0));
      clusters_out[0].lods.push_back(std::move(l1)); }
    // Cluster 1: covers BLAS 1
    clusters_out[1].aabb_min[0] = 2.0f; clusters_out[1].aabb_min[1] = -0.5f; clusters_out[1].aabb_min[2] = -2.0f;
    clusters_out[1].aabb_max[0] = 4.0f; clusters_out[1].aabb_max[1] =  2.0f; clusters_out[1].aabb_max[2] =  2.0f;
    { part_asset::LodLevel l0, l1;
      l0.screen_size_threshold = 150.0f; l0.blas_indices.push_back(idx1);
      l1.screen_size_threshold =   0.0f; l1.blas_indices.push_back(idx1);
      clusters_out[1].lods.push_back(std::move(l0));
      clusters_out[1].lods.push_back(std::move(l1)); }

    const uint64_t kV3Hash = 0xABCDEF0012345678ull;
    const std::string v3_path = std::string(kCacheRoot) + "/parts/test_v3_roundtrip.flat.part";

    bool saved = part_asset::save_flat_v3(v3_path, blas_out, tlas_out, clusters_out, kV3Hash);
    CHECK(saved, "v3: save_flat_v3 returns true");
    if (!saved) { printf("  SKIPPING remaining v3 round-trip checks\n"); return; }

    // Load back.
    BLASManager blas_in;
    TLASManager tlas_in(16);
    std::vector<part_asset::FlatCluster> clusters_in;
    bool loaded = part_asset::load_flat_v3(v3_path, kV3Hash, blas_in, tlas_in, clusters_in);
    CHECK(loaded, "v3: load_flat_v3 returns true");
    if (!loaded) { printf("  SKIPPING cluster checks\n"); return; }

    // Cluster count and per-cluster fields.
    CHECK(clusters_in.size() == 2, "v3: round-trip: 2 clusters");
    if (clusters_in.size() == 2) {
        // AABB equality (bit-exact floats written/read back).
        CHECK(clusters_in[0].aabb_min[0] == clusters_out[0].aabb_min[0] &&
              clusters_in[0].aabb_min[1] == clusters_out[0].aabb_min[1] &&
              clusters_in[0].aabb_min[2] == clusters_out[0].aabb_min[2],
              "v3: cluster0 aabb_min matches");
        CHECK(clusters_in[0].aabb_max[0] == clusters_out[0].aabb_max[0] &&
              clusters_in[0].aabb_max[1] == clusters_out[0].aabb_max[1] &&
              clusters_in[0].aabb_max[2] == clusters_out[0].aabb_max[2],
              "v3: cluster0 aabb_max matches");
        CHECK(clusters_in[1].aabb_min[0] == clusters_out[1].aabb_min[0] &&
              clusters_in[1].aabb_min[1] == clusters_out[1].aabb_min[1] &&
              clusters_in[1].aabb_min[2] == clusters_out[1].aabb_min[2],
              "v3: cluster1 aabb_min matches");
        // LOD counts.
        CHECK(clusters_in[0].lods.size() == 2, "v3: cluster0 has 2 LOD levels");
        CHECK(clusters_in[1].lods.size() == 2, "v3: cluster1 has 2 LOD levels");
        if (clusters_in[0].lods.size() == 2) {
            CHECK(clusters_in[0].lods[0].screen_size_threshold == 200.0f, "v3: cluster0 lod0 threshold");
            CHECK(clusters_in[0].lods[1].screen_size_threshold ==   0.0f, "v3: cluster0 lod1 threshold");
            CHECK(clusters_in[0].lods[0].blas_indices.size() == 1, "v3: cluster0 lod0 index count");
        }
        if (clusters_in[1].lods.size() == 2) {
            CHECK(clusters_in[1].lods[0].screen_size_threshold == 150.0f, "v3: cluster1 lod0 threshold");
            CHECK(clusters_in[1].lods[0].blas_indices.size() == 1, "v3: cluster1 lod0 index count");
        }
    }

    // BLAS entries: 2 entries with 2 tris each.
    CHECK(blas_in.get_entries().size() == 2, "v3: round-trip: 2 BLAS entries");
    if (blas_in.get_entries().size() == 2) {
        CHECK(blas_in.get_entries()[0]->triangles.size() == 2, "v3: blas[0] tri count = 2");
        CHECK(blas_in.get_entries()[1]->triangles.size() == 2, "v3: blas[1] tri count = 2");
    }

    // v6: segment tag + inline_cutover round-trip.
    // Set distinct segment values on the two clusters before re-saving.
    clusters_out[0].segment = 0;
    clusters_out[1].segment = 1;

    // Build two instance refs with distinct inline_cutover values.
    std::vector<part_asset::FlatInstanceRef> refs(2);
    refs[0].child_resolved_hash = 0x1111111111111111ull;
    refs[1].child_resolved_hash = 0x2222222222222222ull;
    for (int i = 0; i < 16; ++i) {
        refs[0].transform[i] = (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0f : 0.0f;
        refs[1].transform[i] = refs[0].transform[i];
    }
    refs[0].inline_cutover = 0.575f;
    refs[1].inline_cutover = 0.0f;

    const uint64_t kV6Hash = 0xABCDEF0012345679ull;
    const std::string v6_path = std::string(kCacheRoot) + "/parts/test_v6_roundtrip.flat.part";

    bool saved_v6 = part_asset::save_flat_v3(v6_path, blas_out, tlas_out, clusters_out, refs, kV6Hash);
    CHECK(saved_v6, "v6: save_flat_v3 with segment+inline_cutover returns true");
    if (!saved_v6) { printf("  SKIPPING v6 field checks\n"); return; }

    BLASManager blas_v6;
    TLASManager tlas_v6(16);
    std::vector<part_asset::FlatCluster> loaded_clusters;
    std::vector<part_asset::FlatInstanceRef> loaded_refs;
    bool loaded_v6 = part_asset::load_flat_v3(v6_path, kV6Hash, blas_v6, tlas_v6,
                                               loaded_clusters, loaded_refs);
    CHECK(loaded_v6, "v6: load_flat_v3 returns true");
    if (loaded_v6 && loaded_clusters.size() == 2) {
        CHECK(loaded_clusters[0].segment == 0, "v6: cluster0 segment == 0");
        CHECK(loaded_clusters[1].segment == 1, "v6: cluster1 segment == 1");
    }
    if (loaded_v6 && loaded_refs.size() == 2) {
        CHECK(loaded_refs[0].inline_cutover == 0.575f, "v6: refs[0].inline_cutover == 0.575f");
        CHECK(loaded_refs[1].inline_cutover == 0.0f,  "v6: refs[1].inline_cutover == 0.0f");
    }
    static_assert(sizeof(part_asset::FlatInstanceRef) == 80, "flat ref layout");

    printf("PASSED\n");
}

static void test_v3_empty_children_and_lods() {
    printf("=== test_v3_empty_children_and_lods ===\n");
    // The v3 body MUST write child_count=0 and level_count=0 (same as the flat v2 invariant).
    // We verify by loading and checking there are no children/top-lods in the TLAS.
    BLASManager blas_out;
    TLASManager tlas_out(16);
    make_blas_n(blas_out, 1, 0);
    // No TLAS instances added => internal instance count is 0, children empty, top-lods empty.
    part_asset::FlatCluster fc;
    fc.aabb_min[0] = fc.aabb_min[1] = fc.aabb_min[2] = 0.0f;
    fc.aabb_max[0] = fc.aabb_max[1] = fc.aabb_max[2] = 1.0f;
    part_asset::LodLevel lv;
    lv.screen_size_threshold = 0.0f;
    lv.blas_indices.push_back(0);
    fc.lods.push_back(std::move(lv));
    std::vector<part_asset::FlatCluster> clusters_out = { fc };

    const uint64_t kHash2 = 0xC0FFEE00DEADBEEFull;
    const std::string path2 = std::string(kCacheRoot) + "/parts/test_v3_empty.flat.part";
    bool saved = part_asset::save_flat_v3(path2, blas_out, tlas_out, clusters_out, kHash2);
    CHECK(saved, "v3_empty: save ok");

    BLASManager blas_in;
    TLASManager tlas_in(16);
    std::vector<part_asset::FlatCluster> clusters_in;
    bool loaded = part_asset::load_flat_v3(path2, kHash2, blas_in, tlas_in, clusters_in);
    CHECK(loaded, "v3_empty: load ok");
    CHECK(clusters_in.size() == 1, "v3_empty: 1 cluster loaded");
    CHECK(tlas_in.get_draw_records().empty(), "v3_empty: no TLAS instances (empty body)");

    printf(loaded ? "PASSED\n" : "FAILED\n");
}

static void test_v3_cross_version_guards() {
    printf("=== test_v3_cross_version_guards ===\n");

    // M4: the guard is now about SECTIONS. It used to matter that a v2 file
    // and a v3 file were distinguishable by their headers, because both could
    // sit at a path a loader might be pointed at. In the bundle they are
    // different tags, so the question becomes: a bundle carrying only a
    // compositional body must not satisfy a flat load, and vice versa.
    //
    // Note kParentHash is NOT usable for this any more -- earlier tests in
    // this suite flatten it, so its bundle legitimately holds BOTH sections.
    const uint64_t kHashV2Guard = 0x2233445566778899ull;
    const std::string v2_path = std::string(kCacheRoot) + "/" +
                                part_asset::cache_path_resolved(kHashV2Guard);
    part_bundle::remove_section(v2_path, kHashV2Guard, part_bundle::kSectionFlat);
    {
        BLASManager blas_only; TLASManager tlas_only(16);
        make_blas_n(blas_only, 1, 0);
        part_asset::LodLevels lods_only;
        part_asset::LodLevel L; L.screen_size_threshold = 0.0f; L.blas_indices.push_back(0);
        lods_only.push_back(std::move(L));
        CHECK(part_asset::save_v2(v2_path, blas_only, tlas_only, nullptr, 0,
                                  lods_only, kHashV2Guard),
              "cross-guard: body-only bundle written");
    }
    BLASManager bv2; TLASManager tv2(16);
    std::vector<part_asset::ChildInstance> ch;
    part_asset::LodLevels lv2;
    bool v2_ok = part_asset::load_v2(v2_path, kHashV2Guard, bv2, tv2, ch, lv2);
    CHECK(v2_ok, "cross-guard: v2 body loads as v2 (sanity)");

    BLASManager bv3_a; TLASManager tv3_a(16);
    std::vector<part_asset::FlatCluster> dummy;
    bool v3_on_v2 = part_asset::load_flat_v3(v2_path, kHashV2Guard, bv3_a, tv3_a, dummy);
    CHECK(!v3_on_v2, "cross-guard: a body-only bundle does not satisfy a flat load");

    // Write a v3 file; load_v2 on it must return false.
    BLASManager blas_v3; TLASManager tlas_v3(16);
    make_blas_n(blas_v3, 1, 0);
    part_asset::FlatCluster fc2;
    fc2.aabb_min[0] = fc2.aabb_min[1] = fc2.aabb_min[2] = 0.0f;
    fc2.aabb_max[0] = fc2.aabb_max[1] = fc2.aabb_max[2] = 1.0f;
    part_asset::LodLevel lv3;
    lv3.screen_size_threshold = 0.0f; lv3.blas_indices.push_back(0);
    fc2.lods.push_back(std::move(lv3));
    std::vector<part_asset::FlatCluster> cls_v3 = { fc2 };
    const uint64_t kHashV3Guard = 0x1122334455667788ull;
    const std::string v3_path = std::string(kCacheRoot) + "/parts/test_v3_guard.flat.part";
    bool sv3 = part_asset::save_flat_v3(v3_path, blas_v3, tlas_v3, cls_v3, kHashV3Guard);
    CHECK(sv3, "cross-guard: save_flat_v3 ok for guard test");

    BLASManager bv2b; TLASManager tv2b(16);
    std::vector<part_asset::ChildInstance> ch2;
    part_asset::LodLevels lv2b;
    bool v2_on_v3 = part_asset::load_v2(v3_path, kHashV3Guard, bv2b, tv2b, ch2, lv2b);
    CHECK(!v2_on_v3, "cross-guard: load_v2 on a v3 file returns false");

    printf("PASSED\n");
}

static void test_peek_format_version() {
    printf("=== test_peek_format_version ===\n");

    // M4: peek answers ONE question -- "is there a flattened artifact here?".
    // It used to report the format version of whatever file sat at the path,
    // which was the same answer only because a `.flat.part` existed exactly
    // when the answer was yes. A bundle holds the compositional body too, so
    // reporting "2" for it would send PartStore's flat-preferred load down its
    // legacy-v2-flat branch and load the body AS a flat, losing the child
    // table. So: kFormatVersionFlat when a FLAT section exists, else 0.
    const uint64_t kPeekHash = 0x33445566778899AAull;
    const std::string path = std::string(kCacheRoot) + "/" +
                             part_asset::cache_path_resolved(kPeekHash);
    part_bundle::remove_section(path, kPeekHash, part_bundle::kSectionFlat);
    {
        BLASManager blas; TLASManager tlas(16);
        make_blas_n(blas, 1, 0);
        part_asset::LodLevels lods;
        part_asset::LodLevel L; L.screen_size_threshold = 0.0f; L.blas_indices.push_back(0);
        lods.push_back(std::move(L));
        CHECK(part_asset::save_v2(path, blas, tlas, nullptr, 0, lods, kPeekHash),
              "peek: body-only bundle written");
    }
    CHECK(part_asset::peek_format_version(path) == 0,
          "peek returns 0 for a bundle with no flat section");

    {
        BLASManager blas; TLASManager tlas(16);
        make_blas_n(blas, 1, 0);
        part_asset::FlatCluster fc;
        fc.aabb_min[0] = fc.aabb_min[1] = fc.aabb_min[2] = 0.0f;
        fc.aabb_max[0] = fc.aabb_max[1] = fc.aabb_max[2] = 1.0f;
        part_asset::LodLevel L; L.screen_size_threshold = 0.0f; L.blas_indices.push_back(0);
        fc.lods.push_back(std::move(L));
        std::vector<part_asset::FlatCluster> cls = { fc };
        CHECK(part_asset::save_flat_v3(path, blas, tlas, cls, kPeekHash),
              "peek: flat section added to the same bundle");
    }
    CHECK(part_asset::peek_format_version(path) == part_asset::kFormatVersionFlat,
          "peek returns the flat version once a flat section exists");

    // A missing bundle is 0, not a crash.
    CHECK(part_asset::peek_format_version(std::string(kCacheRoot) +
                                          "/parts/does_not_exist.bundle") == 0,
          "peek returns 0 for a missing bundle");

    printf("PASSED\n");
}

static void test_v2_byte_stability() {
    printf("=== test_v2_byte_stability ===\n");

    // Save a v2 fixture to a known path, read its bytes.
    // Then save again to a different path and compare byte-for-byte.
    // This catches any refactor drift in the v2 serialization path.
    const uint64_t kStabHash = 0xF0F0F0F0A5A5A5A5ull;
    const std::string stab_path_a = std::string(kCacheRoot) + "/parts/stab_a.part";
    const std::string stab_path_b = std::string(kCacheRoot) + "/parts/stab_b.part";

    BLASManager bla; TLASManager tla(16);
    auto hv = make_blas_n(bla, 2, 5);
    uint32_t si0 = blas_handle_index(bla, hv[0]);
    uint32_t si1 = blas_handle_index(bla, hv[1]);

    part_asset::LodLevels lods_stab;
    { part_asset::LodLevel l0, l1;
      l0.screen_size_threshold = 100.0f; l0.blas_indices.push_back(si0);
      l1.screen_size_threshold =   0.0f; l1.blas_indices.push_back(si1);
      lods_stab.push_back(std::move(l0));
      lods_stab.push_back(std::move(l1)); }

    bool s1 = part_asset::save_v2(stab_path_a, bla, tla, nullptr, 0, lods_stab, kStabHash);
    CHECK(s1, "v2_stability: first save ok");
    bool s2 = part_asset::save_v2(stab_path_b, bla, tla, nullptr, 0, lods_stab, kStabHash);
    CHECK(s2, "v2_stability: second save ok");

    std::vector<char> bytes_a, bytes_b;
    bool ra = read_bytes(stab_path_a, bytes_a);
    bool rb = read_bytes(stab_path_b, bytes_b);
    CHECK(ra && rb, "v2_stability: both files readable");
    CHECK(!bytes_a.empty() && bytes_a == bytes_b,
          "v2_stability: save_v2 is byte-identical across calls (refactor-stable)");

    // Also round-trip to confirm v2 still loads.
    BLASManager blb; TLASManager tlb(16);
    std::vector<part_asset::ChildInstance> ch_stab;
    part_asset::LodLevels lods_loaded;
    bool loaded = part_asset::load_v2(stab_path_a, kStabHash, blb, tlb, ch_stab, lods_loaded);
    CHECK(loaded, "v2_stability: saved file loads back as v2");
    CHECK(lods_loaded.size() == 2, "v2_stability: 2 LOD levels survive round-trip");

    printf("PASSED\n");
}

// ----------------------------------------------------------------- Task 11 tests --

// Large-mesh test: synthesize a 40k-tri grid, save as a one-BLAS v2 part, then
// flatten it. Verifies that:
//  - the flat artifact is v3
//  - result.clusters > 1 (40k >> 16000 target)
//  - every cluster's level-0 tri range <= 16000
//  - tri counts across all cluster level-0 entries sum to full_tris
static const uint64_t kBigHash = 0x4040404040404040ull;

static void test_flatten_clustered_v3() {
    printf("=== test_flatten_clustered_v3 ===\n");

    // Write a 40k-tri flat grid as a single v2 part.
    const int NX = 200, NZ = 100;
    std::vector<Tri> big_tris = grid_sheet_tris(NX, NZ, 200.0f, 100.0f);
    CHECK(big_tris.size() == 40000u, "big mesh: 40000 tris");

    {
        BLASManager blas; TLASManager tlas(16);
        std::vector<TriEx> ex(big_tris.size(), make_triex(99));
        BLASHandle h = blas.register_triangles(big_tris.data(), (int)big_tris.size(), ex.data());
        uint32_t idx = UINT32_MAX;
        const auto& entries = blas.get_entries();
        for (size_t k = 0; k < entries.size(); ++k)
            if (entries[k]->handle == h) { idx = (uint32_t)k; break; }
        CHECK(idx != UINT32_MAX, "big mesh: blas registration ok");
        part_asset::LodLevels lods;
        part_asset::LodLevel L; L.screen_size_threshold = 0.0f; L.blas_indices.push_back(idx);
        lods.push_back(L);
        const std::string path = std::string(kCacheRoot) + "/" + part_asset::cache_path_resolved(kBigHash);
        bool sv = part_asset::save_v2(path, blas, tlas, nullptr, 0, lods, kBigHash);
        CHECK(sv, "big mesh: save_v2 ok");
        if (!sv) { printf("  SKIPPING remaining big-mesh tests\n"); return; }
    }

    const std::string big_flat = std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(kBigHash);
    part_bundle::remove_section(big_flat, kBigHash, part_bundle::kSectionFlat);

    part_flatten::FlattenTargets tgt;
    // Use tight cluster size so we definitely get multiple clusters.
    tgt.cluster_target_tris = 16000;

    part_flatten::FlattenResult res = part_flatten::flatten_part(kCacheRoot, kBigHash, tgt);
    CHECK(res.ok, "big mesh: flatten_part ok");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); return; }

    CHECK(res.full_tris == 40000u, "big mesh: full_tris == 40000");
    CHECK(res.clusters > 1, "big mesh: result.clusters > 1 (split required)");
    printf("  clusters=%zu, levels=%zu, full_tris=%zu\n", res.clusters, res.levels, res.full_tris);

    // Verify current bake-version format.
    uint32_t fv = part_asset::peek_format_version(big_flat);
    CHECK(fv == part_asset::kFormatVersionFlat, "big mesh: flat artifact is current bake version");

    // Load v3 and verify cluster invariants.
    BLASManager blas_in; TLASManager tlas_in(16);
    std::vector<part_asset::FlatCluster> clusters_in;
    bool loaded = part_asset::load_flat_v3(big_flat, kBigHash, blas_in, tlas_in, clusters_in);
    CHECK(loaded, "big mesh: load_flat_v3 ok");
    if (!loaded) return;

    CHECK(clusters_in.size() == res.clusters, "big mesh: cluster count matches result");

    // Every cluster level-0 tri count must be <= 16000.
    const auto& entries = blas_in.get_entries();
    uint32_t total_l0 = 0;
    bool all_le_target = true;
    for (const auto& cl : clusters_in) {
        if (cl.lods.empty()) continue;
        uint32_t cl_l0 = 0;
        for (uint32_t bi : cl.lods[0].blas_indices) {
            if (bi < entries.size()) cl_l0 += (uint32_t)entries[bi]->triangles.size();
        }
        if (cl_l0 > 16000) { all_le_target = false; }
        total_l0 += cl_l0;
    }
    CHECK(all_le_target, "big mesh: every cluster level-0 tri count <= 16000");
    CHECK(total_l0 == 40000u, "big mesh: cluster level-0 tri counts sum to 40000");

    printf(res.ok && res.clusters > 1 && all_le_target && total_l0 == 40000u ? "PASSED\n" : "FAILED\n");
}

// Watertight invariant (Task 8 payoff): for the 40k grid flatten, shared
// cluster-boundary vertices must remain bit-identical across clusters at EVERY
// LOD level. Verifies that decimate_to_error with use_aabb_bounds=false plus
// the topological boundary lock (lock_boundary=true) freezes seam vertices.
static void test_flatten_watertight_invariant() {
    printf("=== test_flatten_watertight_invariant ===\n");

    // Re-use the big-mesh flat from the previous test (same kBigHash).
    const std::string big_flat = std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(kBigHash);

    BLASManager blas_in; TLASManager tlas_in(16);
    std::vector<part_asset::FlatCluster> clusters_in;
    bool loaded = part_asset::load_flat_v3(big_flat, kBigHash, blas_in, tlas_in, clusters_in);
    CHECK(loaded, "watertight: load_flat_v3 ok");
    if (!loaded || clusters_in.size() < 2) {
        CHECK(false, "watertight: need >= 2 clusters");
        return;
    }

    const auto& entries = blas_in.get_entries();

    // Helper: collect all vertex positions from a set of BLAS indices.
    struct FP3 { float x, y, z;
        bool operator<(const FP3& o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z;
        }
    };
    auto collect_verts = [&](const std::vector<uint32_t>& blas_indices) {
        std::set<FP3> verts;
        for (uint32_t bi : blas_indices) {
            if (bi >= entries.size()) continue;
            for (const Tri& t : entries[bi]->triangles) {
                const float3* vs[3] = { &t.vertex0, &t.vertex1, &t.vertex2 };
                for (const float3* v : vs)
                    verts.insert({v->x, v->y, v->z});
            }
        }
        return verts;
    };

    // Collect level-0 vertex sets per cluster.
    std::vector<std::set<FP3>> per_cluster_verts(clusters_in.size());
    for (size_t ci = 0; ci < clusters_in.size(); ++ci) {
        if (clusters_in[ci].lods.empty()) continue;
        per_cluster_verts[ci] = collect_verts(clusters_in[ci].lods[0].blas_indices);
    }

    // Find cross-cluster shared positions: vertices appearing in >= 2 clusters.
    std::map<FP3, int> vert_cluster_count;
    for (const auto& vs : per_cluster_verts)
        for (const FP3& p : vs) vert_cluster_count[p]++;
    std::set<FP3> shared_verts;
    for (const auto& kv : vert_cluster_count)
        if (kv.second >= 2) shared_verts.insert(kv.first);
    printf("  shared boundary vertices: %zu\n", shared_verts.size());
    CHECK(!shared_verts.empty(), "watertight: 40k grid has cross-cluster shared boundary vertices");

    // For EVERY cluster and EVERY LOD level: each shared vertex that belongs to the
    // cluster at level 0 must also be present bit-identical at every coarser level.
    int missing_total = 0;
    for (size_t ci = 0; ci < clusters_in.size(); ++ci) {
        // Shared vertices belonging to this cluster at level 0.
        std::set<FP3> cluster_shared;
        for (const FP3& p : per_cluster_verts[ci])
            if (shared_verts.count(p)) cluster_shared.insert(p);
        if (cluster_shared.empty()) continue;

        // Check every coarser MESH level. The terminal billboard rung (M2.5)
        // is deliberately exempt: it is not a decimation of the cluster, it is
        // a picture of it, and its four corners are a camera-facing quad that
        // shares no vertex with anything. Watertightness is a property of the
        // mesh ladder, and the mesh ladder now stops one rung earlier.
        for (size_t li = 1; li < clusters_in[ci].lods.size(); ++li) {
            const auto& bi = clusters_in[ci].lods[li].blas_indices;
            if (bi.size() == 1 && bi[0] < entries.size() &&
                impostor::is_billboard_rung(entries[bi[0]]->triangles,
                                            entries[bi[0]]->tri_extra))
                continue;
            std::set<FP3> level_verts = collect_verts(clusters_in[ci].lods[li].blas_indices);
            for (const FP3& p : cluster_shared) {
                if (level_verts.find(p) == level_verts.end()) {
                    ++missing_total;
                    printf("  MISSING shared vertex (%.6f, %.6f, %.6f) in cluster %zu level %zu\n",
                           p.x, p.y, p.z, ci, li);
                    if (missing_total > 5) { printf("  (further mismatches suppressed)\n"); goto done; }
                }
            }
        }
    }
done:
    CHECK(missing_total == 0,
          "watertight: all shared boundary vertices preserved bit-identical at every LOD level");
    printf(missing_total == 0 ? "PASSED\n" : "FAILED\n");
}

// Task 7: small part (old min_tris=2000 floor would freeze it at LOD0) now
// gets a real ladder thanks to the new 32-tri stop rule.
static void test_small_part_gets_ladder() {
    printf("=== test_small_part_gets_ladder ===\n");

    // Remove any stale flat artifact first.
    std::string flat = std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(kSmallSphereHash);
    part_bundle::remove_section(flat, kSmallSphereHash, part_bundle::kSectionFlat);

    // Remove any stale lods sidecar to ensure QEM path, not budget-ladder path.
    std::string lods = std::string(kCacheRoot) + "/" + part_asset::cache_path_lods(kSmallSphereHash);
    part_bundle::remove_section(lods, kSmallSphereHash, part_bundle::kSectionVariants);

    uint64_t hash = write_small_sphere_part();
    CHECK(hash != 0, "small sphere part written");
    if (hash == 0) { printf("  SKIPPING\n"); return; }

    auto res = part_flatten::flatten_part(kCacheRoot, hash);
    CHECK(res.ok, "small part: flatten_part ok");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); return; }
    CHECK(res.levels >= 2,
          "small part: laddered despite being small (>= 2 levels, not frozen at LOD0)");
    CHECK(res.coarsest_tris <= 64,
          "small part: coarsest level driven down near the 32-tri floor");

    printf("  levels=%zu, coarsest_tris=%zu, full_tris=%zu\n",
           res.levels, res.coarsest_tris, res.full_tris);
    printf(res.levels >= 2 && res.coarsest_tris <= 64 ? "PASSED\n" : "FAILED\n");
}

// Task 7: ratio-2 divisor schedule yields a deep ladder with monotonically
// decreasing rung tri-counts on a dense fixture.
static void test_ratio2_ladder_shape() {
    printf("=== test_ratio2_ladder_shape ===\n");

    std::string flat = std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(kDenseSphereHash);
    part_bundle::remove_section(flat, kDenseSphereHash, part_bundle::kSectionFlat);

    uint64_t hash = write_dense_sphere_part();
    CHECK(hash != 0, "dense sphere part written");
    if (hash == 0) { printf("  SKIPPING\n"); return; }

    auto res = part_flatten::flatten_part(kCacheRoot, hash);
    CHECK(res.ok, "dense part: flatten_part ok");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); return; }
    CHECK(res.levels >= 6, "dense part: >= 6 levels with ratio-2 schedule");

    // Load the flat artifact and check per-cluster monotonic decrease.
    BLASManager blas; TLASManager tlas(4);
    std::vector<part_asset::FlatCluster> clusters;
    bool loaded = part_asset::load_flat_v3(flat, hash, blas, tlas, clusters);
    CHECK(loaded, "dense part: load_flat_v3 ok");
    if (!loaded) { printf("  SKIPPING monotonic check\n"); return; }

    bool monotonic = true;
    for (const auto& cl : clusters) {
        size_t prev = SIZE_MAX;
        for (const auto& lvl : cl.lods) {
            if (lvl.blas_indices.empty()) continue;
            size_t tris = blas.get_entries()[lvl.blas_indices[0]]->triangles.size();
            if (prev != SIZE_MAX && tris >= prev) { monotonic = false; break; }
            prev = tris;
        }
        if (!monotonic) break;
    }
    CHECK(monotonic, "dense part: per-cluster LOD tri-counts strictly decrease");

    printf("  levels=%zu, clusters=%zu, full_tris=%zu\n",
           res.levels, res.clusters, res.full_tris);
    printf(res.levels >= 6 && monotonic ? "PASSED\n" : "FAILED\n");
}

// ----------------------------------------------------------------- Task 14 test --

// Write a tiny part (~40 tris) as a childless .part in the cache.
// Uses a fixed hash distinct from any other fixture.
static const uint64_t kTinyPartHash = 0xCCCC000033330003ull;

static uint64_t write_tiny_part() {
    // 5 segs x 4 rings => 5*4*2=40 tris; small enough to be "low" variant
    std::vector<Tri> tris = sphere_tris(5, 4);
    if (!save_fixture(kTinyPartHash, 8, {tris}, {})) return 0;
    return kTinyPartHash;
}

// Test that flatten_part detects a .lods sidecar and assembles the budget ladder:
// single cluster, two levels (one per variant), no QEM decimation.
static void test_budget_ladder_assembly() {
    printf("=== test_budget_ladder_assembly ===\n");

    // Two hand-built childless parts standing in for budget variants:
    // "full" (~300 tris) and "low" (~40 tris). A hand-written sidecar binds them.
    uint64_t full_hash = write_small_sphere_part();
    uint64_t low_hash  = write_tiny_part();
    CHECK(full_hash != 0, "budget ladder: full part written");
    CHECK(low_hash  != 0, "budget ladder: low part written");
    if (!full_hash || !low_hash) { printf("  SKIPPING\n"); return; }

    // Remove any stale flat artifact so the sidecar path re-runs.
    std::string flat = std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(full_hash);
    part_bundle::remove_section(flat, full_hash, part_bundle::kSectionFlat);

    // Write the budget-variant sidecar for full_hash. M4: it is a bundle
    // section, so it goes through its serializer rather than an ofstream at a
    // sibling path.
    {
        std::string sidecar = std::string(kCacheRoot) + "/" + part_asset::cache_path_lods(full_hash);
        part_asset::LodVariants v;
        v.anchor_size = 0.5;
        v.budgets = {1.0, 0.3};
        v.hashes  = {full_hash, low_hash};
        CHECK(part_asset::save_lod_sidecar(sidecar, full_hash, v),
              "budget ladder: sidecar written");
    }

    auto res = part_flatten::flatten_part(kCacheRoot, full_hash);
    CHECK(res.ok, "budget ladder: flatten_part ok");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); return; }
    CHECK(res.clusters == 1, "budget ladder: single cluster");
    CHECK(res.levels   == 2, "budget ladder: 2 levels (one per variant)");

    BLASManager blas; TLASManager tlas(4);
    std::vector<part_asset::FlatCluster> clusters;
    bool loaded = part_asset::load_flat_v3(flat, full_hash, blas, tlas, clusters);
    CHECK(loaded, "budget ladder: load_flat_v3 ok");
    if (!loaded) return;
    CHECK(clusters.size() == 1, "budget ladder: 1 cluster in artifact");
    const auto& lods = clusters[0].lods;
    CHECK(lods.size() == 2, "budget ladder: 2 LOD levels in cluster");
    if (lods.size() < 2) return;

    // Level tri counts must match variant parts exactly (no decimation).
    const auto& entries = blas.get_entries();
    CHECK(!lods[0].blas_indices.empty(), "budget ladder: lod[0] has blas index");
    CHECK(!lods[1].blas_indices.empty(), "budget ladder: lod[1] has blas index");
    if (lods[0].blas_indices.empty() || lods[1].blas_indices.empty()) return;

    size_t t0 = entries[lods[0].blas_indices[0]]->triangles.size();
    size_t t1 = entries[lods[1].blas_indices[0]]->triangles.size();
    CHECK(t0 > t1, "budget ladder: fine level has more tris than coarse");

    // Thresholds: fine (lod0) > 0, coarse (lod1) == 0.
    CHECK(lods[0].screen_size_threshold > 0.0f, "budget ladder: lod[0] threshold > 0");
    CHECK(lods[1].screen_size_threshold == 0.0f, "budget ladder: lod[1] threshold == 0 (never hides)");

    // Native TriEx present at both levels.
    CHECK(!entries[lods[0].blas_indices[0]]->tri_extra.empty(),
          "budget ladder: TriEx present at lod[0]");
    CHECK(!entries[lods[1].blas_indices[0]]->tri_extra.empty(),
          "budget ladder: TriEx present at lod[1]");

    printf("  t0=%zu t1=%zu thr0=%.4f clusters=%zu levels=%zu\n",
           t0, t1, lods[0].screen_size_threshold, clusters.size(), lods.size());
    printf("  test_budget_ladder_assembly OK\n");

    // Clean up the sidecar we wrote so ordering doesn't matter on reruns.
    std::string sidecar = std::string(kCacheRoot) + "/" + part_asset::cache_path_lods(full_hash);
    part_bundle::remove_section(sidecar, full_hash, part_bundle::kSectionVariants);
}

// ---------------------------------------------------------------------------
// M3 (docs/lod-vt-redesign-2026-08-04.md §3.1): an AUTHORED ladder does what
// it says. A `static lods` block naming switch distances in metres and named
// generators drives the flatten directly, and the thresholds it writes make
// the runtime selection walk (lod_distance.h — the ONE rule) flip rungs at
// exactly those metres.
//
// Fixture hash is its own so this test cannot be perturbed by, or perturb,
// the budget-ladder fixture's sidecar.
static const uint64_t kAuthoredHash = 0xA110000011110001ull;

static void test_authored_ladder_switch_distances() {
    printf("=== test_authored_ladder_switch_distances ===\n");

    std::vector<Tri> tris = sphere_tris(20, 10);        // ~400 tris, radius 1
    CHECK(save_fixture(kAuthoredHash, 5, {tris}, {}), "authored: fixture written");

    const std::string flat =
        std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(kAuthoredHash);
    const std::string slods =
        std::string(kCacheRoot) + "/" + part_asset::cache_path_static_lods(kAuthoredHash);
    part_bundle::remove_section(flat, kAuthoredHash, part_bundle::kSectionFlat);

    // Three reps: build() verbatim out to 18 m, a coarse decimation out to
    // 45 m, a coarser one past that. Written through the real save/load so the
    // sidecar round-trip is under test too.
    const double kAt1 = 18.0, kAt2 = 45.0;
    {
        part_asset::StaticLodPlan plan;
        plan.level_hashes = { kAuthoredHash, kAuthoredHash, kAuthoredHash };
        plan.level_exclude_masks = { 0u, 0u, 0u };
        plan.level_at  = { 0.0, kAt1, kAt2 };
        plan.level_gen = { "", "decimate {\"error\":0.02}", "decimate {\"divisor\":8}" };
        CHECK(part_asset::save_static_lod_plan(slods, kAuthoredHash, plan), "authored: sidecar written");

        part_asset::StaticLodPlan rt;
        CHECK(part_asset::load_static_lod_plan(slods, kAuthoredHash, rt), "authored: sidecar reloads");
        CHECK(rt.level_at == plan.level_at, "authored: distances round-trip");
        CHECK(rt.level_gen == plan.level_gen, "authored: generators round-trip");
        CHECK(rt.drives_ladder(), "authored: plan drives the ladder");
    }

    auto res = part_flatten::flatten_part(kCacheRoot, kAuthoredHash);
    CHECK(res.ok, "authored: flatten_part ok");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); part_bundle::remove_section(slods, kAuthoredHash, part_bundle::kSectionPlan); return; }
    CHECK(res.clusters == 1, "authored: single cluster");
    CHECK(res.levels == 3, "authored: one rep per authored entry");

    BLASManager blas; TLASManager tlas(4);
    std::vector<part_asset::FlatCluster> clusters;
    CHECK(part_asset::load_flat_v3(flat, kAuthoredHash, blas, tlas, clusters),
          "authored: load_flat_v3 ok");
    if (clusters.size() != 1 || clusters[0].lods.size() != 3) {
        printf("  SKIPPING (unexpected artifact shape)\n");
        part_bundle::remove_section(slods, kAuthoredHash, part_bundle::kSectionPlan); return;
    }
    const auto& lods = clusters[0].lods;
    const auto& entries = blas.get_entries();
    const size_t t0 = entries[lods[0].blas_indices[0]]->triangles.size();
    const size_t t1 = entries[lods[1].blas_indices[0]]->triangles.size();
    const size_t t2 = entries[lods[2].blas_indices[0]]->triangles.size();
    CHECK(t0 == tris.size(), "authored: rep 0 is build() verbatim (undecimated)");
    CHECK(t1 < t0 && t2 < t1, "authored: the named generator actually decimated");

    // The radius the RUNTIME will use: half the cluster AABB diagonal
    // (part_store.cpp). The authored metres were normalized against this.
    const float dx = clusters[0].aabb_max[0] - clusters[0].aabb_min[0];
    const float dy = clusters[0].aabb_max[1] - clusters[0].aabb_min[1];
    const float dz = clusters[0].aabb_max[2] - clusters[0].aabb_min[2];
    const float radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);

    // The table is switch-OUT: rep i's threshold is rep (i+1)'s switch-IN.
    const float want0 = radius / (float)kAt1;
    const float want1 = radius / (float)kAt2;
    CHECK(std::fabs(lods[0].screen_size_threshold - want0) < 1e-4f * want0,
          "authored: rep 0's threshold is radius / at(rep 1)");
    CHECK(std::fabs(lods[1].screen_size_threshold - want1) < 1e-4f * want1,
          "authored: rep 1's threshold is radius / at(rep 2)");
    CHECK(lods[2].screen_size_threshold == 0.0f,
          "authored: the last rep never hides");

    // ...and the selection walk flips exactly there. reach for a unit-scale
    // instance at the default dial is the cluster radius itself.
    float sw[3];
    for (int i = 0; i < 3; ++i)
        sw[i] = lod::normalized_switch_distance(lods[i].screen_size_threshold);
    const float reach = lod::reach(radius, 1.0f, 1.0f);
    CHECK(lod::select_rep(sw, 3, 1.0f,   reach) == 0, "authored: 1 m  -> rep 0");
    CHECK(lod::select_rep(sw, 3, 17.9f,  reach) == 0, "authored: 17.9 m -> rep 0");
    CHECK(lod::select_rep(sw, 3, 18.1f,  reach) == 1, "authored: 18.1 m -> rep 1");
    CHECK(lod::select_rep(sw, 3, 44.9f,  reach) == 1, "authored: 44.9 m -> rep 1");
    CHECK(lod::select_rep(sw, 3, 45.1f,  reach) == 2, "authored: 45.1 m -> rep 2");
    CHECK(lod::select_rep(sw, 3, 5000.f, reach) == 2, "authored: far away -> rep 2");

    // Determinism: a second cold flatten of the same inputs is byte-identical.
    std::vector<char> first = read_all_bytes(flat);
    part_bundle::remove_section(flat, kAuthoredHash, part_bundle::kSectionFlat);
    auto res2 = part_flatten::flatten_part(kCacheRoot, kAuthoredHash);
    CHECK(res2.ok, "authored: second flatten ok");
    std::vector<char> second = read_all_bytes(flat);
    CHECK(!first.empty() && first == second,
          "authored: two cold bakes are byte-identical");

    printf("  t=%zu/%zu/%zu radius=%.4f thr=%.5f/%.5f/%.1f\n", t0, t1, t2, radius,
           lods[0].screen_size_threshold, lods[1].screen_size_threshold,
           lods[2].screen_size_threshold);
    printf("  test_authored_ladder_switch_distances OK\n");

    part_bundle::remove_section(slods, kAuthoredHash, part_bundle::kSectionPlan);
}

// ---------------------------------------------------------------------------
// §3.4: the AUTHORED terminal impostor. An authored ladder ending in
// `LOD.impostor({ at })` gets the billboard as its last rung, switching in at
// the metres the author named — which is the number M6.5's baked distant
// shadows have to hand off at, and the answer to "trees render geometry way
// too far out" that does not require dialling the global pixel budget down.
//
// The fixture is deliberately TALL AND THIN (a stretched sphere, the shape of
// the trees this exists for) because that is what makes the AABB assertion
// below bite: build_quad squares the card off at 1.10× the bounding-sphere
// radius, so admitting it to the cluster AABB would inflate a 0.4 m-wide
// trunk's X extent to 6.6 m. cluster_radius is the number every authored `at`
// is normalized against, so that would silently move every switch on the
// ladder — including the one the author wrote in metres.
static const uint64_t kImpostorLadderHash = 0xA110000033330003ull;

static void test_authored_ladder_impostor_terminal() {
    printf("=== test_authored_ladder_impostor_terminal ===\n");

    // This test drives MATTER_IMPOSTOR itself for one phase. If the whole
    // process was already started with impostors off (a diagnostic run), the
    // "one billboard baked" phase cannot hold and the failure would say
    // nothing about the code. Skip rather than report a false red.
    {
        const char* v = std::getenv("MATTER_IMPOSTOR");
        if (v && v[0] == '0') {
            printf("  SKIPPED (MATTER_IMPOSTOR=0 in the environment)\n");
            return;
        }
    }

    // Tall and thin: 0.4 m across, 6 m tall.
    std::vector<Tri> tris = sphere_tris(20, 10);
    auto stretch = [](float3& v) { v.x *= 0.2f; v.z *= 0.2f; v.y *= 3.0f; };
    for (Tri& t : tris) {
        stretch(t.vertex0); stretch(t.vertex1); stretch(t.vertex2);
        t.centroid = make_float3((t.vertex0.x + t.vertex1.x + t.vertex2.x) / 3.0f,
                                 (t.vertex0.y + t.vertex1.y + t.vertex2.y) / 3.0f,
                                 (t.vertex0.z + t.vertex1.z + t.vertex2.z) / 3.0f);
    }
    CHECK(save_fixture(kImpostorLadderHash, 5, {tris}, {}), "imp ladder: fixture written");

    const std::string flat =
        std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(kImpostorLadderHash);
    const std::string slods =
        std::string(kCacheRoot) + "/" + part_asset::cache_path_static_lods(kImpostorLadderHash);
    const std::string fimp =
        std::string(kCacheRoot) + "/" + impostor::cache_path_impostor(kImpostorLadderHash);

    // kCacheRoot is created but NEVER wiped, so it survives between runs of
    // this binary. Phase (a) below asserts that no atlas exists when no
    // terminal is declared — which the PREVIOUS run's phase (b) would satisfy
    // from disk. Clear both sections up front so each run starts cold; without
    // this the test passes exactly once per fresh temp directory.
    part_bundle::remove_section(fimp, kImpostorLadderHash, part_bundle::kSectionImpostor);
    part_bundle::remove_section(slods, kImpostorLadderHash, part_bundle::kSectionPlan);
    part_bundle::remove_section(flat, kImpostorLadderHash, part_bundle::kSectionFlat);

    // Helper: write a plan and flatten cold, returning the cluster radius the
    // RUNTIME would derive (part_store.cpp: half the AABB diagonal).
    auto bake = [&](const std::vector<double>& at,
                    const std::vector<std::string>& gen,
                    part_flatten::FlattenResult& res_out,
                    std::vector<part_asset::FlatCluster>& clusters_out,
                    BLASManager& blas_out) -> float {
        part_asset::StaticLodPlan plan;
        plan.level_hashes.assign(at.size(), kImpostorLadderHash);
        plan.level_exclude_masks.assign(at.size(), 0u);
        plan.level_at  = at;
        plan.level_gen = gen;
        if (!part_asset::save_static_lod_plan(slods, kImpostorLadderHash, plan)) return -1.0f;
        part_bundle::remove_section(flat, kImpostorLadderHash, part_bundle::kSectionFlat);
        res_out = part_flatten::flatten_part(kCacheRoot, kImpostorLadderHash);
        if (!res_out.ok) return -1.0f;
        TLASManager tlas(4);
        if (!part_asset::load_flat_v3(flat, kImpostorLadderHash, blas_out, tlas, clusters_out))
            return -1.0f;
        if (clusters_out.size() != 1) return -1.0f;
        const float dx = clusters_out[0].aabb_max[0] - clusters_out[0].aabb_min[0];
        const float dy = clusters_out[0].aabb_max[1] - clusters_out[0].aabb_min[1];
        const float dz = clusters_out[0].aabb_max[2] - clusters_out[0].aabb_min[2];
        return 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
    };

    // (a) The mesh-only reference: the same two reps, no terminal declared.
    float mesh_radius = 0.0f;
    {
        part_flatten::FlattenResult r;
        std::vector<part_asset::FlatCluster> cl;
        BLASManager b;
        mesh_radius = bake({0.0, 18.0}, {"", "decimate {\"divisor\":8}"}, r, cl, b);
        CHECK(mesh_radius > 0.0f, "imp ladder: mesh-only reference bakes");
        CHECK(r.impostors == 0, "imp ladder: no terminal declared -> no impostor");
        CHECK(!fs::exists(fimp) ||
                  !part_bundle::has_section(fimp, kImpostorLadderHash,
                                            part_bundle::kSectionImpostor),
              "imp ladder: an undeclared impostor is never implied");
    }

    // (b) The same ladder with the terminal declared at 140 m.
    const double kImpAt = 140.0;
    part_flatten::FlattenResult res;
    std::vector<part_asset::FlatCluster> clusters;
    BLASManager blas;
    const float radius = bake({0.0, 18.0, kImpAt},
                              {"", "decimate {\"divisor\":8}", "impostor {}"},
                              res, clusters, blas);
    CHECK(radius > 0.0f, "imp ladder: flatten ok");
    if (!(radius > 0.0f)) {
        printf("  error: %s\n", res.error.c_str());
        part_bundle::remove_section(slods, kImpostorLadderHash, part_bundle::kSectionPlan);
        return;
    }
    CHECK(res.levels == 3, "imp ladder: the impostor is a rep of the same table");
    CHECK(res.impostors == 1, "imp ladder: one billboard baked");

    // THE GUARD. Same reps, same geometry, same radius — the billboard is a
    // picture of the ladder, not a member of its bounds. Delete the
    // `if (!is_impostor) acc(tris)` in part_flatten and this fails by ~55%.
    CHECK(std::fabs(radius - mesh_radius) < 1e-5f * mesh_radius,
          "imp ladder: the billboard does not enter the cluster AABB");

    const auto& lods = clusters[0].lods;
    const auto& entries = blas.get_entries();
    if (lods.size() != 3) {
        printf("  SKIPPING (unexpected artifact shape)\n");
        part_bundle::remove_section(slods, kImpostorLadderHash, part_bundle::kSectionPlan);
        return;
    }
    // The terminal rung IS the billboard, by the one predicate every consumer
    // asks (impostor::is_billboard_rung), not by triangle count alone.
    const BLASManager::BLASEntry* term = entries[lods[2].blas_indices[0]].get();
    CHECK(term && impostor::is_billboard_rung(term->triangles, term->tri_extra),
          "imp ladder: rung 2 is the billboard");
    CHECK(entries[lods[1].blas_indices[0]]->triangles.size() > 2,
          "imp ladder: rung 1 is still mesh");

    // The atlas is on disk, and it answers to the depicts-hash PartStore
    // recomputes from the rung the billboard takes over from. This is the
    // whole staleness contract: an atlas that does not match the mesh it
    // depicts is rejected rather than drawn.
    {
        // REP 0, not the rung the billboard takes over from. The bake sources
        // rep 0 now (a 16x16 cell resolves silhouette and shading, and the
        // coarsest rung has already discarded both), and PartStore folds the
        // same rung -- this must track them or it asserts a contract nothing
        // implements.
        uint64_t depicts = impostor::depicts_hash_begin();
        impostor::depicts_hash_add_cluster(
            depicts, 0u, entries[lods[0].blas_indices[0]]->triangles,
            entries[lods[0].blas_indices[0]]->tri_extra);
        impostor::PartImpostor loaded;
        impostor::LoadFailure fail = impostor::LoadFailure::None;
        std::string reason;
        const bool ok = impostor::load(fimp, kImpostorLadderHash,
                                       impostor::depicts_hash_finish(depicts),
                                       loaded, &fail, &reason);
        CHECK(ok && loaded.clusters.size() == 1,
              "imp ladder: the atlas loads against the rung it depicts");
        if (!ok) printf("  impostor load: %s (%s)\n",
                        impostor::load_failure_text(fail), reason.c_str());
    }

    // The authored metres land where they were authored. Rung 1's threshold is
    // rung 2's switch-IN, so this IS the impostor distance.
    const float want_imp = radius / (float)kImpAt;
    CHECK(std::fabs(lods[1].screen_size_threshold - want_imp) < 1e-4f * want_imp,
          "imp ladder: rung 1's threshold is radius / at(impostor)");
    CHECK(lods[2].screen_size_threshold == 0.0f,
          "imp ladder: the billboard never hides");

    float sw[3];
    for (int i = 0; i < 3; ++i)
        sw[i] = lod::normalized_switch_distance(lods[i].screen_size_threshold);
    const float reach = lod::reach(radius, 1.0f, 1.0f);
    CHECK(lod::select_rep(sw, 3, 139.0f, reach) == 1, "imp ladder: 139 m -> mesh");
    CHECK(lod::select_rep(sw, 3, 141.0f, reach) == 2, "imp ladder: 141 m -> billboard");
    CHECK(lod::select_rep(sw, 3, 9000.f, reach) == 2, "imp ladder: far away -> billboard");

    // MATTER_IMPOSTOR=0 drops the declared terminal rather than failing the
    // bake: the ladder ends at the last mesh rung, exactly as it would have
    // had the author declared no terminal at all.
    {
#ifdef _WIN32
        _putenv_s("MATTER_IMPOSTOR", "0");
#else
        setenv("MATTER_IMPOSTOR", "0", 1);
#endif
        part_flatten::FlattenResult r;
        std::vector<part_asset::FlatCluster> cl;
        BLASManager b;
        const float r_off = bake({0.0, 18.0, kImpAt},
                                 {"", "decimate {\"divisor\":8}", "impostor {}"}, r, cl, b);
        CHECK(r_off > 0.0f && r.ok, "imp ladder: MATTER_IMPOSTOR=0 still bakes");
        CHECK(r.levels == 2 && r.impostors == 0,
              "imp ladder: MATTER_IMPOSTOR=0 ends the ladder at the mesh rung");
        CHECK(cl.size() == 1 && cl[0].lods.size() == 2 &&
                  cl[0].lods[1].screen_size_threshold == 0.0f,
              "imp ladder: the surviving mesh rung holds at any distance");
#ifdef _WIN32
        _putenv_s("MATTER_IMPOSTOR", "");
#else
        unsetenv("MATTER_IMPOSTOR");
#endif
    }

    // Determinism: two cold bakes of the atlas-bearing ladder are identical.
    {
        part_flatten::FlattenResult r;
        std::vector<part_asset::FlatCluster> cl;
        BLASManager b;
        CHECK(bake({0.0, 18.0, kImpAt},
                   {"", "decimate {\"divisor\":8}", "impostor {}"}, r, cl, b) > 0.0f,
              "imp ladder: re-bake for the determinism gate");
        std::vector<char> first = read_all_bytes(flat);
        // The ATLAS specifically, not the bundle: `fimp` and `flat` are the
        // same file (both parts/<hash>.bundle), so a whole-file compare here
        // would just repeat the ladder check above under a different name.
        std::vector<uint8_t> first_atlas;
        CHECK(part_bundle::read_section(fimp, kImpostorLadderHash,
                                        part_bundle::kSectionImpostor, first_atlas),
              "imp ladder: the atlas section reads back");
        part_flatten::FlattenResult r2;
        std::vector<part_asset::FlatCluster> cl2;
        BLASManager b2;
        CHECK(bake({0.0, 18.0, kImpAt},
                   {"", "decimate {\"divisor\":8}", "impostor {}"}, r2, cl2, b2) > 0.0f,
              "imp ladder: second cold bake ok");
        CHECK(!first.empty() && first == read_all_bytes(flat),
              "imp ladder: two cold bakes are byte-identical");
        std::vector<uint8_t> second_atlas;
        CHECK(part_bundle::read_section(fimp, kImpostorLadderHash,
                                        part_bundle::kSectionImpostor, second_atlas),
              "imp ladder: the atlas section reads back after the second bake");
        CHECK(!first_atlas.empty() && first_atlas == second_atlas,
              "imp ladder: the atlas is byte-identical too");
    }

    // A plan longer than the serialized cap would TRUNCATE, and the terminal
    // is by definition the last entry — so the impostor would vanish and the
    // result would look like a deliberate mesh-only ladder. bake_static_lods
    // refuses to write such a plan, but a plan is a file. Prove the guard
    // fires rather than trusting that it would.
    {
        const size_t over = matter::kMaxSerializedLodLevels + 1;
        std::vector<double> at(over, -1.0);
        std::vector<std::string> gen(over);
        at[0] = 0.0;
        for (size_t i = 1; i + 1 < over; ++i) {
            at[i] = 10.0 * (double)i;
            gen[i] = "decimate {\"divisor\":8}";
        }
        at[over - 1] = 500.0;
        gen[over - 1] = "impostor {}";
        part_flatten::FlattenResult r;
        std::vector<part_asset::FlatCluster> cl;
        BLASManager b;
        const float got = bake(at, gen, r, cl, b);
        CHECK(got < 0.0f && !r.ok,
              "imp ladder: an over-cap plan whose terminal falls off the end fails");
        CHECK(r.error.find("impostor terminal falls outside") != std::string::npos,
              "imp ladder: ...and says so, rather than baking a mesh-only ladder");
        if (r.ok) printf("  UNEXPECTED: over-cap plan baked %zu levels\n", r.levels);
    }

    printf("  radius=%.4f (mesh-only %.4f) thr=%.5f/%.5f/%.1f\n", radius, mesh_radius,
           lods[0].screen_size_threshold, lods[1].screen_size_threshold,
           lods[2].screen_size_threshold);
    printf("  test_authored_ladder_impostor_terminal OK\n");

    part_bundle::remove_section(slods, kImpostorLadderHash, part_bundle::kSectionPlan);
}

// kRepresentation 1->2: the per-part `static noImpostor` opt-out. This is the
// DEFAULT-ladder case the flag exists for (LightingGarden's iso objects author
// no `static lods`, so the only thing that gives them a billboard is the global
// impostor_terminal). The flag reaches flatten as StaticLodPlan::no_impostor —
// bake_static_lods writes a LEVELLESS plan carrying only the flag for a part
// that opts out without authoring a ladder — so this test writes exactly that
// plan directly, the same way the sibling impostor test writes its plan, and
// asserts the terminal billboard is present without it and gone with it.
static const uint64_t kNoImpostorHash = 0xA110000044440004ull;

static void test_no_impostor_optout_terminal() {
    printf("=== test_no_impostor_optout_terminal ===\n");

    // Phase (a) below needs impostors globally ON to have a billboard to drop.
    // A diagnostic run with MATTER_IMPOSTOR=0 can't establish that, so skip
    // rather than report a red that says nothing about this code (mirrors
    // test_authored_ladder_impostor_terminal).
    {
        const char* v = std::getenv("MATTER_IMPOSTOR");
        if (v && v[0] == '0') { printf("  SKIPPED (MATTER_IMPOSTOR=0)\n"); return; }
    }

    // A plain part with no authored ladder: enough triangles that its coarsest
    // default rung clears kMinTerminalTris (16) and earns a billboard.
    std::vector<Tri> tris = sphere_tris(20, 10);
    CHECK(save_fixture(kNoImpostorHash, 5, {tris}, {}), "no-imp: fixture written");

    const std::string flat =
        std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(kNoImpostorHash);
    const std::string slods =
        std::string(kCacheRoot) + "/" + part_asset::cache_path_static_lods(kNoImpostorHash);
    const std::string fimp =
        std::string(kCacheRoot) + "/" + impostor::cache_path_impostor(kNoImpostorHash);

    // Cold start: kCacheRoot survives between runs, so a prior run's artifacts
    // would let phase (a) pass without re-baking. Clear all three sections.
    auto reset_cold = [&]() {
        part_bundle::remove_section(flat,  kNoImpostorHash, part_bundle::kSectionFlat);
        part_bundle::remove_section(slods, kNoImpostorHash, part_bundle::kSectionPlan);
        part_bundle::remove_section(fimp,  kNoImpostorHash, part_bundle::kSectionImpostor);
    };

    // (a) UNFLAGGED — no plan at all. The default ladder appends its terminal
    //     billboard, and the atlas lands on disk.
    reset_cold();
    part_flatten::FlattenResult a = part_flatten::flatten_part(kCacheRoot, kNoImpostorHash);
    CHECK(a.ok, "no-imp: unflagged flatten ok");
    if (!a.ok) { printf("  error: %s\n", a.error.c_str()); return; }
    CHECK(a.impostors == 1, "no-imp: unflagged default ladder grows a billboard");
    CHECK(part_bundle::has_section(fimp, kNoImpostorHash, part_bundle::kSectionImpostor),
          "no-imp: unflagged bake writes an impostor atlas");
    const size_t unflagged_levels = a.levels;

    // (b) FLAGGED — a LEVELLESS plan carrying only no_impostor, exactly what
    //     bake_static_lods writes for `static noImpostor = true` on a part with
    //     no `static lods`. Same geometry, same hash; only the flag differs.
    reset_cold();
    {
        part_asset::StaticLodPlan plan;
        plan.no_impostor = true;                 // no levels — just the opt-out
        CHECK(part_asset::save_static_lod_plan(slods, kNoImpostorHash, plan),
              "no-imp: levelless opt-out plan written");
        part_asset::StaticLodPlan rt;
        CHECK(part_asset::load_static_lod_plan(slods, kNoImpostorHash, rt),
              "no-imp: a levelless plan is a VALID plan (round-trips)");
        CHECK(rt.no_impostor && rt.level_hashes.empty() && !rt.drives_ladder(),
              "no-imp: it carries the flag, no levels, and does not drive a ladder");
    }
    part_flatten::FlattenResult b = part_flatten::flatten_part(kCacheRoot, kNoImpostorHash);
    CHECK(b.ok, "no-imp: flagged flatten ok");
    if (!b.ok) { printf("  error: %s\n", b.error.c_str()); return; }

    // THE GUARD. Drop `&& !slod_plan.no_impostor` in part_flatten's impostors_on
    // and this becomes 1 — the billboard reappears on the flagged part.
    CHECK(b.impostors == 0, "no-imp: flagged part bakes NO billboard");
    CHECK(!part_bundle::has_section(fimp, kNoImpostorHash, part_bundle::kSectionImpostor),
          "no-imp: flagged part writes NO impostor atlas");
    // The billboard was one extra rung; without it the ladder is one shorter and
    // terminates at the coarsest MESH rung.
    CHECK(b.levels == unflagged_levels - 1,
          "no-imp: the ladder ends one rung earlier — at the coarsest mesh rung");

    printf("  unflagged: levels=%zu impostors=%zu | flagged: levels=%zu impostors=%zu\n",
           a.levels, a.impostors, b.levels, b.impostors);
    printf("  test_no_impostor_optout_terminal OK\n");
    part_bundle::remove_section(slods, kNoImpostorHash, part_bundle::kSectionPlan);
}

// M3 companion: a `static lods` block that names NEITHER a distance nor a
// generator — the pre-M3 W5 surface, params/exclude only — must not touch the
// ladder at all. Same fixture, same flatten, byte-identical artifact with and
// without the sidecar present.
static void test_w5_plan_does_not_drive_the_ladder() {
    printf("=== test_w5_plan_does_not_drive_the_ladder ===\n");

    const uint64_t h = 0xA110000022220002ull;
    std::vector<Tri> tris = sphere_tris(20, 10);
    CHECK(save_fixture(h, 5, {tris}, {}), "w5 plan: fixture written");

    const std::string flat = std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(h);
    const std::string slods =
        std::string(kCacheRoot) + "/" + part_asset::cache_path_static_lods(h);

    part_bundle::remove_section(slods, h, part_bundle::kSectionPlan);
    part_bundle::remove_section(flat, h, part_bundle::kSectionFlat);
    CHECK(part_flatten::flatten_part(kCacheRoot, h).ok, "w5 plan: baseline flatten ok");
    // M4: compare the FLAT SECTION, not the file. The with-sidecar run also
    // adds a PLAN section to the same bundle, so whole-file bytes would differ
    // for a reason that has nothing to do with the ladder this test is about.
    std::vector<uint8_t> baseline;
    CHECK(part_bundle::read_section(flat, h, part_bundle::kSectionFlat, baseline),
          "w5 plan: baseline flat section reads back");

    {
        part_asset::StaticLodPlan plan;             // params/exclude only
        plan.level_hashes = { h, h };
        plan.level_exclude_masks = { 0u, 2u };
        plan.level_at  = { -1.0, -1.0 };
        plan.level_gen = { "", "" };
        CHECK(part_asset::save_static_lod_plan(slods, h, plan), "w5 plan: sidecar written");
        part_asset::StaticLodPlan rt;
        CHECK(part_asset::load_static_lod_plan(slods, h, rt) && !rt.drives_ladder(),
              "w5 plan: does NOT drive the ladder");
    }

    part_bundle::remove_section(flat, h, part_bundle::kSectionFlat);
    CHECK(part_flatten::flatten_part(kCacheRoot, h).ok, "w5 plan: flatten with sidecar ok");
    std::vector<uint8_t> with_plan;
    CHECK(part_bundle::read_section(flat, h, part_bundle::kSectionFlat, with_plan),
          "w5 plan: with-sidecar flat section reads back");
    CHECK(!baseline.empty() && baseline == with_plan,
          "w5 plan: artifact byte-identical to the no-sidecar bake");

    printf("  test_w5_plan_does_not_drive_the_ladder OK\n");
    part_bundle::remove_section(slods, h, part_bundle::kSectionPlan);
}

// Bake-hardening #2: a parent with N instances of a heavy child should trip
// the flatten budget, land the parent on the BOUNDARY path, and emit
// instance_refs instead of inlining the child's mesh. Fixture: 10 instances
// of a ~2000-tri dense sphere ≈ 20000 * 96 B = 1.9 MB; set the parent budget
// to 100 KB so the estimate blows past it and we know the boundary tripped.
static void test_instance_boundary_records_refs() {
    printf("=== test_instance_boundary_records_refs ===\n");
    const uint64_t kParentBHash = 0x3333000033330000ull;   // fresh hash so fixtures don't collide
    const uint64_t kDenseHash   = kDenseSphereHash;        // ~21600 tris

    // Ensure the dense sphere fixture (Task 7) is on disk.
    uint64_t dh = write_dense_sphere_part();
    CHECK(dh == kDenseHash, "dense sphere fixture written");

    // Parent with 10 placements of the dense sphere; no own geometry.
    std::vector<part_asset::ChildInstance> kids(10);
    for (int i = 0; i < 10; ++i) {
        kids[i].child_resolved_hash = kDenseHash;
        set_translate(kids[i].transform, (float)(i * 5), 0.0f, 0.0f);
    }
    // Parent has no own geometry: pass an empty LOD-0 tri set.
    std::vector<Tri> empty_lod0;
    (void)empty_lod0;
    bool ok = save_fixture(kParentBHash, 3, {std::vector<Tri>{}}, kids);
    // save_fixture requires at least one non-empty triangle set for BLAS
    // registration to succeed — so give it a single degenerate placeholder tri
    // (that's still fine: the flatten pass reads it and skips over an
    // effectively-zero-area triangle from the merge).
    if (!ok) {
        std::vector<Tri> placeholder;
        placeholder.push_back(make_tri(make_float3(0,0,0), make_float3(0,0,0), make_float3(0,0,0)));
        ok = save_fixture(kParentBHash, 3, {placeholder}, kids);
    }
    CHECK(ok, "parent-B fixture written (10x dense-sphere placements)");

    // Tight budget: 100 KB. 10 * 21600 * 96 = 20 MB — trips easily.
    part_flatten::FlattenTargets targets;
    targets.budget_tri_bytes = 100 * 1024ull;

    const std::string flat = std::string(kCacheRoot) + "/" +
                             part_asset::cache_path_flat(kParentBHash);
    part_bundle::remove_section(flat, kParentBHash, part_bundle::kSectionFlat);
    part_flatten::FlattenResult res =
        part_flatten::flatten_part(kCacheRoot, kParentBHash, targets);
    CHECK(res.ok, "flatten with tight budget succeeds");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); return; }
    CHECK(res.instance_refs == 10, "instance_refs count matches child placements");

    // Reload and verify the trailer.
    BLASManager blas; TLASManager tlas(16);
    std::vector<part_asset::FlatCluster> clusters;
    std::vector<part_asset::FlatInstanceRef> refs;
    bool loaded = part_asset::load_flat_v3(flat, kParentBHash, blas, tlas,
                                           clusters, refs);
    CHECK(loaded, "load_flat_v3 (v5) recovers instance_refs");
    CHECK(refs.size() == 10, "reloaded instance_refs matches source");
    // Every ref points at the dense sphere with the placement transform we set.
    bool refs_ok = true;
    for (size_t i = 0; i < refs.size() && refs_ok; ++i) {
        if (refs[i].child_resolved_hash != kDenseHash) refs_ok = false;
        // translation lives at [3]/[7]/[11] in row-major convention.
        if (std::fabs(refs[i].transform[3] - (float)(i * 5)) > 1e-5f) refs_ok = false;
    }
    CHECK(refs_ok, "each instance_ref carries correct hash + placement translation");

    // The flat.part should contain NO inlined dense-sphere triangles (they'd
    // put the merged mesh well over the placeholder tri).
    size_t inlined_tris = 0;
    for (const auto& cl : clusters)
        for (uint32_t bi : cl.lods.empty() ? std::vector<uint32_t>{} : cl.lods[0].blas_indices)
            if (bi < blas.get_entries().size())
                inlined_tris += blas.get_entries()[bi]->triangles.size();
    // The placeholder degenerate tri may live in a cluster; but the 10*21600
    // sphere tris must not appear. Tolerate a small placeholder count.
    CHECK(inlined_tris < 100, "no dense-sphere geometry inlined into flat.part");

    printf("  instance_refs=%zu inlined_tris=%zu clusters=%zu\n",
           refs.size(), inlined_tris, clusters.size());
    printf("  test_instance_boundary_records_refs OK\n");

    // Clean up the parent-B fixture / flat.
    part_bundle::remove_section(flat, kParentBHash, part_bundle::kSectionFlat);
}

// Bake-hardening #2: when the budget is generous, the same fixture should
// take the INLINE path — inline the dense-sphere mesh into the flat.part
// with zero instance_refs.
static void test_generous_budget_inlines() {
    printf("=== test_generous_budget_inlines ===\n");
    const uint64_t kParentCHash = 0x4444000044440000ull;
    const uint64_t kDenseHash   = kDenseSphereHash;

    // Reuse the dense sphere; add a smaller (4 placement) parent so the merged
    // mesh is still bounded.
    std::vector<part_asset::ChildInstance> kids(4);
    for (int i = 0; i < 4; ++i) {
        kids[i].child_resolved_hash = kDenseHash;
        set_translate(kids[i].transform, (float)(i * 5), 0.0f, 0.0f);
    }
    std::vector<Tri> placeholder;
    placeholder.push_back(make_tri(make_float3(0,0,0), make_float3(0,0,0), make_float3(0,0,0)));
    CHECK(save_fixture(kParentCHash, 3, {placeholder}, kids),
          "parent-C fixture written (4x dense-sphere placements)");

    // Generous budget: 512 MB default — 4 * 21600 * 96 = 8.3 MB, well under.
    part_flatten::FlattenTargets targets;   // default budget
    const std::string flat = std::string(kCacheRoot) + "/" +
                             part_asset::cache_path_flat(kParentCHash);
    part_bundle::remove_section(flat, kParentCHash, part_bundle::kSectionFlat);
    part_flatten::FlattenResult res =
        part_flatten::flatten_part(kCacheRoot, kParentCHash, targets);
    CHECK(res.ok, "flatten with generous budget succeeds");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); return; }
    CHECK(res.instance_refs == 0, "no instance_refs (all children inlined)");

    BLASManager blas; TLASManager tlas(16);
    std::vector<part_asset::FlatCluster> clusters;
    std::vector<part_asset::FlatInstanceRef> refs;
    CHECK(part_asset::load_flat_v3(flat, kParentCHash, blas, tlas, clusters, refs),
          "reload succeeds under generous budget");
    CHECK(refs.empty(), "reloaded refs empty in INLINE path");
    // Sanity: some dense-sphere geometry did get inlined.
    size_t total_tris = 0;
    for (const auto& e : blas.get_entries()) total_tris += e->triangles.size();
    CHECK(total_tris > 4 * 20000u, "dense-sphere geometry (4x ~21600) is present in flat.part");
    printf("  total_tris=%zu clusters=%zu\n", total_tris, clusters.size());
    printf("  test_generous_budget_inlines OK\n");

    part_bundle::remove_section(flat, kParentCHash, part_bundle::kSectionFlat);
}

static void test_cutover_helpers() {
    part_flatten::FlattenTargets t;  // defaults: pb=1.0, pa=1.047/720, div={512,256,128,64,32,16,8,4,2}
    // Computed pb*pa*div[i] thresholds (i=0..8):
    //   0.744533, 0.372267, 0.186133, 0.093067, 0.046533, 0.023267, 0.011633, 0.005817, 0.002908
    // (pa = 1.047/720 = 0.001454167; pb = 1.0)

    // cutover_level_index: smallest i whose threshold is <= cutover
    // 1.0 >= thr[0]=0.7445 => index 0
    CHECK(part_flatten::cutover_level_index(1.0f, t) == 0, "cutover=1.0 maps to index 0");
    // 0.5 >= thr[1]=0.3723 but < thr[0]=0.7445 => index 1
    CHECK(part_flatten::cutover_level_index(0.5f, t) == 1, "cutover=0.5 maps to index 1");
    // 0.1 >= thr[3]=0.0931 but < thr[2]=0.1861 => index 3
    CHECK(part_flatten::cutover_level_index(0.1f, t) == 3, "cutover=0.1 maps to index 3");
    // 0.001 < thr[8]=0.0029 => none qualify => div.size()=9
    CHECK(part_flatten::cutover_level_index(0.001f, t) == (int)t.radius_divisor.size(),
          "cutover=0.001 maps to div.size() (coarsest)");
    // 0.0 never >= any positive threshold => div.size()=9 (coarsest)
    CHECK(part_flatten::cutover_level_index(0.0f, t) == (int)t.radius_divisor.size(),
          "cutover=0.0 maps to div.size() (coarsest)");

    // ref_cutover_threshold: px*pa*pb*parent_r / (child_r * scale)
    // = 64 * 0.00145417 * 1.0 * 10.7 / (1.732 * 1.0) = 0.57495 ≈ 0.575
    float c = part_flatten::ref_cutover_threshold(64.0f, 10.7f, 1.732f, 1.0f, t);
    CHECK(std::fabs(c - 0.575f) < 0.001f, "ref_cutover_threshold(64px,10.7,1.732,1) ≈ 0.575");
    // zero guards: degenerate inputs return 0
    CHECK(part_flatten::ref_cutover_threshold(64.0f, 10.7f, 0.0f, 1.0f, t) == 0.0f,
          "ref_cutover_threshold: zero child_radius returns 0");
    CHECK(part_flatten::ref_cutover_threshold(64.0f, 0.0f, 1.0f, 1.0f, t) == 0.0f,
          "ref_cutover_threshold: zero parent_radius returns 0");

    // transform_uniform_scale: column-0 length of row-major 4x4
    float m[16] = {0.35f,0,0,0,  0,0.35f,0,0,  0,0,0.35f,0,  0,0,0,1};
    CHECK(std::fabs(part_flatten::transform_uniform_scale(m) - 0.35f) < 1e-6f,
          "transform_uniform_scale(0.35-scale matrix) == 0.35");

    printf("  test_cutover_helpers OK\n");
}

static void test_flat_version_bump() {
    uint64_t hash = write_small_sphere_part();   // Task 7 fixture
    auto res = part_flatten::flatten_part(kCacheRoot, hash);
    assert(res.ok);
    std::string p = std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(hash);

    // New flats carry the bumped version.
    assert(part_asset::peek_format_version(p) == part_asset::kFormatVersionFlat);
    // M2.5 bumped 8 -> 9: the ladder a flat carries changed (M1.5's benefit
    // floor removed rungs, and the bottom-out point gained a billboard rung),
    // so a v8 flat is not merely older bytes -- it is a different ladder whose
    // impostor sidecar does not exist.
    assert(part_asset::kFormatVersionFlat == 9u);

    // Patch the version field back to 3 (a pre-retune bake): loader must reject.
    // Header layout: magic (u32) then format_version (u32) — verify the write
    // offset against write_file_atomic in part_asset_v2.cpp before relying on it.
    {
        // M4: the version field lives at offset 4 of the SECTION, not of the
        // file -- offset 4 of a bundle is the bundle's own format version, and
        // poking that would test the container instead of the artifact.
        std::vector<uint8_t> section;
        assert(part_bundle::read_section(p, hash, part_bundle::kSectionFlat, section));
        const uint32_t old = 3u;
        std::memcpy(section.data() + 4, &old, sizeof old);
        assert(part_bundle::write_section(p, hash, part_bundle::kSectionFlat,
                                          section.data(), section.size()));
    }
    // peek reports "a flat section is present" -- it does not re-read the
    // artifact's own version, which is the loader's gate and is checked next.
    assert(part_asset::peek_format_version(p) == part_asset::kFormatVersionFlat);
    BLASManager b2; TLASManager t2(4);
    std::vector<part_asset::FlatCluster> cl2;
    assert(!part_asset::load_flat_v3(p, hash, b2, t2, cl2));
    printf("  test_flat_version_bump OK\n");
}

// ------------------------------------------------------ Task 6: segmented --

// A dedicated parent hash for the segmented-flatten fixtures. Kept distinct
// from kParentHash so the existing unhinted merge/determinism tests (which run
// first in main() and expect NO hints sidecar) are never perturbed by the
// hints file this fixture writes.
static const uint64_t kSegParentHash = 0x5555000055550000ull;
// Second parent hash: SAME geometry as kSegParentHash but flattened WITHOUT a
// hints sidecar, so the guard test can prove the unhinted path is unchanged.
static const uint64_t kSegParentNoHintHash = 0x6666000066660000ull;

// Dedicated seg child: a small multi-LOD sphere (segs=20,rings=10 => 360 tris
// full-res, with a genuinely coarser LOD1). Distinct hash so it re-flattens on
// its own into a real ladder — the coarse segment of the parent then sources
// from the child's DECIMATED coarse levels, provably fewer than full-res.
static const uint64_t kSegChildHash = 0x7777000077770007ull;

// child_full_res_tris = seg child LOD0 tri count (360). Used by the coarse-L0
// range assertion below (coarse-sourced geometry is strictly less than this).
static const size_t kChildFullResTris = 360;

static bool write_seg_child() {
    std::vector<Tri> full = sphere_tris(20, 10);          // ~360 tris
    std::vector<Tri> coarse = sphere_tris(8, 4);          // ~48 tris (real LOD1)
    return save_fixture(kSegChildHash, 7, {full, coarse}, {});
}

// Write a parent fixture: 2-tri quad trunk (material 3) with two child
// instances (kSegChildHash, a multi-LOD sphere) at translate(+10,0,0)/(-10,0,0).
static bool write_seg_parent(uint64_t parent_hash) {
    std::vector<Tri> quad = quad_tris();
    std::vector<part_asset::ChildInstance> children(2);
    children[0].child_resolved_hash = kSegChildHash;
    set_translate(children[0].transform, 10, 0, 0);
    children[1].child_resolved_hash = kSegChildHash;
    set_translate(children[1].transform, -10, 0, 0);
    return save_fixture(parent_hash, 3, {quad}, children);
}

static void test_flatten_segmented() {
    printf("=== test_flatten_segmented ===\n");
    CHECK(write_seg_child(), "seg child fixture written");
    CHECK(write_seg_parent(kSegParentHash), "seg parent fixture written");

    // Hints sidecar: inline both children below 8 px.
    //
    // This was 64 px, and M1.5's ladder BENEFIT RULE invalidated that number
    // (2026-08-05). The seg child's ladder now reads
    //   512:360- 256:360- 128:332- 64:312- 32:280- 16:156+ 8:90+ 4:54+ 2:28+
    // — every rung down to divisor 32 REJECTED, because decimating 360 tris to
    // 332 / 312 / 280 never buys the 30% the benefit rule requires. Those
    // near-duplicate rungs used to exist (rungs were admitted on any reduction
    // at all), and a 64 px hint landed on one of them.
    //
    // With them gone the child's first real switch is a 16 px-equivalent, so
    // at 64 px the child correctly still wants LOD 0 — `src = min(C,E)` picks
    // 0 and the coarse segment inlines full-res, which made `coarse L0 <
    // trunk + 2 * child full-res` fail by exactly zero (722 vs 722). The
    // mechanism under test was never broken; the fixture stopped exercising
    // it. 8 px lands inside the child's ADMITTED ladder (the 156-tri rung),
    // which is what this test exists to prove gets sourced.
    part_asset::FlattenHints h;
    h.child_px[0] = 8.0f;
    h.child_px[1] = 8.0f;
    CHECK(part_asset::save_flatten_hints(
              std::string(kCacheRoot) + "/" + part_asset::cache_path_hints(kSegParentHash),
              kSegParentHash, h),
          "seg hints sidecar written");

    const std::string flat = std::string(kCacheRoot) + "/" +
                             part_asset::cache_path_flat(kSegParentHash);
    part_bundle::remove_section(flat, kSegParentHash, part_bundle::kSectionFlat);

    part_flatten::FlattenTargets t;
    auto res = part_flatten::flatten_part(kCacheRoot, kSegParentHash, t);
    CHECK(res.ok, "seg flatten_part ok");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); return; }

    BLASManager blas; TLASManager tlas(16);
    std::vector<part_asset::FlatCluster> cl;
    std::vector<part_asset::FlatInstanceRef> refs;
    bool loaded = part_asset::load_flat_v3(flat, kSegParentHash, blas, tlas, cl, refs);
    CHECK(loaded, "seg flat loads as v6");
    if (!loaded) return;

    bool has_fine = false, has_coarse = false;
    for (auto& c : cl) { if (c.segment == 0) has_fine = true; else has_coarse = true; }
    CHECK(has_fine && has_coarse, "artifact has both fine and coarse clusters");

    // Fine segment level 0 = trunk only = exactly the 2 quad tris.
    // Coarse segment level 0 = trunk + child coarse LODs.
    auto tri_of = [&](uint32_t bi) -> size_t {
        return (bi < blas.get_entries().size())
                   ? blas.get_entries()[bi]->triangles.size() : 0;
    };
    size_t fine_l0 = 0, coarse_l0 = 0;
    for (auto& c : cl) {
        if (c.lods.empty()) continue;
        size_t l0 = 0;
        for (uint32_t bi : c.lods[0].blas_indices) l0 += tri_of(bi);
        if (c.segment == 0) fine_l0   += l0;
        else                coarse_l0 += l0;
    }
    CHECK(fine_l0 == 2, "fine L0 == 2 (trunk-only)");
    // Coarse L0 = trunk + child coarse LODs: strictly more than the bare trunk,
    // and strictly less than trunk + 2 * child FULL-res (proves coarse source,
    // not full-res gather).
    CHECK(coarse_l0 > 2, "coarse L0 > 2 (trunk + child coarse)");
    CHECK(coarse_l0 < 2 + 2 * kChildFullResTris,
          "coarse L0 < trunk + 2 * child full-res (coarse-sourced, not full gather)");

    // Two refs, equal positive cutover (unified max).
    CHECK(refs.size() == 2, "two hinted instance refs written");
    if (refs.size() == 2) {
        CHECK(refs[0].inline_cutover > 0.0f, "ref cutover positive");
        CHECK(refs[0].inline_cutover == refs[1].inline_cutover, "refs share unified cutover");
    }

    // fine/coarse counters populated.
    CHECK(res.fine_tris == 2, "res.fine_tris == 2 (trunk)");
    CHECK(res.coarse_input_tris == res.full_tris,
          "res.coarse_input_tris mirrors res.full_tris (merged coarse input)");
    CHECK(res.coarse_input_tris > 2, "coarse input tris > trunk");

    printf("  fine_l0=%zu coarse_l0=%zu coarse_input=%zu cutover=%.5f\n",
           fine_l0, coarse_l0, res.coarse_input_tris,
           refs.empty() ? 0.0f : refs[0].inline_cutover);
    printf("  test_flatten_segmented OK\n");
}

// Guard 1: the SAME geometry flattened WITHOUT a hints sidecar must produce an
// artifact with ZERO segment-1 clusters and all refs cutover == 0 (byte-for-byte
// the classic streaming path). Guard 2: flattening WITH hints twice is
// byte-identical (segmented path is deterministic).
static void test_flatten_unhinted_unchanged() {
    printf("=== test_flatten_unhinted_unchanged ===\n");

    // --- Guard 1: no hints sidecar => no segment-1 clusters, no cutover refs.
    CHECK(write_seg_child(), "seg child fixture written (guard)");
    CHECK(write_seg_parent(kSegParentNoHintHash), "no-hint parent fixture written");
    // Make sure no stray hints file exists for this hash.
    part_bundle::remove_section(std::string(kCacheRoot) + "/" +
                                    part_asset::cache_path_hints(kSegParentNoHintHash),
                                kSegParentNoHintHash, part_bundle::kSectionHints);
    const std::string flat_nh = std::string(kCacheRoot) + "/" +
                                part_asset::cache_path_flat(kSegParentNoHintHash);
    part_bundle::remove_section(flat_nh, kSegParentNoHintHash, part_bundle::kSectionFlat);
    auto res_nh = part_flatten::flatten_part(kCacheRoot, kSegParentNoHintHash);
    CHECK(res_nh.ok, "no-hint flatten ok");
    if (res_nh.ok) {
        BLASManager b; TLASManager tl(16);
        std::vector<part_asset::FlatCluster> cl;
        std::vector<part_asset::FlatInstanceRef> refs;
        CHECK(part_asset::load_flat_v3(flat_nh, kSegParentNoHintHash, b, tl, cl, refs),
              "no-hint flat loads");
        bool any_coarse = false;
        for (auto& c : cl) if (c.segment != 0) any_coarse = true;
        CHECK(!any_coarse, "no-hint artifact has ZERO segment-1 clusters");
        bool all_zero_cut = true;
        for (auto& r : refs) if (r.inline_cutover != 0.0f) all_zero_cut = false;
        CHECK(all_zero_cut, "no-hint refs all have inline_cutover == 0");
        CHECK(res_nh.fine_tris == 0 && res_nh.coarse_input_tris == 0,
              "no-hint path leaves fine/coarse counters at 0");
    }

    // --- Guard 2: hinted flatten is deterministic (byte-identical re-runs).
    const std::string flat_h = std::string(kCacheRoot) + "/" +
                               part_asset::cache_path_flat(kSegParentHash);
    // (Fixture + hints sidecar were written by test_flatten_segmented, which
    // runs immediately before this in main(); re-assert they exist.)
    part_bundle::remove_section(flat_h, kSegParentHash, part_bundle::kSectionFlat);
    auto a = part_flatten::flatten_part(kCacheRoot, kSegParentHash);
    std::vector<char> bytes_a;
    CHECK(a.ok && read_bytes(flat_h, bytes_a), "hinted flatten #1 written");
    part_bundle::remove_section(flat_h, kSegParentHash, part_bundle::kSectionFlat);
    auto b2 = part_flatten::flatten_part(kCacheRoot, kSegParentHash);
    std::vector<char> bytes_b;
    CHECK(b2.ok && read_bytes(flat_h, bytes_b), "hinted flatten #2 written");
    CHECK(bytes_a == bytes_b, "hinted re-flatten is byte-identical (deterministic)");

    printf("  test_flatten_unhinted_unchanged OK\n");
}

// Portable setenv shim (pre-existing test used bare POSIX setenv/unsetenv,
// which MinGW lacks — same pattern as script_host_tests.cpp's profile-env
// helper). value == nullptr unsets. Behavior on POSIX is unchanged.
static void pf_set_env(const char* name, const char* value) {
#ifdef _WIN32
    std::string kv = std::string(name) + "=" + (value ? value : "");
    _putenv(kv.c_str());
#else
    if (value) setenv(name, value, 1);
    else       unsetenv(name);
#endif
}

// Task 3: retained vs re-materialised flatten must produce byte-identical artifacts.
static void test_flatten_retain_budget_identical() {
    printf("=== test_flatten_retain_budget_identical ===\n");

    // Two separate cache roots so the two flatten runs don't share a cached flat.
    const std::string root_a =
        (fs::temp_directory_path() / "pftest_retain_a").string();
    const std::string root_b =
        (fs::temp_directory_path() / "pftest_retain_b").string();
    fs::create_directories(fs::path(root_a) / "parts");
    fs::create_directories(fs::path(root_b) / "parts");

    // Write the same 40k-tri grid source .part into both cache roots.
    const int NX = 200, NZ = 100;
    static const uint64_t kRetainHash = 0x5050505050505050ull;
    {
        std::vector<Tri> big_tris = grid_sheet_tris(NX, NZ, 200.0f, 100.0f);
        std::vector<TriEx> ex(big_tris.size(), make_triex(99));
        for (const std::string& root : {root_a, root_b}) {
            BLASManager blas; TLASManager tlas(16);
            BLASHandle h = blas.register_triangles(big_tris.data(), (int)big_tris.size(), ex.data());
            uint32_t idx = UINT32_MAX;
            const auto& entries = blas.get_entries();
            for (size_t k = 0; k < entries.size(); ++k)
                if (entries[k]->handle == h) { idx = (uint32_t)k; break; }
            part_asset::LodLevels lods;
            part_asset::LodLevel L; L.screen_size_threshold = 0.0f; L.blas_indices.push_back(idx);
            lods.push_back(L);
            const std::string path = root + "/" + part_asset::cache_path_resolved(kRetainHash);
            bool sv = part_asset::save_v2(path, blas, tlas, nullptr, 0, lods, kRetainHash);
            CHECK(sv, "retain test: fixture written");
            if (!sv) { printf("  SKIPPING\n"); return; }
        }
    }

    part_flatten::FlattenTargets tgt;
    tgt.cluster_target_tris = 16000;

    // Run A: retention enabled (512 MB budget — takes the retained path).
    pf_set_env("MATTER_FLATTEN_RETAIN_MB", "512");
    const std::string flat_a = root_a + "/" + part_asset::cache_path_flat(kRetainHash);
    part_bundle::remove_section(flat_a, kRetainHash, part_bundle::kSectionFlat);
    auto res_a = part_flatten::flatten_part(root_a, kRetainHash, tgt);
    CHECK(res_a.ok, "retain test (budget=512): flatten ok");
    if (!res_a.ok) { printf("  error: %s\n", res_a.error.c_str()); pf_set_env("MATTER_FLATTEN_RETAIN_MB", nullptr); return; }
    std::vector<char> bytes_a;
    CHECK(read_bytes(flat_a, bytes_a), "retain test: flat_a readable");

    // Run B: retention disabled (budget=0 — forces re-materialization).
    pf_set_env("MATTER_FLATTEN_RETAIN_MB", "0");
    const std::string flat_b = root_b + "/" + part_asset::cache_path_flat(kRetainHash);
    part_bundle::remove_section(flat_b, kRetainHash, part_bundle::kSectionFlat);
    auto res_b = part_flatten::flatten_part(root_b, kRetainHash, tgt);
    CHECK(res_b.ok, "retain test (budget=0): flatten ok");
    if (!res_b.ok) { printf("  error: %s\n", res_b.error.c_str()); pf_set_env("MATTER_FLATTEN_RETAIN_MB", nullptr); return; }
    std::vector<char> bytes_b;
    CHECK(read_bytes(flat_b, bytes_b), "retain test: flat_b readable");

    pf_set_env("MATTER_FLATTEN_RETAIN_MB", nullptr);

    CHECK(bytes_a == bytes_b,
          "retained and streamed flatten artifacts are byte-identical");
    printf(bytes_a == bytes_b ? "PASSED\n" : "FAILED\n");
}

// ===========================================================================
// Bake Lab task 1.5 — trace-shape tests (docs/bake-lab.md §II.6): flatten and
// LOD-ladder instrumentation must produce the expected spans + counters when a
// collector is current, guarding against silent instrumentation rot.
// ===========================================================================

static const bake_trace::Counter* find_counter(const bake_trace::Span& s,
                                               const char* name) {
    for (const auto& c : s.counters)
        if (std::strcmp(c.name, name) == 0) return &c;
    return nullptr;
}

// flatten_part with a collector current -> one closed kSpanFlatten span whose
// counters mirror the returned FlattenResult fields.
static void test_flatten_trace_spans() {
    printf("=== test_flatten_trace_spans ===\n");
    drop_flat(flat_path(), kParentHash);
    bake_trace::Collector col;
    bake_trace::set_current(&col);
    part_flatten::FlattenResult res =
        part_flatten::flatten_part(kCacheRoot, kParentHash);
    bake_trace::set_current(nullptr);
    CHECK(res.ok, "trace: flatten_part ok");
    if (!res.ok) return;

    bake_trace::Span snap = col.snapshot();
    CHECK(snap.children.size() == 1, "trace: one top-level flatten span");
    if (snap.children.size() != 1) return;
    const bake_trace::Span& fl = snap.children[0];
    CHECK(std::strcmp(fl.name, bake_trace::kSpanFlatten) == 0,
          "trace: top-level span is flatten");
    CHECK(fl.end_ms != bake_trace::kOpenEndMs, "trace: flatten span closed");

    struct { const char* name; double expect; } want[] = {
        { "levels",        (double)res.levels },
        { "clusters",      (double)res.clusters },
        { "full_tris",     (double)res.full_tris },
        { "coarsest_tris", (double)res.coarsest_tris },
        { "instance_refs", (double)res.instance_refs },
    };
    for (const auto& w : want) {
        const bake_trace::Counter* c = find_counter(fl, w.name);
        CHECK(c != nullptr, "trace: flatten counter present");
        if (c) CHECK(c->value == w.expect,
                     "trace: flatten counter matches FlattenResult");
    }
}

// bake_lods with a collector current -> one closed kSpanLod span with one
// kSpanLodRung child per BakeTargets level, each carrying
// tris_in/tris_out/keep_ratio counters.
static void test_lod_rung_trace_spans() {
    printf("=== test_lod_rung_trace_spans ===\n");
    bake_trace::Collector col;
    bake_trace::set_current(&col);
    BLASManager blas;
    std::vector<Tri> tris = quad_tris();
    lod_bake::BakeTargets targets;   // default ladder {1.0, 0.1, 0.01}
    lod_bake::LodLevels lods = lod_bake::bake_lods(tris, targets, blas);
    bake_trace::set_current(nullptr);
    CHECK(lods.size() == targets.keep_ratio.size(), "trace: ladder built");

    bake_trace::Span snap = col.snapshot();
    CHECK(snap.children.size() == 1, "trace: one top-level lod span");
    if (snap.children.size() != 1) return;
    const bake_trace::Span& lod = snap.children[0];
    CHECK(std::strcmp(lod.name, bake_trace::kSpanLod) == 0,
          "trace: top-level span is lod");
    CHECK(lod.end_ms != bake_trace::kOpenEndMs, "trace: lod span closed");
    CHECK(lod.children.size() == targets.keep_ratio.size(),
          "trace: one lod-rung child per ladder level");
    if (lod.children.size() != targets.keep_ratio.size()) return;
    for (size_t i = 0; i < lod.children.size(); ++i) {
        const bake_trace::Span& rung = lod.children[i];
        CHECK(std::strcmp(rung.name, bake_trace::kSpanLodRung) == 0,
              "trace: rung span named lod-rung");
        CHECK(rung.end_ms != bake_trace::kOpenEndMs, "trace: rung span closed");
        const bake_trace::Counter* tin  = find_counter(rung, "tris_in");
        const bake_trace::Counter* tout = find_counter(rung, "tris_out");
        const bake_trace::Counter* keep = find_counter(rung, "keep_ratio");
        CHECK(tin && tout && keep, "trace: rung has tris_in/tris_out/keep_ratio");
        if (!(tin && tout && keep)) continue;
        CHECK(tin->value == (double)tris.size(), "trace: tris_in = input size");
        CHECK(tout->value >= 1.0, "trace: tris_out non-empty");
        CHECK(keep->value == (double)targets.keep_ratio[i],
              "trace: keep_ratio matches ladder level");
    }
}

// Bake Lab W3: lod_bake::bake_lods() with an observer set delivers
// on_rung_ready callbacks in ladder order (level 0, 1, 2, ... matching
// BakeTargets), each with a plausible (non-negative) tri count and a
// non-negative wall time. This is the direct exercise of the gate's "rung
// callbacks in order" requirement: bake_lods is where PartStore's load-time
// LOD re-bake (the production per-rung generator) actually calls the
// observer -- see part_store.cpp's get_or_load(). A NULL observer (every
// other bake_lods call site in this suite, including the trace-shape test
// right above) must be completely unaffected -- that's the byte-identity
// guarantee for production, which never sets an observer.
struct RecordingRungObserver : public BakeObserver {
    struct Rung { int level; int tris; double ms; };
    std::vector<Rung> rungs;
    int mesh_ready_calls = 0;  // must stay 0: bake_lods never calls on_mesh_ready
    void on_mesh_ready(int) override { ++mesh_ready_calls; }
    void on_rung_ready(int level, int tris, double ms) override {
        rungs.push_back({level, tris, ms});
    }
};

static void test_bake_lods_observer_rungs() {
    printf("=== test_bake_lods_observer_rungs ===\n");

    // --- Pass 1: NULL observer -- must bake fine (byte-identity baseline;
    // this is the exact call test_lod_rung_trace_spans makes above, repeated
    // here so this test is self-contained and doesn't rely on execution
    // order between the two).
    {
        BLASManager blas;
        std::vector<Tri> tris = quad_tris();
        lod_bake::BakeTargets targets;
        lod_bake::LodLevels lods = lod_bake::bake_lods(tris, targets, blas, nullptr, nullptr);
        CHECK(lods.size() == targets.keep_ratio.size(),
              "observer: null-observer bake_lods still builds the full ladder");
    }

    // --- Pass 2: observed bake -- rungs must arrive in ladder order with
    // plausible tri counts.
    BLASManager blas;
    std::vector<Tri> tris = quad_tris();
    lod_bake::BakeTargets targets;   // default ladder {1.0, 0.1, 0.01} -> 3 levels
    RecordingRungObserver obs;
    lod_bake::LodLevels lods = lod_bake::bake_lods(tris, targets, blas, nullptr, &obs);

    CHECK(lods.size() == targets.keep_ratio.size(), "observer: ladder built");
    CHECK(obs.mesh_ready_calls == 0,
          "observer: bake_lods never calls on_mesh_ready (that's bake_source's hook)");
    CHECK(obs.rungs.size() == targets.keep_ratio.size(),
          "observer: on_rung_ready fired once per ladder level");
    if (obs.rungs.size() != targets.keep_ratio.size()) return;

    for (size_t i = 0; i < obs.rungs.size(); ++i) {
        const RecordingRungObserver::Rung& r = obs.rungs[i];
        CHECK(r.level == (int)i,
              "observer: rungs arrive in ladder order (level == index)");
        CHECK(r.tris >= 0, "observer: rung reports a plausible (non-negative) tri count");
        CHECK(r.ms >= 0.0, "observer: rung reports a plausible (non-negative) wall time");
    }
    // LOD0 (keep_ratio 1.0, index 0) is the undecimated input: exact count.
    CHECK(obs.rungs[0].tris == (int)tris.size(),
          "observer: LOD0 rung tri count matches the undecimated input");
    printf("  test_bake_lods_observer_rungs OK (%zu rungs)\n", obs.rungs.size());
}

static void test_emitter_flat_round_trip();


// ------------------------------------------------------- M2.5 impostor rep --
//
// The terminal representation: past the ladder's bottom-out point a part draws
// a two-triangle camera-facing billboard sampling a baked view atlas. These
// tests cover the three properties the milestone is judged on -- ELIGIBILITY
// (a 2-triangle part must never get one), DETERMINISM (two bakes are
// byte-identical), and FAILABILITY (every rejection is reported with a named
// reason, so a tier can never go missing in silence again).

static std::vector<TriEx> triex_for(const std::vector<Tri>& tris, int material) {
    std::vector<TriEx> ex(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) {
        ex[i] = make_triex(material);
        // Sphere fixture: the outward radial direction IS the surface normal.
        ex[i].N0 = tris[i].vertex0;
        ex[i].N1 = tris[i].vertex1;
        ex[i].N2 = tris[i].vertex2;
        ex[i].tint = make_float4(0.5f, 0.25f, 0.75f, 1.0f);
        ex[i].ao0 = ex[i].ao1 = ex[i].ao2 = 0.5f;
    }
    return ex;
}

// The impostor depicts REP 0, and the distance scale moves only the impostor.
//
// Both are things a screenshot cannot tell you. A billboard baked from the
// coarsest rung looks like a billboard; it is only wrong against what it COULD
// have depicted. And a distance knob that silently moved the mesh rungs too
// would look exactly like one that worked.
static void test_impostor_source_and_distance() {
    printf("=== test_impostor_source_and_distance ===\n");

    // Tall/thin so decimation has somewhere to go and rep 0 differs sharply
    // from the coarsest rung.
    std::vector<Tri> tris = sphere_tris(24, 12);
    auto stretch = [](float3& v) { v.x *= 0.2f; v.z *= 0.2f; v.y *= 3.0f; };
    for (Tri& t : tris) {
        stretch(t.vertex0); stretch(t.vertex1); stretch(t.vertex2);
        t.centroid = make_float3((t.vertex0.x + t.vertex1.x + t.vertex2.x) / 3.0f,
                                 (t.vertex0.y + t.vertex1.y + t.vertex2.y) / 3.0f,
                                 (t.vertex0.z + t.vertex1.z + t.vertex2.z) / 3.0f);
    }
    std::vector<TriEx> ex = triex_for(tris, 6);

    // Two atlases from the SAME cluster: rep 0, and a decimated stand-in for
    // the coarsest rung. If those came out identical the whole question would
    // be moot, so assert they differ FIRST -- otherwise the source assertion
    // below proves nothing.
    {
        impostor::ClusterImpostor from_rep0, from_coarse;
        CHECK(impostor::bake_cluster(0u, tris, ex, from_rep0),
              "imp src: rep 0 bakes");
        std::vector<Tri> coarse = lod_bake::decimate_to_error(tris, 0.35f, false);
        CHECK(!coarse.empty() && coarse.size() < tris.size(),
              "imp src: the coarse stand-in actually decimated");
        if (!coarse.empty()) {
            std::vector<TriEx> cex = triex_for(coarse, 6);
            CHECK(impostor::bake_cluster(0u, coarse, cex, from_coarse),
                  "imp src: the coarse rung bakes");
            CHECK(from_rep0.atlas != from_coarse.atlas,
                  "imp src: the two sources DO produce different atlases, so "
                  "which one the bake picks is an observable choice");
        }
    }

    const uint64_t h = 0xA110000044440004ull;
    CHECK(save_fixture(h, 6, {tris}, {}), "imp src: fixture written");
    const std::string flat = std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(h);
    const std::string fimp = std::string(kCacheRoot) + "/" + impostor::cache_path_impostor(h);

    // The scale is driven through MATTER_IMPOSTOR_DISTANCE, not through a
    // FlattenTargets field. That is the production surface (an operator sets
    // the env var), and it is the only surface the flat's ladder-shape
    // identity can see: a per-call struct override writes an artifact stamped
    // with the AMBIENT shape, which flatten_part now warns about. The env is
    // cleared again as soon as the load is done, so it is live for exactly the
    // bake+load pair that has to agree about it.
    auto bake_ladder = [&](const char* dist_scale,
                           std::vector<part_asset::FlatCluster>& cl,
                           BLASManager& blas) -> part_flatten::FlattenResult {
        part_bundle::remove_section(flat, h, part_bundle::kSectionFlat);
        part_bundle::remove_section(fimp, h, part_bundle::kSectionImpostor);
        pf_set_env("MATTER_IMPOSTOR_DISTANCE", dist_scale);
        auto r = part_flatten::flatten_part(kCacheRoot, h);
        if (r.ok) {
            TLASManager tl(4);
            part_asset::load_flat_v3(flat, h, blas, tl, cl);
        }
        pf_set_env("MATTER_IMPOSTOR_DISTANCE", nullptr);
        return r;
    };

    std::vector<part_asset::FlatCluster> cl1; BLASManager b1;
    auto r1 = bake_ladder(nullptr, cl1, b1);
    CHECK(r1.ok, "imp src: ladder bakes");
    if (!r1.ok || cl1.empty() || cl1[0].lods.size() < 2) {
        printf("  SKIPPING (no impostor rung on this fixture)\n");
        return;
    }
    const auto& lods1 = cl1[0].lods;
    const auto& e1 = b1.get_entries();
    const size_t term1 = lods1.size() - 1;
    const BLASManager::BLASEntry* bb = e1[lods1[term1].blas_indices[0]].get();
    if (!bb || !impostor::is_billboard_rung(bb->triangles, bb->tri_extra)) {
        printf("  SKIPPING (terminal rung is not a billboard)\n");
        return;
    }

    // THE SOURCE ASSERTION, made exactly the way PartStore makes it: fold the
    // depicts-hash over rep 0 and require the sidecar to accept it. If the bake
    // and the loader disagreed here, every atlas in the world would be rejected
    // as stale and silently fall back to mesh-only.
    {
        uint64_t d0 = impostor::depicts_hash_begin();
        impostor::depicts_hash_add_cluster(
            d0, 0u, e1[lods1[0].blas_indices[0]]->triangles,
            e1[lods1[0].blas_indices[0]]->tri_extra);
        impostor::PartImpostor loaded;
        impostor::LoadFailure fail = impostor::LoadFailure::None;
        std::string why;
        const bool ok0 = impostor::load(fimp, h, impostor::depicts_hash_finish(d0),
                                        loaded, &fail, &why);
        CHECK(ok0, "imp src: the atlas answers to REP 0's depicts-hash");
        if (!ok0)
            printf("  load said: %s (%s)\n", impostor::load_failure_text(fail),
                   why.c_str());

        // ...and NOT to the coarsest mesh rung's, which is what it used to
        // depict. Without this the assertion above would still pass on a
        // ladder whose rep 0 and coarsest rung happen to be the same mesh.
        if (lods1.size() >= 3) {
            uint64_t dc = impostor::depicts_hash_begin();
            impostor::depicts_hash_add_cluster(
                dc, 0u, e1[lods1[term1 - 1].blas_indices[0]]->triangles,
                e1[lods1[term1 - 1].blas_indices[0]]->tri_extra);
            impostor::PartImpostor other;
            CHECK(!impostor::load(fimp, h, impostor::depicts_hash_finish(dc), other),
                  "imp src: it does NOT answer to the coarsest rung's hash, so "
                  "the source really moved");
        }
    }

    // THE DISTANCE ASSERTION. Halving the scale must bring the billboard in,
    // and must leave every MESH rung untouched -- that independence is the
    // entire point of the knob.
    std::vector<part_asset::FlatCluster> cl2; BLASManager b2;
    auto r2 = bake_ladder("0.5", cl2, b2);
    CHECK(r2.ok && !cl2.empty(), "imp dist: half-distance ladder bakes");
    if (r2.ok && !cl2.empty() && cl2[0].lods.size() == lods1.size()) {
        const auto& lods2 = cl2[0].lods;
        // The impostor's switch-IN is stored as the PREVIOUS rung's threshold;
        // a LARGER threshold means it switches in nearer.
        const float t1 = lods1[term1 - 1].screen_size_threshold;
        const float t2 = lods2[term1 - 1].screen_size_threshold;
        CHECK(t2 > t1 * 1.5f,
              "imp dist: scale 0.5 brings the billboard substantially closer");
        bool mesh_unchanged = true;
        for (size_t i = 0; i + 2 < lods1.size(); ++i)
            if (lods1[i].screen_size_threshold != lods2[i].screen_size_threshold)
                mesh_unchanged = false;
        CHECK(mesh_unchanged,
              "imp dist: every MESH rung's distance is untouched, so the knob "
              "moves only the impostor");
        printf("  impostor switch threshold %.5f -> %.5f (larger = nearer)\n", t1, t2);
    }
    printf("  test_impostor_source_and_distance OK\n");

    part_bundle::remove_section(flat, h, part_bundle::kSectionFlat);
    part_bundle::remove_section(fimp, h, part_bundle::kSectionImpostor);
}

// ===========================================================================
// The mesh rung cap, and the staleness gate that makes it observable.
//
// THE REJECTION FIRING IS THE TEST. A knob that reshapes the ladder changes
// the bytes a bake would write without changing the part hash or the format
// version, so before the ladder-shape digest existed
// LocalProvider::ensure_part_flattened found a "compatible" flat and returned
// without baking -- the old ladder was served and the knob appeared to do
// nothing. That failure only shows up on a WARM cache, which is why every
// step below re-probes an EXISTING artifact instead of removing it first.
// A version of this test that wiped the flat between bakes would pass against
// the bug it exists to catch.
// ===========================================================================
static void test_mesh_rung_cap_and_stale_rejection() {
    printf("=== test_mesh_rung_cap_and_stale_rejection ===\n");

    // Dense enough that the uncapped ladder earns several mesh rungs, so a cap
    // of 2 is visibly a cap and not a coincidence.
    std::vector<Tri> tris = sphere_tris(32, 16);
    const uint64_t h = 0xA110000055550001ull;
    CHECK(save_fixture(h, 7, {tris}, {}), "rung cap: fixture written");
    const std::string flat = std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(h);
    const std::string fimp = std::string(kCacheRoot) + "/" + impostor::cache_path_impostor(h);

    // Every probe below asks EXACTLY the question local_provider asks before
    // it decides to skip the bake.
    auto provider_would_skip = [&]() {
        return part_asset::is_cache_artifact_header_compatible(
            flat, h, part_asset::kFormatVersionFlat);
    };
    auto levels_of = [&](std::vector<part_asset::FlatCluster>& cl) -> size_t {
        return cl.empty() ? 0 : cl[0].lods.size();
    };

    // --- 1. cold bake at the shipped default shape --------------------------
    part_bundle::remove_section(flat, h, part_bundle::kSectionFlat);
    part_bundle::remove_section(fimp, h, part_bundle::kSectionImpostor);
    auto r0 = part_flatten::flatten_part(kCacheRoot, h);
    CHECK(r0.ok, "rung cap: default ladder bakes");
    if (!r0.ok) { printf("  error: %s\n", r0.error.c_str()); return; }
    std::vector<char> bytes_default;
    CHECK(read_bytes(flat, bytes_default), "rung cap: default flat readable");
    CHECK(provider_would_skip(),
          "rung cap: a warm default flat is served (no pointless rebake)");

    std::vector<part_asset::FlatCluster> cl0; BLASManager b0;
    { TLASManager tl(4); part_asset::load_flat_v3(flat, h, b0, tl, cl0); }
    const size_t levels_default = levels_of(cl0);
    CHECK(levels_default >= 4,
          "rung cap: the uncapped ladder has room to be capped");
    printf("  default ladder: %zu levels\n", levels_default);

    // --- 2. THE GATE. Change ONLY the env knob and re-probe the SAME file. ---
    pf_set_env("MATTER_LOD_MAX_MESH_RUNGS", "2");
    CHECK(!provider_would_skip(),
          "STALE REJECTION: with the cap set, the existing flat is NOT served");
    {
        // ...and the full load refuses it too, so nothing downstream of the
        // probe can pick the stale artifact up by another route.
        BLASManager b; TLASManager tl(4);
        std::vector<part_asset::FlatCluster> cl;
        CHECK(!part_asset::load_flat_v3(flat, h, b, tl, cl),
              "STALE REJECTION: load_flat_v3 refuses the stale-shape flat");
    }

    // --- 3. rebake IN PLACE (no remove_section: the warm-cache path) --------
    auto r2 = part_flatten::flatten_part(kCacheRoot, h);
    CHECK(r2.ok, "rung cap: capped ladder bakes over the stale one");
    std::vector<part_asset::FlatCluster> cl2; BLASManager b2;
    if (r2.ok) { TLASManager tl(4); part_asset::load_flat_v3(flat, h, b2, tl, cl2); }
    const size_t levels_capped = levels_of(cl2);
    CHECK(provider_would_skip(),
          "rung cap: the rebaked flat now matches, so the next run is warm");
    // 2 mesh rungs, plus at most the terminal impostor.
    CHECK(levels_capped > 0 && levels_capped <= 3,
          "rung cap: cap=2 publishes at most 2 mesh rungs + the impostor");
    CHECK(levels_capped < levels_default,
          "rung cap: the cap actually shortened the ladder");
    std::vector<char> bytes_capped;
    CHECK(read_bytes(flat, bytes_capped), "rung cap: capped flat readable");
    CHECK(bytes_capped != bytes_default,
          "rung cap: the capped artifact really is different bytes -- the "
          "rejection was not rejecting an identical file");
    printf("  capped ladder: %zu levels\n", levels_capped);

    // The impostor needs NO cap-awareness: it steps off level_metas.back(),
    // so a shorter mesh ladder hands over EARLIER (nearer the camera = a
    // LARGER stored threshold on the rung before the terminal).
    if (levels_capped >= 2 && levels_default >= 2 && !cl2.empty() && !cl0.empty()) {
        const auto& e2 = b2.get_entries();
        const uint32_t term = cl2[0].lods.back().blas_indices[0];
        const bool billboard =
            term < e2.size() &&
            impostor::is_billboard_rung(e2[term]->triangles, e2[term]->tri_extra);
        CHECK(billboard,
              "rung cap: the capped ladder still terminates in a billboard, so "
              "the part switches straight from a mesh rung to an impostor");
        if (billboard) {
            const float thr_capped = cl2[0].lods[levels_capped - 2].screen_size_threshold;
            const float thr_default = cl0[0].lods[levels_default - 2].screen_size_threshold;
            CHECK(thr_capped > thr_default,
                  "rung cap: capping pulls the impostor IN (it takes over from "
                  "a higher-resolution rung, which is the whole point)");
            printf("  impostor switch threshold %.5f -> %.5f (larger = nearer)\n",
                   thr_default, thr_capped);
        }
    }

    // --- 4. the gate fires in BOTH directions -------------------------------
    // Clearing the knob must reject the CAPPED flat just as firmly. A gate
    // that only invalidated in one direction would strand every cache the
    // moment someone tried a setting and changed their mind.
    pf_set_env("MATTER_LOD_MAX_MESH_RUNGS", nullptr);
    CHECK(!provider_would_skip(),
          "STALE REJECTION: clearing the cap rejects the capped flat too");
    auto r3 = part_flatten::flatten_part(kCacheRoot, h);
    CHECK(r3.ok, "rung cap: default ladder re-bakes");
    std::vector<char> bytes_back;
    CHECK(read_bytes(flat, bytes_back), "rung cap: restored flat readable");
    // Round trip AND determinism in one: back at the default shape the bytes
    // are the ones the very first cold bake wrote.
    CHECK(bytes_back == bytes_default,
          "rung cap: returning to the default shape reproduces the default "
          "artifact byte-for-byte");

    // --- 5. fail closed ------------------------------------------------------
    // Garbage must be IGNORED, not guessed at, and a cap at/above the
    // serialized capacity is not a cap -- kMaxSerializedLodLevels already
    // binds. Both must leave the warm cache alone: if either reshaped the
    // ladder or merely moved the digest, the probe would miss here.
    for (const char* junk : {"abc", "3x", "-1", "", "1e3", "3.5"}) {
        pf_set_env("MATTER_LOD_MAX_MESH_RUNGS", junk);
        const std::string why =
            std::string("rung cap: garbage value '") + junk +
            "' is ignored, not trusted";
        CHECK(provider_would_skip(), why.c_str());
    }
    for (const char* wide : {"9", "10", "99"}) {
        pf_set_env("MATTER_LOD_MAX_MESH_RUNGS", wide);
        const std::string why =
            std::string("rung cap: a cap of '") + wide +
            "' is not a cap (kMaxSerializedLodLevels binds) and does not "
            "invalidate the cache";
        CHECK(provider_would_skip(), why.c_str());
    }
    pf_set_env("MATTER_LOD_MAX_MESH_RUNGS", nullptr);

    // A cap of 1 is the degenerate end of the knob: rep 0 and the billboard,
    // nothing between. It must still produce a VALID ladder, not an empty one.
    pf_set_env("MATTER_LOD_MAX_MESH_RUNGS", "1");
    CHECK(!provider_would_skip(), "rung cap: cap=1 invalidates the default flat");
    auto r1 = part_flatten::flatten_part(kCacheRoot, h);
    CHECK(r1.ok, "rung cap: cap=1 bakes");
    if (r1.ok) {
        BLASManager b1; TLASManager tl(4);
        std::vector<part_asset::FlatCluster> cl1;
        CHECK(part_asset::load_flat_v3(flat, h, b1, tl, cl1),
              "rung cap: cap=1 flat loads");
        const size_t n = levels_of(cl1);
        CHECK(n >= 1 && n <= 2, "rung cap: cap=1 gives rep 0 (+ impostor) only");
        printf("  cap=1 ladder: %zu levels\n", n);
    }
    pf_set_env("MATTER_LOD_MAX_MESH_RUNGS", nullptr);

    // The digest itself: distinct shapes must not alias, and the shipped
    // default must be the 0 sentinel (that is what keeps a default bake
    // byte-identical to every flat already on disk).
    // The SHIPPED shape means no overrides at all -- including the cell
    // resolution main() pins for bake speed, which is an override like any
    // other and legitimately digests away from the sentinel. Clear it for the
    // length of this one assertion, exactly as the rung cap above is cleared,
    // then put it back for the tests that follow.
    pf_set_env("MATTER_IMPOSTOR_CELL_PX", nullptr);
    CHECK(part_flatten::active_ladder_shape_digest() == 0ull,
          "rung cap: the shipped default shape digests to the 0 sentinel");
    pf_set_env("MATTER_IMPOSTOR_CELL_PX", "32");
    {
        part_flatten::FlattenTargets a, b;
        a.max_mesh_rungs = 2; b.max_mesh_rungs = 3;
        CHECK(part_flatten::ladder_shape_digest(a) != 0ull &&
                  part_flatten::ladder_shape_digest(b) != 0ull &&
                  part_flatten::ladder_shape_digest(a) !=
                      part_flatten::ladder_shape_digest(b),
              "rung cap: two different caps give two different digests");
    }

    part_bundle::remove_section(flat, h, part_bundle::kSectionFlat);
    part_bundle::remove_section(fimp, h, part_bundle::kSectionImpostor);
    printf("  test_mesh_rung_cap_and_stale_rejection OK\n");
}

static void test_impostor_eligibility() {
    printf("=== test_impostor_eligibility ===\n");
    // The floor is an ORDER-OF-MAGNITUDE reduction (2 / N <= 0.125), not the
    // ladder's 30 %: unlike a mesh rung the impostor carries a per-part atlas.
    CHECK(!impostor::cluster_earns_impostor(0), "0 triangles: no impostor");
    CHECK(!impostor::cluster_earns_impostor(2),
          "2-triangle terminal: no impostor (it would ADD geometry)");
    CHECK(!impostor::cluster_earns_impostor(15), "15 triangles: below the floor");
    CHECK(impostor::cluster_earns_impostor(16), "16 triangles: 8x reduction, admitted");
    CHECK(impostor::cluster_earns_impostor(4258), "hero boulder: admitted");
    printf("PASSED\n");
}

static void test_impostor_bake_deterministic() {
    printf("=== test_impostor_bake_deterministic ===\n");
    std::vector<Tri> tris = sphere_tris(12, 8);
    std::vector<TriEx> ex = triex_for(tris, 3);

    impostor::ClusterImpostor a, b;
    CHECK(impostor::bake_cluster(0, tris, ex, a), "bake_cluster succeeds");
    CHECK(impostor::bake_cluster(0, tris, ex, b), "bake_cluster succeeds twice");
    CHECK(a.atlas.size() == impostor::atlas_bytes(), "atlas is the declared size");
    CHECK(a.atlas == b.atlas, "two bakes produce a byte-identical atlas");
    CHECK(a.half_extent > 0.0f && a.half_extent == b.half_extent,
          "half extent is positive and reproducible");
    CHECK(a.material_index == 3u, "dominant material is the mesh's material");
    CHECK(a.source_tris == tris.size(), "source triangle count recorded");

    // Coverage: the sphere fills a good fraction of every cell, and the cell
    // borders stay clear (the 2 % margin) so bilinear filtering cannot bleed a
    // neighbouring view in.
    size_t covered = 0;
    for (size_t t = 0; t < impostor::layer_px() * impostor::layer_px(); ++t)
        if (a.atlas[t * 4 + 3] > 0) ++covered;
    CHECK(covered > impostor::layer_px() * impostor::layer_px() / 4,
          "the baked silhouette covers a substantial share of the atlas");
    bool border_clear = true;
    for (uint32_t v = 0; v < impostor::kViews; ++v) {
        const uint32_t cx = (v % impostor::kGridDim) * impostor::cell_px();
        const uint32_t cy = (v / impostor::kGridDim) * impostor::cell_px();
        for (uint32_t k = 0; k < impostor::cell_px(); ++k) {
            const size_t top = (size_t(cy) * impostor::layer_px() + cx + k) * 4;
            const size_t left = (size_t(cy + k) * impostor::layer_px() + cx) * 4;
            if (a.atlas[top + 3] != 0 || a.atlas[left + 3] != 0) border_clear = false;
        }
    }
    CHECK(border_clear,
          "every cell border texel is empty (the guard band stops cross-view bleed)");

    // Fractional coverage is what antialiases the silhouette: a hard 0/255
    // cutout would leave every covered texel at 255.
    bool any_partial = false;
    for (size_t t = 0; t < impostor::layer_px() * impostor::layer_px(); ++t) {
        const uint8_t cov = a.atlas[t * 4 + 3];
        if (cov > 0 && cov < 255) { any_partial = true; break; }
    }
    CHECK(any_partial, "supersampling yields fractional edge coverage");

    // A different mesh must produce a different atlas, or the bake is not
    // actually looking at the geometry.
    std::vector<Tri> other = sphere_tris(12, 8);
    for (auto& t : other) {
        t.vertex0.y *= 0.4f; t.vertex1.y *= 0.4f; t.vertex2.y *= 0.4f;
    }
    impostor::ClusterImpostor c;
    CHECK(impostor::bake_cluster(0, other, triex_for(other, 3), c), "squashed bake ok");
    CHECK(c.atlas != a.atlas, "a different shape produces a different atlas");
    printf("PASSED\n");
}

// Edge padding (impostor format v7). Every uncovered texel within kPadTexels
// (Chebyshev) of the silhouette must carry its covered neighbours' values so a
// runtime bilinear tap never blends against the zero-fill; every texel beyond
// that distance must still BE the zero-fill; and coverage alpha must be 0 on
// all of them -- the silhouette the cutout sees does not move. The fixture
// tint is uniform, so the padded tint is an EQUALITY assertion, not a
// tolerance: any dilution (the pale-olive canopy wash of issue 6e0c76fc) is a
// byte mismatch here.
static void test_impostor_atlas_padding() {
    printf("=== test_impostor_atlas_padding ===\n");
    std::vector<Tri> tris = sphere_tris(12, 8);
    // The card's colour now comes from the MATERIAL, not TriEx::tint, so the
    // fixture needs a material whose albedo is EXACTLY representable -- this
    // assertion is a byte equality and to_u8 rounds at .5.
    //
    // Builtin 3 (the previous fixture material) is 0.8,0.7,0.3, and 0.7*255 is
    // exactly 178.5: dead on the rounding boundary, so the subsample average's
    // last-bit wobble flips covered texels between 178 and 179 and the
    // equality fails for reasons that have nothing to do with padding. These
    // three values are the same ones the old vertex tint carried, so the
    // expected bytes below are unchanged and the assertion stays strict.
    MaterialDef pad_mat{};
    MaterialRegistryDefaultDynamicDef(&pad_mat);
    pad_mat.albedo[0] = 0.5f; pad_mat.albedo[1] = 0.25f; pad_mat.albedo[2] = 0.75f;
    const int pad_id = MaterialRegistryDefineDynamic(&pad_mat, "pf_pad_fixture");
    CHECK(pad_id >= 0, "padding: fixture material defined");
    std::vector<TriEx> ex = triex_for(tris, pad_id >= 0 ? pad_id : 3);
    impostor::ClusterImpostor a;
    CHECK(impostor::bake_cluster(0, tris, ex, a), "bake ok");
    const uint32_t cell = impostor::cell_px();
    const uint32_t edge = impostor::layer_px();
    const size_t layer_sz = impostor::layer_bytes();
    // View 0's cell only: its baked normals face the +z bake camera, which is
    // what makes the "not the zero encoding" check below sound (a +z
    // hemisphere direction can never octahedral-encode to RG == (0,0)).
    auto shade_at = [&](uint32_t x, uint32_t y) {
        return a.atlas.data() + (size_t(y) * edge + x) * 4;
    };
    auto tint_at = [&](uint32_t x, uint32_t y) {
        return a.atlas.data() + layer_sz + (size_t(y) * edge + x) * 4;
    };
    // Chebyshev distance from each texel to the nearest covered texel -- the
    // metric of an 8-neighbour dilation, so "dist <= kPadTexels" is exactly
    // the set the padding must have filled.
    std::vector<uint32_t> dist(size_t(cell) * cell, UINT32_MAX);
    for (uint32_t y = 0; y < cell; ++y)
        for (uint32_t x = 0; x < cell; ++x) {
            if (shade_at(x, y)[3] == 0) continue;
            for (uint32_t ty = 0; ty < cell; ++ty)
                for (uint32_t tx = 0; tx < cell; ++tx) {
                    const uint32_t dx = tx > x ? tx - x : x - tx;
                    const uint32_t dy = ty > y ? ty - y : y - ty;
                    const uint32_t d = dx > dy ? dx : dy;
                    uint32_t& slot = dist[size_t(ty) * cell + tx];
                    if (d < slot) slot = d;
                }
        }
    // Unchanged from before the albedo switch: 0.5/0.25/0.75 -> 128/64/191,
    // now sourced from the fixture material above instead of TriEx::tint.
    // Alpha is 255 because a resolved material albedo writes tint.a = 1.
    const uint8_t expect_tint[4] = {128, 64, 191, 255};
    size_t ring = 0, beyond = 0;
    size_t wrong_tint = 0, zero_normal = 0, alpha_grown = 0, not_zero_fill = 0;
    for (uint32_t y = 0; y < cell; ++y)
        for (uint32_t x = 0; x < cell; ++x) {
            const uint32_t d = dist[size_t(y) * cell + x];
            if (d == 0 || d == UINT32_MAX) continue;   // covered, or empty cell
            const uint8_t* sh = shade_at(x, y);
            const uint8_t* tn = tint_at(x, y);
            if (sh[3] != 0) ++alpha_grown;             // silhouette must not move
            if (d <= impostor::kPadTexels) {
                ++ring;
                if (std::memcmp(tn, expect_tint, 4) != 0) ++wrong_tint;
                if (sh[0] == 0 && sh[1] == 0) ++zero_normal;
            } else {
                ++beyond;
                if (sh[0] || sh[1] || sh[2] || tn[0] || tn[1] || tn[2] || tn[3])
                    ++not_zero_fill;
            }
        }
    CHECK(ring > 0, "padding: the sphere has a padding ring to check");
    CHECK(beyond > 0, "padding: and texels beyond it (the fixture is not degenerate)");
    CHECK(alpha_grown == 0, "padding: coverage alpha untouched, silhouette identical");
    CHECK(wrong_tint == 0, "padding: every ring texel carries the covered tint exactly");
    CHECK(zero_normal == 0, "padding: no ring texel keeps the zero normal encoding");
    CHECK(not_zero_fill == 0, "padding: texels beyond kPadTexels stay zero-filled");
    // Leave the registry as we found it: dynamic entries are per-world, and a
    // fixture material outliving its test would shift every later test's ids.
    MaterialRegistryResetDynamic();
    printf("PASSED\n");
}

static void test_impostor_quad_shape() {
    printf("=== test_impostor_quad_shape ===\n");
    impostor::ClusterImpostor imp;
    imp.center[0] = 1.0f; imp.center[1] = 2.0f; imp.center[2] = -3.0f;
    imp.half_extent = 0.75f;
    imp.material_index = 7;
    std::vector<Tri> tris; std::vector<TriEx> ex;
    impostor::build_quad(imp, tris, ex);
    CHECK(tris.size() == 2, "the billboard rung is exactly two triangles");
    CHECK(ex.size() == 2, "with parallel TriEx");
    if (tris.size() != 2 || ex.size() != 2) { printf("FAILED\n"); return; }
    CHECK(ex[0].uv0.x >= impostor::kQuadMarker, "corner 0 carries the impostor marker");
    CHECK(ex[1].uv2.x >= impostor::kQuadMarker, "every corner carries the marker");
    CHECK(ex[0].materialId == 7, "the quad inherits the dominant material");
    CHECK(ex[0].ao0 == imp.half_extent,
          "the AO channel transports the half extent to the vertex stage");
    // The vertex stage recovers the centre as position - sign(corner)*extent;
    // that only works if every corner sits exactly one extent from the centre
    // on both axes.
    bool centred = true;
    for (const Tri& t : tris) {
        const float3 vs[3] = {t.vertex0, t.vertex1, t.vertex2};
        for (const float3& v : vs) {
            if (std::fabs(std::fabs(v.x - imp.center[0]) - imp.half_extent) > 1e-6f ||
                std::fabs(std::fabs(v.y - imp.center[1]) - imp.half_extent) > 1e-6f ||
                std::fabs(v.z - imp.center[2]) > 1e-6f)
                centred = false;
        }
    }
    CHECK(centred, "every corner is one half-extent from the centre on both axes");
    printf("PASSED\n");
}

// ---------------------------------------------------------------------------
// ELEVATION RINGS (2026-08-05, impostor_bake.h "ELEVATION ROWS").
//
// The atlas holds kAzimuths x kElevations views. The bake writes cell
// (elevation, azimuth) from a direction it CONSTRUCTS; raster.vert picks that
// same cell from a direction it MEASURES, by inverting the construction with
// atan2/asin. If those two ever stop being inverses, every impostor samples a
// neighbouring view — which on screen reads as a rendering bug and localises
// nowhere near the arithmetic. The shader cannot be unit-tested from here, so
// this asserts the property the shader's formula stands on: the inverse
// recovers the index for every view in the atlas.
static void test_impostor_view_index_inverse() {
    printf("=== test_impostor_view_index_inverse ===\n");

    bool all_round_trip = true;
    uint32_t worst_view = 0;
    for (uint32_t view = 0; view < impostor::kViews; ++view) {
        const uint32_t az_i = view % impostor::kAzimuths;
        const uint32_t el_i = view / impostor::kAzimuths;
        // Exactly impostor_bake.cpp's construction.
        const float angle = 6.28318530717958647692f *
                            static_cast<float>(az_i) /
                            static_cast<float>(impostor::kAzimuths);
        const float elev = static_cast<float>(el_i) * impostor::kElevationStep;
        const float ce = std::cos(elev), se = std::sin(elev);
        const float vx = std::sin(angle) * ce, vy = se, vz = std::cos(angle) * ce;

        // Exactly raster.vert's inverse.
        float azimuth = std::atan2(vx, vz);
        const float kTwoPi = 6.28318530717958647692f;
        if (azimuth < 0.0f) azimuth += kTwoPi;
        const uint32_t got_az =
            static_cast<uint32_t>(std::floor(azimuth / kTwoPi *
                                             static_cast<float>(impostor::kAzimuths) + 0.5f)) %
            impostor::kAzimuths;
        const float elevation = std::asin(std::fmax(-1.0f, std::fmin(1.0f, vy)));
        const float ring = std::floor(elevation / impostor::kElevationStep + 0.5f);
        const uint32_t got_el = static_cast<uint32_t>(
            std::fmax(0.0f, std::fmin(ring, static_cast<float>(impostor::kElevations - 1))));

        if (impostor::view_index(got_az, got_el) != view) {
            all_round_trip = false;
            worst_view = view;
        }
    }
    CHECK(all_round_trip, "every baked view direction recovers its own view index");
    if (!all_round_trip) printf("  first mismatch at view %u\n", worst_view);

    // A camera outside the baked elevation range CLAMPS to the nearest ring
    // rather than wrapping into a view from the other side. Straight down is
    // the case that matters — it is where a flying camera ends up.
    {
        auto ring_for = [](float elev_rad) {
            const float r = std::floor(elev_rad / impostor::kElevationStep + 0.5f);
            return static_cast<uint32_t>(
                std::fmax(0.0f, std::fmin(r, static_cast<float>(impostor::kElevations - 1))));
        };
        const float kDeg = 3.14159265358979f / 180.0f;
        CHECK(ring_for(-45.0f * kDeg) == 0, "below the equator clamps to ring 0");
        CHECK(ring_for(14.0f * kDeg) == 0, "just under the first boundary stays ring 0");
        CHECK(ring_for(16.0f * kDeg) == 1, "just over it steps to ring 1");
        CHECK(ring_for(44.0f * kDeg) == 1, "just under the second boundary stays ring 1");
        CHECK(ring_for(46.0f * kDeg) == 2, "just over it steps to ring 2");
        CHECK(ring_for(90.0f * kDeg) == impostor::kElevations - 1,
              "straight down clamps to the top ring, never wraps");
    }
    printf("PASSED\n");
}

// The rings have to DEPICT something different, or they are 48 copies of one
// image at three times the bake cost. A sphere cannot show this — it looks
// identical from every elevation by construction — so the fixture is a cone,
// which presents a triangle from the side and a disc from above.
static void test_impostor_elevation_rings_differ() {
    printf("=== test_impostor_elevation_rings_differ ===\n");

    std::vector<Tri> cone;
    {
        const int segs = 24;
        const float h = 1.0f, r = 0.6f;
        const float3 apex = make_float3(0.0f, h, 0.0f);
        const float3 base_c = make_float3(0.0f, -h, 0.0f);
        for (int s = 0; s < segs; ++s) {
            const float a0 = 6.2831853f * float(s) / float(segs);
            const float a1 = 6.2831853f * float(s + 1) / float(segs);
            const float3 p0 = make_float3(r * std::cos(a0), -h, r * std::sin(a0));
            const float3 p1 = make_float3(r * std::cos(a1), -h, r * std::sin(a1));
            cone.push_back(make_tri(apex, p0, p1));      // side
            cone.push_back(make_tri(base_c, p1, p0));    // cap
        }
    }
    std::vector<TriEx> ex = triex_for(cone, 4);
    impostor::ClusterImpostor imp;
    CHECK(impostor::bake_cluster(0u, cone, ex, imp), "cone impostor bakes");
    if (imp.atlas.size() != impostor::atlas_bytes()) { printf("FAILED\n"); return; }

    // Coverage per ring, over the shade layer's alpha. A cone seen edge-on
    // covers a triangle; seen from 60 degrees up it covers more of its cell,
    // because the circular base turns toward the camera.
    auto ring_coverage = [&](uint32_t el_i) {
        size_t sum = 0;
        for (uint32_t az = 0; az < impostor::kAzimuths; ++az) {
            const uint32_t v = impostor::view_index(az, el_i);
            const uint32_t cx = (v % impostor::kGridDim) * impostor::cell_px();
            const uint32_t cy = (v / impostor::kGridDim) * impostor::cell_px();
            for (uint32_t y = 0; y < impostor::cell_px(); ++y)
                for (uint32_t x = 0; x < impostor::cell_px(); ++x)
                    sum += imp.atlas[(size_t(cy + y) * impostor::layer_px() + cx + x) * 4 + 3];
        }
        return sum;
    };
    const size_t c0 = ring_coverage(0);
    const size_t c2 = ring_coverage(impostor::kElevations - 1);
    CHECK(c0 > 0 && c2 > 0, "both the equator and the top ring depict something");
    CHECK(c0 != c2,
          "the top ring is a different image from the equator (rings are not copies)");

    // And the NORMALS differ, which is the half of this that fixes the
    // brightness divergence: an equator cell's normals point sideways, so a
    // card tilted toward an overhead camera would shade against the wrong
    // hemisphere if it kept them.
    auto ring_normal_sum = [&](uint32_t el_i) {
        long long acc = 0;
        for (uint32_t az = 0; az < impostor::kAzimuths; ++az) {
            const uint32_t v = impostor::view_index(az, el_i);
            const uint32_t cx = (v % impostor::kGridDim) * impostor::cell_px();
            const uint32_t cy = (v / impostor::kGridDim) * impostor::cell_px();
            for (uint32_t y = 0; y < impostor::cell_px(); ++y)
                for (uint32_t x = 0; x < impostor::cell_px(); ++x) {
                    const size_t o = (size_t(cy + y) * impostor::layer_px() + cx + x) * 4;
                    if (imp.atlas[o + 3] == 0) continue;
                    acc += imp.atlas[o] + imp.atlas[o + 1];
                }
        }
        return acc;
    };
    CHECK(ring_normal_sum(0) != ring_normal_sum(impostor::kElevations - 1),
          "the rings carry different surface normals, not just different silhouettes");

    // Byte neutrality is the whole reason this was affordable; assert it here
    // too so a future cell-size edit that quietly grows the atlas is caught by
    // a test and not only by a static_assert someone can delete.
    //
    // This used to pin a literal byte count (131072, then 524288). It cannot
    // any more: the cell resolution is a runtime setting. So it pins the
    // RELATIONSHIP instead, which is the thing that would actually be wrong if
    // the layout drifted -- the atlas is two RGBA8 layers of an 8x8 grid of
    // cell_px cells, whatever cell_px currently is. main() pins cell_px itself
    // for this suite.
    const size_t expect_atlas =
        size_t(impostor::kGridDim) * impostor::cell_px() *
        impostor::kGridDim * impostor::cell_px() * 4u * 2u;
    CHECK(imp.atlas.size() == impostor::atlas_bytes() &&
              impostor::atlas_bytes() == expect_atlas,
          "the atlas is two RGBA8 layers of an 8x8 grid of cell_px cells");
    printf("PASSED\n");
}

// The cell resolution is a SETTING, and every gate that keeps a stale atlas
// off the screen has to notice when it moves. This bakes the same mesh at two
// resolutions inside ONE process (which is exactly why impostor::cell_px()
// caches nothing in a static) and proves four things, each of which would let
// a wrong-resolution atlas be drawn if it silently stopped holding.
static void test_impostor_cell_px_parametrized() {
    printf("=== test_impostor_cell_px_parametrized ===\n");
    std::vector<Tri> tris = sphere_tris(12, 8);
    std::vector<TriEx> ex = triex_for(tris, 2);

    // Two SMALL sizes on purpose: the bake is O(cell_px^2) per view, so
    // running this at the 128 px default would cost 16x what the whole rest of
    // the suite does and prove nothing extra.
    pf_set_env("MATTER_IMPOSTOR_CELL_PX", "16");
    const uint32_t cell_a = impostor::cell_px();
    const size_t bytes_a = impostor::atlas_bytes();
    const float band_a = impostor::guard_band();
    const uint64_t ladder_a = part_flatten::active_ladder_shape_digest();
    impostor::PartImpostor part_a;
    part_a.clusters.resize(1);
    CHECK(impostor::bake_cluster(0, tris, ex, part_a.clusters[0]), "bake at 16 px");
    uint64_t ha = impostor::depicts_hash_begin();
    impostor::depicts_hash_add_cluster(ha, 0, tris, ex);
    const uint64_t depicts_a = impostor::depicts_hash_finish(ha);

    pf_set_env("MATTER_IMPOSTOR_CELL_PX", "32");
    const uint32_t cell_b = impostor::cell_px();
    const size_t bytes_b = impostor::atlas_bytes();
    const float band_b = impostor::guard_band();
    const uint64_t ladder_b = part_flatten::active_ladder_shape_digest();
    impostor::PartImpostor part_b;
    part_b.clusters.resize(1);
    CHECK(impostor::bake_cluster(0, tris, ex, part_b.clusters[0]), "bake at 32 px");
    uint64_t hb = impostor::depicts_hash_begin();
    impostor::depicts_hash_add_cluster(hb, 0, tris, ex);
    const uint64_t depicts_b = impostor::depicts_hash_finish(hb);

    // 1. The setting reaches the bake at all.
    CHECK(cell_a == 16u && cell_b == 32u, "the env value reaches cell_px()");
    CHECK(bytes_b == bytes_a * 4u, "atlas bytes scale as cell_px^2");
    CHECK(part_a.clusters[0].atlas.size() == bytes_a &&
              part_b.clusters[0].atlas.size() == bytes_b,
          "each bake produced its own resolution's atlas");

    // 2. The guard band tracks it, holding the margin CONSTANT in texels --
    //    the invariant a fixed band silently broke every time the cell moved.
    const float margin_a = (0.5f - 0.5f / band_a) * float(cell_a);
    const float margin_b = (0.5f - 0.5f / band_b) * float(cell_b);
    CHECK(band_a > band_b, "a smaller cell needs a wider band");
    CHECK(margin_a > 1.0f && std::fabs(margin_a - margin_b) < 0.01f,
          "the guard margin is the same texel count at both resolutions");

    // 3. Both cache identities move, so nothing baked at one size can be
    //    served at the other: the atlas (depicts) and the FLAT (ladder shape,
    //    which matters because the band above changes the quad's half_extent).
    CHECK(depicts_a != depicts_b, "depicts-hash separates the two resolutions");
    CHECK(ladder_a != ladder_b, "ladder-shape digest separates them too");

    // 4. And the sidecar gate actually rejects, rather than mis-decoding an
    //    atlas of the wrong size. Written at 32 px, read back at 16 px.
    const uint64_t part_hash = 0x5150C0DE12345678ull;
    const std::string path =
        std::string(kCacheRoot) + "/" + impostor::cache_path_impostor(part_hash);
    CHECK(impostor::save(path, part_hash, depicts_b, part_b),
          "32 px sidecar written");
    pf_set_env("MATTER_IMPOSTOR_CELL_PX", "16");
    impostor::PartImpostor out;
    impostor::LoadFailure fail = impostor::LoadFailure::None;
    std::string reason;
    const bool loaded = impostor::load(path, part_hash, depicts_b, out, &fail,
                                       &reason);
    CHECK(!loaded, "a 32 px atlas does not load into a 16 px engine");
    CHECK(fail == impostor::LoadFailure::Version ||
              fail == impostor::LoadFailure::Stale,
          "and it is rejected as a format/staleness mismatch, not swallowed");
    CHECK(out.clusters.empty(), "nothing is handed back on rejection");

    // Restore the suite's pinned resolution for whatever runs after this.
    pf_set_env("MATTER_IMPOSTOR_CELL_PX", "32");
    printf("PASSED\n");
}

// The card's tint layer must carry the MATERIAL REGISTRY's albedo -- the same
// number vt_composite.comp uses for a slotless material -- and NOT TriEx::tint.
// Those are two independently authored colours; before this, a mesh rung that
// reached its VT page and the card that replaces it disagreed by whatever the
// content happened to author (~3x on world_demo's conifers).
//
// Also gates the IDENTITY: baking a registry value in means recolouring that
// material has to invalidate the card, or every existing atlas keeps the old
// colour while reporting Fresh.
static void test_impostor_bakes_material_albedo() {
    printf("=== test_impostor_bakes_material_albedo ===\n");
    const int kMat = 6;
    std::vector<Tri> tris = sphere_tris(12, 8);
    std::vector<TriEx> ex = triex_for(tris, kMat);   // fixture tint 0.5,0.25,0.75

    impostor::ClusterImpostor imp;
    CHECK(impostor::bake_cluster(0, tris, ex, imp), "bake ok");

    const MaterialDef* def = MaterialRegistryGet(kMat);
    CHECK(def != nullptr, "registry has the fixture's material");

    // First well-covered texel. Coverage lives in the SHADE layer's alpha;
    // the tint layer is the second layer at the same texel offset.
    const size_t layer = impostor::layer_bytes();
    const std::vector<uint8_t>& a = imp.atlas;
    CHECK(a.size() >= layer * 2, "atlas has both layers");
    size_t found = SIZE_MAX;
    for (size_t t = 0; t + 3 < layer; t += 4)
        if (a[t + 3] > 200u) { found = t; break; }
    CHECK(found != SIZE_MAX, "the bake covered at least one texel");

    if (found != SIZE_MAX) {
        const auto u8 = [](float v) {
            return int(v <= 0.f ? 0.f : (v >= 1.f ? 255.f : v * 255.f + 0.5f));
        };
        const int got[3] = {a[layer + found + 0], a[layer + found + 1],
                            a[layer + found + 2]};
        const int want[3] = {u8(def->albedo[0]), u8(def->albedo[1]),
                             u8(def->albedo[2])};
        // +-2 absorbs the supersample average and the pad dilation; the claim
        // is "this is the material's colour", not a bit-exact round trip.
        CHECK(std::abs(got[0] - want[0]) <= 2 &&
              std::abs(got[1] - want[1]) <= 2 &&
              std::abs(got[2] - want[2]) <= 2,
              "the tint layer carries the MATERIAL's registry albedo");
        // And is demonstrably NOT the vertex tint the fixture authored, which
        // is what it used to be. Without this the check above would pass on a
        // material whose albedo happened to resemble the tint.
        const int tint_u8[3] = {u8(0.5f), u8(0.25f), u8(0.75f)};
        CHECK(std::abs(got[0] - tint_u8[0]) > 2 ||
              std::abs(got[1] - tint_u8[1]) > 2 ||
              std::abs(got[2] - tint_u8[2]) > 2,
              "and NOT TriEx::tint (the pre-fix behaviour)");
        CHECK(a[layer + found + 3] >= 253u,
              "tint alpha is 1, so resolveBaseColor returns these bytes as-is");
    }

    // ---- identity failability -------------------------------------------
    // Same geometry, same material id, DIFFERENT registry albedo => different
    // depicts hash. Recolouring a material must invalidate the cards that
    // depict it; if this ever stops firing, stale cards validate as fresh.
    MaterialDef dyn{};
    MaterialRegistryDefaultDynamicDef(&dyn);
    dyn.albedo[0] = 0.11f; dyn.albedo[1] = 0.22f; dyn.albedo[2] = 0.33f;
    const int id_a = MaterialRegistryDefineDynamic(&dyn, "pf_albedo_probe");
    CHECK(id_a >= 0, "dynamic material defined");
    if (id_a >= 0) {
        std::vector<TriEx> dex = triex_for(tris, id_a);
        uint64_t h1 = impostor::depicts_hash_begin();
        impostor::depicts_hash_add_cluster(h1, 0, tris, dex);
        const uint64_t depicts_a = impostor::depicts_hash_finish(h1);

        MaterialRegistryResetDynamic();
        dyn.albedo[0] = 0.91f; dyn.albedo[1] = 0.92f; dyn.albedo[2] = 0.93f;
        const int id_b = MaterialRegistryDefineDynamic(&dyn, "pf_albedo_probe");
        CHECK(id_b == id_a, "the recoloured material reuses its id");
        uint64_t h2 = impostor::depicts_hash_begin();
        impostor::depicts_hash_add_cluster(h2, 0, tris, dex);
        const uint64_t depicts_b = impostor::depicts_hash_finish(h2);

        CHECK(depicts_a != depicts_b,
              "recolouring a material changes the depicts hash, so its cards "
              "rebake instead of validating stale");
        MaterialRegistryResetDynamic();
    }
    printf("PASSED\n");
}

static void test_impostor_sidecar_failability() {
    printf("=== test_impostor_sidecar_failability ===\n");
    std::vector<Tri> tris = sphere_tris(12, 8);
    std::vector<TriEx> ex = triex_for(tris, 2);
    impostor::PartImpostor part;
    part.clusters.resize(1);
    CHECK(impostor::bake_cluster(0, tris, ex, part.clusters[0]), "bake ok");

    uint64_t h = impostor::depicts_hash_begin();
    impostor::depicts_hash_add_cluster(h, 0, tris, ex);
    const uint64_t depicts = impostor::depicts_hash_finish(h);
    const uint64_t part_hash = 0xABCDEF0123456789ull;
    const std::string path =
        std::string(kCacheRoot) + "/" + impostor::cache_path_impostor(part_hash);

    CHECK(impostor::save(path, part_hash, depicts, part), "sidecar written");

    impostor::PartImpostor out;
    impostor::LoadFailure fail = impostor::LoadFailure::None;
    std::string reason;
    CHECK(impostor::load(path, part_hash, depicts, out, &fail, &reason),
          "sidecar loads");
    CHECK(fail == impostor::LoadFailure::None, "no failure reported on a good load");
    CHECK(out.clusters.size() == 1, "one cluster round-trips");
    if (out.clusters.size() == 1) {
        CHECK(out.clusters[0].atlas == part.clusters[0].atlas,
              "atlas bytes survive the round trip exactly");
        CHECK(out.clusters[0].half_extent == part.clusters[0].half_extent,
              "half extent round-trips");
        CHECK(out.clusters[0].material_index == part.clusters[0].material_index,
              "material index round-trips");
    }

    // Byte-identical file for a byte-identical bake: the .gtex double-bake
    // discipline applied to the atlas.
    const std::string path2 = path + ".second";
    impostor::PartImpostor part2;
    part2.clusters.resize(1);
    impostor::bake_cluster(0, tris, ex, part2.clusters[0]);
    CHECK(impostor::save(path2, part_hash, depicts, part2), "second sidecar written");
    {
        std::ifstream fa(path, std::ios::binary), fb(path2, std::ios::binary);
        std::string sa((std::istreambuf_iterator<char>(fa)),
                       std::istreambuf_iterator<char>());
        std::string sb((std::istreambuf_iterator<char>(fb)),
                       std::istreambuf_iterator<char>());
        CHECK(!sa.empty() && sa == sb, "two cold bakes write byte-identical sidecars");
    }

    // --- every rejection reports a distinct, named reason -------------------
    //
    // M4: asking THIS bundle for another part's atlas no longer reaches the
    // atlas at all -- the bundle is keyed on the part, so it rejects first and
    // reports Absent. That is a strictly better outcome, and it is asserted
    // here so the change is deliberate rather than discovered later.
    CHECK(!impostor::load(path, part_hash ^ 1ull, depicts, out, &fail, &reason) &&
              fail == impostor::LoadFailure::Absent,
          "another part's bundle does not hold this part's atlas (Absent)");

    // The atlas's OWN identity guard is defence in depth behind that, so it
    // needs a case the bundle cannot answer: a bundle correctly keyed on
    // part_hash whose IMPO payload names a different part. Only a corrupted or
    // mis-assembled bundle looks like this, which is exactly what the guard is
    // for.
    {
        const uint64_t kImposterHash = part_hash ^ 0x5A5A5A5Aull;
        const std::string foreign_payload = path + ".foreign";
        impostor::PartImpostor foreign;
        foreign.clusters.resize(1);
        impostor::bake_cluster(0, tris, ex, foreign.clusters[0]);
        CHECK(impostor::save(foreign_payload, kImposterHash, depicts, foreign),
              "foreign-payload atlas written under its own key");
        std::vector<uint8_t> payload;
        CHECK(part_bundle::read_section(foreign_payload, kImposterHash,
                                        part_bundle::kSectionImpostor, payload),
              "foreign atlas payload reads back");

        const std::string mismatched = path + ".mismatched";
        std::error_code mismatch_ec;
        fs::remove(mismatched, mismatch_ec);
        CHECK(part_bundle::write_section(mismatched, part_hash,
                                         part_bundle::kSectionImpostor,
                                         payload.data(), payload.size()),
              "mismatched bundle published: keyed on this part, payload names another");
        CHECK(!impostor::load(mismatched, part_hash, depicts, out, &fail, &reason) &&
                  fail == impostor::LoadFailure::Identity,
              "an atlas naming another part is rejected as Identity");
    }
    CHECK(!impostor::load(path, part_hash, depicts ^ 1ull, out, &fail, &reason) &&
              fail == impostor::LoadFailure::Stale,
          "an atlas depicting another mesh is rejected as Stale");
    CHECK(!impostor::load(path + ".missing", part_hash, depicts, out, &fail, &reason) &&
              fail == impostor::LoadFailure::Absent,
          "a missing sidecar reports Absent");

    // M4: the atlas is a bundle SECTION, so its own guards -- checksum,
    // truncation, magic -- are exercised by corrupting the section, not the
    // file. Corrupting the file would be caught one layer earlier by the
    // bundle's directory checksum and reported as Absent, which would test the
    // container rather than the atlas.
    std::vector<uint8_t> atlas;
    CHECK(part_bundle::read_section(path, part_hash, part_bundle::kSectionImpostor, atlas),
          "atlas section reads back");
    CHECK(atlas.size() > 4096, "atlas section has a payload to corrupt");

    const auto republish = [&](const std::string& dst, const std::vector<uint8_t>& payload) {
        std::error_code copy_ec;
        fs::remove(dst, copy_ec);
        return part_bundle::write_section(dst, part_hash,
                                          part_bundle::kSectionImpostor,
                                          payload.data(), payload.size());
    };

    // Corrupt one atlas byte deep in the payload: the checksum must catch it.
    {
        const std::string corrupt = path + ".corrupt";
        std::vector<uint8_t> copy = atlas;
        copy[copy.size() / 2] ^= 0x5A;
        CHECK(republish(corrupt, copy), "corrupt fixture published");
        CHECK(!impostor::load(corrupt, part_hash, depicts, out, &fail, &reason) &&
                  fail == impostor::LoadFailure::Checksum,
              "one flipped atlas byte is rejected as Checksum");
        CHECK(out.clusters.empty(), "a rejected load leaves nothing behind");
    }
    // Truncation and a bad magic are separate reasons, not one catch-all.
    {
        const std::string trunc = path + ".trunc";
        std::vector<uint8_t> shortened(atlas.begin(), atlas.end() - 64);
        CHECK(republish(trunc, shortened), "truncated fixture published");
        CHECK(!impostor::load(trunc, part_hash, depicts, out, &fail, &reason) &&
                  fail == impostor::LoadFailure::Truncated,
              "a short atlas is rejected as Truncated");
        const std::string magic = path + ".magic";
        std::vector<uint8_t> copy = atlas;
        copy[0] = 'X';
        CHECK(republish(magic, copy), "bad-magic fixture published");
        CHECK(!impostor::load(magic, part_hash, depicts, out, &fail, &reason) &&
                  fail == impostor::LoadFailure::Header,
              "a foreign atlas is rejected as Header");
    }
    printf("PASSED\n");
}

static void test_flatten_appends_impostor_rung() {
    printf("=== test_flatten_appends_impostor_rung ===\n");
    const std::string flat =
        std::string(kCacheRoot) + "/" + part_asset::cache_path_flat(kSmallSphereHash);
    const std::string fimp =
        std::string(kCacheRoot) + "/" + impostor::cache_path_impostor(kSmallSphereHash);
    part_bundle::remove_section(flat, kSmallSphereHash, part_bundle::kSectionFlat);
    part_bundle::remove_section(fimp, kSmallSphereHash, part_bundle::kSectionImpostor);

    uint64_t hash = write_small_sphere_part();
    CHECK(hash != 0, "small sphere fixture written");
    if (hash == 0) { printf("  SKIPPING\n"); return; }

    auto res = part_flatten::flatten_part(kCacheRoot, hash);
    CHECK(res.ok, "flatten ok");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); return; }
    CHECK(res.impostors > 0, "the ladder bottom-out point earned an impostor");
    CHECK(part_bundle::has_section(fimp, kSmallSphereHash, part_bundle::kSectionImpostor),
          "the atlas landed in the part's bundle beside the flat ladder");

    BLASManager blas; TLASManager tlas(4);
    std::vector<part_asset::FlatCluster> clusters;
    CHECK(part_asset::load_flat_v3(flat, hash, blas, tlas, clusters), "flat loads");
    CHECK(!clusters.empty(), "flat has clusters");
    if (clusters.empty()) { printf("FAILED\n"); return; }

    // The impostor is an ORDINARY rung: same lods table, same blas_indices,
    // its own threshold. Nothing about it is a separate list.
    uint64_t depicts = impostor::depicts_hash_begin();
    size_t impostor_rungs = 0, terminal_mesh_tris = 0;
    for (size_t ci = 0; ci < clusters.size(); ++ci) {
        const auto& lods = clusters[ci].lods;
        CHECK(lods.size() >= 2, "cluster has a ladder with a terminal");
        if (lods.size() < 2) continue;
        const auto& last = blas.get_entries()[lods.back().blas_indices[0]];
        if (last->triangles.size() != 2 || last->tri_extra.size() != 2 ||
            !(last->tri_extra[0].uv0.x >= impostor::kQuadMarker))
            continue;
        ++impostor_rungs;
        // TWO DIFFERENT RUNGS, deliberately:
        //   - the eligibility floor is about the rung being REPLACED, so it
        //     still reads lods[size-2];
        //   - what the atlas DEPICTS is rep 0, so the depicts-hash folds
        //     lods[0]. Conflating them is what made the first run of this
        //     change fail three tests at once.
        const auto& replaced = blas.get_entries()[lods[lods.size() - 2].blas_indices[0]];
        terminal_mesh_tris = replaced->triangles.size();
        const auto& depicted = blas.get_entries()[lods[0].blas_indices[0]];
        impostor::depicts_hash_add_cluster(depicts, static_cast<uint32_t>(ci),
                                           depicted->triangles,
                                           depicted->tri_extra);
        // The rung the impostor takes over from now has a REAL switch
        // threshold; before M2.5 the terminal rung threshold was always 0.
        CHECK(lods[lods.size() - 2].screen_size_threshold > 0.0f,
              "the terminal mesh rung gained a switch distance");
        CHECK(lods.back().screen_size_threshold == 0.0f,
              "the impostor is the terminal rung (threshold 0 = always qualifies)");
    }
    CHECK(impostor_rungs == res.impostors,
          "every impostor the bake reported is present in the ladder");
    CHECK(terminal_mesh_tris >= impostor::kMinTerminalTris,
          "the mesh the impostor replaces cleared the eligibility floor");
    printf("  terminal mesh rung = %zu tris -> impostor = 2 tris\n", terminal_mesh_tris);

    // The sidecar written by this bake validates against the ladder in the
    // artifact -- which is exactly the check part_store performs at load.
    impostor::PartImpostor loaded;
    impostor::LoadFailure fail = impostor::LoadFailure::None;
    std::string reason;
    CHECK(impostor::load(fimp, hash, impostor::depicts_hash_finish(depicts),
                         loaded, &fail, &reason),
          "the sidecar validates against the ladder it was baked with");
    if (fail != impostor::LoadFailure::None) printf("  reason: %s\n", reason.c_str());
    CHECK(loaded.clusters.size() == impostor_rungs, "one atlas per impostor rung");
    printf("PASSED\n");
}

int main() {
    // Unbuffered: an abort deep in a bake must not swallow the log that
    // says which test was running when it happened.
    setvbuf(stdout, nullptr, _IONBF, 0);
    // PIN THE CELL RESOLUTION for this suite. The impostor bake is O(cell_px^2)
    // per view, so inheriting the 128 px default would make every impostor test
    // here 16x slower -- this suite already runs for minutes -- while testing
    // nothing the 32 px path does not. test_impostor_cell_px_parametrized is
    // where the setting itself is exercised, and it restores this value.
    pf_set_env("MATTER_IMPOSTOR_CELL_PX", "32");
    if (!write_fixtures()) {
        printf("FAIL: could not write fixture parts under %s\n", kCacheRoot);
        return 1;
    }
    test_impostor_cell_px_parametrized();
    // Early on purpose: the padding assertions are the cheapest tripwire for
    // an atlas-content regression, and a broken bake should say so before the
    // suite spends minutes on flattens.
    test_impostor_atlas_padding();
    test_flatten_merge();
    test_flatten_deterministic();
    test_flatten_missing_part();
    test_error_bound_calibration();
    test_open_grid_border_preserved();
    test_reproject_two_materials();
    test_topological_boundary_lock();
    test_cluster_split_40k();
    test_cluster_split_small();
    test_cluster_split_deterministic();
    test_v3_round_trip();
    test_v3_empty_children_and_lods();
    test_v3_cross_version_guards();
    test_peek_format_version();
    test_v2_byte_stability();
    test_flatten_clustered_v3();
    test_flatten_watertight_invariant();
    test_small_part_gets_ladder();
    test_ratio2_ladder_shape();
    test_budget_ladder_assembly();
    test_authored_ladder_switch_distances();
    test_authored_ladder_impostor_terminal();
    test_no_impostor_optout_terminal();   // kRepresentation 1->2: static noImpostor
    test_impostor_source_and_distance();
    test_w5_plan_does_not_drive_the_ladder();
    test_instance_boundary_records_refs();
    test_generous_budget_inlines();
    test_flat_version_bump();
    test_cutover_helpers();
    test_flatten_segmented();
    test_flatten_unhinted_unchanged();
    test_flatten_retain_budget_identical();
    test_flatten_trace_spans();      // Bake Lab task 1.5 instrumentation guard
    test_lod_rung_trace_spans();     // Bake Lab task 1.5 instrumentation guard
    test_bake_lods_observer_rungs(); // Bake Lab W3: per-rung bake observer seam

    test_emitter_flat_round_trip();  // volumetric emitter metadata persistence

    test_impostor_eligibility();          // M2.5 terminal impostor rep
    test_impostor_bake_deterministic();
    test_impostor_view_index_inverse();
    test_impostor_elevation_rings_differ();
    test_impostor_quad_shape();
    test_impostor_bakes_material_albedo();
    test_impostor_sidecar_failability();
    test_flatten_appends_impostor_rung();
    test_mesh_rung_cap_and_stale_rejection();  // the cap + its staleness gate

    if (g_failures == 0) { printf("part_flatten_tests: ALL PASS\n"); return 0; }
    printf("part_flatten_tests: %d FAILURE(S)\n", g_failures);
    return 1;
}

static void test_emitter_flat_round_trip() {
    printf("=== test_emitter_flat_round_trip ===\n");

    BLASManager blas_out;
    TLASManager tlas_out(16);
    auto handles = make_blas_n(blas_out, 1, 4);

    std::vector<part_asset::FlatCluster> clusters(1);
    clusters[0].aabb_min[0] = 0; clusters[0].aabb_min[1] = 0; clusters[0].aabb_min[2] = 0;
    clusters[0].aabb_max[0] = 1; clusters[0].aabb_max[1] = 1; clusters[0].aabb_max[2] = 1;
    part_asset::LodLevel l0;
    l0.screen_size_threshold = 100.0f;
    l0.blas_indices.push_back(blas_handle_index(blas_out, handles[0]));
    clusters[0].lods.push_back(std::move(l0));

    part_asset::VolumeEmitter e{};
    e.pos[0] = 0; e.pos[1] = 5; e.pos[2] = 0;
    e.dir[0] = 0; e.dir[1] = 1; e.dir[2] = 0;
    e.radius = 1.5f; e.spread = 0.2f; e.length = 10.0f;
    e.density = 0.7f;
    e.color[0] = 0.8f; e.color[1] = 0.85f; e.color[2] = 0.9f;
    e.rise = 1.0f; e.turbulence = 0.5f;
    std::vector<part_asset::VolumeEmitter> emitters_out = {e};

    const uint64_t hash = 0xEE11AABB00CC0000ull;
    const std::string path = std::string(kCacheRoot) + "/parts/test_emitter_flat.flat.part";

    bool saved = part_asset::save_flat_v3(path, blas_out, tlas_out, clusters,
                                          std::vector<part_asset::FlatInstanceRef>{},
                                          hash, emitters_out);
    CHECK(saved, "emitter_flat: save_flat_v3 with emitters succeeds");

    BLASManager blas_in;
    TLASManager tlas_in(16);
    std::vector<part_asset::FlatCluster> clusters_in;
    std::vector<part_asset::FlatInstanceRef> refs_in;
    std::vector<part_asset::VolumeEmitter> emitters_in;
    bool loaded = part_asset::load_flat_v3(path, hash, blas_in, tlas_in,
                                           clusters_in, refs_in, emitters_in);
    CHECK(loaded, "emitter_flat: load_flat_v3 with emitters succeeds");
    CHECK(emitters_in.size() == 1u, "emitter_flat: one emitter round-trips");
    if (!emitters_in.empty()) {
        CHECK(std::abs(emitters_in[0].radius - 1.5f) < 1e-5f,
              "emitter_flat: radius round-trips");
        CHECK(std::abs(emitters_in[0].density - 0.7f) < 1e-5f,
              "emitter_flat: density round-trips");
        CHECK(std::abs(emitters_in[0].color[2] - 0.9f) < 1e-5f,
              "emitter_flat: color[2] round-trips");
    }

    // Verify that load_flat_v3 WITHOUT emitter param still works (back-compat).
    BLASManager blas_bc;
    TLASManager tlas_bc(16);
    std::vector<part_asset::FlatCluster> clusters_bc;
    std::vector<part_asset::FlatInstanceRef> refs_bc;
    bool loaded_bc = part_asset::load_flat_v3(path, hash, blas_bc, tlas_bc,
                                              clusters_bc, refs_bc);
    CHECK(loaded_bc, "emitter_flat: back-compat load_flat_v3 still works");
}
