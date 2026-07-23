#include "animation/anim_asset.h"
#include "animation/anim_bundle.h"
#include "check.h"

#include <cstdio>
#include <fstream>
#include <vector>

using namespace matter::animation;

static const char* kAnimPath = "animation_asset_tests.anim";

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

static AnimAsset sample_asset() {
    AnimAsset asset;
    asset.resolved_hash = 0x0123456789abcdefull;
    asset.nonce = {0x1020304050607080ull, 0x90a0b0c0d0e0f000ull};
    asset.target_abi_tag = 0x41524942u;
    asset.ozz_tag_hash = 0x00160000u;
    asset.sections.push_back({AnimSectionKind::RigSchema, {1, 2, 3}});
    asset.sections.push_back({AnimSectionKind::OzzSkeleton, {4, 5, 6, 7}});
    return asset;
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
    std::filesystem::remove(cache_path_anim_commit(root, hash));
    CHECK(!load_committed_animation_bundle(root, hash, unused, loaded, diagnostics),
          "ANLK part without MACM is an animated cache miss");
    std::filesystem::remove_all(root);
}

int main() {
    test_anim_round_trip_and_corruption();
    test_committed_bundle_rejects_torn_and_mixed_siblings();
    return check_summary();
}
