// Bake-time subtree flattening + error-bounded LOD ladder tests.
// Harness convention mirrors composition_tests.cpp (CHECK + failures counter).
//
// Fixtures: synthetic parent/child .part v2 files written into a temp cache dir
// (parts/<hash>.part), then flatten_part() merges them and we verify the flat
// artifact via load_v2.
#include "../include/part_flatten.h"
#include "../include/part_asset_v2.h"
#include "../include/lod_bake.h"
#include "../include/part_cluster.h"
#include "../../MatterSurfaceLib/include/blas_manager.hpp"
#include "../../MatterSurfaceLib/include/tlas_manager.hpp"
#include "../../MatterSurfaceLib/include/mesh_simplifier.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
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
    CHECK(res.clusters >= 1, "result has at least 1 cluster");

    // Task 11: flatten writes v3; load_flat_v3 must succeed.
    uint32_t fv = part_asset::peek_format_version(flat_path());
    CHECK(fv == 3, "flat artifact is v3 (peek_format_version == 3)");

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
    std::remove(flat_path().c_str());
    part_flatten::FlattenResult a = part_flatten::flatten_part(kCacheRoot, kParentHash);
    std::vector<char> bytes_a;
    CHECK(a.ok && read_bytes(flat_path(), bytes_a), "first flatten written (v3)");
    CHECK(part_asset::peek_format_version(flat_path()) == 3, "first flatten is v3");

    std::remove(flat_path().c_str());
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
static uint64_t write_small_sphere_part(const std::string& cache_root) {
    (void)cache_root; // kCacheRoot is the same value used by save_fixture
    // segs=20, rings=10 => ~20*10*2=400 tris (well under old min_tris=2000)
    std::vector<Tri> tris = sphere_tris(20, 10);
    if (!save_fixture(kSmallSphereHash, 5, {tris}, {})) return 0;
    return kSmallSphereHash;
}

// Write a dense sphere part (>=20k tris) as a childless .part in the cache.
// Returns kDenseSphereHash on success, 0 on failure.
static uint64_t write_dense_sphere_part(const std::string& cache_root) {
    (void)cache_root;
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
    std::vector<TriEx> ex = lod_bake::reproject_triex(dec, tris, triex);
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

    // Write a v2 file using save_v2 (parent fixture already written above).
    // load_flat_v3 on a v2 file must return false.
    const std::string v2_path = std::string(kCacheRoot) + "/" +
                                part_asset::cache_path_resolved(kParentHash);
    BLASManager bv2; TLASManager tv2(16);
    std::vector<part_asset::ChildInstance> ch;
    part_asset::LodLevels lv2;
    bool v2_ok = part_asset::load_v2(v2_path, kParentHash, bv2, tv2, ch, lv2);
    CHECK(v2_ok, "cross-guard: v2 file loads as v2 (sanity)");

    BLASManager bv3_a; TLASManager tv3_a(16);
    std::vector<part_asset::FlatCluster> dummy;
    bool v3_on_v2 = part_asset::load_flat_v3(v2_path, kParentHash, bv3_a, tv3_a, dummy);
    CHECK(!v3_on_v2, "cross-guard: load_flat_v3 on a v2 file returns false");

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

    // v2 file (parent fixture).
    const std::string v2_path = std::string(kCacheRoot) + "/" +
                                part_asset::cache_path_resolved(kParentHash);
    uint32_t pv2 = part_asset::peek_format_version(v2_path);
    CHECK(pv2 == 2, "peek returns 2 for a v2 file");

    // v3 file (created in cross-version guard test or create a fresh one).
    BLASManager bpk; TLASManager tpk(16);
    make_blas_n(bpk, 1, 0);
    part_asset::FlatCluster fcp;
    fcp.aabb_min[0] = fcp.aabb_min[1] = fcp.aabb_min[2] = 0.0f;
    fcp.aabb_max[0] = fcp.aabb_max[1] = fcp.aabb_max[2] = 1.0f;
    part_asset::LodLevel lvp;
    lvp.screen_size_threshold = 0.0f; lvp.blas_indices.push_back(0);
    fcp.lods.push_back(std::move(lvp));
    std::vector<part_asset::FlatCluster> clspk = { fcp };
    const uint64_t kPeekHash = 0x9988776655443322ull;
    const std::string v3_path = std::string(kCacheRoot) + "/parts/test_peek_v3.flat.part";
    bool spk = part_asset::save_flat_v3(v3_path, bpk, tpk, clspk, kPeekHash);
    CHECK(spk, "peek: v3 file saved");
    uint32_t pv3 = part_asset::peek_format_version(v3_path);
    CHECK(pv3 == 3, "peek returns 3 for a v3 file");

    // Garbage / non-existent file.
    uint32_t pg = part_asset::peek_format_version("/tmp/__no_such_file_matter_v3__.part");
    CHECK(pg == 0, "peek returns 0 for a non-existent file");

    // Write a garbage file (wrong magic).
    const std::string garbage_path = std::string(kCacheRoot) + "/parts/garbage.part";
    {
        FILE* fg = std::fopen(garbage_path.c_str(), "wb");
        if (fg) {
            uint32_t bad_magic = 0xDEADDEADu;
            uint32_t bad_version = 99u;
            std::fwrite(&bad_magic, 4, 1, fg);
            std::fwrite(&bad_version, 4, 1, fg);
            std::fclose(fg);
        }
    }
    uint32_t pbad = part_asset::peek_format_version(garbage_path);
    CHECK(pbad == 0, "peek returns 0 for a wrong-magic file");

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
    std::remove(big_flat.c_str());

    part_flatten::FlattenTargets tgt;
    // Use tight cluster size so we definitely get multiple clusters.
    tgt.cluster_target_tris = 16000;

    part_flatten::FlattenResult res = part_flatten::flatten_part(kCacheRoot, kBigHash, tgt);
    CHECK(res.ok, "big mesh: flatten_part ok");
    if (!res.ok) { printf("  error: %s\n", res.error.c_str()); return; }

    CHECK(res.full_tris == 40000u, "big mesh: full_tris == 40000");
    CHECK(res.clusters > 1, "big mesh: result.clusters > 1 (split required)");
    printf("  clusters=%zu, levels=%zu, full_tris=%zu\n", res.clusters, res.levels, res.full_tris);

    // Verify v3 format.
    uint32_t fv = part_asset::peek_format_version(big_flat);
    CHECK(fv == 3, "big mesh: flat artifact is v3");

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

        // Check every coarser level.
        for (size_t li = 1; li < clusters_in[ci].lods.size(); ++li) {
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
    std::remove(flat.c_str());

    uint64_t hash = write_small_sphere_part(kCacheRoot);
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
    std::remove(flat.c_str());

    uint64_t hash = write_dense_sphere_part(kCacheRoot);
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

int main() {
    if (!write_fixtures()) {
        printf("FAIL: could not write fixture parts under %s\n", kCacheRoot);
        return 1;
    }
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

    if (failures == 0) { printf("part_flatten_tests: ALL PASS\n"); return 0; }
    printf("part_flatten_tests: %d FAILURE(S)\n", failures);
    return 1;
}
