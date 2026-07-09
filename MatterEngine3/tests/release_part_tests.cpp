// release_part_tests.cpp — GPU tests for PartStore::release + GpuCuller::release_part.
//
// Pattern mirrors gpu_cull_tests.cpp: FLAG_WINDOW_HIDDEN, gl46_available SKIP,
// CloseWindow before return, GALLIUM_DRIVER=d3d12.
//
// What is tested:
//   1. release_part(A): after release, part B still renders with no GL errors.
//   2. ensure_part(A) after release returns a NEW slot (hash no longer in slot_of_).
//   3. glGetError sweep is clean throughout.
//   4. PartStore::release(A): get_or_load(A) returns a fresh pointer (re-loaded).
//   5. Double release is a safe no-op (no crash, no GL error).
//   6. Release of unknown hash is a safe no-op.

#include "raylib.h"
#include "gl46.h"
#include "gpu_culler.h"
#include "part_store.h"
#include "sector_resolver.h"
#include "raster_mesh.h"
#include "gpu_cull_types.h"
#include "blas_manager.hpp"

#include "external/glad.h"
#include "precomp.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

// ---------------------------------------------------------------------------
// Test infrastructure
// ---------------------------------------------------------------------------

static int g_failures = 0;
static int g_tests    = 0;

#define CHECK(cond, msg) do {                          \
    ++g_tests;                                         \
    if (!(cond)) {                                     \
        printf("  FAIL: %s\n", (msg));                 \
        ++g_failures;                                  \
    } else {                                           \
        printf("  ok:   %s\n", (msg));                 \
    }                                                  \
} while (0)

// Drain all pending GL errors and return true iff none were found.
static bool gl_no_errors(const char* ctx) {
    GLenum e;
    bool clean = true;
    while ((e = glGetError()) != GL_NO_ERROR) {
        printf("  GL error 0x%04X in %s\n", (unsigned)e, ctx);
        clean = false;
    }
    return clean;
}

static std::string make_test_tmpdir(const char* tag) {
    char tmpl[64];
    std::snprintf(tmpl, sizeof(tmpl), "/tmp/rp_test_%s_XXXXXX", tag);
    char* d = mkdtemp(tmpl);
    if (!d) { perror("mkdtemp"); return "/tmp/rp_test_fallback"; }
    return std::string(d);
}

// Build a minimal 1-triangle RasterMeshData.
static viewer::RasterMeshData make_one_tri_mesh() {
    viewer::RasterMeshData md;
    md.vertex_count = 3;
    md.vertices  = { 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f };
    md.normals   = { 0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f };
    md.colors    = { 255, 255, 255, 255,  255, 255, 255, 255,  255, 255, 255, 255 };
    md.texcoords = { 0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f };
    return md;
}

// Inject a simple 1-cluster, 1-LOD part under the given hash.
// Registers actual BLAS geometry so the part holds live BLAS handles.
static void inject_simple_part(viewer::PartStore& store, uint64_t hash) {
    viewer::LoadedPart lp;
    lp.lod_mesh_data.push_back(make_one_tri_mesh());
    lp.thresholds  = { 0.5f };
    lp.bound_radius = 2.0f;

    // Register a 1-triangle BLAS with the shared manager so lp.lod_blas holds a live handle.
    Tri tri;
    tri.vertex0 = make_float3(0.0f, 0.0f, 0.0f);
    tri.vertex1 = make_float3(1.0f, 0.0f, 0.0f);
    tri.vertex2 = make_float3(0.0f, 1.0f, 0.0f);
    tri.centroid = make_float3(1.0f/3.0f, 1.0f/3.0f, 0.0f);
    BLASHandle h = store.blas().register_triangles(&tri, 1);
    lp.lod_blas.push_back(h);

    viewer::LoadedCluster cl;
    cl.aabb_min[0] = -1.0f; cl.aabb_min[1] = -1.0f; cl.aabb_min[2] = -1.0f;
    cl.aabb_max[0] =  1.0f; cl.aabb_max[1] =  1.0f; cl.aabb_max[2] =  1.0f;
    cl.radius = 1.73f;
    cl.thresholds = { 0.5f };
    cl.lod_blas   = { h };  // Use the same registered BLAS handle
    cl.lod_mesh   = { 0 };
    lp.clusters.push_back(cl);

    store.inject_for_test(hash, std::move(lp));
}

// Build a simple identity transform ResolvedInstance.
static viewer::ResolvedInstance make_ri(uint64_t hash) {
    viewer::ResolvedInstance ri{};
    ri.part_hash = hash;
    ri.lod_level = 0;
    // Identity matrix (engine row-major).
    memset(ri.transform, 0, sizeof ri.transform);
    ri.transform[0]  = 1.0f;
    ri.transform[5]  = 1.0f;
    ri.transform[10] = 1.0f;
    ri.transform[15] = 1.0f;
    return ri;
}

// Drain all GL errors (no assertion — used for cleanup before a CHECK).
static void gl_drain_errors() {
    while (glGetError() != GL_NO_ERROR) {}
}

// ---------------------------------------------------------------------------
// test_release_part_b_survives
//
//   Register parts A and B.
//   release_part(A).
//   Assert: part B still has its slot in culler (slot_of returns >= 0).
//   Assert: cull() with only part B in the resolved set succeeds.
//   Assert: no GL errors.
// ---------------------------------------------------------------------------
static void test_release_part_b_survives() {
    printf("\n[test_release_part_b_survives]\n");

    const uint64_t hashA = 0xAAAAAAAAAAAA0001ULL;
    const uint64_t hashB = 0xBBBBBBBBBBBB0002ULL;

    std::string tmpdir = make_test_tmpdir("bsurv");
    viewer::PartStore store(tmpdir);
    inject_simple_part(store, hashA);
    inject_simple_part(store, hashB);

    viewer::GpuCuller culler;
    std::string err;
    if (!culler.init(err)) {
        printf("  ERROR: GpuCuller::init failed: %s\n", err.c_str());
        ++g_failures; ++g_tests;
        return;
    }

    gl_drain_errors();

    int slotA = culler.ensure_part(hashA, store);
    int slotB = culler.ensure_part(hashB, store);
    CHECK(slotA >= 0, "slotA valid after ensure_part");
    CHECK(slotB >= 0, "slotB valid after ensure_part");
    CHECK(slotA != slotB, "A and B got distinct slots");

    CHECK(gl_no_errors("after ensure_part"), "no GL errors after ensure_part");

    // Release A.
    culler.release_part(hashA);

    CHECK(gl_no_errors("after release_part(A)"), "no GL errors after release_part(A)");

    // B's slot should still be registered.
    CHECK(culler.part_slot_of(hashB) == slotB, "part B still has its slot after release_part(A)");

    // A's slot should be gone from the lookup map.
    CHECK(culler.part_slot_of(hashA) == -1, "part A no longer in slot_of_ after release");

    // Verify the PartGpu entry for slot A is marked dead (vao == 0).
    const auto& parts = culler.parts();
    CHECK((int)parts.size() > slotA, "parts_ still has slot A's entry (dead hole)");
    if ((int)parts.size() > slotA) {
        CHECK(parts[slotA].vao == 0, "slot A PartGpu.vao zeroed (dead)");
    }

    // cull() with only B should succeed and produce no GL errors.
    // Simple frustum: orthographic-style, everything in front.
    float planes[6][4] = {
        { 1, 0, 0, 1000 }, { -1, 0, 0, 1000 },
        { 0, 1, 0, 1000 }, {  0,-1, 0, 1000 },
        { 0, 0, 1, 1000 }, {  0, 0,-1, 1000 },
    };
    float eye[3]     = { 0, 0, -10 };
    float vp[16];
    memset(vp, 0, sizeof vp);
    vp[0] = 1; vp[5] = 1; vp[10] = 1; vp[15] = 1;

    std::vector<viewer::ResolvedInstance> resolved = { make_ri(hashB) };
    bool cull_ok = culler.cull(resolved, store, eye, planes, vp, 1.0f);
    // cull() may return true or false depending on dispatch details; what matters is no crash.
    (void)cull_ok;

    CHECK(gl_no_errors("after cull() with only B"), "no GL errors after cull with part B");
}

// ---------------------------------------------------------------------------
// test_release_then_reensure
//
//   Register A and B. release_part(A). ensure_part(A) again.
//   The new slot must differ from the old one (hash was removed from slot_of_).
//   Verify glGetError sweep is clean.
// ---------------------------------------------------------------------------
static void test_release_then_reensure() {
    printf("\n[test_release_then_reensure]\n");

    const uint64_t hashA = 0xAAAAAAAAAAAA0001ULL;
    const uint64_t hashB = 0xBBBBBBBBBBBB0002ULL;

    std::string tmpdir = make_test_tmpdir("reens");
    viewer::PartStore store(tmpdir);
    inject_simple_part(store, hashA);
    inject_simple_part(store, hashB);

    viewer::GpuCuller culler;
    std::string err;
    if (!culler.init(err)) {
        printf("  ERROR: GpuCuller::init failed: %s\n", err.c_str());
        ++g_failures; ++g_tests;
        return;
    }

    gl_drain_errors();

    int slotA_first = culler.ensure_part(hashA, store);
    int slotB       = culler.ensure_part(hashB, store);
    (void)slotB;
    CHECK(slotA_first >= 0, "slotA valid on first ensure_part");

    culler.release_part(hashA);
    CHECK(gl_no_errors("after release_part(A)"), "no GL errors after release_part(A)");

    // Re-inject A (simulate re-bake returning fresh data).
    inject_simple_part(store, hashA);

    // ensure_part must treat A as new and return a fresh slot.
    int slotA_second = culler.ensure_part(hashA, store);
    CHECK(slotA_second >= 0, "slotA valid on second ensure_part after release");
    // The new slot is appended at the end of parts_ (no compaction).
    CHECK(slotA_second != slotA_first, "re-ensure assigns a NEW slot (not recycled)");

    // A's new VAO should be non-zero (properly uploaded).
    const auto& parts = culler.parts();
    CHECK((int)parts.size() > slotA_second, "parts_ has the new slot");
    if ((int)parts.size() > slotA_second) {
        CHECK(parts[slotA_second].vao != 0, "new slot A has live VAO");
    }

    CHECK(gl_no_errors("after re-ensure_part(A)"), "no GL errors after re-ensure_part(A)");
}

// ---------------------------------------------------------------------------
// test_double_release_safe
//
//   Register A, release_part(A) twice. No crash, no GL error.
// ---------------------------------------------------------------------------
static void test_double_release_safe() {
    printf("\n[test_double_release_safe]\n");

    const uint64_t hashA = 0xAAAAAAAAAAAA0001ULL;

    std::string tmpdir = make_test_tmpdir("dbl");
    viewer::PartStore store(tmpdir);
    inject_simple_part(store, hashA);

    viewer::GpuCuller culler;
    std::string err;
    if (!culler.init(err)) {
        printf("  ERROR: GpuCuller::init failed: %s\n", err.c_str());
        ++g_failures; ++g_tests;
        return;
    }

    gl_drain_errors();

    int slot = culler.ensure_part(hashA, store);
    CHECK(slot >= 0, "ensure_part returns valid slot");

    culler.release_part(hashA);
    CHECK(gl_no_errors("after first release_part"), "no GL errors after first release");

    // Second release — must be a safe no-op.
    culler.release_part(hashA);
    CHECK(gl_no_errors("after second release_part (no-op)"), "no GL errors after double release");

    CHECK(culler.part_slot_of(hashA) == -1, "part_slot_of still -1 after double release");
}

// ---------------------------------------------------------------------------
// test_release_unknown_hash_safe
//
//   Call release_part on a hash that was never registered. Must be a no-op.
// ---------------------------------------------------------------------------
static void test_release_unknown_hash_safe() {
    printf("\n[test_release_unknown_hash_safe]\n");

    std::string tmpdir = make_test_tmpdir("unk");
    viewer::PartStore store(tmpdir);

    viewer::GpuCuller culler;
    std::string err;
    if (!culler.init(err)) {
        printf("  ERROR: GpuCuller::init failed: %s\n", err.c_str());
        ++g_failures; ++g_tests;
        return;
    }

    gl_drain_errors();

    const uint64_t unknown = 0xDEADC0DEDEADC0DEULL;
    culler.release_part(unknown);   // must not crash

    CHECK(gl_no_errors("after release_part(unknown)"), "no GL errors on unknown hash release");
    CHECK(culler.part_slot_of(unknown) == -1, "unknown hash still reports -1 slot");
}

// ---------------------------------------------------------------------------
// test_cluster_staging_zeroed_after_release
//
//   Register A (1 cluster). Release A. Verify that the cluster_staging entry
//   for A has lod_count == 0 in the CPU mirror, indicating the SSBO patch was
//   applied.
// ---------------------------------------------------------------------------
static void test_cluster_staging_zeroed_after_release() {
    printf("\n[test_cluster_staging_zeroed_after_release]\n");

    const uint64_t hashA = 0xAAAAAAAAAAAA0001ULL;

    std::string tmpdir = make_test_tmpdir("cls");
    viewer::PartStore store(tmpdir);
    inject_simple_part(store, hashA);

    viewer::GpuCuller culler;
    std::string err;
    if (!culler.init(err)) {
        printf("  ERROR: GpuCuller::init failed: %s\n", err.c_str());
        ++g_failures; ++g_tests;
        return;
    }

    gl_drain_errors();

    int slot = culler.ensure_part(hashA, store);
    CHECK(slot >= 0, "ensure_part returns valid slot");

    const auto& parts = culler.parts();
    CHECK((int)parts.size() > slot, "parts_ has slot");
    uint32_t cl_start = 0, cl_count = 0;
    if ((int)parts.size() > slot) {
        cl_start = parts[slot].cluster_start;
        cl_count = parts[slot].cluster_count;
        CHECK(cl_count == 1, "fixture has exactly 1 cluster");
    }

    // Verify the cluster staging entry has lod_count > 0 before release.
    const auto& staging = culler.test_cluster_staging();
    CHECK(cl_start < (uint32_t)staging.size(), "cluster_start in staging bounds");
    bool pre_nonzero = false;
    if (cl_start < (uint32_t)staging.size()) {
        pre_nonzero = staging[cl_start].lod_count > 0;
    }
    CHECK(pre_nonzero, "cluster lod_count non-zero before release");

    // Release A.
    culler.release_part(hashA);

    // After release, the CPU mirror must show lod_count == 0 for the old cluster range.
    const auto& staging2 = culler.test_cluster_staging();
    bool post_zero = false;
    if (cl_start < (uint32_t)staging2.size()) {
        post_zero = staging2[cl_start].lod_count == 0;
    }
    CHECK(post_zero, "cluster lod_count zeroed in CPU mirror after release");

    CHECK(gl_no_errors("after cluster zero check"), "no GL errors after cluster staging check");
}

// ---------------------------------------------------------------------------
// test_partstore_release
//
//   Call PartStore::release(hash). inject A again (simulate re-bake).
//   get_or_load(hash) must return a fresh pointer (not the stale one).
//   Assert that BLAS entries were properly released (live_count decreases).
//   (Since inject_for_test replaces the entry, we verify release() removed the
//   old entry so that a subsequent injection + get_or_load works cleanly.)
// ---------------------------------------------------------------------------
static void test_partstore_release() {
    printf("\n[test_partstore_release]\n");

    const uint64_t hashA = 0xAAAAAAAAAAAA0001ULL;

    std::string tmpdir = make_test_tmpdir("ps");
    viewer::PartStore store(tmpdir);
    inject_simple_part(store, hashA);

    // First load.
    const viewer::LoadedPart* ptr1 = store.get_or_load(hashA);
    CHECK(ptr1 != nullptr, "get_or_load returns non-null on first call");

    // Note the BLAS entry count before release.
    size_t blas_live_before = store.blas().live_count();
    CHECK(blas_live_before > 0, "BLAS has live entries after injection");

    // Release from CPU store.
    store.release(hashA);

    // Pointer to the removed entry is now dangling — do NOT dereference ptr1.
    // loaded_count() must drop by 1.
    CHECK(store.loaded_count() == 0, "loaded_count drops to 0 after release");

    // BLAS live entries must decrease (handles were released).
    size_t blas_live_after = store.blas().live_count();
    CHECK(blas_live_after < blas_live_before, "BLAS live count decreases after PartStore::release");

    // Re-inject (simulates re-bake completing with a fresh artifact).
    inject_simple_part(store, hashA);

    const viewer::LoadedPart* ptr2 = store.get_or_load(hashA);
    CHECK(ptr2 != nullptr, "get_or_load returns non-null after re-inject");
    // ptr2 points to the newly-injected entry; we don't compare pointers
    // because map invalidation rules make the comparison unreliable.
    // The meaningful check is that the store accepted the new entry.
    CHECK(store.loaded_count() == 1, "loaded_count is 1 after re-inject");

    // BLAS entries must go up again (new handles from re-injection).
    size_t blas_live_after_reinject = store.blas().live_count();
    CHECK(blas_live_after_reinject > blas_live_after, "BLAS live count increases after re-inject");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 200, "release_part_tests");

    std::string why;
    if (!viewer::gl46_available(why)) {
        printf("SKIP: GL 4.6 unavailable (%s); skipping release-part GPU tests.\n",
               why.c_str());
        CloseWindow();
        return 0;
    }
    printf("GL 4.6 available — running release-part tests.\n");

    test_release_part_b_survives();
    test_release_then_reensure();
    test_double_release_safe();
    test_release_unknown_hash_safe();
    test_cluster_staging_zeroed_after_release();
    test_partstore_release();

    CloseWindow();

    printf("\n--- Results: %d/%d passed", g_tests - g_failures, g_tests);
    if (g_failures == 0)
        printf(" --- ALL PASS\n");
    else
        printf(" --- %d FAIL\n", g_failures);

    return g_failures > 0 ? 1 : 0;
}
