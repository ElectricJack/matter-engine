#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace matter_stream {

struct Ring { float radius; int rung; };

struct Config {
    float sector_size = 16.0f;
    // Innermost first. A sector's desired rung = the rung of the first ring
    // whose radius covers the anchor->sector-centre distance; beyond the last
    // ring the sector is not desired.
    std::vector<Ring> rings { {48.0f, 3}, {120.0f, 2}, {300.0f, 1}, {800.0f, 0} };
    float hysteresis = 16.0f;        // extra distance before demote/evict
    int   max_inflight = 8;
    int   fail_cooldown_updates = 64;
};

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

    // Drain sectors to unpublish + release (each was previously accepted).
    std::vector<Eviction> take_evictions();

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

    struct KeyHash { size_t operator()(uint64_t k) const { return k ^ (k >> 33); } };
    std::unordered_map<uint64_t, SectorState, KeyHash> sectors_;

    std::vector<Eviction> evictions_;
    int inflight_ = 0;
    float last_anchor_x_ = 0.0f;
    float last_anchor_z_ = 0.0f;

    // desired_rung for a given anchor distance
    int desired_rung_for_dist(float d) const;
    // anchor-to-sector-centre distance
    float sector_dist(int64_t tx, int64_t tz) const;
};

} // namespace matter_stream
