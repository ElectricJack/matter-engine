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
    if (level_radius_.empty()) cfg_.nested_sectors = false;
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
float SectorStreamer::tile_near_dist(int level, int64_t tx, int64_t tz) const {
    const float S = level_size(level);
    const float x0 = float(tx) * S, x1 = x0 + S;
    const float z0 = float(tz) * S, z1 = z0 + S;
    const float dx = std::max(std::max(x0 - last_anchor_x_, 0.0f),
                              last_anchor_x_ - x1);
    const float dz = std::max(std::max(z0 - last_anchor_z_, 0.0f),
                              last_anchor_z_ - z1);
    return std::sqrt(dx * dx + dz * dz);
}

int SectorStreamer::nested_scatter_tier(float d) const {
    for (const auto& ring : cfg_.rings)
        if (d <= ring.radius) return ring.rung;
    // Past the last ring: the coarsest tier, NOT "undesired". In nested mode
    // the terrain bands bound residency and the rings only grade scatter.
    return cfg_.rings.empty() ? 0 : cfg_.rings.back().rung;
}

int SectorStreamer::min_edge_level(bool vary_z, float fixed, float t0, float t1,
                                   int seg_level, int stop_below) const {
    // Probe the segment's own CENTRE -- an interior point of whichever tile
    // covers it, never a tile boundary, so the answer is unambiguous.
    const float mid = 0.5f * (t0 + t1);
    const int m = vary_z ? desired_level_at(fixed, mid)
                         : desired_level_at(mid, fixed);
    // Nothing desired out there (past the reach): no adjacency constraint.
    if (m < 0) return kMaxLevel + 1;
    // A tile at or above this segment's level spans the whole segment, because
    // the grids nest. That is the early exit that makes this cheap.
    if (m >= seg_level || seg_level <= 0) return m;
    const int a = min_edge_level(vary_z, fixed, t0, mid, seg_level - 1, stop_below);
    if (a < stop_below) return a;                 // already a violation
    const int b = min_edge_level(vary_z, fixed, mid, t1, seg_level - 1, stop_below);
    return std::min(a, b);
}

int SectorStreamer::desired_level_at(float wx, float wz) const {
    for (int L = 0; L <= max_level(); ++L) {
        const float S = level_size(L);
        const int64_t tx = int64_t(std::floor(wx / S));
        const int64_t tz = int64_t(std::floor(wz / S));
        auto it = sectors_.find(nested_key(L, tx, tz));
        if (it != sectors_.end() && it->second.desired_level == L) return L;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Nested mode: the desired set
// ---------------------------------------------------------------------------

void SectorStreamer::mark_desired(int level, int64_t tx, int64_t tz) {
    auto& st = sectors_[nested_key(level, tx, tz)];
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
    int level, int64_t tx, int64_t tz,
    const std::unordered_map<uint64_t, char, KeyHash>& finer_resident) {
    const float nd = tile_near_dist(level, tx, tz);

    // A tile with anything of its own resident -- itself, or something finer
    // under it -- is OCCUPIED, and every boundary it can cross gets hysteresis
    // on the way out. Two boundaries matter and both churned without this: the
    // split radius (below) and the reach (here). At the reach, a bare `nd >
    // reach()` drops the outermost ring of tiles the instant the anchor drifts
    // a metre outward and re-requests them when it drifts back -- the uniform
    // path has always held that edge with `outer_r + hysteresis`.
    const bool is_split =
        finer_resident.find(nested_key(level, tx, tz)) != finer_resident.end();
    bool self_resident = false;
    {
        auto it = sectors_.find(nested_key(level, tx, tz));
        self_resident = it != sectors_.end() &&
                        (it->second.resident_rung >= 0 ||
                         it->second.inflight_rung >= 0);
    }
    const bool occupied = is_split || self_resident;
    if (nd > reach() + (occupied ? cfg_.hysteresis : 0.0f)) return;
    if (level == 0) { mark_desired(0, tx, tz); return; }

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
    // The footprint, not the neighbourhood at large, is the right region to ask
    // about. Asking about cardinal neighbours too would fight the legitimate
    // band gradient: a level-0 tile near the anchor is *supposed* to sit beside
    // level-1 tiles, and a clamp that took the coarsest neighbour would pin the
    // whole disc at the outermost band's level forever. What we want to detect
    // is specifically "this patch of world is currently being drawn coarse",
    // and the tile that is drawing it coarse is this tile's own ancestor.
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
        const int resident_level = resident_level_over(level, tx, tz);
        // Descending to level-1 asks for level-1; the clamp floor is
        // resident_level - 1, so the descent is admissible iff
        // level - 1 >= resident_level - 1.
        if (resident_level >= 0 && level < resident_level) split = false;
    }

    if (split) {
        // Hysteresis acts on the whole sibling quad, never on one child: a tile
        // cannot be half-merged, so the four children are decided together.
        for (int c = 0; c < 4; ++c)
            descend(level - 1, 2 * tx + (c & 1), 2 * tz + (c >> 1),
                    finer_resident);
    } else {
        mark_desired(level, tx, tz);
    }
}

bool SectorStreamer::staged_refinement_active() const {
    return cfg_.staged_refinement && !stream_no_staging();
}

int SectorStreamer::resident_level_over(int level, int64_t tx,
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
        auto it = sectors_.find(nested_key(up, tx >> sh, tz >> sh));
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
    for (int sweep = 0; sweep < 8; ++sweep) {
        std::vector<std::pair<int, std::pair<int64_t, int64_t>>> to_split;
        for (const auto& [k, st] : sectors_) {
            if (st.desired_level < 1) continue;
            int lvl; int64_t tx, tz;
            nested_unkey(k, lvl, tx, tz);
            const float S = level_size(lvl);
            const float ox = float(tx) * S, oz = float(tz) * S;
            const float out = 0.5f * cfg_.sector_size;   // just outside an edge
            // The whole edge, not a midpoint sample: a lone over-fine
            // neighbour against a long coarse edge is exactly the case that
            // matters, and a midpoint probe would walk past it.
            const int worst = std::min(
                std::min(min_edge_level(true,  ox + S + out, oz, oz + S, lvl, lvl - 1),
                         min_edge_level(true,  ox - out,     oz, oz + S, lvl, lvl - 1)),
                std::min(min_edge_level(false, oz + S + out, ox, ox + S, lvl, lvl - 1),
                         min_edge_level(false, oz - out,     ox, ox + S, lvl, lvl - 1)));
            if (worst < lvl - 1) to_split.push_back({lvl, {tx, tz}});
        }
        if (to_split.empty()) return;
        for (const auto& e : to_split) {
            const int lvl = e.first;
            const int64_t tx = e.second.first, tz = e.second.second;
            auto it = sectors_.find(nested_key(lvl, tx, tz));
            if (it != sectors_.end()) {
                it->second.desired_level = -1;
                it->second.desired_lod   = -1;
                it->second.desired_rung  = -1;
            }
            for (int c = 0; c < 4; ++c)
                mark_desired(lvl - 1, 2 * tx + (c & 1), 2 * tz + (c >> 1));
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

void SectorStreamer::scan_subtree(int level, int64_t tx, int64_t tz,
                                  bool& any_desired, bool& all_resident) const {
    auto it = sectors_.find(nested_key(level, tx, tz));
    if (it != sectors_.end() && it->second.desired_level == level) {
        any_desired = true;
        if (it->second.resident_rung < 0) all_resident = false;
        return;                       // this tile covers its own footprint
    }
    if (level == 0) return;           // nothing desired covers this column
    for (int c = 0; c < 4; ++c)
        scan_subtree(level - 1, 2 * tx + (c & 1), 2 * tz + (c >> 1),
                     any_desired, all_resident);
}

void SectorStreamer::scan_footprint(int level, int64_t tx, int64_t tz,
                                    bool& any_desired,
                                    bool& all_resident) const {
    // A merge: one coarser tile has taken over this footprint. At most one
    // ancestor can be desired, because exactly one level covers any column.
    for (int up = level + 1; up <= max_level(); ++up) {
        const int sh = up - level;
        auto it = sectors_.find(nested_key(up, tx >> sh, tz >> sh));
        if (it != sectors_.end() && it->second.desired_level == up) {
            any_desired = true;
            if (it->second.resident_rung < 0) all_resident = false;
            return;
        }
    }
    // Otherwise a split (or nothing at all).
    scan_subtree(level, tx, tz, any_desired, all_resident);
}

void SectorStreamer::update_nested(float anchor_x, float anchor_z) {
    last_anchor_x_ = anchor_x;
    last_anchor_z_ = anchor_z;

    // "Is anything finer than level L resident under this tile?" -- answered by
    // walking each resident tile's ancestors once (O(resident x levels)) rather
    // than by a range query per descent node. Inflight counts as residency in
    // the making: a tile whose children are being baked is already split.
    std::unordered_map<uint64_t, char, KeyHash> finer_resident;
    for (const auto& [k, st] : sectors_) {
        if (st.resident_rung < 0 && st.inflight_rung < 0) continue;
        int lvl; int64_t tx, tz;
        nested_unkey(k, lvl, tx, tz);
        for (int up = lvl + 1; up <= max_level(); ++up) {
            const int sh = up - lvl;
            finer_resident[nested_key(up, tx >> sh, tz >> sh)] = 1;
        }
    }

    for (auto& [k, st] : sectors_) {
        st.desired_rung  = -1;
        st.desired_lod   = -1;
        st.desired_level = -1;
        if (st.cooldown > 0) --st.cooldown;
        int lvl; int64_t tx, tz;
        nested_unkey(k, lvl, tx, tz);
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
    for (int64_t tz = tz_min; tz <= tz_max; ++tz)
        for (int64_t tx = tx_min; tx <= tx_max; ++tx)
            descend(top, tx, tz, finer_resident);

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
            int lvl; int64_t tx, tz;
            nested_unkey(k, lvl, tx, tz);
            bool any_desired = false, all_resident = true;
            scan_footprint(lvl, tx, tz, any_desired, all_resident);
            if (any_desired && !all_resident) continue;   // hold: still baking
            evictions_.push_back({tx, tz, st.resident_rung});
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

void SectorStreamer::update(float anchor_x, float anchor_z) {
    if (cfg_.nested_sectors) { update_nested(anchor_x, anchor_z); return; }

    last_anchor_x_ = anchor_x;
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
                    evictions_.push_back({0, 0, st.resident_rung});
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
        const float score = is_hole ? st.dist - width : st.dist;
        if (score < best_score) {
            best_score = score;
            best_k = k;
            found = true;
        }
    }

    if (!found) return false;

    auto& st = sectors_.at(best_k);
    int level; int64_t tx, tz;
    sunkey(best_k, level, tx, tz);
    out.tx   = tx;
    out.tz   = tz;
    out.rung = st.desired_rung;
    st.inflight_rung = st.desired_rung;
    ++inflight_;
    return true;
}

// ---------------------------------------------------------------------------
// on_published()
// ---------------------------------------------------------------------------

bool SectorStreamer::on_published(int64_t tx, int64_t tz, int rung) {
    uint64_t k = key_for(tx, tz, rung);
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
        evictions_.push_back({tx, tz, st.resident_rung});
    }
    st.resident_rung = rung;
    return true;
}

// ---------------------------------------------------------------------------
// on_failed()
// ---------------------------------------------------------------------------

void SectorStreamer::on_failed(int64_t tx, int64_t tz, int rung) {
    uint64_t k = key_for(tx, tz, rung);
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
    int64_t tz,
    int rung) noexcept {
    const auto it = sectors_.find(key_for(tx, tz, rung));
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
            int level; int64_t tx, tz;
            sunkey(k, level, tx, tz);
            evictions_.push_back({tx, tz, st.resident_rung});
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
