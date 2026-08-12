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
// voxel). The packed value flows through the coordinator and resident
// bookkeeping opaquely — equality is identity — and is unpacked only where the
// bake request JSON is built. With the flag off the value is the bare scatter
// rung, exactly as before.
// ---------------------------------------------------------------------------
// THE EDGE MASK IS GONE FROM THIS ENCODING. Bits 7-10 used to carry a
// four-bit "which cardinal neighbours are one level coarser" mask, matching a
// `terrain_mesher::EdgeMaskBits` enum that has since been deleted outright, and
// the mesher gated its cross-level ownership reach on it. Two things killed it
// (docs/volumetric-sectors-design-2026-08-10.md §2.4, §4.1 "Consequences" 2):
//
//   * The mask was a promise about the DESIRED map that the DRAWN map
//     routinely broke. It was computed from desired_at(level+1, ...), but the
//     neighbour that is actually on screen is a different tile whenever it is
//     mid-split, mid-merge, held by the transition-group rule, parked, or
//     still baking. When drawn state and baked assumption disagreed the reach
//     was 0 where it needed to be 2 and a one-voxel strip of triangles was
//     emitted by neither tile — the "minor gaps while terrain is baking /
//     changing LOD" report, filed after the gap fix itself had landed.
//   * The mask was part of the BAKE IDENTITY, so every neighbour level change
//     forced a full rebake of this tile — and the two tiles' rebakes finished
//     seconds apart, which maximized rather than minimized the window in which
//     they disagreed. The mechanism meant to close the seam was the mechanism
//     widening it.
//
// The retracted premise was that cross-level seam geometry can be baked at
// all. It cannot be baked neighbour-independently (§4.1 records the proof:
// a shared boundary vertex set forces one canonical resolution for every
// border in the world), and baking it neighbour-DEPENDENTLY is what the above
// two bullets cost. So cross-level seams left the bake entirely: an engine-side
// welder now generates them at runtime from the two tiles that are actually
// drawn, and the streamer no longer knows seams exist. Bits 7-10 join 12-14 in
// the reserved pool; nothing is written there.
// ---------------------------------------------------------------------------
// Bit 11 marks a packed variant, making the encoding self-identifying: a bare
// scatter rung (any legacy value < 16) decodes as terrain LOD 5 (native
// voxel), so consumers can unpack unconditionally.
constexpr int kVariantMarker = 1 << 11;
constexpr int pack_variant(int scatter, int terrain_lod) {
    return kVariantMarker | (scatter & 0xF) | ((terrain_lod & 0x7) << 4);
}
constexpr bool variant_packed(int v)     { return v >= 0 && (v & kVariantMarker) != 0; }
constexpr int variant_scatter(int v)     { return variant_packed(v) ? (v & 0xF) : v; }
constexpr int variant_terrain_lod(int v) { return variant_packed(v) ? ((v >> 4) & 0x7) : 5; }

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

// ---------------------------------------------------------------------------
// THE TILE COORDINATE RANGE (volumetric-sectors M1, design §3.1 and risk §7.8)
//
// The nested key used to be `4 bits level | 30 tx | 30 tz` with zero spare
// bits, which is exactly why the third axis could not simply be added: there
// was nowhere to put it. The repack to `4 level | 20 ty | 20 tx | 20 tz` buys
// Y by spending range, and the trade is worth stating in numbers rather than
// asserting is fine:
//
//   * 20 bits two's complement is [-524288, +524287] TILES per axis. At the
//     smallest S_0 any world in this repo authors (16 m) that is +-8,388 km;
//     at StreamMountain's and StreamCaverns' 64 m it is +-33,550 km. The
//     largest streamed reach anywhere in the repo is StreamMountain's ~10 km,
//     so the bound is roughly 3000x the demonstrated need.
//   * The reduction from 30 bits is real (a factor of 1024 per axis) and it is
//     therefore ASSERTED at world load rather than assumed -- see
//     make_streaming_profile in matter_engine.cpp, which checks the authored
//     reach against sector_coord_fits() and refuses to pretend. A silent
//     overflow here would not crash; it would alias two distant tiles onto one
//     key and stream one of them into the other's footprint, which is the
//     worst possible failure to debug from the outside.
//   * If the bound is ever genuinely a problem the fallback is a 16-byte
//     struct key (design §8), not more folding. Not worth it now.
//
// Encoding is plain two's complement masked to the field width -- "offset
// encoded" in the design's phrasing -- and nested_unkey sign-extends it back.
// Three of every four quadrants have negative coordinates, so the round trip on
// negatives is not an edge case, it is the common case.
// ---------------------------------------------------------------------------
constexpr int     kSectorCoordBits = 20;
constexpr uint64_t kSectorCoordMask = (1ull << kSectorCoordBits) - 1ull;
constexpr int64_t kSectorCoordMin = -(int64_t(1) << (kSectorCoordBits - 1));
constexpr int64_t kSectorCoordMax =  (int64_t(1) << (kSectorCoordBits - 1)) - 1;
constexpr bool sector_coord_fits(int64_t t) {
    return t >= kSectorCoordMin && t <= kSectorCoordMax;
}

// The nested tile key: `4 bits level | 20 ty | 20 tx | 20 tz`.
//
// It was `4 | 30 tx | 30 tz` with ZERO spare bits, and that is the whole reason
// the third axis needed a repack rather than an addition: there was nothing
// left to spend. tz keeps the low field so the low-order behaviour of the
// hashes built on this moves as little as it can; the ordering is otherwise
// arbitrary.
//
// Y IS ALWAYS 0 IN M1 -- every call site in sector_streamer.cpp passes
// SectorStreamer::kFlatTy. The field is here so the identity is right before
// anything depends on it.
//
// At namespace scope, not inside SectorStreamer, for two reasons that are the
// same reason: the format is a property of the streaming coordinate system
// rather than of a streamer instance. matter_engine.cpp's world-load range
// check reads the constants above, and the round-trip test reads these -- and a
// key format whose only test is "streaming still works" is a sign-extension bug
// waiting to surface a week later as inexplicable tiles.
inline uint64_t nested_key(int level, int64_t tx, int64_t ty, int64_t tz) {
    return (uint64_t(uint32_t(level) & 0xFu) << 60) |
           ((uint64_t(ty) & kSectorCoordMask) << 40) |
           ((uint64_t(tx) & kSectorCoordMask) << 20) |
           (uint64_t(tz) & kSectorCoordMask);
}
inline void nested_unkey(uint64_t k, int& level, int64_t& tx, int64_t& ty,
                         int64_t& tz) {
    level = int((k >> 60) & 0xFu);
    // Sign-extend the 20-bit field. Mirrors the sext30 this replaced, and is
    // load-bearing for exactly the same reason: three of the four quadrants
    // around any origin have negative tile indices, so a key that did not
    // round-trip them would not be a rare failure, it would be the usual one.
    const auto sext20 = [](uint64_t v) -> int64_t {
        const int64_t s = int64_t(v & kSectorCoordMask);
        return (s & (int64_t(1) << (kSectorCoordBits - 1)))
                   ? s - (int64_t(1) << kSectorCoordBits)
                   : s;
    };
    ty = sext20(k >> 40);
    tx = sext20(k >> 20);
    tz = sext20(k);
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

    // VOLUMETRIC SECTORS (design §3, volumetric-sectors M3). Nested only,
    // default OFF. The quadtree becomes an OCTREE: a tile is the cube
    // [ty*S, (ty+1)*S) in y as well as x and z, `descend` produces 8 children
    // rather than 4, and `restrict_levels` runs over 6 faces rather than 4.
    //
    // What it buys, and the number that motivates it: a column tile covers
    // [y_min, surface] for its (tx, tz), so a StreamCaverns level-0 tile samples
    // ~590 rows of density to reach -1000 m whether or not there is anything
    // down there. A cube tile samples at most (n+5)^3, and one away from the
    // surface or a cavern shell costs a single all-air/all-solid pass and yields
    // nothing at all.
    //
    // The mesher half already exists and is pinned (`mesh_sector_tiled`, M2);
    // this flag is what makes the streamer ask for vertical stacks so that
    // meshing a 64 m band is the whole tile rather than a tile's worth of world.
    // Forced off when `nested_sectors` is off -- see the note there.
    bool volumetric_sectors = false;

    // ---- occlusion detail cap (M4 Phase A, design 5.2) --------------------
    // Ticks a tile may go unseen before its desired level is capped coarser.
    // Zero DISABLES the cap outright, which is the default and the rollback
    // position -- an engine that never calls mark_visible must behave exactly
    // as it did before this existed.
    int occlusion_grace_ticks = 0;
    // How many levels coarser an unseen tile is allowed to fall. One is the
    // design's conservative suggestion; the 2:1 restriction pass still runs
    // afterwards, so a cap that would break the +-1 invariant against a visible
    // neighbour is undone by restrict_levels rather than drawn.
    int occlusion_cap_levels = 1;

    // The OCTREE'S VERTICAL EXTENT, and only that (volumetric_sectors only).
    // The selector will not descend outside [y_min, y_max] and no tile is
    // requested there, which is what stops an octree over an unbounded axis.
    // These are the world's authored yMin/yMax -- the same two numbers that
    // bound a COLUMN tile's mesh slab today, reinterpreted rather than added,
    // because a cube tile is its own slab (mesh_sector_tiled takes no y range).
    //
    // Ignored entirely with the flag off, where a column already spans them.
    float y_min = -64.0f;
    float y_max = 192.0f;

    // STAGED REFINEMENT (docs/volumetric-sectors-design-2026-08-10.md §4.5,
    // first half). Nested only, default ON.
    //
    // `restrict_levels` constrains the DESIRED map. Nothing constrained the
    // DRAWN one: under fast flight a region's desired level jumps several
    // levels at once (3 -> 0), the tiles rebake independently, and whichever
    // finishes first is drawn at level 0 beside a neighbour still showing
    // level 3. That is both the 8x-detail pop and a face pair outside the seam
    // welder's +-1 domain entirely.
    //
    // On, a tile is never REQUESTED more than one level finer than the
    // resident coverage over its own footprint, so a multi-level jump is
    // served as monotone coarse->fine waves (3, then 2, then 1, then 0), each
    // wave fully resident before the next is requested. Off is the rollback
    // position and the A/B baseline; MATTER_STREAM_NO_STAGING=1 forces it off
    // globally without a rebuild. The rule itself lives in descend(), where
    // the split decision is made — see the long block there.
    bool staged_refinement = true;

    // LATERAL STAGING (§4.5 again, and the M0 acceptance defect R12). The
    // footprint clamp above synchronises a column with its own ancestors and
    // deliberately says nothing about the tiles BESIDE it. That is the hole
    // the M0 soak measured: StreamCaverns settled at 590 sectors logged
    // `drawn_level_violations = 28`, tiles drawn at level 2 against a DRAWN
    // level-4 face neighbour. Two adjacent columns are staged independently,
    // so one whose transition group completes early evicts its coarse parent,
    // is unblocked by the footprint rule, and refines a second level while its
    // neighbour is still holding the coarse tile it has not finished replacing.
    // A two-level face is outside the seam welder's domain entirely (§4.4), so
    // those faces draw with no seam at all.
    //
    // On, the clamp gains a lateral term:
    //
    //     effective_level = max(desired_level,
    //                           resident_level_over(own footprint) - 1,
    //                           coarsest face-neighbour resident level - 1)
    //
    // Requires staged_refinement (it is the same rule widened, and
    // MATTER_STREAM_NO_STAGING=1 kills both); MATTER_STREAM_NO_LATERAL=1 turns
    // off only this term, so the lateral half can be A/B'd against the
    // footprint half in a captured editor flight without a rebuild.
    //
    // It is a NO-OP in any state whose residency is already ±1-balanced,
    // which is what every settled state is — proof in descend(). That is why
    // it cannot lower settled residency: it only ever fires on the transient.
    bool lateral_staging = true;
};

// Fill Config::terrain_bands with the default radial profile (scaled by
// sector_size) when the ladder is enabled and no bands were provided. The
// streamer constructor applies this itself; external profile consumers (the
// editor's LOD Settings query) call it to display the same resolved values.
void resolve_terrain_defaults(Config& cfg);

// A tile's identity as it crosses the streamer's public boundary.
//
// `ty` is the VERTICAL tile index, added by volumetric-sectors M1. It is
// carried, compared, hashed and mixed everywhere `tx`/`tz` are -- and in M1 it
// is always 0, because the descent below is still the XZ quadtree. M3 is what
// makes Y a real streaming axis; M1 exists so that when it does, the identity
// plumbing is already right and the diff is the selection code alone.
struct SectorRequest { int64_t tx, ty, tz; int rung; };
struct Eviction      { int64_t tx, ty, tz; int rung; };

class SectorStreamer {
public:
    explicit SectorStreamer(Config cfg);

    // Recompute the desired set for this anchor position (call once per tick).
    //
    // ANCHOR Y IS STORED AND NOT USED (volumetric-sectors M1). Every distance
    // helper below -- sector_dist, tile_centre_dist, tile_near_dist -- is still
    // a 2-D XZ distance, so passing a camera altitude changes nothing about
    // which tiles are selected. It is threaded now because the alternative is
    // to thread it in M3 alongside the change that makes bands spheres instead
    // of cylinders, where an anchor plumbing mistake would be indistinguishable
    // from a selection-rule mistake.
    void update(float anchor_x, float anchor_y, float anchor_z);

    // Next bake to launch: holes (nothing resident) before upgrades, nearest
    // first within each class. Returns false when nothing is needed or
    // max_inflight is reached. Marks the sector in-flight.
    bool next_request(SectorRequest& out);

    // Bake finished. Returns true if the streamer accepted it as resident
    // (the caller publishes). Returns false if it is no longer desired
    // (anchor moved on / clear() happened) — the caller must discard the
    // artifact WITHOUT publishing. On an accepted upgrade, the previously
    // resident rung is queued as an eviction (publish-then-evict: no hole).
    bool on_published(int64_t tx, int64_t ty, int64_t tz, int rung);

    // Bake failed: drop from inflight, cool down before re-requesting.
    void on_failed(int64_t tx, int64_t ty, int64_t tz, int rung);

    // Caller could not retain bookkeeping for a request it just received.
    // Drop that exact inflight marker without treating it as a bake failure or
    // applying cooldown. Returns false when the tag is no longer inflight.
    bool cancel_request(int64_t tx, int64_t ty, int64_t tz,
                        int rung) noexcept;

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

    // ---- OCCLUSION FEEDBACK (M4 Phase A, design §5.2) ----------------------
    //
    // Report which tiles the renderer actually drew, and how long ago. The
    // streamer folds that into TWO things and never into a third:
    //
    //   * PRIORITY -- a visible hole outranks an offscreen one (next_request);
    //   * a DETAIL CAP -- a tile unseen for longer than `occlusion_grace_ticks`
    //     is desired one or more levels COARSER than its band asks.
    //
    // IT NEVER GATES EXISTENCE. The desired set still covers everything the
    // bands reach, so a wrong or stale visibility bit costs a few seconds of
    // detail and can never open a hole. That asymmetry is the whole safety
    // argument: the readback is fenced and a few frames old by construction,
    // and a signal that could remove coverage would turn that staleness into
    // the exact class of bug M0 spent itself closing.
    //
    // ABSENT BY DEFAULT, which is what keeps headless streaming deterministic:
    // a streamer nobody calls `mark_visible` on has every tile permanently at
    // "never seen", and the cap is written to treat never-seen as VISIBLE
    // rather than as occluded. A test that does not know about occlusion gets
    // the pre-M4 desired map, byte for byte.
    void mark_visible(int64_t tx, int64_t ty, int64_t tz, int rung);
    // Advance the visibility clock. One call per streamer tick, by the caller
    // that owns the readback; `mark_visible` stamps this value.
    void set_visibility_frame(uint64_t frame) noexcept { vis_frame_ = frame; }

    // The RESOLVED config, which is not the one you passed in. The constructor
    // rewrites it: `terrain_lod_enabled` is forced on by nesting, defaults are
    // filled into an empty band table, and both `nested_sectors` and
    // `volumetric_sectors` are forced OFF when the authored bands resolve no
    // level ladder. Every one of those was silent and unobservable until this
    // accessor existed -- a world could ask for volumetric sectors, be refused
    // on the spot, and nothing outside this object could tell.
    const Config& config() const noexcept { return cfg_; }

private:
    Config cfg_;

    struct SectorState {
        int   resident_rung = -1;   // -1 = nothing resident
        int   inflight_rung = -1;   // -1 = no request outstanding
        int   desired_rung  = -1;   // recomputed each update(); -1 = not desired
        int   desired_lod   = -1;   // transient terrain LOD during update()
        int   desired_level = -1;   // transient nesting level during update()
        // Visibility clock stamp of the last frame this tile was reported
        // drawn. 0 = NEVER SEEN, which the cap deliberately reads as "visible"
        // rather than "occluded" -- a tile that has never been drawn is usually
        // one that has never been resident, and demoting it on that basis would
        // make the cap fire hardest during the cold fill it can least afford.
        uint64_t last_visible = 0;
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

    // THE M1 FLAT-Y MARKER. Every nested_key() call in sector_streamer.cpp that
    // names a tile the DESCENT chose passes this rather than a literal 0, so
    // "where does the streamer still assume the world is one tile tall?" is a
    // single grep. M3 replaces these with real vertical indices; nothing else
    // about the key changes then.
    static constexpr int64_t kFlatTy = 0;
    uint64_t skey(int level, int64_t tx, int64_t ty, int64_t tz) const {
        return cfg_.nested_sectors ? nested_key(level, tx, ty, tz)
                                   : key(tx, tz);
    }
    void sunkey(uint64_t k, int& level, int64_t& tx, int64_t& ty,
                int64_t& tz) const {
        if (cfg_.nested_sectors) { nested_unkey(k, level, tx, ty, tz); return; }
        level = 0;
        ty = 0;   // the uniform grid is a single row of columns, by definition
        unkey(k, tx, tz);
    }
    // The key an EXTERNAL (tx, ty, tz, rung) tuple names. This is why level
    // needs no place in SectorRequest/Eviction: it is already inside the packed
    // variant, so a publish or a failure finds its own entry with no new field
    // and no coordinator change.
    uint64_t key_for(int64_t tx, int64_t ty, int64_t tz, int rung) const {
        return cfg_.nested_sectors
                   ? nested_key(variant_level(rung), tx, ty, tz)
                   : key(tx, tz);
    }

    struct KeyHash {
        size_t operator()(uint64_t k) const noexcept { return k ^ (k >> 33); }
    };
    std::unordered_map<uint64_t, SectorState, KeyHash> sectors_;

    std::vector<Eviction> evictions_;
    int inflight_ = 0;
    float last_anchor_x_ = 0.0f;
    // READ ONLY UNDER `volumetric_sectors` (M3-WP2; stored and unread in M1).
    // `tile_near_dist` adds the y term and the top-level scan sweeps a `ty`
    // range only when the flag is on, so with it off a camera 900 m underground
    // still selects exactly the tiles it selected before this field existed --
    // asserted, not assumed (sector_streamer_tests' flag-off trace).
    //
    // It was threaded in M1, one milestone before anything consulted it,
    // precisely so that the anchor's route -- WorldTransform m[7] ->
    // submit_anchor -> AnchorSample -> here -- was proven end to end before a
    // selection rule depended on it. A wrong altitude and a wrong octree would
    // otherwise have been indistinguishable.
    // Visibility clock, advanced by the caller that owns the readback.
    uint64_t vis_frame_ = 0;
    float last_anchor_y_ = 0.0f;
    float last_anchor_z_ = 0.0f;

    // desired_rung for a given anchor distance
    int desired_rung_for_dist(float d) const;
    // desired terrain LOD for a given anchor distance (terrain_lod_enabled)
    int desired_lod_for_dist(float d) const;
    // anchor-to-sector-centre distance
    float sector_dist(int64_t tx, int64_t tz) const;
    // terrain-LOD pass over the freshly scanned desired map: per-sector band
    // lookup with demotion hysteresis, 2:1 balance relaxation, then repack
    // every desired_rung as a variant.
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
    //
    // THE TWO DIVERGE UNDER `volumetric_sectors`, on purpose.
    //
    // `tile_near_dist` becomes 3D: it takes `ty` and measures to the cube, so
    // the bands become spheres and the descent an octree. That is the selection
    // rule and it has to see altitude.
    //
    // `tile_centre_dist` stays an XZ distance. It feeds `st.dist`, which grades
    // the SCATTER TIER, and scatter is vegetation standing on the surface -- a
    // horizontal notion. Adding the y term there would coarsen a surface tile's
    // vegetation because the camera happens to be flying above it, and would
    // make a tile 500 m underground compete for a scatter tier it must not have
    // at all. What that tile actually needs is to own no scatter, which is the
    // column-ownership rule (M3-WP5), not a distance.
    float tile_centre_dist(int level, int64_t tx, int64_t tz) const;
    float tile_near_dist(int level, int64_t tx, int64_t ty, int64_t tz) const;
    // The scatter tier for a tile centre. In nested mode the rings no longer
    // bound residency, so past the last ring this clamps to the coarsest tier
    // instead of returning "not desired".
    int nested_scatter_tier(float centre_dist) const;
    // Which level is desired at a world point right now, or -1. Exactly one
    // level covers any point (coverage rule), so this walks 0..max_level.
    int desired_level_at(float wx, float wy, float wz) const;
    // (desired_at() lived here. Its only caller was assign_nested_masks, which
    // the edge-mask retirement deleted; desired_level_at above is the survivor
    // and answers the geometric question the descent actually asks.)
    // The finest level found along one edge, by recursive halving. A neighbour
    // at or above a segment's own level covers that whole segment (grids nest),
    // so a well-formed ladder costs ONE probe per side; only where the far side
    // is genuinely finer does this subdivide, and it stops as soon as it finds
    // a level low enough to be a violation. Probing the edge at the finest tile
    // pitch instead -- the obvious implementation -- costs 2^level probes per
    // side and made update() the most expensive thing in the streamer.
    // THE COLUMN PATH'S adjacency probe: a LINE walked along one horizontal
    // axis at a fixed other. Correct while a tile is unbounded in y, because
    // then a lateral face is fully described by the line across it.
    int min_edge_level(bool vary_z, float fixed, float t0, float t1, float probe_y,
                       int seg_level, int stop_below) const;

    // THE OCTREE'S adjacency probe: a FACE, walked as a 2-D quadtree over the
    // square rather than a 1-D one over a line. A cube's lateral face is no
    // longer described by any single line across it -- a neighbour may be
    // over-fine in one corner and correct everywhere else -- so the line probe
    // that serves the column path would walk straight past exactly the
    // violation this pass exists to find. `axis` names the face NORMAL
    // (0 = x, 1 = y, 2 = z); (u, v) are the two in-plane world axes in the
    // order x < y < z with the normal removed.
    int min_face_level(int axis, float fixed, float u0, float u1,
                       float v0, float v1,
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
    void scan_footprint(int level, int64_t tx, int64_t ty, int64_t tz,
                        bool& any_desired, bool& all_resident) const;
    void scan_subtree(int level, int64_t tx, int64_t ty, int64_t tz,
                      bool& any_desired, bool& all_resident) const;

    void update_nested(float anchor_x, float anchor_y, float anchor_z);
    // Tree descent: mark (level, tx, ty, tz) desired or recurse into its
    // children -- FOUR under the quadtree, EIGHT under `volumetric_sectors`,
    // which is the only structural difference between the two modes here.
    // `finer_resident` holds the ancestors of every resident tile, so a tile
    // that is currently SPLIT can be told apart from one that is not -- which is
    // the difference between promotion (no hysteresis) and a merge (hysteresis),
    // exactly as the uniform path distinguishes them.
    //
    // With the flag off `ty` is pinned to kFlatTy on every path, so the octree
    // code is inert rather than bypassed: one descent, one set of hysteresis
    // rules, no second implementation to drift.
    void descend(int level, int64_t tx, int64_t ty, int64_t tz,
                 const std::unordered_map<uint64_t, char, KeyHash>& finer_resident);
    void mark_desired(int level, int64_t tx, int64_t ty, int64_t tz);
    // Cardinal-adjacent desired tiles must differ by at most one level. Splits
    // the coarser side to a fixpoint; a no-op for band tables whose every
    // annulus is wider than one tile of the coarser level, which the default
    // profile satisfies by construction.
    void restrict_levels();

    // STAGED REFINEMENT. The COARSEST level at which anything is RESIDENT over
    // this tile's footprint, searching the tile itself and then its ancestors,
    // or -1 when nothing at or above `level` is resident. Six hash lookups
    // worst case (the ancestor chain), which is what keeps the staging gate off
    // update()'s critical path — the naive alternative, probing residency per
    // finest-grid cell, is the mistake min_edge_level's comment records.
    // Coarsest rather than finest because the resident map transiently stacks
    // levels over one column (the transition-group hold); the .cpp records what
    // answering with the finest one broke.
    //
    // -1 deliberately means "no clamp", not "clamp to the coarsest". Two
    // distinct situations produce it and both want the desired level directly:
    // a COLD region (nothing resident anywhere near — a fresh world, or a
    // frontier tile entering reach), and a region whose only residency is
    // FINER than `level` (a merge in progress; waves are a coarse->fine idea
    // and merging coarser is a single bake with nothing to stage).
    int  resident_level_over(int level, int64_t tx, int64_t ty,
                             int64_t tz) const;
    bool staged_refinement_active() const;

    // LATERAL STAGING. "Is anything resident STRICTLY COARSER than `level`
    // touching one of this tile's four faces?" — the lateral half of the
    // clamp, and deliberately a predicate rather than a level.
    //
    // `max(desired, coarsest_face_neighbour_resident - 1)` blocks a descent
    // from `level` to `level - 1` exactly when some face neighbour is resident
    // at `level + 1` or coarser, so the value is never needed, only whether
    // one exists — which lets the search stop at the first hit instead of
    // ranking them. Existence is also the coarsest-wins answer R7 demands,
    // for free.
    //
    // COST. One tile at level L has exactly ONE face neighbour per side at
    // level L, and any resident tile coarser than L that touches a side must
    // cover that neighbour's column (coarser grids nest and are aligned), so
    // four ancestor chains answer the question exactly — 4 x (max_level -
    // level) hash lookups, no wider probe, and it is only reached on a node
    // the descent was otherwise going to split. `resident_at_level_` skips a
    // whole rank of four when no tile is resident at that level at all, which
    // is every rank on a cold world. This is the bound min_edge_level's
    // comment is about: probing the face at the finest tile pitch would be
    // 2^level lookups a side and would put update() back at the top of the
    // profile.
    bool coarser_resident_beside(int level, int64_t tx, int64_t ty,
                                 int64_t tz) const;
    bool lateral_staging_active() const;

    // Resident tile count per level, refreshed at the top of update_nested()
    // in the same sweep that builds `finer_resident`. Only a filter for the
    // probe above; nothing decides anything on it.
    int resident_at_level_[kMaxLevel + 1] = {0};
};

} // namespace matter_stream
