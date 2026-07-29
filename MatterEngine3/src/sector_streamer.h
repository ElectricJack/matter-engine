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
};

} // namespace matter_stream
