#pragma once

// MatterEngine3/src/animation/anim_asset.h
//
// The in-memory shape of a baked animation asset, plus the constants that
// decide whether a `.anim` file on disk is usable by the running build.
//
// An `AnimAsset` is deliberately dumb: an identity (`resolved_hash` +
// `nonce`), two runtime-compatibility tags, and a list of typed, opaque
// byte sections. Nothing here interprets a section payload. Section
// producers/consumers live next door:
//  - `animation_binding_bake.h` owns GeometryBindings, InverseBindMatrices,
//    ClusterBounds, RigidSegments and Attachments.
//  - the animation compiler owns RigSchema, InputTargetSchemas,
//    GraphControllerBytecode, OzzSkeleton and OzzClips.
// `anim_asset.cpp` owns the file container; `anim_bundle.h` owns the
// three-file publish transaction (part + anim + commit manifest) that makes
// an asset visible; `animation_asset_store.h` owns process-lifetime
// ownership of loaded assets.
//
// Version gating. Four independent knobs, each meaning something different:
//  - `kAnimFormatVersion`  — the container layout in anim_asset.cpp.
//  - `kAnimationSchemaVersion` — which sections exist and what they mean.
//  - `kAnimationBakeEpoch` — same layout, different *values* produced; bump
//    to force a rebake after fixing a bake bug (see the epoch-3 note below).
//  - `kAnimationCompilerIdentifier` — recorded in the bundle manifest.
// A mismatch on any of the first three makes `load_anim` fail with
// `anim.version`, which callers treat as "rebake", not "corrupt".
//
// All five constants are compared against the *running build*, never
// against another persisted copy — see the note on the ABI/Ozz tags below.
#include "animation_ir.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace matter::animation {

constexpr uint32_t kAnimFormatVersion = 1;
// v2 adds mandatory typed rigid-segment and attachment binding sections. An
// older executable must reject the asset rather than silently dropping them.
// v3 replaces the unframed authoring text/concatenated Ozz clip payload with
// independently versioned, count-framed runtime sections.
constexpr uint32_t kAnimationSchemaVersion = 3;
// Epoch 3: encode_clips previously leaked the authored bake sampleRate into
// the serialized clip's PLAYBACK rate slot, so a `sampleRate: 16` clip played
// 16x too fast at runtime. The section layout is unchanged; only the written
// value is, so this is an epoch bump (assets must rebake), not a schema bump.
constexpr uint32_t kAnimationBakeEpoch = 3;
// Identifies the animation compiler that produced an asset. Not stored in
// the `.anim` header — it lives in the bundle commit manifest, where
// `publish_animation_bundle` and `load_committed_animation_bundle` require
// it to equal this build's value.
constexpr uint32_t kAnimationCompilerIdentifier = 2;
// Runtime-owned compatibility values.  These must be checked against the
// running build at publish and load time; comparing a bundle's copies only
// proves that its siblings agree with one another.
constexpr uint32_t kAnimationTargetAbiTag = 0x41524942u;
constexpr uint32_t kAnimationOzzTagHash = 0x00160000u;

// 128-bit identity of a single bake run, stamped into the part file, its
// `.anim` sibling and the commit manifest. Equality of the three copies is
// what proves the trio came from the same bake; all-zero is the reserved
// "unset" value and is rejected at publish and load.
struct BuildNonce { uint64_t high = 0; uint64_t low = 0; bool operator==(const BuildNonce& v) const { return high == v.high && low == v.low; } };
BuildNonce generate_build_nonce();
// Section discriminator. The numeric values are on-disk format and must not
// be renumbered or reused. All ten must be present exactly once in a valid
// asset (`required_sections` in anim_asset.cpp), so adding a kind is a
// schema-version bump that invalidates every existing asset.
//   1-3  authoring/runtime schema and the compiled controller bytecode
//   4-6  skin binding produced by animation_binding_bake.cpp
//   7-8  Ozz runtime skeleton and clip payloads
//   9-10 rigid-segment and attachment bindings (added in schema v2)
enum class AnimSectionKind : uint32_t {
    RigSchema = 1, InputTargetSchemas = 2, GraphControllerBytecode = 3,
    GeometryBindings = 4, InverseBindMatrices = 5, ClusterBounds = 6,
    OzzSkeleton = 7, OzzClips = 8, RigidSegments = 9, Attachments = 10,
};
// One typed, opaque payload. `bytes` may legitimately be empty (e.g. a bake
// with no rigid segments still writes an RBND section with a zero count) —
// absence of the *section* is an error, emptiness of its payload is not.
struct AnimSection { AnimSectionKind kind{}; std::vector<uint8_t> bytes; bool operator==(const AnimSection& v) const { return kind == v.kind && bytes == v.bytes; } };
// A fully loaded animation asset. Value type: copyable, comparable, owns its
// section bytes; there is no GPU or file handle inside it. Once published it
// is treated as immutable — `AnimationAssetStore` hands out `const AnimAsset*`
// and runtime pose state is kept elsewhere.
//
// `target_abi_tag` / `ozz_tag_hash` are compatibility stamps recorded at bake
// time and compared against this build's `kAnimationTargetAbiTag` /
// `kAnimationOzzTagHash`; they guard against loading Ozz payloads produced by
// a different Ozz version. `sections` is kept in on-disk offset order by
// `load_anim`.
struct AnimAsset {
    uint64_t resolved_hash = 0;
    BuildNonce nonce{};
    uint32_t target_abi_tag = 0;
    uint32_t ozz_tag_hash = 0;
    std::vector<AnimSection> sections;
    bool operator==(const AnimAsset& v) const { return resolved_hash == v.resolved_hash && nonce == v.nonce && target_abi_tag == v.target_abi_tag && ozz_tag_hash == v.ozz_tag_hash && sections == v.sections; }
};

// `<cache_root>/parts/<hash:016x>.anim` — the published asset path.
std::filesystem::path cache_path_anim(const std::filesystem::path& cache_root, uint64_t resolved_hash);
// The same path with `.commit` appended: the bundle manifest that makes the
// part/anim pair visible. Its existence and content are the commit record.
std::filesystem::path cache_path_anim_commit(const std::filesystem::path& cache_root, uint64_t resolved_hash);
// Serialize + fsync to a candidate path (never directly to the published
// path). Returns false and records an `anim.*` diagnostic on failure.
bool save_anim_candidate(const AnimAsset&, const std::filesystem::path&, Diagnostics&);
// Read and fully validate; leaves the output cleared on failure. An
// `anim.version` diagnostic means "baked by a different build" (rebake),
// not "damaged file".
bool load_anim(const std::filesystem::path&, AnimAsset&, Diagnostics&);
// FNV-1a over the section list only — independent of header, nonce and file
// offsets. Stored in `BundleIdentity::anim_body_checksum`.
uint64_t anim_body_checksum(const AnimAsset&);

} // namespace matter::animation
