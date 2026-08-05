#pragma once
#include <cstdint>

// =============================================================================
// THE VERSION VECTOR — the ONE place a version enters a cache key. (M4)
// =============================================================================
//
// WHY THIS EXISTS. Before M4, versions were plumbed per-artifact-kind: the
// engine bake version reached the resolve key (resolve_cache.cpp) and the
// .gtex content hash (tileset_gtex.cpp) and the settle key (tileset_bake.cpp),
// while the PART resolved hash — which names every `parts/<hash>.*` artifact
// on disk — folded no version at all. A rule change therefore invalidated
// SOME things and not others, and the engine cheerfully served stale geometry
// under new rules. That bug class was found twice in one month
// (docs/lod-vt-redesign-2026-08-04.md §9.3), each time by someone noticing
// wrong pixels rather than by anything failing.
//
// THE PROPERTY THIS ESTABLISHES. There is exactly one place a version can
// enter a key: `matter_version::fold`. Every cache key in the engine ends with
// that call, and nothing outside this header names a component. Adding a
// version therefore means adding ONE line below, and it reaches every key by
// construction. Forgetting one is not merely unlikely; there is nothing to
// forget.
//
// HOW TO ADD A COMPONENT. Add a constant to `components` and fold it in
// `digest()`. Do not add a version anywhere else, and in particular do NOT
// pass a version as a parameter to a key-forming function — that is precisely
// the shape this replaced.
//
// HOW TO BUMP. Increment the component whose meaning changed. Every artifact
// in every cache is then a miss, everywhere, at once. That is intended and is
// the only correct behaviour: two bakes that are not bit-identical must not
// share a cache identity (the `.gtex` precedent, tileset_gtex.h).
//
// WHAT IS *NOT* A KEY. A few file headers record a version as informational
// provenance (`GTexHeader::engine_bake_version`, and the diagnostic printf in
// local_provider.cpp). Those are RECORDS, not keys — they are written so a
// human reading a stale file can see what produced it. They must never be the
// thing a cache probe compares; the probe compares a key, and the key folded
// the whole vector.

namespace matter_version {

// -----------------------------------------------------------------------------
// The vector. Every component that changes ARTIFACT CONTENT lives here and
// nowhere else.
// -----------------------------------------------------------------------------
namespace components {

// Bake semantics of the engine itself: shader edits, tracing rules, geometry
// registration — anything that changes baked BYTES without changing a script.
// History (moved here from tileset_gtex.h at M4, value preserved):
//   1 -> 2 : the .gtex bake moved from GL software-BVH compute to Vulkan HW RT.
//   2 -> 3 : (see above; numbering as recorded in tileset_gtex.h)
//   3 -> 4 : tileset_bake_ao.comp traces around the geometric normal.
//   4 -> 5 : assemble_torus_bvh registers EVERY BLAS entry a part draws.
inline constexpr uint32_t kEngineBake = 5u;

// The physics library. A settle that lands differently is different content.
inline constexpr uint32_t kBox3d = 1u;

// The Representation programme's own version: bump when the LOD ladder RULE,
// the distance table's meaning, or the rep set changes without any format
// change. M1.5's benefit floor and M2.5's terminal impostor rung are the kind
// of change this covers — both moved baked ladders while every part hash and
// every format version stayed put, which is why they had to be smuggled into
// kFormatVersionFlat instead.
inline constexpr uint32_t kRepresentation = 1u;

// On-disk layout versions. These are ALSO written into their file headers as a
// self-describing guard (a truncated or mismatched file must fail to parse,
// not mis-parse); folding them here is what makes them cache KEYS as well.
inline constexpr uint32_t kPartFormat   = 2u;  // part_asset::kFormatVersionV2
inline constexpr uint32_t kFlatFormat   = 9u;  // part_asset::kFormatVersionFlat
// 2: elevation rings. The atlas is the same 128 KiB but the cells inside it
// mean something different (8x8 of 16x16 holding 16 azimuths x 3 elevations,
// was 4x4 of 32x32 holding one equator ring), so a v1 atlas read by a v2
// shader samples a quarter of the wrong cell. Byte-identical SIZE with
// different CONTENT is exactly the case a format version exists for.
inline constexpr uint32_t kImpostorFormat = 2u;  // impostor_bake::kFormatVersion
inline constexpr uint32_t kBundleFormat = 1u;  // part_bundle::kBundleFormatVersion

// Reserved for M3b's content-addressed stage outputs. Zero means "no stage
// format is defined yet"; the bundle already carries a section for them, so
// landing stages is a bump here rather than another format change.
inline constexpr uint32_t kStageFormat = 0u;

}  // namespace components

// -----------------------------------------------------------------------------
// The digest of the whole vector. Pure, constexpr, and the only consumer of
// `components`.
// -----------------------------------------------------------------------------
constexpr uint64_t digest() {
    // SplitMix64 avalanche over each component in a fixed order. Any single
    // component changing changes the digest; the mixer makes near-miss
    // component values produce unrelated digests, so two bumps cannot alias.
    constexpr uint32_t kComponents[] = {
        components::kEngineBake,
        components::kBox3d,
        components::kRepresentation,
        components::kPartFormat,
        components::kFlatFormat,
        components::kImpostorFormat,
        components::kBundleFormat,
        components::kStageFormat,
    };
    uint64_t h = 0x4D56455230303031ull;  // "MVER0001"
    for (uint32_t c : kComponents) {
        uint64_t x = h ^ static_cast<uint64_t>(c);
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        h = x ^ (x >> 31);
    }
    return h ? h : 1ull;  // 0 is a sentinel in several key spaces
}

// -----------------------------------------------------------------------------
// THE FOLD. Every cache key ends here.
//
// Call this as the LAST step of every key computation:
//     return matter_version::fold(h);
// -----------------------------------------------------------------------------
constexpr uint64_t fold(uint64_t key) {
    uint64_t x = key ^ digest();
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x = x ^ (x >> 31);
    return x ? x : 1ull;
}

// The fold must actually move every key — a fold that returned its input would
// be a silent no-op and every acceptance below it would still pass.
static_assert(fold(0ull) != 0ull, "version fold must not pass 0 through");
static_assert(fold(1ull) != 1ull, "version fold must not be the identity");
static_assert(digest() != 0ull, "version digest must never be the 0 sentinel");

// -----------------------------------------------------------------------------
// RECORDS. Provenance written into file headers for humans and diagnostics.
// These are NOT keys — see the note at the top of this file. Anything that
// compares one of these as a cache gate is a bug: compare the key.
// -----------------------------------------------------------------------------
namespace record {
inline constexpr uint32_t engine_bake() { return components::kEngineBake; }
inline constexpr uint32_t box3d()       { return components::kBox3d; }
}  // namespace record

}  // namespace matter_version
