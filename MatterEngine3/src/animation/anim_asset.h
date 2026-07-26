#pragma once

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
constexpr uint32_t kAnimationBakeEpoch = 2;
constexpr uint32_t kAnimationCompilerIdentifier = 2;
// Runtime-owned compatibility values.  These must be checked against the
// running build at publish and load time; comparing a bundle's copies only
// proves that its siblings agree with one another.
constexpr uint32_t kAnimationTargetAbiTag = 0x41524942u;
constexpr uint32_t kAnimationOzzTagHash = 0x00160000u;

struct BuildNonce { uint64_t high = 0; uint64_t low = 0; bool operator==(const BuildNonce& v) const { return high == v.high && low == v.low; } };
BuildNonce generate_build_nonce();
enum class AnimSectionKind : uint32_t {
    RigSchema = 1, InputTargetSchemas = 2, GraphControllerBytecode = 3,
    GeometryBindings = 4, InverseBindMatrices = 5, ClusterBounds = 6,
    OzzSkeleton = 7, OzzClips = 8, RigidSegments = 9, Attachments = 10,
};
struct AnimSection { AnimSectionKind kind{}; std::vector<uint8_t> bytes; bool operator==(const AnimSection& v) const { return kind == v.kind && bytes == v.bytes; } };
struct AnimAsset {
    uint64_t resolved_hash = 0;
    BuildNonce nonce{};
    uint32_t target_abi_tag = 0;
    uint32_t ozz_tag_hash = 0;
    std::vector<AnimSection> sections;
    bool operator==(const AnimAsset& v) const { return resolved_hash == v.resolved_hash && nonce == v.nonce && target_abi_tag == v.target_abi_tag && ozz_tag_hash == v.ozz_tag_hash && sections == v.sections; }
};

std::filesystem::path cache_path_anim(const std::filesystem::path& cache_root, uint64_t resolved_hash);
std::filesystem::path cache_path_anim_commit(const std::filesystem::path& cache_root, uint64_t resolved_hash);
bool save_anim_candidate(const AnimAsset&, const std::filesystem::path&, Diagnostics&);
bool load_anim(const std::filesystem::path&, AnimAsset&, Diagnostics&);
uint64_t anim_body_checksum(const AnimAsset&);

} // namespace matter::animation
