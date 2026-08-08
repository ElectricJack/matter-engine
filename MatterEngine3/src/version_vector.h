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
//   5 -> 6 : reproject_triex(SampleSource) keeps faceted authored fields
//            faceted: a rung whose corner donors all author N0=N1=N2 = their
//            geometric face normal now takes its OWN face normal instead of
//            interpolating between facet constants. Found while investigating
//            issue 7b64dc27 (whose visible pale/dark split measured out as an
//            ALBEDO difference, tracked separately); what THIS fixes is the
//            manufactured smooth gradient on coarse rungs of faceted DSL
//            geometry — AlpineDeciduous terminal rung: facet fraction 0.00,
//            composite lit-factor mean +11.4% and contrast loss vs rep 0,
//            now facet 1.00 / +5.5% (lod_normal_consistency_tests).
//            Every reprojected ladder rung's TriEx bytes can differ.
//   6 -> 7 : the BLAS table serializes Tri through zeroed staging. Tri's four
//            union{float3;__m128} slots each leave 4 bytes a float3 assignment
//            never touches, and those were written verbatim — so every .part
//            and .flat.part carried 16 bytes of allocator garbage per triangle
//            and two processes baking identical geometry produced different
//            bytes. NOT a format bump: the layout, the field order and the
//            reader are all unchanged, and a pre-fix artifact still parses to
//            the same geometry. What changed is the BYTES a bake emits, which
//            is precisely what this component means. Every part artifact must
//            miss so the cache holds only reproducible bytes — otherwise the
//            cross-process gate diffs a new bake against a garbage-padded one.
inline constexpr uint32_t kEngineBake = 7u;

// The physics library. A settle that lands differently is different content.
inline constexpr uint32_t kBox3d = 1u;

// The Representation programme's own version: bump when the LOD ladder RULE,
// the distance table's meaning, or the rep set changes without any format
// change. M1.5's benefit floor and M2.5's terminal impostor rung are the kind
// of change this covers — both moved baked ladders while every part hash and
// every format version stayed put, which is why they had to be smuggled into
// kFormatVersionFlat instead.
//   1 -> 2 : the per-part `static noImpostor` opt-out. A flagged part's default
//            (and authored) ladder now TERMINATES at its coarsest mesh rung
//            instead of appending the two-triangle billboard, and bakes no
//            impostor atlas. The part hash already distinguishes a flagged
//            authoring from an unflagged one (the flag lives in the source the
//            resolved hash folds), so this bump is not what separates them; it
//            is what forces every EXISTING flat on disk — baked under the old
//            "every eligible default ladder grows an impostor" rule — to rebake
//            under the new rule so a flag added to a cached part actually drops
//            the billboard rather than being served the stale one.
inline constexpr uint32_t kRepresentation = 2u;

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
// 3: the atlas depicts REP 0, not the coarsest mesh rung. Same layout, same
// bytes, different picture -- and the depicts-hash is now folded over rep 0's
// triangles, so a v2 atlas beside a v3 ladder would be rejected as stale
// rather than mis-drawn. Bumping is what turns that rejection into a rebake.
// 4: cell resolution 16 -> 32 px (layer 128 -> 256, atlas 128 -> 512 KiB). The
// rings' "free" claim was wrong -- the alpha cutout binds on thin features at
// cell resolution, which is what ate the trunks -- and impostors are now pulled
// closer than the ~10 px handover the atlas was budgeted for.
// 5: cell resolution became a SETTING (MATTER_IMPOSTOR_CELL_PX) and its
// default moved 32 -> 128 px, so the atlas is 512 KiB -> 8 MiB per slot and
// the derived guard band moved half_extent with it. Both the pixels and the
// quad's geometry differ from every v4 artifact on disk.
// 6: the shade layer's B channel carries DEPTH from the near bound instead of
// an AO that was constant 1.0 for every DSL-built part. Same bytes, entirely
// different meaning -- a v5 atlas read as v6 parallaxes against an occlusion
// term -- and the runtime card also moved to the near bound to keep the depth
// write inside layout(depth_less).
// 7: edge padding. Uncovered texels no longer hold the zero-fill: each
// covered texel's normal, depth and tint are dilated kPadTexels rings into
// the silhouette's surround (coverage stays 0, the silhouette does not move).
// A v6 atlas drawn by this engine still slides silhouette taps toward the
// voted material's base colour (tint.a -> 0) and decodes oct (0,0) as a real
// direction -- the pale-olive canopy wash of issue 6e0c76fc -- so v6 bytes
// must rebake, not redraw.
// 8: the tint layer carries the MATERIAL REGISTRY's albedo, not TriEx::tint.
// gbuffer.frag shades a charted mesh rung from the VT page (which, for a
// material with no detail tileset slot, is MaterialGpuRecord::base_roughness)
// and an unrouted one from resolveBaseColor's vertex tint -- two independently
// authored colours that differ ~3x on world_demo's conifers. Cards were always
// on the tint side, so once a6331fba routed the mesh rungs to their pages the
// cards were the only pale thing left. A v7 atlas decodes structurally fine
// and simply shows the old colour, so v7 bytes must rebake, not redraw.
// depicts_hash_add_cluster now folds the referenced materials' albedos too, so
// recolouring a material invalidates the cards that depict it.
inline constexpr uint32_t kImpostorFormat = 8u;  // impostor_bake::kFormatVersion
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
