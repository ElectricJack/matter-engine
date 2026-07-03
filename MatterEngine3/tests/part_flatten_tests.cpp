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

    if (failures == 0) { printf("part_flatten_tests: ALL PASS\n"); return 0; }
    printf("part_flatten_tests: %d FAILURE(S)\n", failures);
    return 1;
}
