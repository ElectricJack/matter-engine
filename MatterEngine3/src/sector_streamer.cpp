#include "sector_streamer.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <limits>

namespace matter_stream {

// Diagnostic (issues/render-streaming-build-cpu follow-up): with
// MATTER_STREAM_NO_EVICT=1 a sector that becomes resident stays resident at
// its first rung forever — no out-of-ring evictions, no rung-change rebakes
// (which would double-publish a tile if only the eviction half were
// suppressed). Movement then only ADDS sectors, which isolates publish cost
// from eviction cost when chasing movement hitches. Residency grows without
// bound; test runs only.
static bool stream_no_evict() {
    static const bool value = [] {
        const char* env = std::getenv("MATTER_STREAM_NO_EVICT");
        const bool active = env != nullptr && env[0] == '1';
        // Self-report so a diagnostic run can prove the toggle reached this
        // exe (env prefixes and stale builds have burned that assumption
        // before — worktree-bootstrap gotcha #8).
        if (active)
            std::fprintf(stderr,
                         "[stream] MATTER_STREAM_NO_EVICT=1: evictions and "
                         "rung rebakes DISABLED (diagnostic)\n");
        return active;
    }();
    return value;
}

// A/B kill switch for staged refinement (Config::staged_refinement, §4.5 of
// docs/volumetric-sectors-design-2026-08-10.md). With
// MATTER_STREAM_NO_STAGING=1 the descent ignores resident coverage and jumps
// straight to the banded level, which is exactly the pre-M0-WP4 behaviour —
// the baseline every "did staging cost us bake work / did it fix the pop"
// measurement has to be taken against, and one we must be able to take without
// a rebuild because the interesting runs are captured flights in the editor.
static bool stream_no_staging() {
    static const bool value = [] {
        const char* env = std::getenv("MATTER_STREAM_NO_STAGING");
        const bool active = env != nullptr && env[0] == '1';
        if (active)
            std::fprintf(stderr,
                         "[stream] MATTER_STREAM_NO_STAGING=1: staged "
                         "refinement DISABLED (A/B baseline)\n");
        return active;
    }();
    return value;
}

// A/B kill switch for the LATERAL half of staged refinement alone
// (Config::lateral_staging). Separate from the switch above on purpose: the
// footprint clamp and the lateral clamp fix different halves of the same
// invariant and cost different amounts, and the interesting evidence for both
// is a captured editor flight (`drawn_level_violations` under MATTER_SEAM_TRACE
// against settled residency), where a rebuild between the two arms is not
// available. MATTER_STREAM_NO_STAGING=1 still turns off both.
static bool stream_no_lateral() {
    static const bool value = [] {
        const char* env = std::getenv("MATTER_STREAM_NO_LATERAL");
        const bool active = env != nullptr && env[0] == '1';
        if (active)
            std::fprintf(stderr,
                         "[stream] MATTER_STREAM_NO_LATERAL=1: the lateral "
                         "half of staged refinement is DISABLED (the footprint "
                         "clamp still runs)\n");
        return active;
    }();
    return value;
}

void resolve_terrain_defaults(Config& cfg) {
    if (!cfg.terrain_lod_enabled || !cfg.terrain_bands.empty()) return;
    // Radial profile in sector sizes: near disc native voxel (LOD 5),
    // then heightfield LODs down to a single quad. Wider near bands than
    // the design table's minimum (3S/5S/8S/14S/24S): editor cameras fly
    // hundreds of meters up, where 8-16 m cells at the design's 5-8S
    // radii read as visible facets. Every adjacent pair stays >= 2S
    // apart so the default map is 2:1-balanced by construction; the
    // explicit balance pass still guards custom profiles.
    const float S = cfg.sector_size;
    cfg.terrain_bands = {
        {5.0f * S, 5},  {8.0f * S, 4},  {12.0f * S, 3},
        {18.0f * S, 2}, {27.0f * S, 1}, {40.0f * S, 0},
    };
}

SectorStreamer::SectorStreamer(Config cfg)
    : cfg_(std::move(cfg)) {
    // Nesting IS the terrain ladder expressed as tile size, so it implies the
    // ladder rather than composing with it. Without this a world that set only
    // `nestedSectors` would resolve no bands and fall straight back to uniform.
    if (cfg_.nested_sectors) cfg_.terrain_lod_enabled = true;
    // The octree IS the nested ladder with a third axis -- same levels, same
    // reinterpreted band table, same 2:1 restriction. With nesting off there is
    // nothing to descend, so this cannot be half-honoured.
    if (!cfg_.nested_sectors) cfg_.volumetric_sectors = false;
    resolve_terrain_defaults(cfg_);
    if (!cfg_.nested_sectors) return;

    // Band LOD l is the annulus where level (5 - l) tiles live. Same authored
    // table, same tuning UI, one reinterpretation.
    std::vector<float> radius(kMaxLevel + 1, 0.0f);
    std::vector<char>  seen(kMaxLevel + 1, 0);
    for (const auto& band : cfg_.terrain_bands) {
        const int lvl = kMaxLevel - band.rung;
        if (lvl < 0 || lvl > kMaxLevel) continue;
        radius[lvl] = band.radius;
        seen[lvl] = 1;
    }
    // Only the contiguous run from level 0 is usable. A table missing a level
    // cannot describe a nested ladder -- the descent would have no size to
    // stop at -- and silently skipping the gap would leave a ring-shaped hole
    // in the world, so the ladder ends at the gap instead.
    for (int L = 0; L <= kMaxLevel && seen[L]; ++L)
        level_radius_.push_back(radius[L]);
    // Radii must increase with level. Clamping rather than rejecting keeps a
    // mis-authored table renderable (as fewer effective levels) instead of
    // turning one typo into an empty world.
    for (size_t i = 1; i < level_radius_.size(); ++i)
        level_radius_[i] = std::max(level_radius_[i], level_radius_[i - 1]);
    // A table that resolved no levels leaves nesting itself unusable, and the
    // octree with it -- otherwise a world would fall back to the uniform grid
    // while still believing it was volumetric.
    if (level_radius_.empty()) {
        cfg_.nested_sectors = false;
        cfg_.volumetric_sectors = false;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

float SectorStreamer::sector_dist(int64_t tx, int64_t tz) const {
    float cx = (float(tx) + 0.5f) * cfg_.sector_size;
    float cz = (float(tz) + 0.5f) * cfg_.sector_size;
    float dx = cx - last_anchor_x_, dz = cz - last_anchor_z_;
    return std::sqrt(dx * dx + dz * dz);
}

int SectorStreamer::desired_rung_for_dist(float d) const {
    for (const auto& ring : cfg_.rings)
        if (d <= ring.radius) return ring.rung;
    return -1; // beyond all rings
}

int SectorStreamer::desired_lod_for_dist(float d) const {
    for (const auto& band : cfg_.terrain_bands)
        if (d <= band.radius) return band.rung;
    // Beyond the last band (but still inside a scatter ring, or held resident
    // by hysteresis): coarsest configured level.
    return cfg_.terrain_bands.empty() ? 5 : cfg_.terrain_bands.back().rung;
}

// Terrain-LOD pass (terrain_lod_enabled): band lookup with demotion
// hysteresis, 2:1 cardinal balance, variant repack. Runs after the scatter
// scan/hysteresis has finalized desired_rung as a bare-or-packed value whose
// scatter bits are authoritative.
void SectorStreamer::assign_terrain_lods() {
    // Pass 1: per-sector band LOD with demotion hysteresis.
    for (auto& [k, st] : sectors_) {
        st.desired_lod = -1;
        if (st.desired_rung < 0) continue;
        if (stream_no_evict() && st.resident_rung >= 0) {
            // Diagnostic freeze: keep the resident variant verbatim (the
            // update() scan already pinned desired_rung to it).
            st.desired_lod = variant_terrain_lod(st.resident_rung);
            continue;
        }
        int lod = desired_lod_for_dist(st.dist);
        if (st.resident_rung >= 0) {
            const int res_lod = variant_terrain_lod(st.resident_rung);
            if (res_lod > lod) {
                // Demotion to a coarser level: only past the resident level's
                // band radius plus hysteresis (mirrors the scatter rule).
                float band_radius = 0.0f;
                for (const auto& band : cfg_.terrain_bands)
                    if (band.rung == res_lod) { band_radius = band.radius; break; }
                if (st.dist <= band_radius + cfg_.hysteresis) lod = res_lod;
            }
        }
        st.desired_lod = lod;
    }

    // Pass 2: 2:1 balance. Promote the coarser side of any cardinal pair
    // differing by more than one level. "lod = max(lod, neighbor - 1)" is
    // monotone, so the fixpoint is iteration-order independent; levels are
    // bounded by 5, so a handful of sweeps suffices.
    for (int sweep = 0; sweep < 8; ++sweep) {
        bool changed = false;
        for (auto& [k, st] : sectors_) {
            if (st.desired_lod < 0) continue;
            if (stream_no_evict() && st.resident_rung >= 0) continue;
            int64_t tx, tz;
            unkey(k, tx, tz);
            const int64_t ntx[4] = {tx + 1, tx - 1, tx, tx};
            const int64_t ntz[4] = {tz, tz, tz + 1, tz - 1};
            for (int n = 0; n < 4; ++n) {
                auto it = sectors_.find(key(ntx[n], ntz[n]));
                if (it == sectors_.end() || it->second.desired_lod < 0) continue;
                if (it->second.desired_lod - st.desired_lod > 1) {
                    st.desired_lod = it->second.desired_lod - 1;
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }

    // Pass 3: repack. This pass used to compute a four-bit coarser-neighbour
    // edge mask here as well, and packing was deferred to it precisely BECAUSE
    // the mask needed every neighbour's LOD to be final first. The mask is gone
    // (sector_streamer.h documents why: it was a promise about the desired map
    // that the drawn map broke, and it sat inside the bake identity, so every
    // neighbour level change forced a full rebake of this tile). What survives
    // is the packing itself — scatter tier plus this tile's own terrain LOD,
    // neither of which depends on a neighbour — so the pass could in principle
    // be folded into pass 1. It is kept separate because pass 2 rewrites
    // desired_lod, and a variant packed before the balance sweep would carry a
    // level the balance pass then contradicts.
    for (auto& [k, st] : sectors_) {
        if (st.desired_rung < 0 || st.desired_lod < 0) continue;
        if (stream_no_evict() && st.resident_rung >= 0) continue;
        st.desired_rung = pack_variant(variant_scatter(st.desired_rung),
                                       st.desired_lod);
    }
}

// ---------------------------------------------------------------------------
// Nested mode: geometry helpers
// ---------------------------------------------------------------------------

float SectorStreamer::tile_centre_dist(int level, int64_t tx, int64_t tz) const {
    const float S = level_size(level);
    const float cx = (float(tx) + 0.5f) * S;
    const float cz = (float(tz) + 0.5f) * S;
    const float dx = cx - last_anchor_x_, dz = cz - last_anchor_z_;
    return std::sqrt(dx * dx + dz * dz);
}

// Distance from the anchor to the CLOSEST point of the tile -- zero when the
// anchor is inside it. The split test uses this rather than the centre so that
// a tile splits when any part of it is close enough, which is what makes the
// desired set cover every world column exactly once.
//
// UNDER `volumetric_sectors` THIS IS THE ONLY PLACE THE BANDS BECOME SPHERES.
// A column tile is unbounded in y for selection purposes, so its distance is
// the XZ one and always has been; a cube tile is bounded on all six sides, so
// the same "closest point of the box" formula simply gains its third term. The
// band radii are unchanged and uninterpreted -- what changes is that they now
// measure through the air as well as across the ground, which is what makes a
// camera 900 m down refine the rock around it instead of the surface above it.
float SectorStreamer::tile_near_dist(int level, int64_t tx, int64_t ty,
                                     int64_t tz) const {
    const float S = level_size(level);
    const float x0 = float(tx) * S, x1 = x0 + S;
    const float z0 = float(tz) * S, z1 = z0 + S;
    const float dx = std::max(std::max(x0 - last_anchor_x_, 0.0f),
                              last_anchor_x_ - x1);
    const float dz = std::max(std::max(z0 - last_anchor_z_, 0.0f),
                              last_anchor_z_ - z1);
    if (!cfg_.volumetric_sectors) return std::sqrt(dx * dx + dz * dz);
    const float y0 = float(ty) * S, y1 = y0 + S;
    const float dy = std::max(std::max(y0 - last_anchor_y_, 0.0f),
                              last_anchor_y_ - y1);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

int SectorStreamer::nested_scatter_tier(float d) const {
    for (const auto& ring : cfg_.rings)
        if (d <= ring.radius) return ring.rung;
    // Past the last ring: the coarsest tier, NOT "undesired". In nested mode
    // the terrain bands bound residency and the rings only grade scatter.
    return cfg_.rings.empty() ? 0 : cfg_.rings.back().rung;
}

// `probe_y` is the world y the edge is walked at: the centre of the tile's own
// vertical span under the octree, and ignored with the flag off. The edge stays
// a LINE rather than becoming an area -- this pass still tests the four lateral
// faces, and it tests them within the probing tile's own horizontal slab, which
// is the honest generalisation of "the four cardinal neighbours". The two ±y
// faces are M3-WP3.
int SectorStreamer::min_edge_level(bool vary_z, float fixed, float t0, float t1,
                                   float probe_y,
                                   int seg_level, int stop_below) const {
    // Probe the segment's own CENTRE -- an interior point of whichever tile
    // covers it, never a tile boundary, so the answer is unambiguous.
    const float mid = 0.5f * (t0 + t1);
    const int m = vary_z ? desired_level_at(fixed, probe_y, mid)
                         : desired_level_at(mid, probe_y, fixed);
    // Nothing desired out there (past the reach): no adjacency constraint.
    if (m < 0) return kMaxLevel + 1;
    // A tile at or above this segment's level spans the whole segment, because
    // the grids nest. That is the early exit that makes this cheap.
    if (m >= seg_level || seg_level <= 0) return m;
    const int a = min_edge_level(vary_z, fixed, t0, mid, probe_y,
                                 seg_level - 1, stop_below);
    if (a < stop_below) return a;                 // already a violation
    const int b = min_edge_level(vary_z, fixed, mid, t1, probe_y,
                                 seg_level - 1, stop_below);
    return std::min(a, b);
}

// `wy` is floored per level exactly as wx/wz are, so a probe point resolves to
// whichever tile actually contains it at each level -- the same rule in three
// axes rather than two. With the octree off it collapses to kFlatTy and the
// argument is ignored, which is why callers may pass anything there.
// The octree's face probe. Same shape as min_edge_level one dimension up: probe
// the square's own centre, stop early when the answer already spans it, else
// recurse into four quadrants a level finer.
//
// WHY A FACE AND NOT A LINE. Under the column path a tile is unbounded in y, so
// a lateral face is fully characterised by one horizontal line across it and
// min_edge_level is exact. A cube's face is a square, and a neighbour can be
// over-fine in one corner of it while every line the old probe would have
// walked reports a legal level. Keeping the line probe would not have made this
// pass approximate -- it would have made it silently blind to the vertical half
// of the violations, which is the half the octree introduces.
int SectorStreamer::min_face_level(int axis, float fixed, float u0, float u1,
                                   float v0, float v1,
                                   int seg_level, int stop_below) const {
    const float um = 0.5f * (u0 + u1);
    const float vm = 0.5f * (v0 + v1);
    // Rebuild a world point from the face's normal axis and its two in-plane
    // coordinates. u < v in the x,y,z ordering with the normal dropped.
    float p[3];
    p[axis] = fixed;
    switch (axis) {
        case 0: p[1] = um; p[2] = vm; break;   // x-normal: (u,v) = (y,z)
        case 1: p[0] = um; p[2] = vm; break;   // y-normal: (u,v) = (x,z)
        default: p[0] = um; p[1] = vm; break;  // z-normal: (u,v) = (x,y)
    }
    const int m = desired_level_at(p[0], p[1], p[2]);
    if (m < 0) return kMaxLevel + 1;           // past the reach: no constraint
    if (m >= seg_level || seg_level <= 0) return m;
    int worst = kMaxLevel + 1;
    for (int q = 0; q < 4; ++q) {
        const float qu0 = (q & 1) ? um : u0, qu1 = (q & 1) ? u1 : um;
        const float qv0 = (q & 2) ? vm : v0, qv1 = (q & 2) ? v1 : vm;
        const int r = min_face_level(axis, fixed, qu0, qu1, qv0, qv1,
                                     seg_level - 1, stop_below);
        worst = std::min(worst, r);
        if (worst < stop_below) return worst;  // already a violation
    }
    return worst;
}

int SectorStreamer::desired_level_at(float wx, float wy, float wz) const {
    for (int L = 0; L <= max_level(); ++L) {
        const float S = level_size(L);
        const int64_t tx = int64_t(std::floor(wx / S));
        const int64_t tz = int64_t(std::floor(wz / S));
        const int64_t ty = cfg_.volumetric_sectors
                               ? int64_t(std::floor(wy / S))
                               : kFlatTy;
        auto it = sectors_.find(nested_key(L, tx, ty, tz));
        if (it != sectors_.end() && it->second.desired_level == L) return L;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Nested mode: the desired set
// ---------------------------------------------------------------------------

void SectorStreamer::mark_visible(int64_t tx, int64_t ty, int64_t tz,
                                  int rung) {
    if (cfg_.occlusion_grace_ticks <= 0) return;
    if (!cfg_.nested_sectors) {
        // Unnested worlds have no tree to propagate through and no cap to
        // apply (the cap refuses splits, and there are none). The ledger would
        // be written and never read.
        return;
    }
    // Stamp this tile and every ancestor of it. A drawn leaf proves its whole
    // chain of parents has visible content, which is precisely the question
    // `descend` asks before splitting one of them -- and the parent can never
    // answer it from its own draws, because a node being split is not resident.
    //
    // The walk is over LEVELS, not over a stored tree: an ancestor's tile index
    // is the child's shifted right once per level, so this is a handful of
    // shifts and hash inserts, bounded by max_level() (6 in the deepest world
    // here). Y participates only under the octree; a column world's ancestors
    // share its ty.
    int level = matter_stream::variant_level(rung);
    int64_t ax = tx;
    int64_t ay = cfg_.volumetric_sectors ? ty : kFlatTy;
    int64_t az = tz;
    for (; level <= max_level(); ++level) {
        VisState& v = vis_[nested_key(level, ax, ay, az)];
        v.last_visible = vis_frame_;
        // A tile the readback names before the descent has ever visited it
        // (possible: the readback is several frames stale) gets its eligibility
        // clock opened here rather than left at 0, so it is not instantly
        // eligible for the cap on the descent's next pass.
        if (v.first_visit == 0) v.first_visit = vis_frame_;
        ax >>= 1;
        if (cfg_.volumetric_sectors) ay >>= 1;
        az >>= 1;
    }
}

// Has nothing in this node's subtree been drawn for longer than the grace?
//
// TWO clocks, and the second one is what makes the answer decidable. A node
// that has never been drawn has `last_visible == 0`, and "never drawn" alone
// cannot be read as occluded -- during a cold fill nothing has been drawn yet,
// and demoting the whole world at the moment it is trying to become visible for
// the first time is the one failure this cap must not have. So the node also
// has to have been ELIGIBLE: `first_visit` is when the descent first reached
// it, and the grace is measured from whichever of the two is later.
//
// A node the descent has never reached has neither clock and is not occluded.
bool SectorStreamer::occluded_subtree(int level, int64_t tx, int64_t ty,
                                      int64_t tz) const {
    const auto it = vis_.find(nested_key(level, tx, ty, tz));
    if (it == vis_.end()) return false;
    const uint64_t eligible_since =
        std::max(it->second.last_visible, it->second.first_visit);
    if (eligible_since == 0) return false;
    return vis_frame_ > eligible_since + uint64_t(cfg_.occlusion_grace_ticks);
}

void SectorStreamer::mark_desired(int level, int64_t tx, int64_t ty,
                                  int64_t tz) {
    auto& st = sectors_[nested_key(level, tx, ty, tz)];
    // XZ on purpose even under the octree -- this drives the SCATTER TIER, and
    // scatter is a horizontal notion. See the note on the declaration.
    st.dist          = tile_centre_dist(level, tx, tz);
    st.desired_level = level;
    st.desired_lod   = kMaxLevel - level;

    int tier = nested_scatter_tier(st.dist);
    // Scatter-tier demotion gets the same hysteresis the uniform path gives it
    // (sector_streamer.cpp, update()'s ring comparison). Without it a camera
    // parked on a ring boundary rebakes the tiles under it forever: the tier is
    // part of the variant, so a tier flip is a full rebake and republish.
    if (st.resident_rung >= 0) {
        const int res_tier = variant_scatter(st.resident_rung);
        if (tier < res_tier) {
            float ring_radius = 0.0f;
            for (const auto& ring : cfg_.rings)
                if (ring.rung == res_tier) { ring_radius = ring.radius; break; }
            if (st.dist <= ring_radius + cfg_.hysteresis) tier = res_tier;
        }
    }
    // Pack the variant HERE, which is the new home for the nested path.
    //
    // It used to happen in a separate assign_nested_masks() sweep after the
    // whole descent and restrict_levels() had run, for one reason only: the
    // edge mask was a function of the NEIGHBOURS' final levels, so nothing
    // could be packed until every level in the map was settled. With the mask
    // retired (sector_streamer.h carries the rationale — a baked promise about
    // the desired map that the drawn map kept breaking, and one that put every
    // neighbour's level inside this tile's bake identity) the variant depends
    // on nothing but this tile: its own scatter tier and its own level. So it
    // belongs at the point the tile is decided, and the extra whole-map sweep
    // is gone with it.
    //
    // restrict_levels() calls mark_desired() again for tiles it splits; the
    // pack is a pure function of `tier` and `desired_lod`, so re-marking is
    // idempotent and needs no second pass to fix up.
    st.desired_rung = pack_variant(tier, st.desired_lod);
}

void SectorStreamer::descend(
    int level, int64_t tx, int64_t ty, int64_t tz,
    const std::unordered_map<uint64_t, char, KeyHash>& finer_resident) {
    // THE OCTREE EXTENT, tested per node rather than only at the root -- and
    // the root clamp is not enough, which is the whole reason this is here. A
    // top-level tile is `sector_size << kMaxLevel` tall (2048 m at a 64 m
    // world), so the row containing y_min straddles it by design; its lower
    // children then subdivide ground the world never claimed. Clamping only the
    // root bounds the tree by a tile of the COARSEST size, which is not a bound
    // at all at the levels that matter.
    if (cfg_.volumetric_sectors) {
        const float S  = level_size(level);
        const float y0 = float(ty) * S;
        if (y0 + S <= cfg_.y_min || y0 >= cfg_.y_max) return;
    }

    const float nd = tile_near_dist(level, tx, ty, tz);

    // A tile with anything of its own resident -- itself, or something finer
    // under it -- is OCCUPIED, and every boundary it can cross gets hysteresis
    // on the way out. Two boundaries matter and both churned without this: the
    // split radius (below) and the reach (here). At the reach, a bare `nd >
    // reach()` drops the outermost ring of tiles the instant the anchor drifts
    // a metre outward and re-requests them when it drifts back -- the uniform
    // path has always held that edge with `outer_r + hysteresis`.
    const bool is_split =
        finer_resident.find(nested_key(level, tx, ty, tz)) !=
        finer_resident.end();
    bool self_resident = false;
    {
        auto it = sectors_.find(nested_key(level, tx, ty, tz));
        self_resident = it != sectors_.end() &&
                        (it->second.resident_rung >= 0 ||
                         it->second.inflight_rung >= 0);
    }
    const bool occupied = is_split || self_resident;
    if (nd > reach() + (occupied ? cfg_.hysteresis : 0.0f)) return;
    if (level == 0) { mark_desired(0, tx, ty, tz); return; }

    // Level-(L-1) tiles live inside this radius.
    const float split_r = level_radius_[level - 1];
    // Splitting is promotion and gets no hysteresis; merging back is demotion
    // and does -- the same asymmetry the uniform path applies to rungs. A tile
    // with finer residency under it is currently SPLIT, so the choice facing us
    // is whether to merge; otherwise it is whether to split.
    const float thresh = is_split ? split_r + cfg_.hysteresis : split_r;

    bool split = nd <= thresh;

    // ---- STAGED REFINEMENT ------------------------------------------------
    // (docs/volumetric-sectors-design-2026-08-10.md §4.5, first half.)
    //
    // Everything above decides the DESIRED level. restrict_levels() then makes
    // the desired map 2:1-balanced, and that used to be the whole story. But
    // nothing above constrains the DRAWN map, and the drawn map is what a seam
    // exists between. Fly in fast and a region's desired level jumps 3 -> 0 in
    // a single update: sixty-four independent bakes are launched, they land in
    // whatever order the worker pool finishes them, and the first one to land
    // is drawn at level 0 with a neighbour still showing the level-3 tile. Two
    // separate costs, and neither is fixed by the seam welder:
    //
    //   * the welder implements exactly one level of difference across a face
    //     (§4.4), so a three-level face pair is outside its domain entirely —
    //     not a worse seam, no seam;
    //   * an 8x detail step arriving as a single pop after a long hold, which
    //     is what the ladder exists to avoid.
    //
    // So clamp the REQUEST, not just the desire: a tile is never requested more
    // than one level finer than the resident coverage over its own footprint.
    // effective_level = max(desired_level, resident_level_over(...) - 1),
    // expressed here as a stop condition on the descent because the descent is
    // where the level is chosen. A 3 -> 0 jump becomes monotone waves — 3, then
    // 2, then 1, then 0 — with each wave fully RESIDENT (not merely visible;
    // parking can hold a resident tile back for a while and waiting on
    // visibility would couple the streamer to the draw side) before the next is
    // requested. What the eye gets is progressively refining terrain instead of
    // a hold followed by an 8^n pop.
    //
    // The footprint is the right region to ask about for THIS clamp, and it is
    // not the whole story — see the lateral term below. What the footprint term
    // detects is specifically "this patch of world is currently being drawn
    // coarse", and the tile drawing it coarse is this tile's own ancestor.
    //
    // COLD START IS NOT CLAMPED, and that falls out rather than being a special
    // case: resident_level_over returns -1 when nothing at or above this level
    // is resident, and -1 means no clamp. A fresh world, a `clear()`, or a tile
    // entering reach at the frontier all have nothing resident over them, so
    // they descend straight to their banded level on the first update — the
    // wave rule must never turn "there is nothing here yet" into "so show the
    // coarsest thing first", which would make every world load in six visible
    // stages. Only a region that already HAS coarse residency is staged,
    // because only that region has something to be inconsistent with.
    //
    // NO DEADLOCK against the transition-group hold (see update_nested below).
    // The hold keeps a superseded tile resident until every desired tile over
    // its footprint is resident. Staging only ever makes the desired set
    // COARSER than the bands asked for; it never removes a tile from the
    // desired set, never marks one undesired without marking its replacement,
    // and never gates on anything but residency. So every wave's tiles are
    // desired, therefore requested, therefore eventually resident, therefore
    // the hold releases and the next wave is admitted. The two rules run in
    // opposite directions on the same clock and cannot wait on each other: the
    // hold waits for the CURRENT wave to become resident, and staging waits for
    // exactly the same event. (The one thing that could deadlock them is a wave
    // that can never complete — a permanently failing bake — and that is
    // already the fail-safe case both rules are designed around: the parent
    // stays held and drawn, which beats a hole.)
    //
    // INTERACTION WITH restrict_levels(). That pass runs after the descent and
    // splits tiles to enforce the 2:1 desired invariant, calling mark_desired()
    // directly rather than descend(), so it bypasses this clamp. That is
    // deliberate and the priority order is right: §4.4 makes the 2:1
    // restriction mandatory (it is the premise the welder's fan logic is built
    // on) while staging is a smoothing on top of it. In practice it costs a
    // one-tile-wide rim at the boundary between a staged region and an
    // unstaged one, which restrict_levels pulls finer than the wave would have.
    //
    // Splitting hysteresis is evaluated FIRST and independently: the clamp can
    // only ever suppress a split, never force one, so a clamped tile is exactly
    // as stable on the split radius as it was before.
    //
    // COST. The design bounds the intermediate waves at sum(8^-k) ~ +14% bake
    // work, which is the VOLUMETRIC number -- one level coarser is 8x the
    // volume per tile. This descent is still the 2D quadtree, so the same
    // argument gives sum(4^-k) = 1/3 and the honest M0 ceiling is +33%; the
    // +14% arrives with the third axis. Measured on a four-level teleport with
    // the test band table: +18.9% (sector_streamer_tests.cpp prints it).
    //
    // ONE CONSEQUENCE CALLERS SEE: a quiet update no longer means "settled".
    // A wave is admitted by the ABSENCE of coarser residency, and the coarse
    // tile is evicted at the end of the update in which its replacement quad
    // completes -- so there is exactly one update per wave in which the desired
    // map already equals the resident map and next_request() returns nothing.
    // The engine pumps every frame and never notices. Anything that loops
    // "update until no requests" stops mid-jump, which is a test-harness trap
    // rather than an engine one, and it is why the staged tests wait for
    // several consecutive silent updates.
    if (split && staged_refinement_active()) {
        const int resident_level = resident_level_over(level, tx, ty, tz);
        // Descending to level-1 asks for level-1; the clamp floor is
        // resident_level - 1, so the descent is admissible iff
        // level - 1 >= resident_level - 1.
        if (resident_level >= 0 && level < resident_level) split = false;

        // ---- LATERAL TERM ------------------------------------------------
        // (§4.5 again; the defect is M0 acceptance resolution R12.)
        //
        // Everything above synchronises a column with its OWN ancestors and
        // says nothing about the tiles beside it, and the M0 soak measured
        // exactly that hole: 28 `drawn_level_violations` on a settled
        // StreamCaverns, tiles drawn at level 2 against a DRAWN level-4 face
        // neighbour, which the welder correctly refuses to span (§4.4) so the
        // face gets no seam at all.
        //
        // How two levels open up with the footprint clamp already on. Adjacent
        // columns A and B sit under DIFFERENT coarse parents P1 and P2, both
        // resident at level 4. Within one parent the waves are already
        // lockstepped for free — the transition-group hold keeps P resident
        // until every tile replacing it is resident, and resident_level_over
        // answers with the COARSEST resident level, so no child of P can pass
        // level 3 while P is up. Across parents nothing couples them: P1's
        // group completes, P1 is evicted, A's footprint answer drops to 3 and
        // A refines to 2 — while P2 is still waiting on one slow bake and is
        // still drawn at 4, one face away.
        //
        //     effective_level = max(desired_level,
        //                           resident_level_over(footprint) - 1,
        //                           coarsest face-neighbour resident - 1)
        //
        // and, as above, that reads as a stop condition on the descent: a
        // split from `level` to `level - 1` is inadmissible when some face
        // neighbour is resident at `level + 1` or coarser. Hence a predicate,
        // not a level (coarser_resident_beside).
        //
        // THIS DOES NOT FIGHT THE BAND GRADIENT, which is the objection the
        // footprint-only comment above records and the reason a "clamp to the
        // coarsest neighbour" was rejected. The term is `neighbour - 1`, not
        // `neighbour`: a level-0 tile beside level-1 tiles yields max(0, 0) =
        // 0 and nothing happens. Sharper, it is a NO-OP on any ±1-balanced
        // residency, and every settled state is ±1-balanced because settled
        // residency equals the desired map and restrict_levels() makes that
        // map ±1. Proof: for the term to fire, this node must split (putting a
        // desired tile at level-1 against that face) while a neighbour is
        // resident at level+1 or coarser — a gap of two in a map that has
        // none. So it cannot pin the disc at the outer band, and it cannot
        // lower settled residency: it can only ever fire on a transient.
        //
        // NO DEADLOCK, and the worry is real — the footprint clamp waits on a
        // strict ancestor, which cannot wait back, whereas two neighbours can
        // in principle each wait on the other. They do not, because the wait
        // is not symmetric. Take the coarsest resident level M anywhere in the
        // reach. A node at level M is blocked laterally only by a neighbour
        // resident at M+1 or coarser, and by construction there is none; its
        // footprint answer is M, and `level < resident_level` is `M < M`,
        // false. So EVERY node at the coarsest resident level is free to
        // split, in every state. Its children are then requested, become
        // resident, and the level-M tile is evicted by the transition hold, so
        // M falls. Levels are bounded below by the banded desired level and
        // the clamp only ever moves a tile coarser, never finer, so the
        // descent is monotone and terminates. Informally: refinement always
        // proceeds from the coarsest level downwards, and a tile only ever
        // waits on something strictly coarser than itself, which is a strict
        // order — there is no cycle to close. (The unit suite asserts the
        // consequence rather than the argument: the staged settle terminates
        // and lands on exactly the resident set the unstaged one does.)
        //
        // ONE-SIDED ON PURPOSE. A drawn pair needs both sides, so refusing to
        // request the fine side is enough to prevent it; nothing here holds a
        // COARSE tile back. The case that leaves open is the mirror image — a
        // multi-level MERGE, where the coarse replacement lands while a
        // neighbour is still drawn fine. Merging is one bake with nothing to
        // stage (see resident_level_over's -1 contract), so closing that would
        // mean staging coarsening too, which costs bakes on every fly-out and
        // can conflict with this term over the same tile. Not done; measured
        // instead (sector_streamer_tests.cpp reports the merge direction
        // separately).
        if (split && lateral_staging_active() &&
            coarser_resident_beside(level, tx, ty, tz))
            split = false;
    }

    // ---- OCCLUSION DETAIL CAP (M4 Phase A, design §5.2) --------------------
    //
    // A tile nobody has drawn for `occlusion_grace_ticks` is desired
    // `occlusion_cap_levels` COARSER than its band asks, by refusing the last
    // splits rather than by rewriting a level afterwards. Expressing it as a
    // refusal is what keeps it safe: the desired set still COVERS this
    // footprint at every moment, so a wrong or stale visibility bit costs
    // detail and can never open a hole. Nothing here can make a region
    // undesired.
    //
    // A tile is judged by its SUBTREE, via the visibility ledger -- see
    // VisState and occluded_subtree(). Judging it by its own draws was the
    // first implementation and could not work: the tile under this question is
    // the one about to be split, so it is not resident and is never drawn.
    //
    // The eligibility clock is opened HERE, on the descent's first visit, which
    // is what makes "this has never been drawn" a usable signal instead of an
    // ambiguous one. It is the descent that decides what could be drawn, so the
    // descent is where the stopwatch starts.
    //
    // NOT gated on `level > 0` any more than the ledger is, but the split
    // decision itself already is: `descend` only reaches here with a splittable
    // level.
    if (cfg_.occlusion_grace_ticks > 0) {
        VisState& v = vis_[nested_key(level, tx, ty, tz)];
        if (v.first_visit == 0) v.first_visit = vis_frame_;
        v.last_visit = vis_frame_;
    }
    if (split && cfg_.occlusion_grace_ticks > 0 && level > 0 &&
        occluded_subtree(level, tx, ty, tz)) {
        // The level the bands alone would have put here. The cap is
        // relative to that rather than absolute, so it demotes by a fixed
        // amount of detail wherever the camera is, instead of pinning
        // distant tiles that were already coarse.
        int banded = 0;
        for (int L = 0; L <= max_level(); ++L) {
            banded = L;
            if (nd <= level_radius_[L]) break;
        }
        const int floor_level = banded + cfg_.occlusion_cap_levels;
        if (level <= floor_level) split = false;
    }

    if (split) {
        // Hysteresis acts on the whole SIBLING GROUP, never on one child: a
        // tile cannot be half-merged, so the children are decided together.
        // Four of them under the quadtree, eight under the octree -- the child
        // count is the entire structural difference between the two modes.
        //
        // Bit assignment is x, then z, then y, so the low two bits reproduce
        // the quadtree's exact child order and a flag-off run walks the
        // children in the order it always did.
        const int kids = cfg_.volumetric_sectors ? 8 : 4;
        for (int c = 0; c < kids; ++c)
            descend(level - 1,
                    2 * tx + (c & 1),
                    cfg_.volumetric_sectors ? 2 * ty + ((c >> 2) & 1) : kFlatTy,
                    2 * tz + ((c >> 1) & 1),
                    finer_resident);
    } else {
        mark_desired(level, tx, ty, tz);
    }
}

bool SectorStreamer::staged_refinement_active() const {
    return cfg_.staged_refinement && !stream_no_staging();
}

bool SectorStreamer::lateral_staging_active() const {
    // Subordinate to the master switch: the lateral term is the same rule
    // widened, and an A/B of "staging off" must mean all of it off.
    return staged_refinement_active() && cfg_.lateral_staging &&
           !stream_no_lateral();
}

bool SectorStreamer::coarser_resident_beside(int level, int64_t tx, int64_t ty,
                                             int64_t tz) const {
    // The face neighbours AT THIS LEVEL -- four under the quadtree, SIX under
    // the octree. Only one tile per side exists at this level, and every
    // resident tile coarser than this one that touches a side must cover that
    // side's whole face: the grids nest and are aligned, so a bigger tile
    // adjacent to any part of the face spans all of it and therefore contains
    // the neighbour probed here. These ancestor chains are the exact answer,
    // not a sample of it -- which is why the count has to grow with the tree
    // rather than stay at four and be "close enough".
    const int n_faces = cfg_.volumetric_sectors ? 6 : 4;
    const int64_t ntx[6] = {tx + 1, tx - 1, tx, tx, tx, tx};
    const int64_t ntz[6] = {tz, tz, tz + 1, tz - 1, tz, tz};
    const int64_t nty[6] = {ty, ty, ty, ty, ty + 1, ty - 1};
    // Rank-major so the census filter skips a whole face set at a time.
    // Arithmetic shift is floor division, which is what nesting means for
    // negative tile indices (same convention as resident_level_over and
    // scan_footprint).
    for (int up = level + 1; up <= max_level(); ++up) {
        if (resident_at_level_[up] == 0) continue;
        const int sh = up - level;
        for (int n = 0; n < n_faces; ++n) {
            const int64_t ky = cfg_.volumetric_sectors ? (nty[n] >> sh) : kFlatTy;
            auto it = sectors_.find(
                nested_key(up, ntx[n] >> sh, ky, ntz[n] >> sh));
            if (it != sectors_.end() && it->second.resident_rung >= 0)
                return true;
        }
    }
    return false;
}

int SectorStreamer::resident_level_over(int level, int64_t tx, int64_t ty,
                                        int64_t tz) const {
    // Walk this tile and then its ancestors -- the same ancestor chain
    // scan_footprint walks for the merge case. Arithmetic shift is floor
    // division, which is what nesting means for negative tile indices.
    //
    // Only at-or-above is searched. Residency strictly FINER than `level` means
    // the region is already at least as detailed as the request, so there is
    // nothing to stage; returning -1 there is not an approximation, it is the
    // answer.
    //
    // THE COARSEST match wins, not the first (finest) one, and the difference
    // is not cosmetic -- it was a bug, caught by the superseded-tile dwell
    // gate in sector_streamer_tests.cpp. Unlike the DESIRED map, where exactly
    // one level covers any column, the RESIDENT map transiently holds several
    // levels over one column: the transition-group rule keeps a superseded
    // tile resident until every tile replacing it is resident, so for a window
    // both the coarse parent and its complete replacement quad are up. Return
    // the finest of those and the descent reads "this region is already at
    // level 2" while a level-3 tile is still drawn over it, so wave 3 gets
    // requested before wave 2 has retired anything -- the coarse tile is then
    // held through every remaining wave (measured: 68 ticks of dwell against
    // the 1 tick that is inherent), and the drawn set ends up holding exactly
    // the multi-level spread staged refinement exists to prevent. Answering
    // with the coarsest resident level instead means a wave is admitted only
    // once the previous wave is resident AND the layer above it has been torn
    // down, which is what "each wave fully resident before the next is
    // requested" has to mean to be worth anything.
    int coarsest = -1;
    for (int up = level; up <= max_level(); ++up) {
        const int sh = up - level;
        const int64_t ky = cfg_.volumetric_sectors ? (ty >> sh) : kFlatTy;
        auto it = sectors_.find(nested_key(up, tx >> sh, ky, tz >> sh));
        if (it != sectors_.end() && it->second.resident_rung >= 0) coarsest = up;
    }
    return coarsest;
}

void SectorStreamer::restrict_levels() {
    // Cardinal-adjacent desired tiles must differ by at most one level: that is
    // the premise the edge-mask snap relies on, and the mesher has no wider
    // stitch. Monotone (only splits, levels bounded), so the fixpoint is
    // iteration-order independent, exactly like the uniform 2:1 balance pass.
    //
    // With the default band table this is a no-op by construction -- every
    // annulus is wider than one tile of the coarser level -- but bands are
    // authorable and hysteresis can locally hold a stale level, which is the
    // same reason the uniform path keeps its balance pass.
    struct Split { int lvl; int64_t tx, ty, tz; };
    for (int sweep = 0; sweep < 8; ++sweep) {
        std::vector<Split> to_split;
        for (const auto& [k, st] : sectors_) {
            if (st.desired_level < 1) continue;
            int lvl; int64_t tx, ty, tz;
            nested_unkey(k, lvl, tx, ty, tz);
            const float S = level_size(lvl);
            const float ox = float(tx) * S, oz = float(tz) * S;
            const float oy = float(ty) * S;
            const float out = 0.5f * cfg_.sector_size;   // just outside a face
            // The whole face, not a midpoint sample: a lone over-fine
            // neighbour against a long coarse face is exactly the case that
            // matters, and a midpoint probe would walk past it.
            int worst;
            if (cfg_.volumetric_sectors) {
                // SIX faces of the cube. The two y-normal ones are what the
                // octree adds, and they are not optional decoration: a vertical
                // 2:1 violation puts a face pair outside the weld fan's domain
                // entirely -- seam_weld rejects a rung gap of 2 outright -- so
                // without them the octree produces seams the welder refuses to
                // close, which is a hole rather than a blemish.
                worst = std::min(std::min(
                    std::min(min_face_level(0, ox + S + out, oy, oy + S, oz, oz + S, lvl, lvl - 1),
                             min_face_level(0, ox - out,     oy, oy + S, oz, oz + S, lvl, lvl - 1)),
                    std::min(min_face_level(2, oz + S + out, ox, ox + S, oy, oy + S, lvl, lvl - 1),
                             min_face_level(2, oz - out,     ox, ox + S, oy, oy + S, lvl, lvl - 1))),
                    std::min(min_face_level(1, oy + S + out, ox, ox + S, oz, oz + S, lvl, lvl - 1),
                             min_face_level(1, oy - out,     ox, ox + S, oz, oz + S, lvl, lvl - 1)));
            } else {
                // The column path, byte-for-byte: four lateral EDGES, because a
                // tile unbounded in y is fully described by a line across each.
                const float py = (float(ty) + 0.5f) * S;   // kFlatTy: unused
                worst = std::min(
                    std::min(min_edge_level(true,  ox + S + out, oz, oz + S, py, lvl, lvl - 1),
                             min_edge_level(true,  ox - out,     oz, oz + S, py, lvl, lvl - 1)),
                    std::min(min_edge_level(false, oz + S + out, ox, ox + S, py, lvl, lvl - 1),
                             min_edge_level(false, oz - out,     ox, ox + S, py, lvl, lvl - 1)));
            }
            if (worst < lvl - 1) to_split.push_back({lvl, tx, ty, tz});
        }
        if (to_split.empty()) return;
        for (const Split& e : to_split) {
            auto it = sectors_.find(nested_key(e.lvl, e.tx, e.ty, e.tz));
            if (it != sectors_.end()) {
                it->second.desired_level = -1;
                it->second.desired_lod   = -1;
                it->second.desired_rung  = -1;
            }
            // EIGHT children under the octree, and this is not cosmetic: this
            // pass clears the parent's desired flag before marking the
            // children, so replacing a cube with four of its eight octants
            // would leave the other four covered by nothing at all -- a hole in
            // the desired map rather than a level violation. Same child bit
            // assignment as descend().
            const int kids = cfg_.volumetric_sectors ? 8 : 4;
            for (int c = 0; c < kids; ++c)
                mark_desired(e.lvl - 1,
                             2 * e.tx + (c & 1),
                             cfg_.volumetric_sectors ? 2 * e.ty + ((c >> 2) & 1)
                                                     : kFlatTy,
                             2 * e.tz + ((c >> 1) & 1));
        }
    }
}

// assign_nested_masks() used to live here. It walked every desired tile,
// looked up the level-(L+1) tile on each of the four cardinal sides, and
// packed a "this side is one level coarser" bit into the variant for the
// mesher to gate its cross-level ownership reach on.
//
// It is deleted, not disabled (design §4.1, "Consequences" 2). The premise it
// rested on -- that a tile can be baked with a correct assumption about which
// level its neighbour will be drawn at -- was retracted: the mask was computed
// from the DESIRED map, while the neighbour actually on screen is a different
// tile whenever it is mid-split, mid-merge, held by the transition rule below,
// parked, or still baking, and each of those disagreements printed as a
// one-voxel strip of triangles emitted by neither side. Worse, the mask was
// part of the BAKE IDENTITY, so a neighbour changing level forced a full
// rebake of this tile too -- and the pair's rebakes finished seconds apart,
// which is precisely the window in which the two disagree. The mechanism meant
// to close the seam was what held it open.
//
// Cross-level seam geometry is now generated engine-side at runtime from the
// two tiles that are actually DRAWN, so it cannot disagree with them by
// construction. The streamer no longer knows seams exist, and -- the practical
// win for this file -- a neighbour's level change no longer invalidates this
// tile's bake at all.

void SectorStreamer::scan_subtree(int level, int64_t tx, int64_t ty, int64_t tz,
                                  bool& any_desired, bool& all_resident) const {
    auto it = sectors_.find(nested_key(level, tx, ty, tz));
    if (it != sectors_.end() && it->second.desired_level == level) {
        any_desired = true;
        if (it->second.resident_rung < 0) all_resident = false;
        return;                       // this tile covers its own footprint
    }
    if (level == 0) return;           // nothing desired covers this cell
    // Eight children under the octree. This is the TRANSITION-GROUP test -- it
    // decides whether a superseded tile may be torn down -- so scanning four of
    // eight octants would report "the replacement is complete" while half the
    // volume it covered has nothing resident, and evict the coarse tile into a
    // hole. Same child bit assignment as descend().
    const int kids = cfg_.volumetric_sectors ? 8 : 4;
    for (int c = 0; c < kids; ++c)
        scan_subtree(level - 1,
                     2 * tx + (c & 1),
                     cfg_.volumetric_sectors ? 2 * ty + ((c >> 2) & 1) : kFlatTy,
                     2 * tz + ((c >> 1) & 1),
                     any_desired, all_resident);
}

void SectorStreamer::scan_footprint(int level, int64_t tx, int64_t ty,
                                    int64_t tz, bool& any_desired,
                                    bool& all_resident) const {
    // A merge: one coarser tile has taken over this footprint. At most one
    // ancestor can be desired, because exactly one level covers any cell.
    for (int up = level + 1; up <= max_level(); ++up) {
        const int sh = up - level;
        const int64_t ky = cfg_.volumetric_sectors ? (ty >> sh) : kFlatTy;
        auto it = sectors_.find(nested_key(up, tx >> sh, ky, tz >> sh));
        if (it != sectors_.end() && it->second.desired_level == up) {
            any_desired = true;
            if (it->second.resident_rung < 0) all_resident = false;
            return;
        }
    }
    // Otherwise a split (or nothing at all).
    scan_subtree(level, tx, ty, tz, any_desired, all_resident);
}

void SectorStreamer::update_nested(float anchor_x, float anchor_y,
                                   float anchor_z) {
    last_anchor_x_ = anchor_x;
    last_anchor_y_ = anchor_y;   // stored, never read in M1 (see the header)
    last_anchor_z_ = anchor_z;

    // Prune the occlusion ledger (M4 Phase A). The descent visits a bounded
    // node set per tick, but flying across a world walks that set over new
    // ground, so entries it has stopped visiting accumulate. Swept rarely and
    // with a horizon several graces deep, because the cost of keeping a stale
    // entry one sweep too long is nothing and the cost of dropping a live one
    // is a tile's eligibility clock restarting from zero.
    if (cfg_.occlusion_grace_ticks > 0) {
        const uint64_t horizon =
            std::max<uint64_t>(600, uint64_t(cfg_.occlusion_grace_ticks) * 8);
        if (vis_frame_ > vis_pruned_at_ + horizon) {
            vis_pruned_at_ = vis_frame_;
            for (auto it = vis_.begin(); it != vis_.end();) {
                const uint64_t touched =
                    std::max(it->second.last_visit, it->second.last_visible);
                if (touched + horizon < vis_frame_) it = vis_.erase(it);
                else ++it;
            }
        }
    }

    // "Is anything finer than level L resident under this tile?" -- answered by
    // walking each resident tile's ancestors once (O(resident x levels)) rather
    // than by a range query per descent node. Inflight counts as residency in
    // the making: a tile whose children are being baked is already split.
    //
    // The same sweep counts residency per level for coarser_resident_beside's
    // filter. Residency (not inflight) is what that predicate is about: a
    // neighbour still baking is not DRAWN, so it is not half of a drawn pair,
    // and the transition hold guarantees something coarser is drawn there in
    // the meantime -- which this census does see.
    std::unordered_map<uint64_t, char, KeyHash> finer_resident;
    for (int L = 0; L <= kMaxLevel; ++L) resident_at_level_[L] = 0;
    for (const auto& [k, st] : sectors_) {
        if (st.resident_rung < 0 && st.inflight_rung < 0) continue;
        int lvl; int64_t tx, ty, tz;
        nested_unkey(k, lvl, tx, ty, tz);
        if (st.resident_rung >= 0 && lvl >= 0 && lvl <= kMaxLevel)
            ++resident_at_level_[lvl];
        for (int up = lvl + 1; up <= max_level(); ++up) {
            const int sh = up - lvl;
            // The ancestor's index on every axis is this tile's shifted down.
            // ty is 0 in M1, so `ty >> sh` is 0 too -- written as the general
            // form because it is the general form, not because it does work.
            finer_resident[nested_key(up, tx >> sh, ty >> sh, tz >> sh)] = 1;
        }
    }

    for (auto& [k, st] : sectors_) {
        st.desired_rung  = -1;
        st.desired_lod   = -1;
        st.desired_level = -1;
        if (st.cooldown > 0) --st.cooldown;
        int lvl; int64_t tx, ty, tz;
        nested_unkey(k, lvl, tx, ty, tz);
        (void)ty;   // M1: tile_centre_dist is an XZ distance
        st.dist = tile_centre_dist(lvl, tx, tz);
    }

    // Descend the coarsest grid over the reach. This visits O(resident) nodes
    // -- a few thousand -- where the uniform scan evaluates one distance per
    // finest-grid cell over the whole disc (~100k at StreamMountain's reach).
    const int   top    = max_level();
    const float S      = level_size(top);
    const float margin = reach() + cfg_.hysteresis;
    const int64_t tx_min = int64_t(std::floor((anchor_x - margin) / S));
    const int64_t tx_max = int64_t(std::floor((anchor_x + margin) / S));
    const int64_t tz_min = int64_t(std::floor((anchor_z - margin) / S));
    const int64_t tz_max = int64_t(std::floor((anchor_z + margin) / S));

    // The vertical span of the scan. One row at ty = 0 unless the octree is on,
    // which is what keeps the column path's request stream bit-for-bit.
    //
    // Two independent bounds, INTERSECTED, and both are needed:
    //
    //   * the REACH, mirroring x and z -- without it a world whose authored
    //     extent is 1700 m tall (StreamCaverns: -1024..704) scans every
    //     top-level row on every tick regardless of where the camera is;
    //   * the OCTREE EXTENT, which x and z have no equivalent of -- they are
    //     unbounded and the reach is the only thing that stops them. y is
    //     authored, and descending outside it would request tiles over a
    //     region the world does not claim to define.
    //
    // The extent's upper edge is EXCLUSIVE, so a y_max landing exactly on a
    // tile boundary does not pull in a row of tiles that begins where the world
    // ends. y_min is inclusive for the mirror-image reason: a tile starting
    // exactly at y_min is inside.
    int64_t ty_min = kFlatTy, ty_max = kFlatTy;
    if (cfg_.volumetric_sectors) {
        const int64_t reach_lo = int64_t(std::floor((anchor_y - margin) / S));
        const int64_t reach_hi = int64_t(std::floor((anchor_y + margin) / S));
        const int64_t ext_lo   = int64_t(std::floor(cfg_.y_min / S));
        const int64_t ext_hi   = int64_t(std::ceil(cfg_.y_max / S)) - 1;
        ty_min = std::max(reach_lo, ext_lo);
        ty_max = std::min(reach_hi, ext_hi);
        // An empty intersection is a legitimate outcome, not an error: the
        // camera is outside the world's authored vertical extent by more than
        // the whole reach, and nothing should be resident. The loop below
        // simply does not run, every resident tile stops being desired, and
        // the eviction pass tears the world down -- which is what flying out
        // of a world's bounds should do.
    }

    for (int64_t ty = ty_min; ty <= ty_max; ++ty)
        for (int64_t tz = tz_min; tz <= tz_max; ++tz)
            for (int64_t tx = tx_min; tx <= tx_max; ++tx)
                descend(top, tx, ty, tz, finer_resident);

    // The variant is packed inside mark_desired() now; there is no third sweep
    // here any more, because nothing about a tile's request depends on its
    // neighbours once the edge mask is gone.
    restrict_levels();

    // Evict and prune -- with the transition-group rule.
    //
    // A resident tile that stopped being desired has usually been SUPERSEDED:
    // split into four children, or merged into a parent. Evicting it now would
    // open a hole over its whole footprint until the replacements finish
    // baking, and four independent bakes finish seconds apart. So a superseded
    // tile is HELD, drawn, until every desired tile covering its footprint is
    // resident -- the group's invariant is that the old residency is torn down
    // only once the complete new residency exists.
    //
    // Everything else falls out of the same test. A tile that is genuinely out
    // of range has nothing desired over its footprint and goes immediately. A
    // partially failed split holds the parent, which is exactly the desired
    // fail-safe: a stale coarse tile beats a hole. An abandoned transition (the
    // anchor moved on) stops having desired tiles over the footprint and
    // releases on its own, with no group bookkeeping to unwind.
    std::vector<uint64_t> to_erase;
    for (auto& [k, st] : sectors_) {
        if (st.desired_rung >= 0) continue;
        if (st.resident_rung >= 0) {
            if (stream_no_evict()) continue;
            int lvl; int64_t tx, ty, tz;
            nested_unkey(k, lvl, tx, ty, tz);
            bool any_desired = false, all_resident = true;
            scan_footprint(lvl, tx, ty, tz, any_desired, all_resident);
            if (any_desired && !all_resident) continue;   // hold: still baking
            evictions_.push_back({tx, ty, tz, st.resident_rung});
            if (st.inflight_rung >= 0) --inflight_;
            to_erase.push_back(k);
        } else if (st.inflight_rung < 0 && st.cooldown == 0) {
            to_erase.push_back(k);
        }
    }
    for (uint64_t k : to_erase) sectors_.erase(k);
}

// ---------------------------------------------------------------------------
// update()
// ---------------------------------------------------------------------------

void SectorStreamer::update(float anchor_x, float anchor_y, float anchor_z) {
    if (cfg_.nested_sectors) {
        update_nested(anchor_x, anchor_y, anchor_z);
        return;
    }

    last_anchor_x_ = anchor_x;
    last_anchor_y_ = anchor_y;   // stored, never read in M1 (see the header)
    last_anchor_z_ = anchor_z;

    // The outer ring radius is the last entry (rings are innermost-first).
    float outer = cfg_.rings.empty() ? 0.0f : cfg_.rings.back().radius;
    float S = cfg_.sector_size;

    // Compute sector range covering the outer ring + hysteresis around anchor.
    float margin = outer + cfg_.hysteresis;
    int64_t tx_min = int64_t(std::floor((anchor_x - margin) / S));
    int64_t tx_max = int64_t(std::floor((anchor_x + margin) / S));
    int64_t tz_min = int64_t(std::floor((anchor_z - margin) / S));
    int64_t tz_max = int64_t(std::floor((anchor_z + margin) / S));

    // --- Mark all existing tracked sectors as not-desired initially.
    //     We'll recompute desired_rung below.
    for (auto& [k, st] : sectors_) {
        st.desired_rung = -1;
        // Decrement cooldown.
        if (st.cooldown > 0) --st.cooldown;
        // Recompute dist.
        int64_t stx, stz;
        unkey(k, stx, stz);
        st.dist = sector_dist(stx, stz);
    }

    // --- Scan the anchor square and set desired_rung.
    for (int64_t tz = tz_min; tz <= tz_max; ++tz) {
        for (int64_t tx = tx_min; tx <= tx_max; ++tx) {
            float d = sector_dist(tx, tz);
            int dr = desired_rung_for_dist(d);
            if (dr < 0) continue; // not desired

            uint64_t k = key(tx, tz);
            auto& st = sectors_[k]; // insert if absent
            st.desired_rung = dr;
            st.dist = d;
        }
    }

    // --- Apply evictions and pruning.
    //     Iterate in a copy of keys to avoid invalidation.
    std::vector<uint64_t> to_erase;
    for (auto& [k, st] : sectors_) {
        if (st.desired_rung >= 0) {
            // Still desired. Check if we need to evict for a rung change.
            // Hysteresis: only demote/evict a resident if its distance exceeds
            // (ring_radius_for_current_rung + hysteresis).
            if (st.resident_rung >= 0) {
                if (stream_no_evict()) {
                    // Freeze at the resident rung: no upgrade/demote requests,
                    // so on_published() can never queue a replacement eviction.
                    st.desired_rung = st.resident_rung;
                    continue;
                }
                // Scatter comparisons use the variant's scatter bits: with
                // terrain_lod_enabled the resident value is a packed variant,
                // and the freshly scanned desired value is still bare scatter
                // at this stage (assign_terrain_lods repacks afterwards).
                const int res_scatter = variant_scatter(st.resident_rung);
                // Find the ring radius that produced the resident scatter.
                float ring_radius = 0.0f;
                for (const auto& ring : cfg_.rings) {
                    if (ring.rung == res_scatter) { ring_radius = ring.radius; break; }
                }
                // If desired > resident (promotion): no hysteresis, proceed.
                // If desired < resident (demotion): hysteresis applies.
                if (st.desired_rung < res_scatter) {
                    // Demotion: only proceed if dist > ring_radius + hysteresis.
                    if (st.dist <= ring_radius + cfg_.hysteresis) {
                        // Still within hysteresis — freeze desired scatter at
                        // the resident tier (the terrain pass below reads only
                        // the scatter bits from this value).
                        st.desired_rung = res_scatter;
                    }
                }
            }
        } else {
            // Not desired. Evict if resident.
            if (st.resident_rung >= 0) {
                // Hysteresis: only evict if dist > outer_ring + hysteresis.
                // The outer ring is the last ring's radius.
                float outer_r = cfg_.rings.empty() ? 0.0f : cfg_.rings.back().radius;
                if (!stream_no_evict() && st.dist > outer_r + cfg_.hysteresis) {
                    evictions_.push_back({0, 0, 0, st.resident_rung});
                    int64_t etx, etz;
                    unkey(k, etx, etz);
                    evictions_.back().tx = etx;
                    evictions_.back().tz = etz;
                    if (st.inflight_rung >= 0) --inflight_; // was in flight
                    to_erase.push_back(k);
                }
                // else: still within hysteresis, leave resident.
            } else if (st.inflight_rung < 0 && st.cooldown == 0) {
                // Neither desired, nor resident, nor in-flight, not cooling down.
                // Safe to prune to prevent map growth.
                to_erase.push_back(k);
            }
        }
    }
    for (uint64_t k : to_erase) sectors_.erase(k);

    // Terrain LOD ladder: assign per-sector heightfield LODs, balance to 2:1
    // cardinal adjacency, compute edge masks, and repack desired_rung as a
    // variant. Must run after the scatter hysteresis/eviction pass so it
    // sees the final desired scatter tiers and the surviving entries.
    if (cfg_.terrain_lod_enabled) assign_terrain_lods();
}

// ---------------------------------------------------------------------------
// next_request()
// ---------------------------------------------------------------------------

bool SectorStreamer::next_request(SectorRequest& out) {
    if (inflight_ >= cfg_.max_inflight) return false;

    // Nearest first across BOTH holes and upgrades, holes winning ties
    // within one sector width. The old strict holes-first policy served
    // brand-new frontier sectors kilometers away (fog-hidden, coarsest LOD)
    // before promoting the coarse tiles directly under a moving camera —
    // flying forward left "massive flat triangles" close by while the
    // invisible frontier baked. Distance is what the eye ranks by; a hole
    // only outranks an upgrade when they are at comparable range.
    uint64_t best_k = 0;
    float best_score = std::numeric_limits<float>::max();
    bool found = false;

    for (auto& [k, st] : sectors_) {
        if (st.inflight_rung >= 0) continue;      // already in flight
        if (st.cooldown > 0) continue;             // cooling down
        if (st.desired_rung < 0) continue;        // not desired
        if (st.desired_rung == st.resident_rung) continue; // satisfied

        const bool is_hole = (st.resident_rung < 0);
        // The hole bonus is one tile width, so in nested mode it is the tile's
        // OWN width -- a 2 km level-5 hole should not outrank a 64 m one by the
        // margin a level-0 width would give it.
        const float width = cfg_.nested_sectors
            ? level_size(st.desired_level < 0 ? 0 : st.desired_level)
            : cfg_.sector_size;
        float score = is_hole ? st.dist - width : st.dist;
        // OCCLUSION PRIORITY (M4 Phase A, §5.2): "visible holes, then offscreen
        // holes", expressed as a distance penalty rather than as a separate
        // class so it composes with the hole bonus above instead of overriding
        // it. A tile that has gone unseen past the grace ranks as if it were
        // one further tile-width away -- deliberately the SAME magnitude as the
        // hole bonus, so being unseen exactly cancels being a hole and no more.
        // A stronger penalty would let a large occluded region starve
        // indefinitely behind a trickle of visible upgrades.
        //
        //
        // Same ledger and same two-clock test the cap uses, so priority and
        // detail can never disagree about whether a tile is occluded.
        if (cfg_.occlusion_grace_ticks > 0 && cfg_.nested_sectors) {
            int64_t px, py, pz;
            int plevel;
            nested_unkey(k, plevel, px, py, pz);
            if (occluded_subtree(plevel, px, py, pz)) score += width;
        }
        if (score < best_score) {
            best_score = score;
            best_k = k;
            found = true;
        }
    }

    if (!found) return false;

    auto& st = sectors_.at(best_k);
    int level; int64_t tx, ty, tz;
    sunkey(best_k, level, tx, ty, tz);
    out.tx   = tx;
    out.ty   = ty;
    out.tz   = tz;
    out.rung = st.desired_rung;
    st.inflight_rung = st.desired_rung;
    ++inflight_;
    return true;
}

// ---------------------------------------------------------------------------
// on_published()
// ---------------------------------------------------------------------------

bool SectorStreamer::on_published(int64_t tx, int64_t ty, int64_t tz,
                                  int rung) {
    uint64_t k = key_for(tx, ty, tz, rung);
    auto it = sectors_.find(k);
    if (it == sectors_.end()) {
        // Entry was erased (anchor moved on / clear()).
        return false;
    }
    auto& st = it->second;

    // Always decrement inflight for this entry if it matches.
    if (st.inflight_rung == rung) {
        st.inflight_rung = -1;
        --inflight_;
    }

    // If no longer desired at this rung, reject.
    if (st.desired_rung != rung) return false;

    // Accept: if previously resident at a different rung, queue eviction.
    if (st.resident_rung >= 0 && st.resident_rung != rung) {
        evictions_.push_back({tx, ty, tz, st.resident_rung});
    }
    st.resident_rung = rung;
    return true;
}

// ---------------------------------------------------------------------------
// on_failed()
// ---------------------------------------------------------------------------

void SectorStreamer::on_failed(int64_t tx, int64_t ty, int64_t tz,
                               int rung) {
    uint64_t k = key_for(tx, ty, tz, rung);
    auto it = sectors_.find(k);
    if (it == sectors_.end()) return;
    auto& st = it->second;
    if (st.inflight_rung == rung) {
        st.inflight_rung = -1;
        --inflight_;
    }
    st.cooldown = cfg_.fail_cooldown_updates;
}

bool SectorStreamer::cancel_request(
    int64_t tx,
    int64_t ty,
    int64_t tz,
    int rung) noexcept {
    const auto it = sectors_.find(key_for(tx, ty, tz, rung));
    if (it == sectors_.end() || it->second.inflight_rung != rung) {
        return false;
    }
    it->second.inflight_rung = -1;
    --inflight_;
    return true;
}

// ---------------------------------------------------------------------------
// take_evictions()
// ---------------------------------------------------------------------------

std::vector<Eviction> SectorStreamer::take_evictions() {
    std::vector<Eviction> out;
    out.swap(evictions_);
    return out;
}

const std::vector<Eviction>& SectorStreamer::peek_evictions() const noexcept {
    return evictions_;
}

bool SectorStreamer::commit_evictions(
    const std::vector<Eviction>& source,
    size_t count) noexcept {
    if (&source != &evictions_ || count > evictions_.size()) return false;
    evictions_.erase(evictions_.begin(), evictions_.begin() + count);
    return true;
}

// ---------------------------------------------------------------------------
// clear()
// ---------------------------------------------------------------------------

void SectorStreamer::clear() {
    for (auto& [k, st] : sectors_) {
        if (st.resident_rung >= 0) {
            int level; int64_t tx, ty, tz;
            sunkey(k, level, tx, ty, tz);
            evictions_.push_back({tx, ty, tz, st.resident_rung});
        }
    }
    sectors_.clear();
    inflight_ = 0;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

size_t SectorStreamer::resident_count() const {
    size_t n = 0;
    for (const auto& [k, st] : sectors_)
        if (st.resident_rung >= 0) ++n;
    return n;
}

size_t SectorStreamer::inflight_count() const {
    return size_t(inflight_ < 0 ? 0 : inflight_);
}

} // namespace matter_stream
