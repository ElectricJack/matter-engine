#include "animation/anim_asset.h"
#include "animation/anim_bundle.h"
#include "check.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using namespace matter::animation;

static const char* kAnimPath = "animation_asset_tests_cache/manm.anim";

static uint64_t fnv(const std::vector<uint8_t>& bytes, size_t offset) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = offset; i < bytes.size(); ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
    return h;
}
static uint64_t file_part_body_checksum(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    return fnv(bytes, 40);
}
static std::vector<uint8_t> read_bytes(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}
static void write_bytes(const char* path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
static void put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
}
static void put64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
}
static void refresh_trailing_checksum(std::vector<uint8_t>& bytes) {
    const uint64_t checksum = fnv(bytes, 0) ^ fnv(std::vector<uint8_t>{}, 0);
    // FNV must exclude the trailing checksum itself.
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i + 8 < bytes.size(); ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
    put64(bytes, bytes.size() - 8, h);
    (void)checksum;
}

static AnimAsset sample_asset() {
    AnimAsset asset;
    asset.resolved_hash = 0x0123456789abcdefull;
    asset.nonce = {0x1020304050607080ull, 0x90a0b0c0d0e0f000ull};
    asset.target_abi_tag = 0x41524942u;
    asset.ozz_tag_hash = 0x00160000u;
    asset.sections = {
        {AnimSectionKind::RigSchema, {1}},
        {AnimSectionKind::InputTargetSchemas, {2}},
        {AnimSectionKind::GraphControllerBytecode, {3}},
        {AnimSectionKind::GeometryBindings, {4}},
        {AnimSectionKind::InverseBindMatrices, {5}},
        {AnimSectionKind::ClusterBounds, {6}},
        {AnimSectionKind::OzzSkeleton, {7}},
        {AnimSectionKind::OzzClips, {8}},
    };
    return asset;
}

static void test_anim_required_sections_fail_closed() {
    Diagnostics diagnostics;
    AnimAsset missing = sample_asset();
    missing.sections.pop_back();
    CHECK(!save_anim_candidate(missing, kAnimPath, diagnostics),
          "reject MANM candidate missing required v1 section");
    AnimAsset duplicate = sample_asset();
    duplicate.sections.push_back(duplicate.sections.front());
    CHECK(!save_anim_candidate(duplicate, kAnimPath, diagnostics),
          "reject MANM candidate duplicate section");
    AnimAsset unknown = sample_asset();
    unknown.sections[0].kind = static_cast<AnimSectionKind>(99);
    CHECK(!save_anim_candidate(unknown, kAnimPath, diagnostics),
          "reject MANM candidate unknown section");
}

static void test_anim_cache_paths_and_nonce() {
    const std::filesystem::path root = "cache-root";
    CHECK(cache_path_anim(root, 1) == root / "parts/0000000000000001.anim", "ANIM cache path is hash-only sibling");
    CHECK(cache_path_anim_commit(root, 1) == root / "parts/0000000000000001.anim.commit", "MACM cache path is hash-only sibling");
    const BuildNonce nonce = generate_build_nonce();
    CHECK(nonce.high != 0 || nonce.low != 0, "generated bundle nonce is nonzero before candidate serialization");
}

static void test_anim_header_table_checksum_and_truncation_rejection() {
    Diagnostics diagnostics;
    const AnimAsset asset = sample_asset();
    CHECK(save_anim_candidate(asset, kAnimPath, diagnostics), "save MANM corruption fixture");
    const std::vector<uint8_t> original = read_bytes(kAnimPath);
    const auto reject = [&](const char* message, const std::vector<uint8_t>& changed) {
        write_bytes(kAnimPath, changed);
        AnimAsset output;
        CHECK(!load_anim(kAnimPath, output, diagnostics), message);
    };
    for (const auto offset : {size_t(4), size_t(8), size_t(12)}) {
        auto changed = original;
        put32(changed, offset, 2);
        reject(offset == 4 ? "reject MANM format epoch" : offset == 8 ? "reject MANM schema epoch" : "reject MANM bake epoch", changed);
    }
    { auto changed = original; changed.back() ^= 0x80; reject("reject MANM body checksum", changed); }
    { auto changed = original; put64(changed, 72, UINT64_MAX - 2); reject("reject MANM section offset overflow", changed); }
    { auto changed = original; put64(changed, 92, 228); reject("reject MANM overlapping sections", changed); }
    { auto changed = original; put64(changed, 80, UINT64_MAX); reject("reject MANM section length overflow", changed); }
    for (size_t length : {size_t(0), size_t(67), size_t(100), original.size() - 1}) {
        auto changed = original; changed.resize(length);
        reject("reject truncated MANM", changed);
    }
    std::remove(kAnimPath);
}

static void test_anim_round_trip_and_corruption() {
    std::remove(kAnimPath);
    Diagnostics diagnostics;
    const AnimAsset written = sample_asset();
    CHECK(save_anim_candidate(written, kAnimPath, diagnostics), "save MANM candidate");

    AnimAsset read;
    CHECK(load_anim(kAnimPath, read, diagnostics), "load MANM candidate");
    CHECK(read == written, "MANM round trip preserves identity and sections");

    std::fstream file(kAnimPath, std::ios::binary | std::ios::in | std::ios::out);
    char bad = 'X';
    file.write(&bad, 1);
    file.close();
    CHECK(!load_anim(kAnimPath, read, diagnostics), "reject corrupt MANM magic");
    CHECK(save_anim_candidate(written, kAnimPath, diagnostics), "rewrite candidate");
    file.open(kAnimPath, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(48); // section count in the v1 little-endian header
    const uint32_t huge = 0xffffffffu;
    file.write(reinterpret_cast<const char*>(&huge), sizeof(huge));
    file.close();
    CHECK(!load_anim(kAnimPath, read, diagnostics), "reject overflowed MANM section count");
    std::remove(kAnimPath);
}

static void test_committed_bundle_rejects_torn_and_mixed_siblings() {
    const uint64_t hash = 0x9988776655443322ull;
    const std::filesystem::path root = "animation_asset_bundle_cache";
    const auto candidate_part = root / "candidate.part";
    const auto candidate_anim = root / "candidate.anim";
    std::filesystem::create_directories(root / "parts");
    BLASManager blas;
    TLASManager tlas(4);
    Tri triangle{};
    triangle.vertex0 = make_float3(0, 0, 0);
    triangle.vertex1 = make_float3(1, 0, 0);
    triangle.vertex2 = make_float3(0, 1, 0);
    triangle.centroid = make_float3(0.333f, 0.333f, 0);
    TriEx extra{};
    const BLASHandle handle = blas.register_triangles(&triangle, 1, &extra);
    TLASManager::DrawInstance instance{};
    instance.blas_handle = handle;
    tlas.draw_batch({instance});
    tlas.build(blas);
    part_asset::PartAnimationLink link{1, 1, hash, 0x1111222233334444ull, 0x5555666677778888ull};
    CHECK(part_asset::save_v2(candidate_part.string(), blas, tlas, nullptr, 0,
                              {}, {}, link, hash), "write ANLK part candidate");
    AnimAsset anim = sample_asset();
    anim.resolved_hash = hash;
    anim.nonce = BuildNonce{link.nonce_high, link.nonce_low};
    Diagnostics diagnostics;
    CHECK(save_anim_candidate(anim, candidate_anim, diagnostics), "write MANM candidate");
    BundleIdentity identity;
    identity.resolved_hash = hash;
    identity.nonce = anim.nonce;
    identity.part_body_checksum = file_part_body_checksum(candidate_part.string().c_str());
    identity.anim_body_checksum = anim_body_checksum(anim);
    identity.target_abi_tag = anim.target_abi_tag;
    identity.ozz_tag_hash = anim.ozz_tag_hash;
    // LOD identity is not an ordered numeric sequence, and total packed
    // influences may legitimately exceed the vertex count (up to four each).
    identity.lods = {{9, 2, 8}, {1, 1, 4}};
    BundleIdentity invalid_lod = identity;
    invalid_lod.lods.push_back({0, 1, 1});
    CHECK(!publish_animation_bundle({candidate_part, candidate_anim, root}, invalid_lod, diagnostics),
          "reject manifest LOD with zero indexed-vertex signature");
    CHECK(publish_animation_bundle({candidate_part, candidate_anim, root}, identity, diagnostics),
          "publish coherent bundle with MACM last");
    std::optional<part_asset::PartAnimationLink> saved_link;
    CHECK(part_asset::load_animation_link((root / part_asset::cache_path_resolved(hash)).string(), hash, saved_link) && saved_link,
          "published part retains valid ANLK");
    AnimAsset direct_anim;
    CHECK(load_anim(cache_path_anim(root, hash), direct_anim, diagnostics), "published MANM remains readable");
    AnimAsset loaded;
    BLASManager unused;
    CHECK(load_committed_animation_bundle(root, hash, unused, loaded, diagnostics),
          "load coherent MANM/ANLK/MACM bundle");
    const auto manifest_path = cache_path_anim_commit(root, hash);
    const std::vector<uint8_t> manifest = read_bytes(manifest_path.string().c_str());
    const auto reject_manifest = [&](const char* message, std::vector<uint8_t> changed) {
        write_bytes(manifest_path.string().c_str(), changed);
        CHECK(!load_committed_animation_bundle(root, hash, unused, loaded, diagnostics), message);
        write_bytes(manifest_path.string().c_str(), manifest);
    };
    { auto changed = manifest; changed[0] = 'X'; reject_manifest("reject MACM magic", changed); }
    { auto changed = manifest; put32(changed, 4, 2); refresh_trailing_checksum(changed); reject_manifest("reject MACM version", changed); }
    { auto changed = manifest; put64(changed, 8, hash + 1); refresh_trailing_checksum(changed); reject_manifest("reject MACM resolved hash", changed); }
    { auto changed = manifest; put64(changed, 16, 1); refresh_trailing_checksum(changed); reject_manifest("reject MACM nonce mismatch", changed); }
    { auto changed = manifest; put64(changed, 32, 1); refresh_trailing_checksum(changed); reject_manifest("reject MACM part checksum mismatch", changed); }
    { auto changed = manifest; put64(changed, 40, 1); refresh_trailing_checksum(changed); reject_manifest("reject MACM anim checksum mismatch", changed); }
    { auto changed = manifest; put32(changed, 48, 99); refresh_trailing_checksum(changed); reject_manifest("reject MACM part format mismatch", changed); }
    { auto changed = manifest; put32(changed, 52, 99); refresh_trailing_checksum(changed); reject_manifest("reject MACM schema mismatch", changed); }
    { auto changed = manifest; put32(changed, 56, 99); refresh_trailing_checksum(changed); reject_manifest("reject MACM bake mismatch", changed); }
    { auto changed = manifest; put32(changed, 60, 99); refresh_trailing_checksum(changed); reject_manifest("reject MACM ABI mismatch", changed); }
    { auto changed = manifest; put32(changed, 64, 99); refresh_trailing_checksum(changed); reject_manifest("reject MACM ozz mismatch", changed); }
    { auto changed = manifest; put32(changed, 68, 99); refresh_trailing_checksum(changed); reject_manifest("reject MACM compiler mismatch", changed); }
    { auto changed = manifest; changed.back() ^= 1; reject_manifest("reject MACM checksum", changed); }
    for (uint32_t stage = 1; stage <= 3; ++stage) {
        const auto failed_part = root / "failed.part";
        const auto failed_anim = root / "failed.anim";
        CHECK(part_asset::save_v2(failed_part.string(), blas, tlas, nullptr, 0, {}, {}, link, hash),
              "rewrite candidate before injected publish failure");
        CHECK(save_anim_candidate(anim, failed_anim, diagnostics), "rewrite anim before injected publish failure");
        BundleIdentity failed_identity = identity;
        failed_identity.part_body_checksum = file_part_body_checksum(failed_part.string().c_str());
        CHECK(!publish_animation_bundle({failed_part, failed_anim, root, stage}, failed_identity, diagnostics),
              "interrupted publication reports failure");
        CHECK(load_committed_animation_bundle(root, hash, unused, loaded, diagnostics),
              "last-good committed bundle survives interrupted publication");
    }
    for (uint32_t replacement = 1; replacement <= 3; ++replacement) {
        const auto failed_part = root / "durability.part";
        const auto failed_anim = root / "durability.anim";
        CHECK(part_asset::save_v2(failed_part.string(), blas, tlas, nullptr, 0, {}, {}, link, hash),
              "rewrite candidate before post-rename durability failure");
        CHECK(save_anim_candidate(anim, failed_anim, diagnostics),
              "rewrite anim before post-rename durability failure");
        BundleIdentity failed_identity = identity;
        failed_identity.part_body_checksum = file_part_body_checksum(failed_part.string().c_str());
        CHECK(!publish_animation_bundle({failed_part, failed_anim, root, 0, replacement}, failed_identity, diagnostics),
              "post-rename durability uncertainty reports publication failure");
        CHECK(load_committed_animation_bundle(root, hash, unused, loaded, diagnostics),
              "last-good committed bundle survives post-rename durability uncertainty");
    }
    const auto stale_lock = cache_path_anim_commit(root, hash).string() + ".lock";
    write_bytes(stale_lock.c_str(), {'s', 't', 'a', 'l', 'e'});
    const auto locked_part = root / "stale-lock.part";
    const auto locked_anim = root / "stale-lock.anim";
    CHECK(part_asset::save_v2(locked_part.string(), blas, tlas, nullptr, 0, {}, {}, link, hash),
          "write candidate behind stale lock file");
    CHECK(save_anim_candidate(anim, locked_anim, diagnostics), "write anim behind stale lock file");
    BundleIdentity locked_identity = identity;
    locked_identity.part_body_checksum = file_part_body_checksum(locked_part.string().c_str());
    CHECK(!publish_animation_bundle({locked_part, locked_anim, root, 1}, locked_identity, diagnostics),
          "stale lock file is recovered and publisher reaches deterministic injected failure");
    CHECK(!std::filesystem::exists(stale_lock), "recovered stale lock file is cleaned up");
    const auto contention_part = root / "contention.part";
    const auto contention_anim = root / "contention.anim";
    CHECK(part_asset::save_v2(contention_part.string(), blas, tlas, nullptr, 0, {}, {}, link, hash),
          "write candidate for same-process lock contention");
    CHECK(save_anim_candidate(anim, contention_anim, diagnostics), "write anim for same-process lock contention");
    BundleIdentity contention_identity = identity;
    contention_identity.part_body_checksum = file_part_body_checksum(contention_part.string().c_str());
    CHECK(!publish_animation_bundle({contention_part, contention_anim, root, 0, 0, true}, contention_identity, diagnostics),
          "test holder acquires the publication lock");
    CHECK(!publish_animation_bundle({contention_part, contention_anim, root, 1}, contention_identity, diagnostics),
          "same-process publisher observes held OS lock");
    release_animation_bundle_test_lock();
    CHECK(!publish_animation_bundle({contention_part, contention_anim, root, 1}, contention_identity, diagnostics),
          "publisher proceeds after held OS lock is released");
    std::filesystem::remove(cache_path_anim_commit(root, hash));
    CHECK(!load_committed_animation_bundle(root, hash, unused, loaded, diagnostics),
          "ANLK part without MACM is an animated cache miss");
    std::filesystem::remove_all(root);
}

static void test_anlk_malformed_and_static_compatibility() {
    const std::filesystem::path root = "animation_asset_tests_cache/anlk";
    const auto static_path = root / "legacy.part";
    const auto linked_path = root / "linked.part";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    BLASManager blas;
    TLASManager tlas(1);
    Tri triangle{};
    triangle.vertex0 = make_float3(0, 0, 0); triangle.vertex1 = make_float3(1, 0, 0); triangle.vertex2 = make_float3(0, 1, 0);
    triangle.centroid = make_float3(.333f, .333f, 0);
    TriEx extra{};
    const BLASHandle handle = blas.register_triangles(&triangle, 1, &extra);
    TLASManager::DrawInstance instance{}; instance.blas_handle = handle; tlas.draw_batch({instance}); tlas.build(blas);
    constexpr uint64_t hash = 0xabcdef0123456789ull;
    CHECK(part_asset::save_v2(static_path.string(), blas, tlas, nullptr, 0, {}, hash), "write legacy static part");
    const auto legacy_bytes = read_bytes(static_path.string().c_str());
    // Fixed pre-A4 golden from d0be4f3f's static writer (not a second current
    // write): the one-triangle/material-registry fixture must stay byte-stable.
    CHECK(legacy_bytes.size() == 4944 && fnv(legacy_bytes, 0) == 0x7da5a4b8e5767282ull,
          "legacy static part exactly matches the pre-A4 golden");
    std::optional<part_asset::PartAnimationLink> absent;
    CHECK(part_asset::load_animation_link(static_path.string(), hash, absent) && !absent, "old static part has no ANLK link");
    const part_asset::PartAnimationLink link{1, 1, hash, 3, 4};
    CHECK(part_asset::save_v2(linked_path.string(), blas, tlas, nullptr, 0, {}, {}, link, hash), "write linked part");
    BLASManager legacy_blas;
    TLASManager legacy_tlas(1);
    std::vector<part_asset::ChildInstance> legacy_children;
    part_asset::LodLevels legacy_lods;
    CHECK(!part_asset::load_v2(linked_path.string(), hash, legacy_blas, legacy_tlas,
                               legacy_children, legacy_lods),
          "legacy load_v2 rejects linked ANLK part rather than silently treating it as static");
    std::optional<part_asset::PartAnimationLink> parsed;
    CHECK(part_asset::load_animation_link(linked_path.string(), hash, parsed) && parsed && parsed->nonce_low == 4, "ANLK round trip");
    auto corrupt = read_bytes(linked_path.string().c_str());
    put32(corrupt, corrupt.size() - 32, 2); // ANLK version
    put64(corrupt, 32, fnv(corrupt, 40)); // refresh ordinary part body checksum
    write_bytes(linked_path.string().c_str(), corrupt);
    CHECK(!part_asset::load_animation_link(linked_path.string(), hash, parsed), "reject malformed ANLK trailer");
    std::filesystem::remove_all(root);
}

static void refresh_part_body_checksum(std::vector<uint8_t>& bytes) {
    put64(bytes, 32, fnv(bytes, 40));
}

static void test_part_v2_preflight_boundary_and_no_side_effects() {
    const std::filesystem::path root = "animation_asset_tests_cache/preflight";
    const auto static_path = root / "static.part";
    const auto emit_path = root / "emit-anlk-bytes.part";
    const auto suffix_path = root / "suffix.part";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    constexpr uint64_t hash = 0x2a4d9c7e11335577ull;
    BLASManager source_blas;
    TLASManager source_tlas(1);
    Tri triangle{};
    triangle.vertex0 = make_float3(0, 0, 0); triangle.vertex1 = make_float3(1, 0, 0); triangle.vertex2 = make_float3(0, 1, 0);
    triangle.centroid = make_float3(.333f, .333f, 0);
    TriEx extra{};
    const BLASHandle handle = source_blas.register_triangles(&triangle, 1, &extra);
    TLASManager::DrawInstance instance{}; instance.blas_handle = handle;
    source_tlas.draw_batch({instance}); source_tlas.build(source_blas);
    CHECK(part_asset::save_v2(static_path.string(), source_blas, source_tlas,
                              nullptr, 0, {}, hash), "write preflight static part");

    // A syntactically valid common body followed by an unknown byte must fail
    // before it mutates a caller-owned BLAS manager.
    auto suffix = read_bytes(static_path.string().c_str());
    suffix.push_back(0x7f);
    refresh_part_body_checksum(suffix);
    write_bytes(suffix_path.string().c_str(), suffix);
    BLASManager legacy_blas;
    TLASManager legacy_tlas(1);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    CHECK(!part_asset::load_v2(suffix_path.string(), hash, legacy_blas, legacy_tlas,
                               children, lods),
          "legacy v2 rejects an appended unknown suffix");
    CHECK(legacy_blas.live_count() == 0,
          "legacy malformed suffix leaves BLAS manager unchanged");
    BLASManager animation_blas;
    TLASManager animation_tlas(1);
    std::vector<part_asset::VolumeEmitter> rejected_emitters;
    std::optional<part_asset::PartAnimationLink> rejected_link;
    CHECK(!part_asset::load_v2(suffix_path.string(), hash, animation_blas, animation_tlas,
                               children, lods, rejected_emitters, rejected_link),
          "animation-aware v2 rejects an appended unknown suffix");
    CHECK(animation_blas.live_count() == 0,
          "animation-aware malformed suffix leaves BLAS manager unchanged");

    auto truncated_suffix = read_bytes(static_path.string().c_str());
    truncated_suffix.insert(truncated_suffix.end(), {'E', 'M', 'I'});
    refresh_part_body_checksum(truncated_suffix);
    write_bytes(suffix_path.string().c_str(), truncated_suffix);
    CHECK(!part_asset::load_v2(suffix_path.string(), hash, animation_blas, animation_tlas,
                               children, lods, rejected_emitters, rejected_link),
          "animation-aware v2 rejects a truncated trailer");
    CHECK(animation_blas.live_count() == 0,
          "truncated trailer leaves BLAS manager unchanged");

    // ANLK-shaped bytes inside a raw EMIT payload are data, not a trailer.
    part_asset::VolumeEmitter emitter{};
    const part_asset::PartAnimationLink fake_link{1, 1, hash, 0x1234, 0x5678};
    static_assert(sizeof(fake_link) == 32, "ANLK payload fields are stable");
    std::memcpy(reinterpret_cast<uint8_t*>(&emitter) + sizeof(emitter) - 32,
                reinterpret_cast<const uint8_t*>(&fake_link), sizeof(fake_link));
    // Include the tag immediately before the payload fields so the last 36 bytes
    // of the EMIT payload exactly resemble an ANLK record.
    std::memcpy(reinterpret_cast<uint8_t*>(&emitter) + sizeof(emitter) - 36,
                "KLNA", 4);
    CHECK(part_asset::save_v2(emit_path.string(), source_blas, source_tlas,
                              nullptr, 0, {}, {emitter}, hash),
          "write EMIT payload ending in ANLK-shaped bytes");
    BLASManager emit_blas;
    TLASManager emit_tlas(1);
    std::vector<part_asset::VolumeEmitter> emitters;
    std::optional<part_asset::PartAnimationLink> parsed_link;
    CHECK(part_asset::load_v2(emit_path.string(), hash, emit_blas, emit_tlas,
                              children, lods, emitters, parsed_link),
          "animation-aware loader accepts static EMIT payload");
    CHECK(!parsed_link,
          "ANLK-shaped EMIT payload bytes are not misclassified as a link trailer");
    CHECK(emitters.size() == 1, "EMIT payload still round-trips");

    std::filesystem::remove_all(root);
}

int main() {
    test_anim_round_trip_and_corruption();
    test_anim_required_sections_fail_closed();
    test_anim_cache_paths_and_nonce();
    test_anim_header_table_checksum_and_truncation_rejection();
    test_committed_bundle_rejects_torn_and_mixed_siblings();
    test_anlk_malformed_and_static_compatibility();
    test_part_v2_preflight_boundary_and_no_side_effects();
    return check_summary();
}
