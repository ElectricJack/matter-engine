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

SectorStreamer::SectorStreamer(Config cfg)
    : cfg_(std::move(cfg)) {
    if (cfg_.terrain_lod_enabled && cfg_.terrain_bands.empty()) {
        // Radial profile in sector sizes: near disc native voxel (LOD 5),
        // then heightfield LODs down to a single quad. Wider near bands than
        // the design table's minimum (3S/5S/8S/14S/24S): editor cameras fly
        // hundreds of meters up, where 8-16 m cells at the design's 5-8S
        // radii read as visible facets. Every adjacent pair stays >= 2S
        // apart so the default map is 2:1-balanced by construction; the
        // explicit balance pass still guards custom profiles.
        const float S = cfg_.sector_size;
        cfg_.terrain_bands = {
            {5.0f * S, 5},  {8.0f * S, 4},  {12.0f * S, 3},
            {18.0f * S, 2}, {27.0f * S, 1}, {40.0f * S, 0},
        };
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
// hysteresis, 2:1 cardinal balance, edge masks, variant repack. Runs after
// the scatter scan/hysteresis has finalized desired_rung as a bare-or-packed
// value whose scatter bits are authoritative.
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

    // Pass 3: coarser-neighbor edge masks + repack. Bit layout matches
    // terrain_mesher::EdgeMaskBits (bit 0 = +x, 1 = -x, 2 = +z, 3 = -z).
    for (auto& [k, st] : sectors_) {
        if (st.desired_rung < 0 || st.desired_lod < 0) continue;
        if (stream_no_evict() && st.resident_rung >= 0) continue;
        int64_t tx, tz;
        unkey(k, tx, tz);
        const int64_t ntx[4] = {tx + 1, tx - 1, tx, tx};
        const int64_t ntz[4] = {tz, tz, tz + 1, tz - 1};
        int mask = 0;
        for (int n = 0; n < 4; ++n) {
            auto it = sectors_.find(key(ntx[n], ntz[n]));
            if (it == sectors_.end() || it->second.desired_lod < 0) continue;
            if (it->second.desired_lod == st.desired_lod - 1) mask |= 1 << n;
        }
        st.desired_rung = pack_variant(variant_scatter(st.desired_rung),
                                       st.desired_lod, mask);
    }
}

// ---------------------------------------------------------------------------
// update()
// ---------------------------------------------------------------------------

void SectorStreamer::update(float anchor_x, float anchor_z) {
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

    // Two-pass: holes first (resident_rung == -1 and desired_rung >= 0),
    // then upgrades/demotions (desired_rung != resident_rung), nearest first.
    // A sector with cooldown > 0 is skipped.

    auto pick = [&](bool holes_only) -> bool {
        uint64_t best_k = 0;
        float best_dist = std::numeric_limits<float>::max();
        bool found = false;

        for (auto& [k, st] : sectors_) {
            if (st.inflight_rung >= 0) continue;      // already in flight
            if (st.cooldown > 0) continue;             // cooling down
            if (st.desired_rung < 0) continue;        // not desired
            if (st.desired_rung == st.resident_rung) continue; // satisfied

            bool is_hole = (st.resident_rung < 0);
            if (holes_only && !is_hole) continue;
            if (!holes_only && is_hole) continue;

            if (st.dist < best_dist) {
                best_dist = st.dist;
                best_k = k;
                found = true;
            }
        }

        if (!found) return false;

        auto& st = sectors_.at(best_k);
        int64_t tx, tz;
        unkey(best_k, tx, tz);
        out.tx   = tx;
        out.tz   = tz;
        out.rung = st.desired_rung;
        st.inflight_rung = st.desired_rung;
        ++inflight_;
        return true;
    };

    return pick(true) || pick(false);
}

// ---------------------------------------------------------------------------
// on_published()
// ---------------------------------------------------------------------------

bool SectorStreamer::on_published(int64_t tx, int64_t tz, int rung) {
    uint64_t k = key(tx, tz);
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
    uint64_t k = key(tx, tz);
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
    const auto it = sectors_.find(key(tx, tz));
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
            int64_t tx, tz;
            unkey(k, tx, tz);
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
