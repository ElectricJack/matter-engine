#include "sector_streamer.h"
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <limits>

namespace matter_stream {

SectorStreamer::SectorStreamer(Config cfg)
    : cfg_(std::move(cfg)) {}

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
                // Find the ring radius that produced resident_rung.
                float ring_radius = 0.0f;
                for (const auto& ring : cfg_.rings) {
                    if (ring.rung == st.resident_rung) { ring_radius = ring.radius; break; }
                }
                // If desired_rung > resident_rung (promotion): no hysteresis, proceed.
                // If desired_rung < resident_rung (demotion): hysteresis applies.
                if (st.desired_rung < st.resident_rung) {
                    // Demotion: only proceed if dist > ring_radius + hysteresis.
                    if (st.dist <= ring_radius + cfg_.hysteresis) {
                        // Still within hysteresis — freeze desired at current resident rung.
                        st.desired_rung = st.resident_rung;
                    }
                }
            }
        } else {
            // Not desired. Evict if resident.
            if (st.resident_rung >= 0) {
                // Hysteresis: only evict if dist > outer_ring + hysteresis.
                // The outer ring is the last ring's radius.
                float outer_r = cfg_.rings.empty() ? 0.0f : cfg_.rings.back().radius;
                if (st.dist > outer_r + cfg_.hysteresis) {
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

// ---------------------------------------------------------------------------
// take_evictions()
// ---------------------------------------------------------------------------

std::vector<Eviction> SectorStreamer::take_evictions() {
    std::vector<Eviction> out;
    out.swap(evictions_);
    return out;
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
