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

// Covers CPU-side TriEx retention only: register_triangles must copy the
// per-triangle materialIds into the mesh's triEx[] array. (The GPU upload that
// packs these into the texture .w needs a GL context and is verified visually.)
static void test_triex_material_cpu_retention() {
    printf("=== test_triex_material_cpu_retention ===\n");
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

// Exercises the pure CPU pack logic that the GPU upload uses to write row-0 .w:
// materialId N must pack to float N and read back as int N (exact round-trip),
// and a null triEx must yield the -1.0f sentinel that reads back as a negative
// int (the shader's "fall back to instance material" path).
static void test_triex_material_pack_roundtrip() {
    printf("=== test_triex_material_pack_roundtrip ===\n");
    TriEx ex[3] = {};
    ex[0].materialId = 0;
    ex[1].materialId = 7;
    ex[2].materialId = 123456; // exactly representable as float

    for (int i = 0; i < 3; ++i) {
        float packed = BLASManager::pack_material_w(ex, i);
        assert(packed == static_cast<float>(ex[i].materialId));
        // Shader truncates via int(); verify the round-trip recovers the id exactly.
        assert(static_cast<int>(packed) == ex[i].materialId);
        assert(static_cast<int>(packed) >= 0); // not the fallback sentinel
    }

    // Null triEx -> -1.0f sentinel -> negative int -> fallback path.
    float sentinel = BLASManager::pack_material_w(nullptr, 0);
    assert(sentinel == -1.0f);
    assert(static_cast<int>(sentinel) < 0);
    printf("PASSED\n");
}

// Dedup must key on per-triangle material as well as geometry: two registrations
// with byte-identical geometry but DIFFERENT materialIds must NOT collapse to one
// entry (otherwise the second mesh silently inherits the first's materials).
static void test_dedup_respects_material() {
    printf("=== test_dedup_respects_material ===\n");
    BLASManager m;
    std::vector<Tri> tris = makeTriSet(0.0f);
    std::vector<TriEx> ex0(1), ex1(1);
    ex0[0] = TriEx{}; ex1[0] = TriEx{};
    ex0[0].materialId = 3;
    ex1[0].materialId = 4;

    BLASHandle a = m.register_triangles(tris, ex0);
    BLASHandle b = m.register_triangles(tris, ex1); // same geometry, different material
    assert(a != INVALID_BLAS_HANDLE);
    assert(b != INVALID_BLAS_HANDLE);
    assert(a != b);
    assert(m.get_unique_blas_count() == 2);

    // Same geometry AND same material -> genuine dedup hit.
    BLASHandle a_dup = m.register_triangles(tris, ex0);
    assert(a_dup == a);
    assert(m.get_unique_blas_count() == 2);
    printf("PASSED\n");
}

int main() {
    test_dedup_and_release();
    test_remesh_no_leak();
    test_release_invalid_is_noop();
    test_triex_material_cpu_retention();
    test_triex_material_pack_roundtrip();
    test_dedup_respects_material();
    printf("\nALL BLAS REFCOUNT TESTS PASSED\n");
    return 0;
}
