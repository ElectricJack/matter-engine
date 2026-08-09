#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace matter_stream {

struct Ring { float radius; int rung; };

// ---------------------------------------------------------------------------
// Terrain-LOD variant packing (alpine heightfield ladder).
//
// When Config::terrain_lod_enabled is set, the int the streamer hands out in
// SectorRequest/Eviction/on_published is a packed VARIANT, not a bare scatter
// rung: bits 0-3 scatter tier, bits 4-6 terrain LOD (0 coarsest .. 5 = native
// voxel), bits 7-10 coarser-neighbor edge mask (bit layout matches
// terrain_mesher::EdgeMaskBits). The packed value flows through the
// coordinator and resident bookkeeping opaquely — equality is identity — and
// is unpacked only where the bake request JSON is built. With the flag off
// the value is the bare scatter rung, exactly as before.
// ---------------------------------------------------------------------------
// Bit 11 marks a packed variant, making the encoding self-identifying: a bare
// scatter rung (any legacy value < 16) decodes as terrain LOD 5 (native
// voxel) with an empty edge mask, so consumers can unpack unconditionally.
constexpr int kVariantMarker = 1 << 11;
constexpr int pack_variant(int scatter, int terrain_lod, int edge_mask) {
    return kVariantMarker | (scatter & 0xF) | ((terrain_lod & 0x7) << 4) |
           ((edge_mask & 0xF) << 7);
}
constexpr bool variant_packed(int v)     { return v >= 0 && (v & kVariantMarker) != 0; }
constexpr int variant_scatter(int v)     { return variant_packed(v) ? (v & 0xF) : v; }
constexpr int variant_terrain_lod(int v) { return variant_packed(v) ? ((v >> 4) & 0x7) : 5; }
constexpr int variant_edge_mask(int v)   { return variant_packed(v) ? ((v >> 7) & 0xF) : 0; }

// ---------------------------------------------------------------------------
// NESTED SECTOR LOD (docs/terrain-nested-sector-lod-2026-08-08.md)
//
// Level L has sector size S_0 << L and native voxel 2 << L (mesher rung -L), so
// cells-per-tile is constant and each level's annulus holds a near-constant
// tile count -- instead of the uniform grid's O(R^2), where the two coarsest
// bands hold 78% of 78k sectors and draw one to four quads apiece.
//
// LEVEL NEEDS NO NEW VARIANT BITS. While cells-per-tile is constant, level is a
// pure function of the terrain LOD the variant already carries:
//
//     level = 5 - terrain_lod        size = S_0 << level
//
// so SectorKey, sector_instance_id, same_publication_tag and the whole
// coordinator keep treating the packed value as an opaque identity, and a
// legacy bare rung still decodes to level 0 at S_0. Bits 12-14 are reserved
// for the day level and voxel rung decouple; they do not in this design,
// because constant cells-per-tile IS the design.
//
// Two tiles at different levels can share numeric (tx, tz) -- the level makes
// them distinct, and it is inside the compared value.
constexpr int kMaxLevel = 5;
constexpr int variant_level(int v) {
    return kMaxLevel - variant_terrain_lod(v);
}

struct Config {
    float sector_size = 16.0f;
    // Innermost first. A sector's desired rung = the rung of the first ring
    // whose radius covers the anchor->sector-centre distance; beyond the last
    // ring the sector is not desired.
    std::vector<Ring> rings { {48.0f, 3}, {120.0f, 2}, {300.0f, 1}, {800.0f, 0} };
    float hysteresis = 16.0f;        // extra distance before demote/evict
    int   max_inflight = 8;
    int   fail_cooldown_updates = 64;

    // Heightfield terrain-LOD ladder. Off by default: requests then carry the
    // bare scatter rung and nothing else changes. When enabled, each desired
    // sector additionally gets a terrain LOD from `terrain_bands` (innermost
    // first, radius -> LOD; empty = the design-doc default profile scaled by
    // sector_size: 3S->5, 5S->4, 8S->3, 14S->2, 24S->1, 40S->0), the desired
    // map is balanced so cardinal neighbors differ by at most one level, and
    // the packed variant carries the four-bit coarser-neighbor edge mask.
    bool terrain_lod_enabled = false;
    std::vector<Ring> terrain_bands;   // radius -> terrain LOD when enabled

    // Nested sector LOD. Off by default: the uniform-grid path below runs
    // byte-for-byte as it always has, and that is the rollback position.
    //
    // On, `terrain_bands` is REINTERPRETED rather than replaced: the band with
    // LOD l is the annulus where level (5 - l) tiles live, so the same authored
    // table and the same tuning UI drive both modes. Two further rules follow
    // from nesting and are documented here because they change what an existing
    // world's numbers mean:
    //
    //   * the outermost terrain BAND bounds residency, not the outermost
    //     scatter ring. Today a sector past the last ring is never requested no
    //     matter what the bands say -- the trap StreamMountain's own comment
    //     documents ("the world simply stopped about 1 km out"). Rings become
    //     scatter-tier-only.
    //   * a tile is desired if any part of it is within reach (the coverage
    //     rule), not if its centre is. That is what makes the desired set a
    //     hole-free quadtree.
    bool nested_sectors = false;
};

// Fill Config::terrain_bands with the default radial profile (scaled by
// sector_size) when the ladder is enabled and no bands were provided. The
// streamer constructor applies this itself; external profile consumers (the
// editor's LOD Settings query) call it to display the same resolved values.
void resolve_terrain_defaults(Config& cfg);

struct SectorRequest { int64_t tx, tz; int rung; };
struct Eviction      { int64_t tx, tz; int rung; };

class SectorStreamer {
public:
    explicit SectorStreamer(Config cfg);

    // Recompute the desired set for this anchor position (call once per tick).
    void update(float anchor_x, float anchor_z);

    // Next bake to launch: holes (nothing resident) before upgrades, nearest
    // first within each class. Returns false when nothing is needed or
    // max_inflight is reached. Marks the sector in-flight.
    bool next_request(SectorRequest& out);

    // Bake finished. Returns true if the streamer accepted it as resident
    // (the caller publishes). Returns false if it is no longer desired
    // (anchor moved on / clear() happened) — the caller must discard the
    // artifact WITHOUT publishing. On an accepted upgrade, the previously
    // resident rung is queued as an eviction (publish-then-evict: no hole).
    bool on_published(int64_t tx, int64_t tz, int rung);

    // Bake failed: drop from inflight, cool down before re-requesting.
    void on_failed(int64_t tx, int64_t tz, int rung);

    // Caller could not retain bookkeeping for a request it just received.
    // Drop that exact inflight marker without treating it as a bake failure or
    // applying cooldown. Returns false when the tag is no longer inflight.
    bool cancel_request(int64_t tx, int64_t tz, int rung) noexcept;

    // Drain sectors to unpublish + release (each was previously accepted).
    std::vector<Eviction> take_evictions();

    // Transactional ownership seam for durable consumers. peek_evictions()
    // never drains; commit_evictions() removes only the validated prefix from
    // that exact source vector after the destination has accepted every tag.
    const std::vector<Eviction>& peek_evictions() const noexcept;
    bool commit_evictions(
        const std::vector<Eviction>& source,
        size_t count) noexcept;

    // Reroll: every resident sector moves to the eviction queue; inflight
    // bookkeeping resets (their on_published will return false).
    void clear();

    size_t resident_count() const;
    size_t inflight_count() const;

private:
    Config cfg_;

    struct SectorState {
        int   resident_rung = -1;   // -1 = nothing resident
        int   inflight_rung = -1;   // -1 = no request outstanding
        int   desired_rung  = -1;   // recomputed each update(); -1 = not desired
        int   desired_lod   = -1;   // transient terrain LOD during update()
        int   desired_level = -1;   // transient nesting level during update()
        float dist          = 0.0f; // anchor distance at last update
        int   cooldown      = 0;    // updates remaining before re-request allowed
    };

    // Key: (uint64_t(uint32_t(int32_t(tx))) << 32) | uint32_t(int32_t(tz))
    // Sectors beyond ±2^31 in either axis are out of scope.
    static uint64_t key(int64_t tx, int64_t tz) {
        return (uint64_t(uint32_t(int32_t(tx))) << 32) | uint64_t(uint32_t(int32_t(tz)));
    }
    static void unkey(uint64_t k, int64_t& tx, int64_t& tz) {
        tx = int64_t(int32_t(uint32_t(k >> 32)));
        tz = int64_t(int32_t(uint32_t(k & 0xFFFFFFFFu)));
    }

    // Nested key: 4 bits level, then 30 bits each of tx/tz -- +-2^29 tiles per
    // axis, which is +-34,000 km at a 64 m level 0. The two-axis pack above
    // cannot carry a level, and the two modes never share a streamer instance,
    // so the pair below dispatches on the flag rather than trying to unify.
    static uint64_t nested_key(int level, int64_t tx, int64_t tz) {
        return (uint64_t(uint32_t(level) & 0xFu) << 60) |
               ((uint64_t(tx) & 0x3FFFFFFFull) << 30) |
               (uint64_t(tz) & 0x3FFFFFFFull);
    }
    static void nested_unkey(uint64_t k, int& level, int64_t& tx, int64_t& tz) {
        level = int((k >> 60) & 0xFu);
        const auto sext30 = [](uint64_t v) -> int64_t {
            int64_t s = int64_t(v & 0x3FFFFFFFull);
            return (s & 0x20000000ll) ? s - 0x40000000ll : s;
        };
        tx = sext30(k >> 30);
        tz = sext30(k);
    }
    uint64_t skey(int level, int64_t tx, int64_t tz) const {
        return cfg_.nested_sectors ? nested_key(level, tx, tz) : key(tx, tz);
    }
    void sunkey(uint64_t k, int& level, int64_t& tx, int64_t& tz) const {
        if (cfg_.nested_sectors) { nested_unkey(k, level, tx, tz); return; }
        level = 0;
        unkey(k, tx, tz);
    }
    // The key an EXTERNAL (tx, tz, rung) triple names. This is why level needs
    // no place in SectorRequest/Eviction: it is already inside the packed
    // variant, so a publish or a failure finds its own entry with no new field
    // and no coordinator change.
    uint64_t key_for(int64_t tx, int64_t tz, int rung) const {
        return cfg_.nested_sectors ? nested_key(variant_level(rung), tx, tz)
                                   : key(tx, tz);
    }

    struct KeyHash {
        size_t operator()(uint64_t k) const noexcept { return k ^ (k >> 33); }
    };
    std::unordered_map<uint64_t, SectorState, KeyHash> sectors_;

    std::vector<Eviction> evictions_;
    int inflight_ = 0;
    float last_anchor_x_ = 0.0f;
    float last_anchor_z_ = 0.0f;

    // desired_rung for a given anchor distance
    int desired_rung_for_dist(float d) const;
    // desired terrain LOD for a given anchor distance (terrain_lod_enabled)
    int desired_lod_for_dist(float d) const;
    // anchor-to-sector-centre distance
    float sector_dist(int64_t tx, int64_t tz) const;
    // terrain-LOD pass over the freshly scanned desired map: per-sector band
    // lookup with demotion hysteresis, 2:1 balance relaxation, edge masks,
    // then repack every desired_rung as a variant.
    void assign_terrain_lods();

    // ---- nested mode ------------------------------------------------------
    // Outer radius per level, index = level, built from terrain_bands in the
    // constructor (band LOD l -> level 5 - l). Empty when nesting is off.
    std::vector<float> level_radius_;
    int   max_level() const {
        return level_radius_.empty() ? 0 : int(level_radius_.size()) - 1;
    }
    float reach() const {
        return level_radius_.empty() ? 0.0f : level_radius_.back();
    }
    float level_size(int level) const {
        return cfg_.sector_size * float(1 << level);
    }
    // Anchor to tile centre, and anchor to the tile's NEAREST point. The split
    // test uses the nearest point (a tile splits if any of it is close enough),
    // which is what makes the desired set a coverage-rule quadtree; everything
    // that ranks or bands a tile as a whole uses the centre.
    float tile_centre_dist(int level, int64_t tx, int64_t tz) const;
    float tile_near_dist(int level, int64_t tx, int64_t tz) const;
    // The scatter tier for a tile centre. In nested mode the rings no longer
    // bound residency, so past the last ring this clamps to the coarsest tier
    // instead of returning "not desired".
    int nested_scatter_tier(float centre_dist) const;
    // Which level is desired at a world point right now, or -1. Exactly one
    // level covers any point (coverage rule), so this walks 0..max_level.
    int desired_level_at(float wx, float wz) const;
    bool desired_at(int level, int64_t tx, int64_t tz) const {
        auto it = sectors_.find(nested_key(level, tx, tz));
        return it != sectors_.end() && it->second.desired_level == level;
    }
    // The finest level found along one edge, by recursive halving. A neighbour
    // at or above a segment's own level covers that whole segment (grids nest),
    // so a well-formed ladder costs ONE probe per side; only where the far side
    // is genuinely finer does this subdivide, and it stops as soon as it finds
    // a level low enough to be a violation. Probing the edge at the finest tile
    // pitch instead -- the obvious implementation -- costs 2^level probes per
    // side and made update() the most expensive thing in the streamer.
    int min_edge_level(bool vary_z, float fixed, float t0, float t1,
                       int seg_level, int stop_below) const;

    // TRANSITION GROUPS. A tile that stops being desired under nesting has
    // usually been SUPERSEDED -- split into four children, or merged into a
    // parent -- and evicting it the moment the descent changes its mind leaves
    // a hole for as long as the replacements take to bake, which is seconds.
    //
    // So the rule is: the old residency is torn down only once the complete new
    // residency exists. These two walk the superseded tile's footprint and
    // report whether anything desired covers it (if not, it is genuinely out of
    // range and goes now) and whether all of that is resident yet.
    //
    // Coverage makes this cheap and exact: at most one ANCESTOR can be desired
    // (a merge), and otherwise the desired tiles under it tile its footprint
    // exactly (a split, possibly of differing depths after a restriction pass).
    void scan_footprint(int level, int64_t tx, int64_t tz,
                        bool& any_desired, bool& all_resident) const;
    void scan_subtree(int level, int64_t tx, int64_t tz,
                      bool& any_desired, bool& all_resident) const;

    void update_nested(float anchor_x, float anchor_z);
    // Quadtree descent: mark (level, tx, tz) desired or recurse into its four
    // children. `finer_resident` holds the ancestors of every resident tile, so
    // a tile that is currently SPLIT can be told apart from one that is not --
    // which is the difference between promotion (no hysteresis) and a merge
    // (hysteresis), exactly as the uniform path distinguishes them.
    void descend(int level, int64_t tx, int64_t tz,
                 const std::unordered_map<uint64_t, char, KeyHash>& finer_resident);
    void mark_desired(int level, int64_t tx, int64_t tz);
    // Cardinal-adjacent desired tiles must differ by at most one level. Splits
    // the coarser side to a fixpoint; a no-op for band tables whose every
    // annulus is wider than one tile of the coarser level, which the default
    // profile satisfies by construction.
    void restrict_levels();
    // Bit n iff the cardinal neighbour on side n is exactly one level coarser.
    // A tile's whole edge abuts exactly one coarser tile (grids nest), so four
    // bits still suffice and no half-edge generalisation is needed.
    void assign_nested_masks();
};

} // namespace matter_stream
