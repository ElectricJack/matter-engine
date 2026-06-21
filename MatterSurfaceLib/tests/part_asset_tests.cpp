#include "../include/part_asset.h"
#include "../include/blas_manager.hpp"
#include "../include/tlas_manager.hpp"
#include <cstdio>
#include <cstdint>
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

// Builds a tiny baked scene: 2 BLAS, 3 instances.
static void build_scene(BLASManager& blas, TLASManager& tlas,
                        BLASHandle& hA, BLASHandle& hB) {
    Tri triA[3] = { ptri(0,0), ptri(5,0), ptri(0,5) };
    Tri triB[2] = { ptri(20,0), ptri(25,5) };
    TriEx exA[3] = {}; TriEx exB[2] = {};
    for (auto& e : exA) { e.materialId = 8; e.N0=e.N1=e.N2=make_float3(0,0,1); e.tint=make_float4(1,1,1,0); }
    for (auto& e : exB) { e.materialId = 9; e.N0=e.N1=e.N2=make_float3(0,0,1); e.tint=make_float4(1,1,1,0); }
    hA = blas.register_triangles(triA, 3, exA);
    hB = blas.register_triangles(triB, 2, exB);

    std::vector<TLASManager::DrawInstance> insts(3);
    insts[0].blas_handle = hA; insts[0].material_id = 8; insts[0].transform = Matrix4x4();
    insts[1].blas_handle = hB; insts[1].material_id = 9; insts[1].transform = Matrix4x4();
    insts[1].transform.m[3] = 10.0f; // translate x
    insts[2].blas_handle = hA; insts[2].material_id = 8; insts[2].transform = Matrix4x4();
    insts[2].transform.m[7] = 7.0f;  // translate y
    tlas.draw_batch(insts);
    tlas.build(blas);
}

static uint32_t rd_u32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v; memcpy(&v, b.data()+off, 4); return v;
}
static uint64_t rd_u64(const std::vector<uint8_t>& b, size_t off) {
    uint64_t v; memcpy(&v, b.data()+off, 8); return v;
}
static std::vector<uint8_t> read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f,0,SEEK_END); long n = ftell(f); fseek(f,0,SEEK_SET);
    std::vector<uint8_t> b(n);
    size_t got = fread(b.data(),1,n,f); fclose(f);
    b.resize(got);
    return b;
}

static void test_save_header() {
    using namespace part_asset;
    BLASManager blas; TLASManager tlas(64);
    BLASHandle hA, hB; build_scene(blas, tlas, hA, hB);

    const char* path = "test_save.part";
    remove(path);
    bool ok = save(path, blas, tlas, 0xABCDEF12u);
    CHECK(ok, "save returns true");

    std::vector<uint8_t> b = read_file(path);
    CHECK(b.size() >= 36, "file has at least a header");
    CHECK(rd_u32(b, 0) == kMagic, "magic written");
    CHECK(rd_u32(b, 4) == kFormatVersion, "version written");
    CHECK(rd_u64(b, 8) == 0xABCDEF12ull, "param hash written");
    CHECK(rd_u32(b, 16) == (uint32_t)sizeof(Tri), "sizeof Tri written");
    CHECK(rd_u32(b, 20) == (uint32_t)sizeof(TriEx), "sizeof TriEx written");
    CHECK(rd_u32(b, 24) == (uint32_t)sizeof(BVHNode), "sizeof BVHNode written");
    // content hash covers the body (everything after the 36-byte header).
    uint64_t stored = rd_u64(b, 28);
    uint64_t recomputed = fnv1a64(b.data()+36, b.size()-36);
    CHECK(stored == recomputed, "content hash covers body");

    remove(path);
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
    test_save_header();

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
