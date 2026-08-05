#include "part_asset_v2.h"
#include "part_bundle.h"   // M4: an artifact is a bundle section
#include "../../libs/MatterSurfaceLib/include/blas_manager.hpp"
#include "../../libs/MatterSurfaceLib/include/tlas_manager.hpp"
#include "../../libs/MatterSurfaceLib/include/material_registry.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "check.h"

#ifdef _WIN32
static const char* kCacheRoot = "part_asset_v2_tests_cache";
#else
static const char* kCacheRoot = "/tmp/part_asset_v2_tests_cache";
#endif

static void make_test_dir(const char* path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

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

// ---------------------------------------------------------------------------
// M4 -- THE VERSION VECTOR IS IN THE PART HASH.
//
// This is the structural guard for the milestone's whole point. The part
// resolved hash NAMES every artifact a part owns on disk (`parts/<hash>.*`),
// so if the vector does not reach this hash, a version bump invalidates the
// resolve key and the atlas key while stale GEOMETRY keeps being served under
// the new rules -- the bug found twice in one month
// (docs/lod-vt-redesign-2026-08-04.md 9.3).
//
// The assertion is structural rather than behavioural on purpose: it
// recomputes the pre-fold FNV stream by hand and requires the shipped hash to
// be exactly matter_version::fold() of it. A compute_resolved_hash that
// dropped the fold, or folded something other than the vector, fails here --
// whereas every determinism/sensitivity check above would still pass.
// ---------------------------------------------------------------------------
static void test_resolved_hash_carries_version_vector() {
    using namespace part_asset;
    const char* src = "function part(){ return cube(); }";
    const char* par = "";
    uint64_t kids[3] = { 0xCCCCull, 0xAAAAull, 0xBBBBull };  // unsorted on purpose

    // The pre-fold stream, reproduced independently: FNV-1a over source, then
    // params, then the child hashes ASCENDING.
    uint64_t raw = 1469598103934665603ull;
    auto fold_bytes = [&raw](const void* d, size_t n) {
        const unsigned char* p = static_cast<const unsigned char*>(d);
        for (size_t i = 0; i < n; ++i) { raw ^= p[i]; raw *= 1099511628211ull; }
    };
    fold_bytes(src, strlen(src));
    fold_bytes(par, 4);
    uint64_t sorted_kids[3] = { 0xAAAAull, 0xBBBBull, 0xCCCCull };
    for (uint64_t c : sorted_kids) fold_bytes(&c, sizeof(c));

    const uint64_t shipped = compute_resolved_hash(src, strlen(src), par, 4, kids, 3);
    CHECK(shipped == matter_version::fold(raw),
          "resolved hash is exactly fold(content) -- the version vector is in the part hash");
    CHECK(shipped != raw,
          "the fold is not a no-op: the versioned hash differs from the raw content hash");

    // And the fold is what carries a version change: a hand-perturbed digest
    // must produce a different key for identical content.
    CHECK(matter_version::fold(raw) != matter_version::fold(raw ^ 1ull),
          "the fold separates keys");
    CHECK(matter_version::digest() != 0ull, "version digest is never the 0 sentinel");
}

static void test_cache_path_resolved() {
    using namespace part_asset;
    CHECK(cache_path_resolved(0x1ull) == "parts/0000000000000001.bundle",
          "cache_path_resolved zero-padded hex");
    CHECK(cache_path_resolved(0xDEADBEEFCAFEBABEull) == "parts/deadbeefcafebabe.bundle",
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
    insts[0].blas_handle = hA; insts[0].material_id = 8; insts[0].transform = mm::Mat4();
    insts[1].blas_handle = hB; insts[1].material_id = 9; insts[1].transform = mm::Mat4();
    insts[1].transform.m[3] = 10.0f;
    insts[2].blas_handle = hA; insts[2].material_id = 8; insts[2].transform = mm::Mat4();
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

// M4: an artifact's bytes are a bundle SECTION now, not a whole file. The
// header/corruption tests below reach for the artifact header at offset 0 and
// the body at offset 40, which is still exactly the section's layout -- what
// changed is where those bytes live. These two helpers are the only thing that
// had to move; every assertion about the header, the content hash and the
// material prefix reads the same as before.
static std::vector<uint8_t> read_artifact(const char* path, uint64_t hash,
                                          uint32_t tag) {
    std::vector<uint8_t> b;
    part_bundle::read_section(path, hash, tag, b);
    return b;
}
static void write_artifact(const char* path, uint64_t hash, uint32_t tag,
                           const std::vector<uint8_t>& b) {
    part_bundle::write_section(path, hash, tag, b.data(), b.size());
}

static void write_text_file(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void test_atomic_replace_preserves_target_on_failure() {
    using namespace part_asset;
    const char* target = "test_atomic_replace.target";
    const char* source = "test_atomic_replace.source";
    remove(target); remove(source);
    write_text_file(target, "old");
    CHECK(!replace_file_atomic(source, target),
          "atomic replace reports missing-source failure");
    std::vector<uint8_t> old = read_file(target);
    CHECK(std::string(old.begin(), old.end()) == "old",
          "failed atomic replace preserves old target bytes");

    write_text_file(source, "new");
    CHECK(replace_file_atomic(source, target),
          "atomic replace succeeds over existing target");
    std::vector<uint8_t> replaced = read_file(target);
    CHECK(std::string(replaced.begin(), replaced.end()) == "new",
          "successful atomic replace publishes complete new bytes");
    CHECK(read_file(source).empty(), "successful atomic replace consumes source");
    remove(target); remove(source);
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

    std::vector<uint8_t> b = read_artifact(path, 0xABCDEF12u, part_bundle::kSectionRep0);
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
    std::vector<uint8_t> good = read_artifact(path, 0x1234u, part_bundle::kSectionRep0);

    // Sanity: unmodified file loads.
    { BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(load_v2(path, 0x1234u, b, t, ko, lo), "unmodified v2 file loads"); }

    // Layout guard: corrupt sizeof_ChildInstance (offset 28).
    { auto bad = good; uint32_t v = rd_u32(bad,28) + 1; memcpy(bad.data()+28,&v,4);
      write_artifact(path, 0x1234u, part_bundle::kSectionRep0, bad);
      BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(!load_v2(path, 0x1234u, b, t, ko, lo), "rejects sizeof_ChildInstance mismatch"); }

    // Layout guard: corrupt sizeof_Tri (offset 16).
    { auto bad = good; uint32_t v = rd_u32(bad,16) + 1; memcpy(bad.data()+16,&v,4);
      write_artifact(path, 0x1234u, part_bundle::kSectionRep0, bad);
      BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(!load_v2(path, 0x1234u, b, t, ko, lo), "rejects sizeof_Tri mismatch"); }

    // Version guard / v1 cutover: a format_version=1 file is rejected by the v2 loader.
    { auto bad = good; uint32_t v = 1u; memcpy(bad.data()+4,&v,4);
      write_artifact(path, 0x1234u, part_bundle::kSectionRep0, bad);
      BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(!load_v2(path, 0x1234u, b, t, ko, lo), "v2 loader rejects v1 (format_version=1)"); }

    // Corruption guard: flip a byte deep in the body (child or LOD section).
    // The body is at least the header(40) + materials + BLAS + instances; flipping
    // the last byte lands inside the trailing LOD section.
    { auto bad = good; bad[bad.size()-1] ^= 0xFF;
      write_artifact(path, 0x1234u, part_bundle::kSectionRep0, bad);
      BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(!load_v2(path, 0x1234u, b, t, ko, lo), "rejects trailing-section corruption"); }

    // Magic guard.
    { auto bad = good; bad[0] ^= 0xFF;
      write_artifact(path, 0x1234u, part_bundle::kSectionRep0, bad);
      BLASManager b; TLASManager t(64); std::vector<ChildInstance> ko; LodLevels lo;
      CHECK(!load_v2(path, 0x1234u, b, t, ko, lo), "rejects bad magic"); }

    remove(path);
}

static void test_material_schema_guard() {
    using namespace part_asset;
    BLASManager blasA; TLASManager tlasA(64);
    BLASHandle hA, hB; build_scene(blasA, tlasA, hA, hB);
    auto kids = sample_children();
    auto lods = sample_lods();

    const char* path = "test_v2_material_schema.part";
    remove(path);
    CHECK(save_v2(path, blasA, tlasA, kids.data(), kids.size(), lods, 0x4321u),
          "material-schema guard save ok");
    std::vector<uint8_t> prior = read_artifact(path, 0x4321u, part_bundle::kSectionRep0);
    CHECK(prior.size() >= 44, "material-schema fixture contains common body");
    if (prior.size() >= 44) {
        const uint32_t old_schema = MaterialRegistrySchemaVersion() - 1u;
        memcpy(prior.data() + 40, &old_schema, sizeof(old_schema));
        const uint64_t content_hash = fnv1a64(prior.data() + 40, prior.size() - 40);
        memcpy(prior.data() + 32, &content_hash, sizeof(content_hash));
        write_artifact(path, 0x4321u, part_bundle::kSectionRep0, prior);

        BLASManager b; TLASManager t(64);
        std::vector<ChildInstance> ko; LodLevels lo;
        PartAssetLoadFailure failure = PartAssetLoadFailure::None;
        std::string reason;
        CHECK(!load_v2(path, 0x4321u, b, t, ko, lo, &failure, &reason),
              "load_v2 rejects prior material schema");
        CHECK(failure == PartAssetLoadFailure::MaterialSchema,
              "prior material schema reports MaterialSchema failure");
        CHECK(reason == "material schema mismatch; rebake",
              "prior material schema reports rebake reason");
    }
    remove(path);
}

static void patch_body_u32_and_rehash(std::vector<uint8_t>& bytes,
                                      size_t body_offset, uint32_t value) {
    memcpy(bytes.data() + 40 + body_offset, &value, sizeof(value));
    const uint64_t content_hash =
        part_asset::fnv1a64(bytes.data() + 40, bytes.size() - 40);
    memcpy(bytes.data() + 32, &content_hash, sizeof(content_hash));
}

// Flip one byte at file offset `offset` in place (r+b).
// M4: offsets are into the ARTIFACT (the bundle section), not the file. The
// tests using this are reasoning about the part's own header/body layout, so
// the section is the right frame -- a raw file offset would now land in the
// bundle header or the directory and mean nothing.
static void corrupt_byte_at(const char* path, uint64_t hash, uint32_t tag,
                            size_t offset) {
    std::vector<uint8_t> bytes;
    if (!part_bundle::read_section(path, hash, tag, bytes)) return;
    if (offset >= bytes.size()) return;
    bytes[offset] ^= 0x01;
    part_bundle::write_section(path, hash, tag, bytes.data(), bytes.size());
}

static void test_cache_artifact_compatibility_probe() {
    using namespace part_asset;
    BLASManager blas; TLASManager tlas(64);
    BLASHandle hA, hB; build_scene(blas, tlas, hA, hB);
    auto kids = sample_children();
    auto lods = sample_lods();

    const uint64_t v2_hash = 0x76543210u;
    const char* v2_path = "test_v2_compat.part";
    remove(v2_path);
    CHECK(save_v2(v2_path, blas, tlas, kids.data(), kids.size(), lods, v2_hash),
          "compat probe v2 fixture saved");
    CacheArtifactProbeStats stats{};
    const size_t material_prefix_size =
        2u * sizeof(uint32_t) +
        static_cast<size_t>(MaterialRegistryCount()) * sizeof(MaterialDef);
    CHECK(is_cache_artifact_header_compatible(v2_path, v2_hash, kFormatVersionV2,
                                              &stats),
          "header probe accepts current v2 artifact");
    CHECK(stats.body_bytes <= material_prefix_size,
          "header probe reads at most the material prefix past the header");
    CHECK(stats.retained_material_bytes ==
              8u + static_cast<size_t>(MaterialRegistryCount()) * sizeof(MaterialDef),
          "header probe retains only fixed material prefix");

    const char* large_path = "test_large_compat.part";
    BLASManager large_blas; TLASManager large_tlas(64);
    std::vector<Tri> large_tris(2048);
    for (size_t i = 0; i < large_tris.size(); ++i)
        large_tris[i] = ptri(static_cast<float>(i % 64),
                             static_cast<float>(i / 64));
    large_blas.register_triangles(large_tris.data(),
                                  static_cast<int>(large_tris.size()), nullptr);
    LodLevels large_lods(1);
    large_lods[0].blas_indices = {0u};
    CHECK(save_v2(large_path, large_blas, large_tlas, nullptr, 0,
                  large_lods, v2_hash + 1u),
          "large compatibility fixture saved");
    CacheArtifactProbeStats large_stats{};
    CHECK(is_cache_artifact_header_compatible(large_path, v2_hash + 1u,
                                              kFormatVersionV2, &large_stats),
          "large artifact header probe succeeds");
    CHECK(large_stats.body_bytes <= material_prefix_size,
          "header probe does not read geometry bytes from large artifact");
    CHECK(large_stats.retained_material_bytes ==
              8u + static_cast<size_t>(MaterialRegistryCount()) * sizeof(MaterialDef),
          "large header probe retains no geometry bytes");

    std::vector<uint8_t> current_v2 = read_artifact(v2_path, v2_hash, part_bundle::kSectionRep0);
    std::vector<uint8_t> stale_schema = current_v2;
    patch_body_u32_and_rehash(stale_schema, 0,
                              MaterialRegistrySchemaVersion() - 1u);
    write_artifact(v2_path, v2_hash, part_bundle::kSectionRep0, stale_schema);
    CHECK(!is_cache_artifact_header_compatible(v2_path, v2_hash, kFormatVersionV2),
          "header probe rejects prior-schema v2 artifact");

    std::vector<uint8_t> stale_definition = current_v2;
    // Common body: schema u32, material count u32, then MaterialDef bytes.
    stale_definition[40 + 8 + sizeof(MaterialDef) / 2] ^= 0x01;
    const uint64_t definition_hash =
        fnv1a64(stale_definition.data() + 40, stale_definition.size() - 40);
    memcpy(stale_definition.data() + 32, &definition_hash, sizeof(definition_hash));
    write_artifact(v2_path, v2_hash, part_bundle::kSectionRep0, stale_definition);
    CHECK(!is_cache_artifact_header_compatible(v2_path, v2_hash, kFormatVersionV2),
          "header probe rejects changed material definition in prefix");

    // Corrupt a byte in the BODY PAST the material prefix: header probe must
    // still accept (it does not hash the body); loader must reject it.
    write_artifact(v2_path, v2_hash, part_bundle::kSectionRep0, current_v2);
    std::vector<uint8_t> deep_corrupt = current_v2;
    deep_corrupt.back() ^= 0x01;
    write_artifact(v2_path, v2_hash, part_bundle::kSectionRep0, deep_corrupt);
    CHECK(is_cache_artifact_header_compatible(v2_path, v2_hash, kFormatVersionV2),
          "header probe ignores deep body corruption (loader's job)");
    {
        BLASManager corrupt_blas; TLASManager corrupt_tlas(4);
        std::vector<ChildInstance> corrupt_children;
        LodLevels corrupt_lods;
        CHECK(!load_v2(v2_path, v2_hash, corrupt_blas, corrupt_tlas,
                       corrupt_children, corrupt_lods),
              "loader rejects body-corrupt artifact that header probe accepted");
    }

    write_artifact(v2_path, v2_hash, part_bundle::kSectionRep0, current_v2);
    const uint64_t flat_hash = 0x12345678u;
    const char* flat_path = "test_flat_compat.flat.part";
    remove(flat_path);
    std::vector<FlatCluster> clusters;
    CHECK(save_flat_v3(flat_path, blas, tlas, clusters, flat_hash),
          "compat probe flat fixture saved");
    // M4: this used to be a hand-copied literal (`== 8u`) and had been stale
    // since a0b86a19 bumped the constant to 9 -- a red suite nobody read,
    // because the literal has to be edited by hand every time the format
    // moves. Assert the RELATIONSHIP instead: kFormatVersionFlat is the
    // version vector's flat-format component, so it can never drift from the
    // thing that actually keys the cache.
    CHECK(kFormatVersionFlat == matter_version::components::kFlatFormat,
          "flat format version is the version vector's component, not a copy");
    CHECK(matter_version::fold(flat_hash) != flat_hash,
          "the flat artifact's identity passes through the version fold");
    CHECK(is_cache_artifact_header_compatible(flat_path, flat_hash, kFormatVersionFlat),
          "header probe accepts current flat artifact");
    std::vector<uint8_t> current_flat = read_artifact(flat_path, flat_hash, part_bundle::kSectionFlat);
    std::vector<uint8_t> stale_v6 = current_flat;
    const uint32_t v6 = 6u;
    const uint64_t v6_hash_guard = flat_hash ^ static_cast<uint64_t>(v6);
    memcpy(stale_v6.data() + 4, &v6, sizeof(v6));
    memcpy(stale_v6.data() + 8, &v6_hash_guard, sizeof(v6_hash_guard));
    write_artifact(flat_path, flat_hash, part_bundle::kSectionFlat, stale_v6);
    CHECK(!is_cache_artifact_header_compatible(flat_path, flat_hash, kFormatVersionFlat),
          "header probe rejects stale v6 flat artifact");
    BLASManager stale_blas; TLASManager stale_tlas(64);
    std::vector<FlatCluster> stale_clusters;
    CHECK(!load_flat_v3(flat_path, flat_hash, stale_blas, stale_tlas,
                        stale_clusters),
          "flat loader rejects stale v6 artifact");

    FlatCluster oversized_cluster{};
    for (size_t i = 0; i < 10; ++i) {
        LodLevel level;
        level.blas_indices.push_back(0u);
        oversized_cluster.lods.push_back(std::move(level));
    }
    const char* oversized_path = "test_oversized_lod.flat.part";
    CHECK(!save_flat_v3(oversized_path, blas, tlas,
                        std::vector<FlatCluster>{oversized_cluster}, flat_hash),
          "flat serializer rejects clusters above the shared nine-LOD capacity");

    CHECK(save_flat_v3(flat_path, blas, tlas, clusters, flat_hash),
          "stale v6 flat regenerates through atomic save");
    std::vector<uint8_t> stale_flat = read_artifact(flat_path, flat_hash, part_bundle::kSectionFlat);
    patch_body_u32_and_rehash(stale_flat, 0,
                              MaterialRegistrySchemaVersion() - 1u);
    write_artifact(flat_path, flat_hash, part_bundle::kSectionFlat, stale_flat);
    CHECK(!is_cache_artifact_header_compatible(flat_path, flat_hash, kFormatVersionFlat),
          "header probe rejects prior-schema flat artifact");

    CHECK(save_flat_v3(flat_path, blas, tlas, clusters, flat_hash),
          "stale flat regenerates through atomic save");
    BLASManager flat_blas; TLASManager flat_tlas(64);
    std::vector<FlatCluster> loaded_clusters;
    CHECK(load_flat_v3(flat_path, flat_hash, flat_blas, flat_tlas,
                       loaded_clusters),
          "regenerated flat loads successfully");
    CHECK(is_cache_artifact_header_compatible(flat_path, flat_hash,
                                              kFormatVersionFlat),
          "second flat header probe is warm");

    // Malicious/truncated material prefixes must fail closed without pointer
    // arithmetic wrapping or out-of-bounds reads.
    const char* truncated_path = "test_truncated_prefix.part";
    for (size_t body_size : {size_t(0), size_t(4), size_t(8), size_t(12)}) {
        std::vector<uint8_t> truncated(current_v2.begin(), current_v2.begin() + 40);
        truncated.insert(truncated.end(), current_v2.begin() + 40,
                         current_v2.begin() + 40 + body_size);
        const uint64_t truncated_hash =
            fnv1a64(truncated.data() + 40, truncated.size() - 40);
        memcpy(truncated.data() + 32, &truncated_hash, sizeof(truncated_hash));
        write_artifact(truncated_path, v2_hash, part_bundle::kSectionRep0, truncated);
        CHECK(!is_cache_artifact_header_compatible(truncated_path, v2_hash,
                                                   kFormatVersionV2),
              "header probe rejects truncated material prefix");
        BLASManager truncated_blas; TLASManager truncated_tlas(4);
        std::vector<ChildInstance> truncated_children;
        LodLevels truncated_lods;
        CHECK(!load_v2(truncated_path, v2_hash, truncated_blas,
                       truncated_tlas, truncated_children, truncated_lods),
              "full loader rejects malicious truncated prefix safely");
    }

    remove(v2_path);
    remove(large_path);
    remove(flat_path);
    remove(oversized_path);
    remove(truncated_path);
}

static void test_header_probe() {
    using namespace part_asset;
    BLASManager blas; TLASManager tlas(64);
    BLASHandle hA, hB; build_scene(blas, tlas, hA, hB);
    auto kids = sample_children();
    auto lods = sample_lods();

    const uint64_t v2_hash = 0x76543210u;
    const char* v2_path = "test_header_probe.part";
    remove(v2_path);
    CHECK(save_v2(v2_path, blas, tlas, kids.data(), kids.size(), lods, v2_hash),
          "header probe v2 fixture saved");

    CacheArtifactProbeStats stats{};
    CHECK(is_cache_artifact_header_compatible(
              v2_path, v2_hash, kFormatVersionV2, &stats),
          "header probe accepts valid artifact");
    const size_t material_prefix =
        2 * sizeof(uint32_t) + MaterialRegistryCount() * sizeof(MaterialDef);
    CHECK(stats.body_bytes <= material_prefix,
          "header probe reads at most the material prefix past the header");
    CHECK(!is_cache_artifact_header_compatible(
              v2_path, v2_hash + 1, kFormatVersionV2),
          "header probe rejects wrong resolved hash");
    CHECK(!is_cache_artifact_header_compatible(
              v2_path, v2_hash, kFormatVersionV2 + 1),
          "header probe rejects wrong format version");

    // Corrupt one byte in the BODY PAST the material prefix: header probe must
    // still accept (it does not hash the body)...
    corrupt_byte_at(v2_path, v2_hash, part_bundle::kSectionRep0, 40 + material_prefix + 8);
    CHECK(is_cache_artifact_header_compatible(
              v2_path, v2_hash, kFormatVersionV2),
          "header probe ignores deep body corruption");
    // ...but the loader must reject it (existing fail-closed path).
    BLASManager corrupt_blas; TLASManager corrupt_tlas(4);
    std::vector<ChildInstance> corrupt_kids; LodLevels corrupt_lods;
    CHECK(!load_v2(v2_path, v2_hash, corrupt_blas, corrupt_tlas,
                   corrupt_kids, corrupt_lods),
          "loader rejects corrupt body");

    remove(v2_path);
}

static void test_new_materials() {
    CHECK(MaterialRegistryCount() == 30,
          "registry has 30 materials through the garden presets");
    const MaterialDef* bark = MaterialRegistryGet(14);
    CHECK(bark->albedo[0] > 0.30f && bark->albedo[2] < 0.20f, "material 14 is brown bark");
    const MaterialDef* leaf = MaterialRegistryGet(15);
    CHECK(leaf->albedo[1] > leaf->albedo[0] && leaf->albedo[1] > leaf->albedo[2],
          "material 15 is green leaf");
    const MaterialDef* dirt = MaterialRegistryGet(16);
    CHECK(dirt->albedo[0] > dirt->albedo[2], "material 16 is brown dirt");
}

static void test_flatten_hints_round_trip() {
    // Ensure cache root and parts/ subdir exist.
    make_test_dir(kCacheRoot);
    make_test_dir((std::string(kCacheRoot) + "/parts").c_str());

    const uint64_t h = 0xABCD0000ABCD0000ull;
    // M4: every kind names the ONE bundle; the kind is a section inside it.
    CHECK(part_asset::cache_path_hints(h) == "parts/abcd0000abcd0000.bundle",
          "cache_path_hints names the part's bundle");
    CHECK(part_asset::cache_path_hints(h) == part_asset::cache_path_resolved(h) &&
          part_asset::cache_path_hints(h) == part_asset::cache_path_flat(h) &&
          part_asset::cache_path_hints(h) == part_asset::cache_path_static_lods(h) &&
          part_asset::cache_path_hints(h) == part_asset::cache_path_lods(h),
          "every cache_path_* helper names the same bundle");

    std::string p = std::string(kCacheRoot) + "/" + part_asset::cache_path_hints(h);

    part_asset::FlattenHints out;
    out.child_px[1] = 64.0f;
    out.child_px[5] = 32.0f;
    CHECK(part_asset::save_flatten_hints(p, h, out), "save_flatten_hints returns true");

    part_asset::FlattenHints loaded;
    CHECK(part_asset::load_flatten_hints(p, h, loaded), "load_flatten_hints returns true");
    CHECK(loaded.child_px.size() == 2, "loaded hints has 2 entries");
    CHECK(loaded.child_px.at(1) == 64.0f, "child 1 px preserved");
    CHECK(loaded.child_px.at(5) == 32.0f, "child 5 px preserved");

    part_asset::FlattenHints missing;
    CHECK(!part_asset::load_flatten_hints(p + ".nope", h, missing),
          "load_flatten_hints returns false for missing file");
    CHECK(missing.child_px.empty(), "hints empty after failed load");

    // Malformed-input: valid first line then non-numeric token — must be all-or-nothing.
    std::string pmal = std::string(kCacheRoot) + "/parts/malformed_hints_test.hints";
    {
        std::ofstream ofs(pmal);
        ofs << "1 64\n";
        ofs << "2 notanumber\n";
    }
    part_asset::FlattenHints hmal;
    CHECK(!part_asset::load_flatten_hints(pmal, h, hmal),
          "load_flatten_hints returns false for malformed file");
    CHECK(hmal.child_px.empty(),
          "hints cleared (all-or-nothing) on malformed file");
}

// M4 -- THE BUNDLE IS A PURE FUNCTION OF ITS SECTION SET.
//
// This replaces W5's LMSK byte-identity gate, which went with the trailer. The
// property it guards is the one M4's double-bake acceptance rests on: a bake
// writes a part's sections at different times (body, then flatten, then
// impostor), and worker-pool completion order decides which lands first. If
// the container encoded arrival order, two cold bakes of identical content
// would produce different bytes and the determinism gate would fail
// intermittently -- the worst possible failure mode for a cache.
//
// So: write the same three sections into two bundles in OPPOSITE orders and
// require the files to be byte-identical.
static void test_bundle_is_order_independent() {
    using namespace part_asset;
    BLASManager blasA; TLASManager tlasA(64);
    BLASHandle hA, hB; build_scene(blasA, tlasA, hA, hB);
    auto kids = sample_children();
    auto lods = sample_lods();
    std::vector<VolumeEmitter> emitters;

    const uint64_t hash = 0x7777u;
    const char* path_a = "test_bundle_order_a.bundle";
    const char* path_b = "test_bundle_order_b.bundle";
    remove(path_a); remove(path_b);

    FlattenHints hints;
    hints.child_px[1] = 64.0f;

    // A: body, then hints.
    CHECK(save_v2(path_a, blasA, tlasA, kids.data(), kids.size(), lods, emitters, hash),
          "order A: body written first");
    CHECK(save_flatten_hints(path_a, hash, hints), "order A: hints written second");

    // B: hints, then body.
    CHECK(save_flatten_hints(path_b, hash, hints), "order B: hints written first");
    CHECK(save_v2(path_b, blasA, tlasA, kids.data(), kids.size(), lods, emitters, hash),
          "order B: body written second");

    std::vector<uint8_t> bytes_a = read_file(path_a);
    std::vector<uint8_t> bytes_b = read_file(path_b);
    CHECK(!bytes_a.empty() && bytes_a.size() == bytes_b.size() &&
          memcmp(bytes_a.data(), bytes_b.data(), bytes_a.size()) == 0,
          "bundle bytes depend on the section SET, not the write order");

    // And a merge preserves what it did not write: the body must still load
    // out of the bundle the hints write republished.
    BLASManager blasB; TLASManager tlasB(64);
    std::vector<ChildInstance> kids_out; LodLevels lods_out;
    CHECK(load_v2(path_b, hash, blasB, tlasB, kids_out, lods_out),
          "a section write preserves the sections it did not touch");
    CHECK(kids_out.size() == kids.size(), "merged bundle still has the children");
    FlattenHints hints_out;
    CHECK(load_flatten_hints(path_b, hash, hints_out) && hints_out.child_px.size() == 1,
          "merged bundle still has the hints");

    // A bundle keyed on a different part is not this part's bundle.
    BLASManager blasC; TLASManager tlasC(64);
    std::vector<ChildInstance> kids_c; LodLevels lods_c;
    CHECK(!load_v2(path_b, hash ^ 1ull, blasC, tlasC, kids_c, lods_c),
          "a bundle rejects a load asking for a different part hash");

    remove(path_a); remove(path_b);
}

// W5: cache_path_static_lods / save_static_lod_plan / load_static_lod_plan
// sidecar round-trip (parallel to the existing lodBudgets sidecar test
// pattern), including the malformed-file all-or-nothing guard.
static void test_static_lod_plan_sidecar() {
    using namespace part_asset;
    make_test_dir(kCacheRoot);
    make_test_dir((std::string(kCacheRoot) + "/parts").c_str());

    const uint64_t h = 0x1234000056780000ull;
    CHECK(cache_path_static_lods(h) == "parts/1234000056780000.bundle",
          "cache_path_static_lods names the part's bundle");

    std::string p = std::string(kCacheRoot) + "/" + cache_path_static_lods(h);
    StaticLodPlan plan;
    plan.level_hashes        = { h, 0xAAAAAAAAAAAAAAAAull, h, h };
    plan.level_exclude_masks = { 0u, 0u, 0b11u, 0b1u };
    CHECK(save_static_lod_plan(p, h, plan), "save_static_lod_plan ok");

    StaticLodPlan loaded;
    CHECK(load_static_lod_plan(p, h, loaded), "load_static_lod_plan ok");
    CHECK(loaded.level_hashes == plan.level_hashes, "level hashes preserved");
    CHECK(loaded.level_exclude_masks == plan.level_exclude_masks,
          "level exclude masks preserved");

    // Size mismatch is rejected outright (never silently truncated/padded).
    StaticLodPlan bad;
    bad.level_hashes = { h };
    bad.level_exclude_masks = { 0u, 1u };
    CHECK(!save_static_lod_plan(p + ".bad", h, bad),
          "save_static_lod_plan rejects mismatched array sizes");

    StaticLodPlan missing;
    CHECK(!load_static_lod_plan(p + ".nope", h, missing),
          "load_static_lod_plan returns false for a missing file");
}

int main() {
    test_atomic_replace_preserves_target_on_failure();
    test_cache_path_resolved();
    test_resolved_hash();
    test_resolved_hash_carries_version_vector();
    test_save_v2_header();
    test_round_trip_full();
    test_round_trip_degenerate_lod();
    test_round_trip_no_children();
    test_v2_guards();
    test_material_schema_guard();
    test_cache_artifact_compatibility_probe();
    test_header_probe();
    test_new_materials();
    test_flatten_hints_round_trip();
    test_bundle_is_order_independent();
    test_static_lod_plan_sidecar();
    if (g_failures == 0) printf("All part_asset_v2 tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
