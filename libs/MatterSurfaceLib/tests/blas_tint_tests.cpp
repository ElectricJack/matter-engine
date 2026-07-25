#include "../include/blas_manager.hpp"
#include <cstdio>
#include <cmath>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static Tri make_tri(float ox) {
    Tri t;
    t.vertex0 = make_float3(ox + 0.0f, 0.0f, 0.0f);
    t.vertex1 = make_float3(ox + 1.0f, 0.0f, 0.0f);
    t.vertex2 = make_float3(ox + 0.0f, 1.0f, 0.0f);
    t.centroid = make_float3(ox + 0.333f, 0.333f, 0.0f);
    return t;
}

int main() {
    // --- pack_tint_w: reads each channel; null triex reconstructs (1,1,1,0). ---
    TriEx ex{};
    ex.materialId = 8;
    ex.tint = make_float4(0.2f, 0.4f, 0.6f, 0.8f);
    CHECK(fabsf(BLASManager::pack_tint_w(&ex, 0, 0) - 0.2f) < 1e-6f, "tint.r pack");
    CHECK(fabsf(BLASManager::pack_tint_w(&ex, 0, 1) - 0.4f) < 1e-6f, "tint.g pack");
    CHECK(fabsf(BLASManager::pack_tint_w(&ex, 0, 2) - 0.6f) < 1e-6f, "tint.b pack");
    CHECK(fabsf(BLASManager::pack_tint_w(&ex, 0, 3) - 0.8f) < 1e-6f, "tint.a pack");
    CHECK(BLASManager::pack_tint_w(nullptr, 0, 0) == 1.0f, "null triex tint.r is 1");
    CHECK(BLASManager::pack_tint_w(nullptr, 0, 1) == 1.0f, "null triex tint.g is 1");
    CHECK(BLASManager::pack_tint_w(nullptr, 0, 2) == 1.0f, "null triex tint.b is 1");
    CHECK(BLASManager::pack_tint_w(nullptr, 0, 3) == 0.0f, "null triex tint.a is 0");

    // --- Tint participates in dedup: identical geometry + materialId but
    //     different tint must NOT share a BLAS. ---
    BLASManager mgr;
    Tri tris[2] = { make_tri(0.0f), make_tri(5.0f) };

    TriEx exA[2] = {}; TriEx exB[2] = {};
    for (int i = 0; i < 2; ++i) {
        exA[i].materialId = 8; exB[i].materialId = 8;
        exA[i].N0 = exA[i].N1 = exA[i].N2 = make_float3(0,0,1);
        exB[i].N0 = exB[i].N1 = exB[i].N2 = make_float3(0,0,1);
        exA[i].tint = make_float4(1,0,0,0.5f);
        exB[i].tint = make_float4(0,0,1,0.5f);
    }

    BLASHandle hA  = mgr.register_triangles(tris, 2, exA);
    BLASHandle hB  = mgr.register_triangles(tris, 2, exB);
    BLASHandle hA2 = mgr.register_triangles(tris, 2, exA);

    CHECK(hA != INVALID_BLAS_HANDLE, "register A valid");
    CHECK(hB != INVALID_BLAS_HANDLE, "register B valid");
    CHECK(hA != hB, "different tint must not dedup to same BLAS");
    CHECK(hA == hA2, "identical tint re-registration shares the BLAS");

    if (failures == 0) printf("All blas_tint tests passed\n");
    return failures == 0 ? 0 : 1;
}
