#pragma once

// MatterEngine3/src/animation/anim_bundle.h
//
// The commit protocol for an animated part: three files that must agree
// before the runtime will animate anything.
//
//   <cache_root>/parts/<hash>...        part_asset v2 (geometry + link)
//   <cache_root>/parts/<hash>.anim      the animation asset (anim_asset.h)
//   <cache_root>/parts/<hash>.anim.commit  the `MACM` manifest, written last
//
// `BundleIdentity` is the manifest's payload: the shared `BuildNonce`, the
// body checksums of the other two files, the version/epoch/ABI/compiler
// stamps, and one signature per LOD rung. `BundleCandidates` names the two
// candidate files to promote plus the cache root.
//
// Typical producer flow (script_host / the bake path):
//   1. bake the part and the animation to candidate paths,
//   2. fill a `BundleIdentity` — nonce from `generate_build_nonce()`,
//      `anim_body_checksum(asset)`, `checksum_part(...)`,
//      `manifest_lod_signatures(binding)`,
//   3. call `publish_animation_bundle`.
// Consumers call `load_committed_animation_bundle`, which re-runs the whole
// validation against the live files.
//
// Both entry points are self-contained file operations: they take no engine
// locks, but `publish_animation_bundle` takes an exclusive, NON-BLOCKING
// `<manifest>.lock` and fails (`bundle.lock`) rather than waiting if another
// process is publishing the same hash. Neither is cheap — each fully loads
// the part and the anim and rebuilds indexed geometry per LOD.
//
// Every failure path returns false and appends a `bundle.*` code to the
// `Diagnostics`; the caller's output arguments are left untouched.
#include "anim_asset.h"
#include "part_asset_v2.h"

#include <filesystem>
#include <vector>

namespace matter::animation {

// Fingerprint of one LOD rung's skin binding, stored in the manifest so a
// re-meshed part can be detected without re-deriving the whole binding.
// `indexed_vertex_signature` comes from
// `viewer::indexed_part_geometry_signature` and is salted with the rung
// index; `influence_slot_count` counts influence SLOTS
// (`vertices * kMaxSkinInfluences`), not non-zero weights — every vertex
// stores the full four slots whether or not they all carry weight, so the
// producer always emits exactly `vertex_count * kMaxSkinInfluences` here.
// It was called `influence_count`, which read as a count of live influences.
struct LodBindingSignature {
    uint64_t indexed_vertex_signature = 0;
    uint32_t vertex_count = 0;
    uint32_t influence_slot_count = 0;
    bool operator==(const LodBindingSignature& v) const {
        return indexed_vertex_signature == v.indexed_vertex_signature &&
               vertex_count == v.vertex_count && influence_slot_count == v.influence_slot_count;
    }
};
// The commit manifest's contents — the single record that ties the part
// file, the `.anim` file and this build together.
//
// The version/epoch/compiler fields default to this build's constants, and
// both `publish_animation_bundle` and `load_committed_animation_bundle`
// require them to still equal those constants: comparing only the persisted
// copies to each other would prove the siblings agree while all three are
// stale. `target_abi_tag` / `ozz_tag_hash` default to 0 and must be filled in
// by the producer with `kAnimationTargetAbiTag` / `kAnimationOzzTagHash`.
// A zero `nonce` is rejected as unset.
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
// Inputs to one publish: the two freshly baked candidate files and the cache
// root under which their live paths (and the `parts/` directory) are derived.
// The candidates are consumed by the atomic replace, so they no longer exist
// at their candidate paths after a successful publish.
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

// Validate both candidates against `BundleIdentity` and against each other,
// then atomically promote them and write the commit manifest last. Rolls
// back from backups if any replace fails. Takes a non-blocking cross-process
// lock; returns false with a `bundle.*` diagnostic on any failure, having
// left the previously published state intact.
bool publish_animation_bundle(const BundleCandidates&, const BundleIdentity&, Diagnostics&);
// Load `<cache_root>` / `<hash>`'s committed bundle, re-running the full
// publish-side validation. The `BLASManager` and `AnimAsset` outputs are
// assigned only after every check passes, so a failed load never clobbers
// the caller's existing state.
bool load_committed_animation_bundle(const std::filesystem::path&, uint64_t,
                                     BLASManager&, AnimAsset&, Diagnostics&);
} // namespace matter::animation
