#include "../include/part_asset.h"
#include "../include/blas_manager.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static Tri ptri(float ox, float oy) {
    Tri t;
    t.vertex0 = make_float3(ox + 0.0f, oy + 0.0f, 0.0f);
    t.vertex1 = make_float3(ox + 1.0f, oy + 0.0f, 0.0f);
    t.vertex2 = make_float3(ox + 0.0f, oy + 1.0f, 0.0f);
    t.centroid = make_float3(ox + 0.333f, oy + 0.333f, 0.0f);
    return t;
}

static void test_prebuilt_parity() {
    // Build geometry the normal way (builds a BVH).
    BLASManager built;
    Tri tris[3] = { ptri(0,0), ptri(5,0), ptri(0,5) };
    TriEx ex[3] = {};
    for (int i = 0; i < 3; ++i) {
        ex[i].materialId = 8;
        ex[i].N0 = ex[i].N1 = ex[i].N2 = make_float3(0,0,1);
        ex[i].tint = make_float4(1,1,1,0);
    }
    BLASHandle h = built.register_triangles(tris, 3, ex);
    CHECK(h != INVALID_BLAS_HANDLE, "built register ok");

    const BLASManager::BLASEntry* e = built.get_entry(h);
    CHECK(e != nullptr, "built entry exists");

    // Re-register the SAME baked arrays via register_prebuilt (no BVH build).
    BLASManager prebuilt;
    BLASHandle h2 = prebuilt.register_prebuilt(
        e->triangles.data(), e->mesh->triEx, (int)e->triangles.size(),
        e->bvh->bvhNode, e->bvh->nodesUsed, e->bvh->triIdx,
        e->hash, e->ref_count);
    CHECK(h2 != INVALID_BLAS_HANDLE, "prebuilt register ok");

    // The GPU-facing CPU data must be byte-identical between the two paths.
    std::vector<Tri> ta, tb;
    built.generate_triangle_data(ta);
    prebuilt.generate_triangle_data(tb);
    CHECK(ta.size() == tb.size() && ta.size() == 3, "prebuilt triangle count matches");
    CHECK(ta.size() == tb.size() &&
          memcmp(ta.data(), tb.data(), ta.size()*sizeof(Tri)) == 0,
          "prebuilt triangle bytes match built");

    std::vector<LegacyBVHNode> na, nb;
    built.generate_node_data(na);
    prebuilt.generate_node_data(nb);
    CHECK(na.size() == nb.size(), "prebuilt node count matches");
    CHECK(na.size() == nb.size() &&
          memcmp(na.data(), nb.data(), na.size()*sizeof(LegacyBVHNode)) == 0,
          "prebuilt node bytes match built");
}

static part_asset::PartGenParams sample_params() {
    part_asset::PartGenParams p{};
    p.dimX = 20; p.dimY = 20; p.dimZ = 20;
    p.spacing = 0.8f; p.baseRadius = 0.62f;
    p.posJitter = 0.1f; p.radiusVar = 0.1f; p.voidAmt = 0.05f;
    p.veinFreq = 1.5f; p.veinThresh = 0.3f;
    p.matOpaqueA = 8; p.matOpaqueB = 9; p.matGlass = 4;
    p.simplifyRatio = 0.65f; p.seed = 1234u;
    return p;
}

int main() {
    test_prebuilt_parity();

    using namespace part_asset;

    // fnv1a64 is deterministic and order-sensitive.
    const char* a = "hello"; const char* b = "hellp";
    CHECK(fnv1a64(a, 5) == fnv1a64(a, 5), "fnv deterministic");
    CHECK(fnv1a64(a, 5) != fnv1a64(b, 5), "fnv distinguishes input");

    // compute_param_hash: same params -> same hash; changed field -> different.
    PartGenParams p1 = sample_params();
    PartGenParams p2 = sample_params();
    CHECK(compute_param_hash(p1) == compute_param_hash(p2), "same params same hash");
    p2.seed = 9999u;
    CHECK(compute_param_hash(p1) != compute_param_hash(p2), "seed change rehashes");
    p2 = sample_params(); p2.simplifyRatio = 0.5f;
    CHECK(compute_param_hash(p1) != compute_param_hash(p2), "ratio change rehashes");

    // cache_path format.
    CHECK(cache_path(0x1ull) == "parts/0000000000000001.part", "cache_path zero-padded hex");

    if (failures == 0) printf("All part_asset tests passed\n");
    return failures == 0 ? 0 : 1;
}
