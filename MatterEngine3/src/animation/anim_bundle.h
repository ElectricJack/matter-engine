#pragma once

#include "anim_asset.h"
#include "part_asset_v2.h"

#include <filesystem>
#include <vector>

namespace matter::animation {

struct LodBindingSignature {
    uint64_t indexed_vertex_signature = 0;
    uint32_t vertex_count = 0;
    uint32_t influence_count = 0;
    bool operator==(const LodBindingSignature& v) const {
        return indexed_vertex_signature == v.indexed_vertex_signature &&
               vertex_count == v.vertex_count && influence_count == v.influence_count;
    }
};
struct BundleIdentity {
    uint64_t resolved_hash = 0;
    BuildNonce nonce{};
    uint64_t part_body_checksum = 0;
    uint64_t anim_body_checksum = 0;
    uint32_t part_format_version = part_asset::kFormatVersionV2;
    uint32_t animation_schema_version = kAnimationSchemaVersion;
    uint32_t animation_bake_epoch = kAnimationBakeEpoch;
    uint32_t target_abi_tag = 0;
    uint32_t ozz_tag_hash = 0;
    uint32_t compiler_identifier = kAnimationCompilerIdentifier;
    std::vector<LodBindingSignature> lods;
};
struct BundleCandidates {
    std::filesystem::path part_candidate;
    std::filesystem::path anim_candidate;
    std::filesystem::path cache_root;
    // Test-only fault point: 1/2/3 fails immediately before replacing part/anim/manifest.
    uint32_t test_fail_before_replace = 0;
    // Test-only fault point: report durability uncertainty immediately after
    // replacing part/anim/manifest (the rename itself has completed).
    uint32_t test_fail_after_replace_sync = 0;
    bool test_hold_publication_lock = false;
};
// Test-only companion for deterministic same-process publication contention.
void release_animation_bundle_test_lock();
void set_animation_bundle_test_replace_legacy_lock_directory_once();
// Checksum of a part's REP0 body section, the value stored in
// BundleIdentity::part_body_checksum. The animation link binds to the part
// BODY, so this is deliberately the REP0 section rather than the whole file
// (post-M4 a part shares its .bundle with the flat ladder / impostor atlas).
// Exposed so the bake PRODUCER (script_host) and the publish/load VALIDATORS
// compute it the exact same way — a second private copy is what silently broke
// AnimatedRigGallery's commit after the M4 migration (issue 55f61c18).
bool checksum_part(const std::filesystem::path& part_file, uint64_t part_hash,
                   uint64_t& out);

bool publish_animation_bundle(const BundleCandidates&, const BundleIdentity&, Diagnostics&);
bool load_committed_animation_bundle(const std::filesystem::path&, uint64_t,
                                     BLASManager&, AnimAsset&, Diagnostics&);
} // namespace matter::animation
