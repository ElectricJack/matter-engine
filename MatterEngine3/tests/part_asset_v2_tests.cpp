#include "../include/part_asset_v2.h"
#include "../../MatterSurfaceLib/include/blas_manager.hpp"
#include "../../MatterSurfaceLib/include/tlas_manager.hpp"
#include "../../MatterSurfaceLib/include/material_registry.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "check.h"

static void test_resolved_hash() {
    using namespace part_asset;
    const char* src = "function part(){ return cube(); }";
    const char* par = "\x01\x02\x03\x04";
    uint64_t kids[3] = { 0xAAAAull, 0xBBBBull, 0xCCCCull };

    // Deterministic: same inputs -> same hash.
    uint64_t h1 = compute_resolved_hash(src, strlen(src), par, 4, kids, 3);
    uint64_t h2 = compute_resolved_hash(src, strlen(src), par, 4, kids, 3);
    CHECK(h1 == h2, "resolved hash deterministic");

    // Order-independent over child hashes: shuffled children -> same hash.
    uint64_t shuffled[3] = { 0xCCCCull, 0xAAAAull, 0xBBBBull };
    uint64_t h3 = compute_resolved_hash(src, strlen(src), par, 4, shuffled, 3);
    CHECK(h1 == h3, "resolved hash order-independent over children");

    // Sensitive: changing source changes the hash.
    const char* src2 = "function part(){ return sphere(); }";
    uint64_t h4 = compute_resolved_hash(src2, strlen(src2), par, 4, kids, 3);
    CHECK(h1 != h4, "resolved hash changes when source changes");

    // Sensitive: changing params changes the hash.
    const char* par2 = "\x01\x02\x03\x05";
    uint64_t h5 = compute_resolved_hash(src, strlen(src), par2, 4, kids, 3);
    CHECK(h1 != h5, "resolved hash changes when params change");

    // Sensitive: changing a child hash changes the hash.
    uint64_t kids2[3] = { 0xAAAAull, 0xBBBBull, 0xDDDDull };
    uint64_t h6 = compute_resolved_hash(src, strlen(src), par, 4, kids2, 3);
    CHECK(h1 != h6, "resolved hash changes when a child hash changes");

    // Zero children is valid (null + 0).
    uint64_t h7 = compute_resolved_hash(src, strlen(src), par, 4, nullptr, 0);
    uint64_t h8 = compute_resolved_hash(src, strlen(src), par, 4, nullptr, 0);
    CHECK(h7 == h8, "resolved hash deterministic with zero children");
    CHECK(h7 != h1, "zero children differs from three children");
}

static void test_cache_path_resolved() {
    using namespace part_asset;
    CHECK(cache_path_resolved(0x1ull) == "parts/0000000000000001.part",
          "cache_path_resolved zero-padded hex");
    CHECK(cache_path_resolved(0xDEADBEEFCAFEBABEull) == "parts/deadbeefcafebabe.part",
          "cache_path_resolved full-width hex");
}

static Tri ptri(float ox, float oy) {
    Tri t;
    t.vertex0 = make_float3(ox + 0.0f, oy + 0.0f, 0.0f);
    t.vertex1 = make_float3(ox + 1.0f, oy + 0.0f, 0.0f);
    t.vertex2 = make_float3(ox + 0.0f, oy + 1.0f, 0.0f);
    t.centroid = make_float3(ox + 0.333f, oy + 0.333f, 0.0f);
    return t;
}

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
    insts[1].transform.m[3] = 10.0f;
    insts[2].blas_handle = hA; insts[2].material_id = 8; insts[2].transform = Matrix4x4();
    insts[2].transform.m[7] = 7.0f;
    tlas.draw_batch(insts);
    tlas.build(blas);
}

// A couple of synthetic child rows and a 2-level LOD array for the full round-trip.
static std::vector<part_asset::ChildInstance> sample_children() {
    std::vector<part_asset::ChildInstance> kids(2);
    kids[0].child_resolved_hash = 0x1111222233334444ull;
    for (int i = 0; i < 16; ++i) kids[0].transform[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    kids[0].transform[3] = 2.5f;
    kids[1].child_resolved_hash = 0x5555666677778888ull;
    for (int i = 0; i < 16; ++i) kids[1].transform[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    kids[1].transform[7] = -4.0f;
    return kids;
}
static part_asset::LodLevels sample_lods() {
    part_asset::LodLevels lods(2);
    lods[0].screen_size_threshold = 256.0f;
    lods[0].blas_indices = { 0u, 1u };
    lods[1].screen_size_threshold = 32.0f;
    lods[1].blas_indices = { 0u };
    return lods;
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
static void write_file(const char* path, const std::vector<uint8_t>& b) {
    FILE* f = fopen(path, "wb"); fwrite(b.data(),1,b.size(),f); fclose(f);
}

static void test_save_v2_header() {
    using namespace part_asset;
    BLASManager blas; TLASManager tlas(64);
    BLASHandle hA, hB; build_scene(blas, tlas, hA, hB);
    auto kids = sample_children();
    auto lods = sample_lods();

    const char* path = "test_v2_save.part";
    remove(path);
    bool ok = save_v2(path, blas, tlas, kids.data(), kids.size(), lods, 0xABCDEF12u);
    CHECK(ok, "save_v2 returns true");

    std::vector<uint8_t> b = read_file(path);
    // Header layout (v2): magic u32, version u32, resolved_hash^ver u64, sizeof Tri/TriEx/
    // BVHNode/ChildInstance u32 x4, content_hash u64 => 8 + 16 + 8 = 40-byte header.
    CHECK(b.size() >= 40, "file has at least a v2 header");
    CHECK(rd_u32(b, 0) == kMagic, "magic written");
    CHECK(rd_u32(b, 4) == kFormatVersionV2, "v2 version written");
    CHECK(rd_u64(b, 8) == (0xABCDEF12ull ^ (uint64_t)kFormatVersionV2),
          "resolved hash stored XOR format version");
    CHECK(rd_u32(b, 16) == (uint32_t)sizeof(Tri), "sizeof Tri written");
    CHECK(rd_u32(b, 20) == (uint32_t)sizeof(TriEx), "sizeof TriEx written");
    CHECK(rd_u32(b, 24) == (uint32_t)sizeof(BVHNode), "sizeof BVHNode written");
    CHECK(rd_u32(b, 28) == (uint32_t)sizeof(ChildInstance), "sizeof ChildInstance written");
    uint64_t stored = rd_u64(b, 32);
    uint64_t recomputed = fnv1a64(b.data()+40, b.size()-40);
    CHECK(stored == recomputed, "content hash covers body after 40-byte header");

    remove(path);
}

static void test_round_trip_full() {
    using namespace part_asset;

    // Source scene + children + LODs.
    BLASManager blasA; TLASManager tlasA(64);
    BLASHandle hA, hB; build_scene(blasA, tlasA, hA, hB);
    auto kids = sample_children();
    auto lods = sample_lods();

    std::vector<Tri> triA; blasA.generate_triangle_data(triA);
    std::vector<LegacyBVHNode> nodeA; blasA.generate_node_data(nodeA);
    const auto recsA = tlasA.get_draw_records();

    const char* path = "test_v2_round.part";
    remove(path);
    CHECK(save_v2(path, blasA, tlasA, kids.data(), kids.size(), lods, 0x55AA55AAu),
          "round-trip save_v2 ok");

    // Load into fresh state.
    BLASManager blasB; TLASManager tlasB(64);
    std::vector<ChildInstance> kidsOut;
    LodLevels lodsOut;
    bool ok = load_v2(path, 0x55AA55AAu, blasB, tlasB, kidsOut, lodsOut);
    CHECK(ok, "round-trip load_v2 ok");

    // BLAS CPU data byte-identical.
    std::vector<Tri> triB; blasB.generate_triangle_data(triB);
    std::vector<LegacyBVHNode> nodeB; blasB.generate_node_data(nodeB);
    CHECK(triA.size() == triB.size() &&
          memcmp(triA.data(), triB.data(), triA.size()*sizeof(Tri)) == 0,
          "round-trip triangle bytes");
    CHECK(nodeA.size() == nodeB.size() &&
          memcmp(nodeA.data(), nodeB.data(), nodeA.size()*sizeof(LegacyBVHNode)) == 0,
          "round-trip node bytes");

    // Internal instances preserved.
    const auto recsB = tlasB.get_draw_records();
    CHECK(recsA.size() == recsB.size() && recsB.size() == 3, "round-trip instance count");
    bool inst_ok = recsA.size() == recsB.size();
    for (size_t i = 0; inst_ok && i < recsA.size(); ++i) {
        if (recsA[i].material_id != recsB[i].material_id) inst_ok = false;
        if (memcmp(recsA[i].transform.m, recsB[i].transform.m, 16*sizeof(float)) != 0) inst_ok = false;
    }
    CHECK(inst_ok, "round-trip instance material+transform");

    // Child instances preserved exactly.
    CHECK(kidsOut.size() == kids.size() && kidsOut.size() == 2, "round-trip child count");
    bool kids_ok = kidsOut.size() == kids.size();
    for (size_t i = 0; kids_ok && i < kids.size(); ++i) {
        if (kidsOut[i].child_resolved_hash != kids[i].child_resolved_hash) kids_ok = false;
        if (memcmp(kidsOut[i].transform, kids[i].transform, 16*sizeof(float)) != 0) kids_ok = false;
    }
    CHECK(kids_ok, "round-trip child hash+transform");

    // LOD levels preserved exactly (order, threshold, index arrays).
    CHECK(lodsOut.size() == lods.size() && lodsOut.size() == 2, "round-trip lod count");
    bool lod_ok = lodsOut.size() == lods.size();
    for (size_t i = 0; lod_ok && i < lods.size(); ++i) {
        if (lodsOut[i].screen_size_threshold != lods[i].screen_size_threshold) lod_ok = false;
        if (lodsOut[i].blas_indices != lods[i].blas_indices) lod_ok = false;
    }
    CHECK(lod_ok, "round-trip lod threshold+indices");

    // Wrong expected resolved hash must be rejected.
    BLASManager blasC; TLASManager tlasC(64);
    std::vector<ChildInstance> kc; LodLevels lc;
    CHECK(!load_v2(path, 0xDEADBEEFu, blasC, tlasC, kc, lc),
          "load_v2 rejects wrong resolved hash");

    remove(path);
}

static void test_round_trip_degenerate_lod() {
    using namespace part_asset;
    BLASManager blasA; TLASManager tlasA(64);
    BLASHandle hA, hB; build_scene(blasA, tlasA, hA, hB);
    auto kids = sample_children();

    // Empty LOD array round-trips.
    {
        LodLevels empty;
        const char* path = "test_v2_lod_empty.part";
        remove(path);
        CHECK(save_v2(path, blasA, tlasA, kids.data(), kids.size(), empty, 0x10u),
              "empty-LOD save ok");
        BLASManager b; TLASManager t(64);
        std::vector<ChildInstance> ko; LodLevels lo;
        CHECK(load_v2(path, 0x10u, b, t, ko, lo), "empty-LOD load ok");
        CHECK(lo.empty(), "empty LOD array round-trips empty");
        CHECK(ko.size() == 2, "children still round-trip with empty LOD");
        remove(path);
    }

    // Single-level LOD round-trips.
    {
        LodLevels one(1);
        one[0].screen_size_threshold = 128.0f;
        one[0].blas_indices = { 1u };
        const char* path = "test_v2_lod_one.part";
        remove(path);
        CHECK(save_v2(path, blasA, tlasA, kids.data(), kids.size(), one, 0x11u),
              "single-LOD save ok");
        BLASManager b; TLASManager t(64);
        std::vector<ChildInstance> ko; LodLevels lo;
        CHECK(load_v2(path, 0x11u, b, t, ko, lo), "single-LOD load ok");
        CHECK(lo.size() == 1, "single LOD level round-trips");
        CHECK(lo.size() == 1 && lo[0].screen_size_threshold == 128.0f,
              "single LOD threshold preserved");
        CHECK(lo.size() == 1 && lo[0].blas_indices == std::vector<uint32_t>{1u},
              "single LOD indices preserved");
        remove(path);
    }
}

static void test_round_trip_no_children() {
    using namespace part_asset;
    BLASManager blasA; TLASManager tlasA(64);
    BLASHandle hA, hB; build_scene(blasA, tlasA, hA, hB);
    auto lods = sample_lods();

    std::vector<Tri> triA; blasA.generate_triangle_data(triA);
    std::vector<LegacyBVHNode> nodeA; blasA.generate_node_data(nodeA);

    const char* path = "test_v2_nokids.part";
    remove(path);
    CHECK(save_v2(path, blasA, tlasA, nullptr, 0, lods, 0x20u),
          "no-children save ok");

    BLASManager blasB; TLASManager tlasB(64);
    std::vector<ChildInstance> ko; LodLevels lo;
    CHECK(load_v2(path, 0x20u, blasB, tlasB, ko, lo), "no-children load ok");
    CHECK(ko.empty(), "empty child table round-trips empty");

    // prebuilt-vs-built parity: geometry restored via register_prebuilt is
    // byte-identical to the source built BVH.
    std::vector<Tri> triB; blasB.generate_triangle_data(triB);
    std::vector<LegacyBVHNode> nodeB; blasB.generate_node_data(nodeB);
    CHECK(triA.size() == triB.size() &&
          memcmp(triA.data(), triB.data(), triA.size()*sizeof(Tri)) == 0,
          "prebuilt-vs-built triangle parity through v2");
    CHECK(nodeA.size() == nodeB.size() &&
          memcmp(nodeA.data(), nodeB.data(), nodeA.size()*sizeof(LegacyBVHNode)) == 0,
          "prebuilt-vs-built node parity through v2");

    remove(path);
}

static void test_v2_guards() {
    using namespace part_asset;
    BLASManager blasA; TLASManager tlasA(64);
    BLASHandle hA, hB; build_scene(blasA, tlasA, hA, hB);
    auto kids = sample_children();
    auto lods = sample_lods();

    const char* path = "test_v2_guard.part";
    remove(path);
    CHECK(save_v2(path, blasA, tlasA, kids.data(), kids.size(), lods, 0x1234u),
          "guard save ok");
    std::vector<uint8_t> good = read_file(path);

    // Sanity: unmodified file loads.
    { BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(load_v2(path, 0x1234u, b, t, ko, lo), "unmodified v2 file loads"); }

    // Layout guard: corrupt sizeof_ChildInstance (offset 28).
    { auto bad = good; uint32_t v = rd_u32(bad,28) + 1; memcpy(bad.data()+28,&v,4);
      write_file(path, bad);
      BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(!load_v2(path, 0x1234u, b, t, ko, lo), "rejects sizeof_ChildInstance mismatch"); }

    // Layout guard: corrupt sizeof_Tri (offset 16).
    { auto bad = good; uint32_t v = rd_u32(bad,16) + 1; memcpy(bad.data()+16,&v,4);
      write_file(path, bad);
      BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(!load_v2(path, 0x1234u, b, t, ko, lo), "rejects sizeof_Tri mismatch"); }

    // Version guard / v1 cutover: a format_version=1 file is rejected by the v2 loader.
    { auto bad = good; uint32_t v = 1u; memcpy(bad.data()+4,&v,4);
      write_file(path, bad);
      BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(!load_v2(path, 0x1234u, b, t, ko, lo), "v2 loader rejects v1 (format_version=1)"); }

    // Corruption guard: flip a byte deep in the body (child or LOD section).
    // The body is at least the header(40) + materials + BLAS + instances; flipping
    // the last byte lands inside the trailing LOD section.
    { auto bad = good; bad[bad.size()-1] ^= 0xFF;
      write_file(path, bad);
      BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(!load_v2(path, 0x1234u, b, t, ko, lo), "rejects trailing-section corruption"); }

    // Magic guard.
    { auto bad = good; bad[0] ^= 0xFF;
      write_file(path, bad);
      BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(!load_v2(path, 0x1234u, b, t, ko, lo), "rejects bad magic"); }

    remove(path);
}

static void test_new_materials() {
    CHECK(MaterialRegistryCount() == 17, "registry has 17 materials after bark/leaf/dirt");
    const MaterialDef* bark = MaterialRegistryGet(14);
    CHECK(bark->albedo[0] > 0.30f && bark->albedo[2] < 0.20f, "material 14 is brown bark");
    const MaterialDef* leaf = MaterialRegistryGet(15);
    CHECK(leaf->albedo[1] > leaf->albedo[0] && leaf->albedo[1] > leaf->albedo[2],
          "material 15 is green leaf");
    const MaterialDef* dirt = MaterialRegistryGet(16);
    CHECK(dirt->albedo[0] > dirt->albedo[2], "material 16 is brown dirt");
}

int main() {
    test_cache_path_resolved();
    test_resolved_hash();
    test_save_v2_header();
    test_round_trip_full();
    test_round_trip_degenerate_lod();
    test_round_trip_no_children();
    test_v2_guards();
    test_new_materials();
    if (g_failures == 0) printf("All part_asset_v2 tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
