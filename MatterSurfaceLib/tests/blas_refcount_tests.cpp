// Headless regression tests for BLASManager reference counting and the
// re-mesh leak path that caused the BLAS node texture to overflow
// GL_MAX_TEXTURE_SIZE (black raytrace render + broken particle adding).
//
// register_triangles / release_blas are CPU-only (BVH build, no GL calls),
// so these run without a GL context.

#include <cstdio>
#include <cassert>
#include <cstring>
#include <vector>

#include "blas_manager.hpp"

// One triangle, offset along X by `ox`, with a tiny per-call perturbation
// `eps` to mimic mesh generation NOT being bit-deterministic across rebuilds.
// Tri unions each float3 with a 16-byte __m128, leaving a 4-byte padding lane
// that triangles_equal's memcmp inspects, so zero the whole struct first to
// keep byte-identical geometry byte-identical (deterministic dedup).
static std::vector<Tri> makeTriSet(float ox, float eps = 0.0f) {
    Tri t;
    std::memset(&t, 0, sizeof(Tri));
    t.vertex0 = make_float3(ox + eps, 0.0f, 0.0f);
    t.vertex1 = make_float3(ox + 1.0f, 0.0f, 0.0f);
    t.vertex2 = make_float3(ox + 0.0f, 1.0f, 0.0f);
    t.centroid = (t.vertex0 + t.vertex1 + t.vertex2) * (1.0f / 3.0f);
    return { t };
}

static void test_dedup_and_release() {
    printf("=== test_dedup_and_release ===\n");
    BLASManager m;
    assert(m.get_unique_blas_count() == 0);

    BLASHandle a = m.register_triangles(makeTriSet(0.0f));
    assert(a != INVALID_BLAS_HANDLE);
    assert(m.get_unique_blas_count() == 1);

    BLASHandle b = m.register_triangles(makeTriSet(10.0f));
    assert(b != INVALID_BLAS_HANDLE && b != a);
    assert(m.get_unique_blas_count() == 2);

    // Identical geometry to A -> dedup hit, same handle, ref_count bumps to 2.
    BLASHandle a_dup = m.register_triangles(makeTriSet(0.0f));
    assert(a_dup == a);
    assert(m.get_unique_blas_count() == 2);

    // First release only decrements ref_count; entry stays.
    m.release_blas(a);
    assert(m.get_unique_blas_count() == 2);
    assert(m.has_blas(a));

    // Second release drops the last owner; A is removed.
    m.release_blas(a);
    assert(m.get_unique_blas_count() == 1);
    assert(!m.has_blas(a));
    assert(m.has_blas(b));

    m.release_blas(b);
    assert(m.get_unique_blas_count() == 0);
    printf("PASSED\n");
}

static void test_remesh_no_leak() {
    printf("=== test_remesh_no_leak ===\n");
    BLASManager m;

    // Simulate a cell re-meshing every frame: each rebuild produces the "same"
    // geometry with micro-different float coords (non-deterministic generation),
    // so content-hash dedup misses. Releasing the previous handle on re-mesh must
    // keep the live entry count bounded. Without the fix this grew unbounded.
    BLASHandle prev = INVALID_BLAS_HANDLE;
    for (int i = 0; i < 200; ++i) {
        if (prev != INVALID_BLAS_HANDLE) m.release_blas(prev);
        prev = m.register_triangles(makeTriSet(0.0f, 1e-6f * (float)(i + 1)));
        assert(m.get_unique_blas_count() == 1);
    }
    m.release_blas(prev);
    assert(m.get_unique_blas_count() == 0);
    printf("PASSED\n");
}

static void test_release_invalid_is_noop() {
    printf("=== test_release_invalid_is_noop ===\n");
    BLASManager m;
    m.register_triangles(makeTriSet(0.0f));
    assert(m.get_unique_blas_count() == 1);

    m.release_blas(INVALID_BLAS_HANDLE); // no-op
    m.release_blas(99999);               // unknown handle, no-op
    assert(m.get_unique_blas_count() == 1);
    printf("PASSED\n");
}

static void test_triex_material_roundtrip() {
    printf("=== test_triex_material_roundtrip ===\n");
    BLASManager m;
    std::vector<Tri> tris(2);
    std::vector<TriEx> ex(2);
    std::memset(&tris[0], 0, sizeof(Tri));
    std::memset(&tris[1], 0, sizeof(Tri));
    for (int i = 0; i < 2; ++i) {
        tris[i].vertex0 = make_float3(0.0f, 0.0f, 0.0f);
        tris[i].vertex1 = make_float3(1.0f, 0.0f, 0.0f);
        tris[i].vertex2 = make_float3(0.0f, 1.0f, 0.0f);
        tris[i].centroid = (tris[i].vertex0 + tris[i].vertex1 + tris[i].vertex2) * (1.0f/3.0f);
    }
    ex[0] = TriEx{}; ex[1] = TriEx{};
    ex[0].materialId = 8; ex[1].materialId = 9;
    BLASHandle h = m.register_triangles(tris, ex);
    BvhMesh* mesh = m.get_mesh(h);
    assert(mesh != nullptr);
    assert(mesh->triEx != nullptr);
    assert(mesh->triEx[0].materialId == 8);
    assert(mesh->triEx[1].materialId == 9);
    printf("PASSED\n");
}

int main() {
    test_dedup_and_release();
    test_remesh_no_leak();
    test_release_invalid_is_noop();
    test_triex_material_roundtrip();
    printf("\nALL BLAS REFCOUNT TESTS PASSED\n");
    return 0;
}
