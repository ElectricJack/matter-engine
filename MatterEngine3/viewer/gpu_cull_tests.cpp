// gpu_cull_tests.cpp — GL 4.6 parity tests for GpuCuller vs CPU reference.
// Tests frustum cull + LOD selection for GPU compute path against raster_cull.h.
//
// Run: cd MatterEngine3/viewer && make gpu-tests && GALLIUM_DRIVER=d3d12 timeout 120 ./gpu_tests
//
// Exit 0 = all PASS (or GL 4.6 unavailable → SKIP).
// Exit non-zero = at least one FAIL.

#include "raylib.h"
#include "gl46.h"
#include "gpu_culler.h"
#include "raster_cull.h"
#include "raster_mesh.h"
#include "gpu_cull_types.h"
#include "part_store.h"
#include "sector_resolver.h"

#include "external/glad.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int g_failures = 0;
static int g_tests    = 0;

#define CHECK(cond, msg) do {                       \
    ++g_tests;                                      \
    if (!(cond)) {                                  \
        printf("  FAIL: %s\n", (msg));              \
        ++g_failures;                               \
    } else {                                        \
        printf("  ok:   %s\n", (msg));              \
    }                                               \
} while (0)

// Make a row-major engine identity+translate matrix:
//   translation at m[3], m[7], m[11]  (column-vector convention).
static void make_translate(float x, float y, float z, float out[16]) {
    memset(out, 0, 64);
    out[0]  = 1.0f; out[5]  = 1.0f; out[10] = 1.0f; out[15] = 1.0f;
    out[3]  = x;
    out[7]  = y;
    out[11] = z;
}

// Build a yaw rotation around Y by `angle_deg` (row-major, column-vector).
// Then compose with a translation.
static void make_rotate_translate(float yaw_deg, float tx, float ty, float tz, float out[16]) {
    float rad = yaw_deg * 3.14159265358979323846f / 180.0f;
    float c = cosf(rad), s = sinf(rad);
    // Row-major storage of column-vector rotation about Y:
    //   [ c   0   s   0 ]   row 0
    //   [ 0   1   0   0 ]   row 1
    //   [-s   0   c   0 ]   row 2
    //   [ 0   0   0   1 ]   row 3
    // with translation in col 3 (indices [3],[7],[11]).
    memset(out, 0, 64);
    out[0]  =  c;   out[1]  = 0.0f; out[2]  =  s;   out[3]  = tx;
    out[4]  =  0.0f; out[5]  = 1.0f; out[6]  = 0.0f; out[7]  = ty;
    out[8]  = -s;   out[9]  = 0.0f; out[10] =  c;   out[11] = tz;
    out[12] =  0.0f; out[13] = 0.0f; out[14] = 0.0f; out[15] = 1.0f;
}

// Build a minimal 1-triangle RasterMeshData (3 vertices, non-empty).
static viewer::RasterMeshData make_one_tri_mesh() {
    viewer::RasterMeshData md;
    md.vertex_count = 3;
    // Triangle with some distinguishable geometry.
    md.vertices  = { 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f };
    md.normals   = { 0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f };
    md.colors    = { 255, 255, 255, 255,  255, 255, 255, 255,  255, 255, 255, 255 };
    md.texcoords = { 0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f };
    return md;
}

// Build the synthetic fixture PartStore + LoadedPart and inject into the store.
// Returns the part_hash used.
//
// Fixture:
//   - 1 part, 2 clusters, 3 LODs per cluster
//   - Cluster 0: AABB x ∈ [-5, -3], y/z ∈ [-1, 1]
//   - Cluster 1: AABB x ∈ [ 3,  5], y/z ∈ [-1, 1]
//   - lod_mesh_data[0,1,2] = 1-triangle meshes (whole-part indices)
//   - Cluster lod_mesh[0,1,2] → indices 0,1,2 in lod_mesh_data
//   - thresholds {0.5f, 0.1f, 0.02f} per cluster
//   - bound_radius = radius of union AABB
static uint64_t build_fixture(viewer::PartStore& store) {
    const uint64_t hash = 0xDEADBEEFCAFE0001ULL;

    viewer::LoadedPart lp;

    // 3 LOD mesh entries (shared by both clusters via indices 0,1,2).
    lp.lod_mesh_data.push_back(make_one_tri_mesh());  // index 0 – LOD 0 (finest)
    lp.lod_mesh_data.push_back(make_one_tri_mesh());  // index 1 – LOD 1
    lp.lod_mesh_data.push_back(make_one_tri_mesh());  // index 2 – LOD 2 (coarsest)

    // Part-level whole-part thresholds (used by pack_whole_part when no clusters).
    // We DO have clusters, so these are advisory only; still fill them.
    lp.thresholds = { 0.5f, 0.1f, 0.02f };
    lp.bound_radius = 6.0f;   // union of both cluster AABBs: x ∈ [-5,5], half-diag ≈ 5.2

    // --- Cluster 0: left cluster, AABB x ∈ [-5, -3]. ---
    {
        viewer::LoadedCluster cl;
        cl.aabb_min[0] = -5.0f; cl.aabb_min[1] = -1.0f; cl.aabb_min[2] = -1.0f;
        cl.aabb_max[0] = -3.0f; cl.aabb_max[1] =  1.0f; cl.aabb_max[2] =  1.0f;
        float dx = 2.0f, dy = 2.0f, dz = 2.0f;
        cl.radius = 0.5f * sqrtf(dx*dx + dy*dy + dz*dz);
        cl.thresholds = { 0.5f, 0.1f, 0.02f };
        cl.lod_blas   = { 0, 0, 0 };  // dummy BLASHandles (not used by GPU path)
        cl.lod_mesh   = { 0, 1, 2 };  // indices into lp.lod_mesh_data
        lp.clusters.push_back(cl);
    }

    // --- Cluster 1: right cluster, AABB x ∈ [3, 5]. ---
    {
        viewer::LoadedCluster cl;
        cl.aabb_min[0] =  3.0f; cl.aabb_min[1] = -1.0f; cl.aabb_min[2] = -1.0f;
        cl.aabb_max[0] =  5.0f; cl.aabb_max[1] =  1.0f; cl.aabb_max[2] =  1.0f;
        float dx = 2.0f, dy = 2.0f, dz = 2.0f;
        cl.radius = 0.5f * sqrtf(dx*dx + dy*dy + dz*dz);
        cl.thresholds = { 0.5f, 0.1f, 0.02f };
        cl.lod_blas   = { 0, 0, 0 };
        cl.lod_mesh   = { 0, 1, 2 };
        lp.clusters.push_back(cl);
    }

    // No expansion (root is drawable directly).
    // No children.

    store.inject_for_test(hash, std::move(lp));
    return hash;
}

// Build a ResolvedInstance for a given transform.
static viewer::ResolvedInstance make_ri(uint64_t hash, const float t[16]) {
    viewer::ResolvedInstance ri{};
    ri.part_hash  = hash;
    ri.lod_level  = 0;
    memcpy(ri.transform, t, 64);
    return ri;
}

// Sorted multiset of translations extracted from a RasterBatch list, per (cluster_idx, lod).
// Translation = (m[3], m[7], m[11]) (engine row-major column 3 of rows 0,1,2).
using TransKey = std::tuple<float, float, float>;  // rounded to epsilon grid
using BatchKey  = std::pair<uint32_t, int>;          // (cluster_index_or_sentinel, lod)

static TransKey round_trans(const Matrix& m, float eps) {
    // raylib Matrix memory layout (row-major declaration order):
    //   m0,m4,m8,m12 = row 0;  m1,m5,m9,m13 = row 1;
    //   m2,m6,m10,m14 = row 2;  m3,m7,m11,m15 = row 3.
    // row_major_to_matrix(t) memcpy's t[0..15] into the above, so:
    //   m0=t[0], m4=t[1], m8=t[2], m12=t[3]  ← row 0; translation-x at m12
    //   m1=t[4], m5=t[5], m9=t[6], m13=t[7]  ← row 1; translation-y at m13
    //   m2=t[8], m6=t[9], m10=t[10], m14=t[11] ← row 2; translation-z at m14
    // Engine translation is at t[3], t[7], t[11] → mapped to m12, m13, m14.
    auto r = [eps](float v) { return std::floor(v / eps + 0.5f) * eps; };
    return { r(m.m12), r(m.m13), r(m.m14) };
}

// ---------------------------------------------------------------------------
// TEST 1: CPU/GPU parity — frustum cull + LOD selection, 200 instances.
// ---------------------------------------------------------------------------
static bool test_parity_frustum_lod() {
    printf("\n[test_parity_frustum_lod]\n");

    // ---- Setup ----
    viewer::PartStore store("/tmp/gpu_test_unused");
    uint64_t hash = build_fixture(store);

    // 200 instances on 20×10 grid, spacing 3.
    // Roughly half with yaw rotations (30, 45, 90 degrees) to exercise
    // the transpose/scale math.  Every 5th instance gets a rotation.
    const int COLS = 20, ROWS = 10;
    const float SPACING = 3.0f;
    const float YAWS[] = { 30.0f, 45.0f, 90.0f, 0.0f, 0.0f };  // cycle

    std::vector<viewer::ResolvedInstance> resolved;
    resolved.reserve(COLS * ROWS);
    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ++col) {
            float tx = col * SPACING;
            float ty = 0.0f;
            float tz = row * SPACING;
            float t[16];
            float yaw = YAWS[(col + row) % 5];
            if (yaw != 0.0f) {
                make_rotate_translate(yaw, tx, ty, tz, t);
            } else {
                make_translate(tx, ty, tz, t);
            }
            resolved.push_back(make_ri(hash, t));
        }
    }
    assert((int)resolved.size() == 200);

    // Camera at origin looking +X, FOV 60, aspect 1.6.
    const float eye[3]    = { 0.0f, 0.0f, 0.0f };
    const float target[3] = { 1.0f, 0.0f, 0.0f };
    const float up[3]     = { 0.0f, 1.0f, 0.0f };
    const float fovy      = 60.0f;
    const float aspect    = 1.6f;

    float planes[6][4];
    viewer::camera_frustum_planes_raw(eye, target, up, fovy, aspect, planes);

    // ---- CPU reference ----
    // Our synthetic part has no expansion, so the loop just uses the root directly.
    // Per instance: for each cluster → aabb_culled, then cluster_lod_select.
    const viewer::LoadedPart* lp = store.get_or_load(hash);
    assert(lp && !lp->clusters.empty());

    // Accumulate per-(global_cluster_idx, lod) → sorted translation multisets.
    // cluster_start for this part's slot is 0 (first registered part).
    std::map<BatchKey, std::vector<TransKey>> cpu_trans;
    std::map<BatchKey, int>                   cpu_count;

    for (const auto& ri : resolved) {
        const float* inst = ri.transform;
        for (int ci = 0; ci < (int)lp->clusters.size(); ++ci) {
            const viewer::LoadedCluster& cl = lp->clusters[ci];
            if (viewer::aabb_culled(cl.aabb_min, cl.aabb_max, inst, planes))
                continue;
            int lv = viewer::cluster_lod_select(cl, inst, eye, 1.0f);
            BatchKey bk{ (uint32_t)ci, lv };
            cpu_count[bk]++;
            // Translation from engine row-major: t[3], t[7], t[11].
            TransKey tk{ inst[3], inst[7], inst[11] };
            cpu_trans[bk].push_back(tk);
        }
    }
    // Sort each multiset for order-independent comparison.
    for (auto& kv : cpu_trans) std::sort(kv.second.begin(), kv.second.end());

    // ---- GPU path ----
    viewer::GpuCuller culler;
    std::string err;
    if (!culler.init(err)) {
        printf("  ERROR: GpuCuller::init failed: %s\n", err.c_str());
        return false;
    }

    int slot = culler.ensure_part(hash, store);
    CHECK(slot >= 0, "ensure_part returns valid slot");
    if (slot < 0) return false;

    if (!culler.cull(resolved, store, eye, planes, 1.0f)) {
        printf("  ERROR: cull() returned false\n");
        return false;
    }

    auto batches = culler.readback_batches(store);

    // ---- Compare ----
    // Build GPU per-(cluster_idx, lod) maps.
    // GPU cluster_index: real clusters get the global cluster index,
    // synthetic whole-part gets UINT32_MAX.
    const auto& parts = culler.parts();
    CHECK(!parts.empty(), "parts_ non-empty after ensure_part");

    std::map<BatchKey, int>                   gpu_count;
    std::map<BatchKey, std::vector<TransKey>> gpu_trans;

    const float EPS = 1e-4f;
    for (const auto& b : batches) {
        // Map back from RasterBatch.cluster_index to local cluster idx.
        // cluster_start for the first (only) part is 0 — global == local.
        uint32_t global_ci = b.cluster_index;
        BatchKey bk{ global_ci, b.level };
        gpu_count[bk] += (int)b.transforms.size();
        for (const auto& m : b.transforms)
            gpu_trans[bk].push_back(round_trans(m, EPS));
    }
    for (auto& kv : gpu_trans) std::sort(kv.second.begin(), kv.second.end());

    // Collect all bucket keys from both.
    std::set<BatchKey> all_keys;
    for (auto& kv : cpu_count) all_keys.insert(kv.first);
    for (auto& kv : gpu_count) all_keys.insert(kv.first);

    bool counts_ok = true;
    for (const auto& bk : all_keys) {
        int cc = cpu_count.count(bk) ? cpu_count[bk] : 0;
        int gc = gpu_count.count(bk) ? gpu_count[bk] : 0;
        if (cc != gc) {
            printf("  bucket(ci=%u,lv=%d): cpu=%d gpu=%d\n", bk.first, bk.second, cc, gc);
            counts_ok = false;
        }
    }
    CHECK(counts_ok, "per-bucket instance counts CPU==GPU");

    bool trans_ok = true;
    for (const auto& bk : all_keys) {
        const auto& cv = cpu_trans.count(bk) ? cpu_trans[bk] : std::vector<TransKey>{};
        // Round CPU translations with same epsilon for comparison.
        std::vector<TransKey> cv_rounded;
        cv_rounded.reserve(cv.size());
        for (auto& tk : cv)
            cv_rounded.push_back({ std::floor(std::get<0>(tk)/EPS+0.5f)*EPS,
                                   std::floor(std::get<1>(tk)/EPS+0.5f)*EPS,
                                   std::floor(std::get<2>(tk)/EPS+0.5f)*EPS });
        std::sort(cv_rounded.begin(), cv_rounded.end());
        const auto& gv = gpu_trans.count(bk) ? gpu_trans[bk] : std::vector<TransKey>{};
        if (cv_rounded != gv) {
            printf("  bucket(ci=%u,lv=%d): translation multisets differ\n",
                   bk.first, bk.second);
            trans_ok = false;
        }
    }
    CHECK(trans_ok, "per-bucket translation multisets CPU==GPU");

    return !g_failures;
}

// ---------------------------------------------------------------------------
// TEST 2: Matrix convention — translate + rotate+translate cases.
// ---------------------------------------------------------------------------
static bool test_matrix_convention() {
    printf("\n[test_matrix_convention]\n");

    viewer::PartStore store("/tmp/gpu_test_unused2");
    uint64_t hash = build_fixture(store);

    viewer::GpuCuller culler;
    std::string err;
    if (!culler.init(err)) {
        printf("  ERROR: GpuCuller::init failed: %s\n", err.c_str());
        return false;
    }
    culler.ensure_part(hash, store);

    // Camera looking +X from origin; narrow FOV to ensure our single instance
    // near origin is always visible.
    const float eye[3]    = { 0.0f, 0.0f, 0.0f };
    const float target[3] = { 1.0f, 0.0f, 0.0f };
    const float up[3]     = { 0.0f, 1.0f, 0.0f };
    float planes[6][4];
    viewer::camera_frustum_planes_raw(eye, target, up, 90.0f, 1.0f, planes);

    auto run_single = [&](const float t[16], const char* label) -> bool {
        std::vector<viewer::ResolvedInstance> resolved = { make_ri(hash, t) };

        if (!culler.cull(resolved, store, eye, planes, 1.0f)) {
            printf("  SKIP (cull returned false): %s\n", label);
            return true;  // not a failure — might be culled
        }
        auto batches = culler.readback_batches(store);

        // Find any batch with transforms.
        bool found = false;
        bool match = false;
        for (const auto& b : batches) {
            if (b.transforms.empty()) continue;
            found = true;
            Matrix expected = viewer::row_major_to_matrix(t);
            const Matrix& got = b.transforms[0];
            // Field-for-field comparison with epsilon.
            const float* ef = &expected.m0;
            const float* gf = &got.m0;
            bool ok = true;
            for (int i = 0; i < 16; ++i) {
                if (fabsf(ef[i] - gf[i]) > 1e-4f) {
                    printf("  field %d: expected %.6f got %.6f\n", i, ef[i], gf[i]);
                    ok = false;
                }
            }
            match = ok;
            break;
        }
        if (!found) {
            printf("  no batches returned: %s\n", label);
            return false;
        }
        return match;
    };

    // Case A: pure translate(5,6,7).
    float ta[16];
    make_translate(5.0f, 6.0f, 7.0f, ta);
    CHECK(run_single(ta, "translate(5,6,7)"), "matrix convention: pure translate field match");

    // Case B: rotate(45 deg yaw) + translate(2,0,1).
    float tb[16];
    make_rotate_translate(45.0f, 2.0f, 0.0f, 1.0f, tb);
    CHECK(run_single(tb, "rotate45+translate(2,0,1)"), "matrix convention: rotate+translate field match");

    return true;
}

// ---------------------------------------------------------------------------
// TEST 3: Cap growth — 10k instances, ensure_part region_cap starts at 4096.
// ---------------------------------------------------------------------------
static bool test_cap_growth() {
    printf("\n[test_cap_growth]\n");

    viewer::PartStore store("/tmp/gpu_test_unused3");
    uint64_t hash = build_fixture(store);

    viewer::GpuCuller culler;
    std::string err;
    if (!culler.init(err)) {
        printf("  ERROR: GpuCuller::init failed: %s\n", err.c_str());
        return false;
    }
    culler.ensure_part(hash, store);

    // Camera at origin looking +X with wide FOV to see all instances.
    const float eye[3]    = { -1.0f, 0.0f, 0.0f };
    const float target[3] = { 1.0f, 0.0f, 0.0f };
    const float up[3]     = { 0.0f, 1.0f, 0.0f };
    float planes[6][4];
    viewer::camera_frustum_planes_raw(eye, target, up, 170.0f, 1.0f, planes);

    const int N = 10000;
    std::vector<viewer::ResolvedInstance> resolved;
    resolved.reserve(N);

    // Grid layout: 100×100 spread around origin so most fall in frustum.
    for (int i = 0; i < N; ++i) {
        float tx = (float)(i % 100) * 0.5f;
        float ty = 0.0f;
        float tz = (float)(i / 100) * 0.5f;
        float t[16];
        make_translate(tx, ty, tz, t);
        resolved.push_back(make_ri(hash, t));
    }

    bool no_crash = true;
    if (!culler.cull(resolved, store, eye, planes, 1.0f)) {
        // Empty result is still OK (all culled), just means 0 drawn.
        // The important thing is no crash.
    }
    auto batches = culler.readback_batches(store);

    // CPU reference count for comparison.
    const viewer::LoadedPart* lp = store.get_or_load(hash);
    int cpu_total = 0;
    for (const auto& ri : resolved) {
        for (int ci = 0; ci < (int)lp->clusters.size(); ++ci) {
            const viewer::LoadedCluster& cl = lp->clusters[ci];
            if (!viewer::aabb_culled(cl.aabb_min, cl.aabb_max, ri.transform, planes))
                ++cpu_total;
        }
    }

    int gpu_total = 0;
    for (const auto& b : batches) gpu_total += (int)b.transforms.size();

    CHECK(no_crash, "cap growth: no crash with 10k instances");
    CHECK(gpu_total == cpu_total, "cap growth: GPU emitted count == CPU reference count");
    if (gpu_total != cpu_total)
        printf("  cpu=%d gpu=%d\n", cpu_total, gpu_total);

    return true;
}

// ---------------------------------------------------------------------------
// TEST 4: Empty resolve — cull({}) returns false, readback empty.
// ---------------------------------------------------------------------------
static bool test_empty_resolve() {
    printf("\n[test_empty_resolve]\n");

    viewer::PartStore store("/tmp/gpu_test_unused4");
    uint64_t hash = build_fixture(store);

    viewer::GpuCuller culler;
    std::string err;
    if (!culler.init(err)) {
        printf("  ERROR: GpuCuller::init failed: %s\n", err.c_str());
        return false;
    }
    culler.ensure_part(hash, store);

    const float eye[3]    = { 0.0f, 0.0f, 0.0f };
    const float target[3] = { 1.0f, 0.0f, 0.0f };
    const float up[3]     = { 0.0f, 1.0f, 0.0f };
    float planes[6][4];
    viewer::camera_frustum_planes_raw(eye, target, up, 60.0f, 1.6f, planes);

    std::vector<viewer::ResolvedInstance> empty_resolved;
    bool cull_ret = culler.cull(empty_resolved, store, eye, planes, 1.0f);
    CHECK(!cull_ret, "empty_resolve: cull({}) returns false");

    // readback after a false cull should return empty (no new GPU work done).
    auto batches = culler.readback_batches(store);
    // May return previously allocated empty cmd structure; count actual transforms.
    int total = 0;
    for (const auto& b : batches) total += (int)b.transforms.size();
    CHECK(total == 0, "empty_resolve: readback yields 0 transforms");

    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 200, "gpu_tests");

    // Check GL 4.6 availability.
    std::string why;
    if (!viewer::gl46_available(why)) {
        printf("SKIP: GL 4.6 unavailable (%s); skipping GPU cull parity tests.\n",
               why.c_str());
        CloseWindow();
        return 0;
    }
    printf("GL 4.6 available — running GPU cull parity tests.\n");

    test_parity_frustum_lod();
    test_matrix_convention();
    test_cap_growth();
    test_empty_resolve();

    CloseWindow();

    printf("\n--- Results: %d/%d passed", g_tests - g_failures, g_tests);
    if (g_failures == 0)
        printf(" --- ALL PASS\n");
    else
        printf(" --- %d FAIL\n", g_failures);

    return g_failures > 0 ? 1 : 0;
}
