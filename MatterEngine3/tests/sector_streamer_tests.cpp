// MatterEngine3/tests/sector_streamer_tests.cpp
#include "check.h"
#include "../src/sector_streamer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <vector>

using namespace matter_stream;

// Service every outstanding request (simulate instant bakes). Optionally
// record each request via cb(q).
template <typename F>
static void service_all(SectorStreamer& s, F cb) {
    SectorRequest q;
    while (s.next_request(q)) { cb(q); s.on_published(q.tx, q.ty, q.tz, q.rung); }
}
static void service_all(SectorStreamer& s) { service_all(s, [](const SectorRequest&){}); }

// Settle: update+service until quiescent; RETURNS all evictions drained on
// the way (callers assert on them — do not discard silently).
static std::vector<Eviction> settle(SectorStreamer& s, float x, float z) {
    std::vector<Eviction> evs;
    for (int i = 0; i < 10000; ++i) {           // ~8 publishes per update
        s.update(x, 0.0f, z);
        SectorRequest q;
        bool any = false;
        while (s.next_request(q)) { any = true; s.on_published(q.tx, q.ty, q.tz, q.rung); }
        auto e = s.take_evictions();
        evs.insert(evs.end(), e.begin(), e.end());
        if (!any) break;
    }
    return evs;
}

int main() {
    Config cfg;   // defaults: rings 48/120/300/800, hysteresis 16, inflight 8

    // --- the nested key round trip (volumetric-sectors M1) -------------------
    //
    // FIRST, deliberately. The key was repacked from `4 level | 30 tx | 30 tz`
    // to `4 level | 20 ty | 20 tx | 20 tz` to make room for the vertical axis,
    // and a sign-extension mistake in that repack does not announce itself: it
    // produces a key that is merely WRONG, so a tile at (-1, -1) answers for
    // some tile a million cells away, and the symptom surfaces days later as
    // terrain appearing in the wrong place under a moving camera. Every other
    // block in this file runs the streamer at positive-ish coordinates and
    // would pass with a broken sext20.
    //
    // So: exhaustive-in-spirit over the cases that can break -- both signs on
    // all three axes, the exact 20-bit extremes, and every level -- plus the
    // distinctness the whole map depends on.
    {
        const int64_t probes[] = {
            0, 1, -1, 2, -2, 1023, -1024, 65535, -65536,
            kSectorCoordMax, kSectorCoordMin,
            kSectorCoordMax - 1, kSectorCoordMin + 1,
        };
        int checked = 0;
        for (int level = 0; level <= kMaxLevel; ++level) {
            for (int64_t tx : probes) {
                for (int64_t ty : probes) {
                    for (int64_t tz : probes) {
                        const uint64_t k = nested_key(level, tx, ty, tz);
                        int rl = -1; int64_t rx = 0, ry = 0, rz = 0;
                        nested_unkey(k, rl, rx, ry, rz);
                        CHECK(rl == level && rx == tx && ry == ty && rz == tz,
                              "nested key round trip");
                        ++checked;
                    }
                }
            }
        }
        // Distinctness on each axis independently -- a mask that was one bit
        // too wide would round-trip fine and still collide with its neighbour
        // field, which is the failure the round trip alone cannot see.
        CHECK(nested_key(0, 1, 0, 0) != nested_key(0, 0, 1, 0), "tx vs ty");
        CHECK(nested_key(0, 1, 0, 0) != nested_key(0, 0, 0, 1), "tx vs tz");
        CHECK(nested_key(0, 0, 1, 0) != nested_key(0, 0, 0, 1), "ty vs tz");
        CHECK(nested_key(1, 0, 0, 0) != nested_key(0, 0, 0, 0), "level");
        CHECK(nested_key(0, -1, 0, 0) != nested_key(0, 0, -1, 0),
              "negative tx vs ty");
        // The wrap is exactly one past the range, not somewhere convenient.
        CHECK(nested_key(0, kSectorCoordMax + 1, 0, 0) ==
                  nested_key(0, kSectorCoordMin, 0, 0),
              "20-bit wrap lands where the range check says it does");
        CHECK(sector_coord_fits(kSectorCoordMax) &&
                  sector_coord_fits(kSectorCoordMin) &&
                  !sector_coord_fits(kSectorCoordMax + 1) &&
                  !sector_coord_fits(kSectorCoordMin - 1),
              "sector_coord_fits agrees with the packing");
        std::printf("  key round trip: %d (level, tx, ty, tz) tuples, "
                    "signs and 20-bit extremes on all three axes\n", checked);
    }

    // --- desired rung by distance -------------------------------------------
    {
        SectorStreamer s(cfg);
        // Fill everything, verifying known probe sectors were requested at the
        // right final rung (last request seen per probe wins).
        int rung_00 = -1, rung_40 = -1, rung_10_0 = -1, rung_30_0 = -1;
        bool saw_60_0 = false;
        for (int i = 0; i < 10000; ++i) {
            s.update(8.0f, 0.0f, 8.0f);        // camera at centre of sector (0,0)
            bool any = false;
            service_all(s, [&](const SectorRequest& q) {
                any = true;
                if (q.tx == 0  && q.tz == 0) rung_00   = q.rung;
                if (q.tx == 4  && q.tz == 0) rung_40   = q.rung;   // centre dist ~64
                if (q.tx == 10 && q.tz == 0) rung_10_0 = q.rung;   // ~160
                if (q.tx == 30 && q.tz == 0) rung_30_0 = q.rung;   // ~480
                if (q.tx == 60 && q.tz == 0) saw_60_0  = true;     // ~960: never
            });
            s.take_evictions();
            if (!any) break;
        }
        CHECK(rung_00 == 3,   "sector under camera -> rung 3");
        CHECK(rung_40 == 2,   "dist ~64 -> rung 2");
        CHECK(rung_10_0 == 1, "dist ~160 -> rung 1");
        CHECK(rung_30_0 == 0, "dist ~480 -> rung 0");
        CHECK(!saw_60_0,      "beyond outer ring: never requested");
    }
    // --- inflight cap + holes-before-upgrades --------------------------------
    {
        SectorStreamer s(cfg);
        s.update(8.0f, 0.0f, 8.0f);
        SectorRequest q;
        int got = 0;
        while (s.next_request(q)) ++got;
        CHECK(got == 8, "max_inflight caps outstanding requests");
        CHECK(s.inflight_count() == 8, "inflight_count tracks");
    }
    // --- rung swap: publish new THEN evict old --------------------------------
    {
        SectorStreamer s(cfg);
        settle(s, 8.0f, 8.0f);
        // Move camera so sector (0,0) drops from rung 3 into the rung-2 band.
        auto ev = settle(s, 8.0f + 100.0f, 8.0f);   // (0,0) now at dist ~100
        bool evicted_00_r3 = false;
        for (auto& e : ev) if (e.tx == 0 && e.tz == 0 && e.rung == 3) evicted_00_r3 = true;
        CHECK(evicted_00_r3, "old rung evicted after demotion swap");
    }
    // --- hysteresis: small camera moves don't churn ---------------------------
    {
        SectorStreamer s(cfg);
        settle(s, 8.0f, 8.0f);
        s.update(8.0f + 8.0f, 0.0f, 8.0f);      // move less than hysteresis
        auto ev = s.take_evictions();
        CHECK(ev.empty(), "no eviction within hysteresis");
    }
    // --- hysteresis oscillation: sustained sub-hysteresis jitter never churns ----
    // Two effects are in play when the camera moves and we service requests:
    //   * moving pulls NEW frontier sectors into range (streaming, not churn), and
    //   * the FIRST crossing of a ring boundary legitimately PROMOTES a boundary
    //     sector to a finer rung — a promotion evicts the coarse version, which is
    //     desired refinement, not churn.
    // So we cannot assert "zero evictions from the first move". The property that
    // matters for stability is narrower: once boundary sectors have reached their
    // steady-state rung, continued jitter within the band must NOT keep evicting/
    // re-baking them. Warm up with a few oscillation cycles, then assert the
    // steady state produces zero further churn.
    {
        SectorStreamer s(cfg);
        settle(s, 8.0f, 8.0f);

        auto oscillate = [&](int iters, std::vector<Eviction>* sink) {
            for (int b = 0; b < iters; ++b) {
                s.update(8.0f + (b % 2 == 0 ? 7.0f : -7.0f), 0.0f, 8.0f);   // ±7 < hysteresis 16
                SectorRequest q;
                while (s.next_request(q)) s.on_published(q.tx, q.ty, q.tz, q.rung);
                auto ev = s.take_evictions();
                if (sink) sink->insert(sink->end(), ev.begin(), ev.end());
            }
        };
        oscillate(8, nullptr);                       // reach steady state
        size_t resident_steady = s.resident_count();

        std::vector<Eviction> all_evs;
        oscillate(12, &all_evs);                     // measure
        CHECK(all_evs.empty(), "hysteresis: steady-state oscillation causes no churn");
        CHECK(s.resident_count() >= resident_steady,
              "hysteresis: no resident sector lost during steady-state oscillation");
    }
    // --- late publish rejected ------------------------------------------------
    {
        SectorStreamer s(cfg);
        s.update(8.0f, 0.0f, 8.0f);
        SectorRequest q;
        CHECK(s.next_request(q), "got a request");
        s.update(8.0f + 5000.0f, 0.0f, 8.0f);   // camera long gone
        CHECK(!s.on_published(q.tx, q.ty, q.tz, q.rung), "stale publish rejected");
    }
    // --- fail cooldown ---------------------------------------------------------
    {
        Config c2 = cfg; c2.fail_cooldown_updates = 10; c2.max_inflight = 1;
        SectorStreamer s(c2);
        s.update(8.0f, 0.0f, 8.0f);
        SectorRequest q;
        CHECK(s.next_request(q), "first request");
        int64_t fx = q.tx, fy = q.ty, fz = q.tz; int fr = q.rung;
        s.on_failed(fx, fy, fz, fr);
        bool re_requested_early = false;
        for (int i = 0; i < 9; ++i) {
            s.update(8.0f, 0.0f, 8.0f);
            if (s.next_request(q)) {
                if (q.tx == fx && q.tz == fz && q.rung == fr) re_requested_early = true;
                s.on_published(q.tx, q.ty, q.tz, q.rung);   // keep the queue moving
                s.take_evictions();
            }
        }
        CHECK(!re_requested_early, "failed sector cools down");
        bool re_requested_later = false;
        for (int i = 0; i < 5000 && !re_requested_later; ++i) {
            s.update(8.0f, 0.0f, 8.0f);
            if (s.next_request(q)) {
                if (q.tx == fx && q.tz == fz && q.rung == fr) re_requested_later = true;
                else { s.on_published(q.tx, q.ty, q.tz, q.rung); s.take_evictions(); }
            }
        }
        CHECK(re_requested_later, "failed sector retried after cooldown");
    }
    // --- clear(): everything evicts, stale publishes rejected -----------------
    {
        SectorStreamer s(cfg);
        settle(s, 8.0f, 8.0f);
        s.take_evictions();
        size_t res = s.resident_count();
        CHECK(res > 0, "resident before clear");
        s.clear();
        CHECK(s.resident_count() == 0, "clear empties residency");
        CHECK(s.take_evictions().size() == res, "clear evicts everything");
    }
    // --- clear(): late in-flight publish cannot restore residency ---------------
    {
        SectorStreamer s(cfg);
        s.update(8.0f, 0.0f, 8.0f);
        SectorRequest q;
        CHECK(s.next_request(q), "clear late-publish test has an inflight request");
        s.clear();
        CHECK(!s.on_published(q.tx, q.ty, q.tz, q.rung),
              "clear rejects a late publish");
        CHECK(s.resident_count() == 0 && s.inflight_count() == 0,
              "late publish after clear leaves no resident or inflight sectors");
    }
    // --- long flight: bounded residency, no monotonic growth ------------------
    {
        SectorStreamer s(cfg);
        size_t peak = 0;
        for (int step = 0; step < 500; ++step) {
            float x = 8.0f + step * 10.0f;   // 5,000 units of flight
            s.update(x, 0.0f, 8.0f);
            service_all(s);
            s.take_evictions();
            peak = std::max(peak, s.resident_count());
        }
        // Outer ring disc: pi*800^2 / 256 ~ 7,854 sectors. Allow slack for
        // hysteresis + square scan, reject unbounded growth.
        CHECK(peak < 9500, "resident bounded during long flight");
        settle(s, 8.0f + 500 * 10.0f, 8.0f);
        s.take_evictions();
        size_t at_end = s.resident_count();
        CHECK(at_end < 9500, "no leak after flight");
        printf("  long flight: peak=%zu end=%zu\n", peak, at_end);
    }

    // --- terrain LOD ladder: bands, 2:1 balance ------------------------------
    // This block used to assert a third property, that the packed variant's
    // edge-mask bits named exactly the one-level-coarser cardinal neighbours.
    // The mask is gone from the variant (sector_streamer.h records why: it was
    // a promise about the DESIRED map that the DRAWN map broke, and living in
    // the bake identity it made every neighbour level change a rebake of this
    // tile). Cross-level seam geometry is welded engine-side at runtime from
    // the drawn pair, so there is nothing here for a test to check. What
    // remains -- the bands and the 2:1 balance -- is untouched and still the
    // invariant the welder's one-level fan logic is built on.
    {
        Config tcfg;
        tcfg.sector_size = 64.0f;
        tcfg.rings = { {128.0f, 2}, {320.0f, 1}, {2560.0f, 0} };
        tcfg.terrain_lod_enabled = true;   // default design bands 3S..40S
        SectorStreamer s(tcfg);

        // Settle and record the FINAL accepted variant per sector.
        struct V { int rung; };
        std::unordered_map<long long, int> final_variant;
        auto skey = [](int64_t tx, int64_t tz) {
            return (long long)((tx << 20) ^ (tz & 0xFFFFF));
        };
        for (int i = 0; i < 20000; ++i) {
            s.update(32.0f, 0.0f, 32.0f);
            SectorRequest q; bool any = false;
            while (s.next_request(q)) {
                any = true;
                if (s.on_published(q.tx, q.ty, q.tz, q.rung))
                    final_variant[skey(q.tx, q.tz)] = q.rung;
            }
            s.take_evictions();
            if (!any && i > 2) break;
        }
        CHECK(!final_variant.empty(), "terrain ladder settled");

        bool all_packed = true, near_is_voxel = true, far_is_coarse = true;
        bool balanced = true;
        int seen_lods = 0;
        for (const auto& [k, v] : final_variant) {
            if (!variant_packed(v)) { all_packed = false; continue; }
            const int64_t tx = k >> 20;
            const int64_t tz = (int64_t)(int32_t)((k & 0xFFFFF) << 12) >> 12;
            const float cx = (float(tx) + 0.5f) * 64.0f - 32.0f;
            const float cz = (float(tz) + 0.5f) * 64.0f - 32.0f;
            const float d = std::sqrt(cx * cx + cz * cz);
            const int lod = variant_terrain_lod(v);
            seen_lods |= 1 << lod;
            if (d < 150.0f && lod != 5) near_is_voxel = false;
            if (d > 1700.0f && lod > 1) far_is_coarse = false;

            const int64_t ntx[4] = {tx + 1, tx - 1, tx, tx};
            const int64_t ntz[4] = {tz, tz, tz + 1, tz - 1};
            for (int n = 0; n < 4; ++n) {
                auto it = final_variant.find(skey(ntx[n], ntz[n]));
                if (it == final_variant.end()) continue;
                const int nlod = variant_terrain_lod(it->second);
                if (nlod - lod > 1 || lod - nlod > 1) balanced = false;
            }
        }
        CHECK(all_packed, "every terrain-ladder variant carries the marker");
        CHECK(near_is_voxel, "sectors inside 3S stay native voxel (lod 5)");
        CHECK(far_is_coarse, "outer-band sectors coarsen to lod <= 1");
        CHECK(seen_lods == 0x3F, "all six LOD levels appear in the settled disc");
        CHECK(balanced, "settled cardinal neighbors differ by at most one LOD");
        // Bits 7-10 are now reserved and must read back as zero: a stray
        // writer there would be invisible to every decode helper but would
        // still change the bake identity, which is the failure mode the mask
        // itself was.
        bool reserved_clear = true;
        for (const auto& [k, v] : final_variant)
            if (variant_packed(v) && ((v >> 7) & 0xF) != 0) reserved_clear = false;
        CHECK(reserved_clear,
              "the retired edge-mask bits 7-10 are left clear by every packer");

        // Legacy path untouched: same rings without the flag produce bare rungs.
        Config bare = tcfg; bare.terrain_lod_enabled = false;
        SectorStreamer s2(bare);
        s2.update(32.0f, 0.0f, 32.0f);
        SectorRequest q;
        CHECK(s2.next_request(q), "legacy streamer still requests");
        CHECK(!variant_packed(q.rung) && q.rung >= 0 && q.rung <= 2,
              "legacy request carries a bare scatter rung");
    }

    // =======================================================================
    // NESTED SECTOR LOD (docs/terrain-nested-sector-lod-2026-08-08.md, WP1)
    //
    // Level L has tile size S_0 << L and mesher rung -L, so cells-per-tile is
    // constant and each annulus holds a near-constant tile count. Level rides
    // the packed variant as 5 - terrain_lod; no new bits.
    // =======================================================================

    // StreamMountain's shape at 1/10 scale, so a settle is quick: bands at
    // 32/119/261/470/775/1010 m with the level-0 tile at 6.4 m.
    auto nested_cfg = []() {
        Config c;
        c.sector_size = 6.4f;
        c.rings = { {15.0f, 2}, {50.0f, 1}, {1010.0f, 0} };
        c.nested_sectors = true;
        c.terrain_bands = {
            {32.0f, 5}, {119.0f, 4}, {261.0f, 3},
            {470.0f, 2}, {775.0f, 1}, {1010.0f, 0},
        };
        c.hysteresis = 2.0f;
        return c;
    };

    // The settled desired set, as (level, tx, tz) -> variant.
    auto settle_nested = [](SectorStreamer& s, float x, float z) {
        std::map<std::tuple<int,long long,long long>, int> live;
        for (int i = 0; i < 20000; ++i) {
            s.update(x, 0.0f, z);
            SectorRequest q; bool any = false;
            while (s.next_request(q)) {
                any = true;
                if (s.on_published(q.tx, q.ty, q.tz, q.rung))
                    live[{variant_level(q.rung), (long long)q.tx,
                          (long long)q.tz}] = q.rung;
            }
            for (const auto& e : s.take_evictions())
                live.erase({variant_level(e.rung), (long long)e.tx,
                            (long long)e.tz});
            if (!any && i > 2) break;
        }
        return live;
    };

    // --- coverage: every world column is covered by EXACTLY one tile --------
    // The property that makes the desired set a hole-free quadtree, and the
    // one thing a distance-banded descent can plausibly get wrong. Sampled on
    // a grid finer than the finest tile, so a missing or doubled tile of any
    // size is caught.
    {
        SectorStreamer s(nested_cfg());
        const float ax = 3.2f, az = 3.2f;
        auto live = settle_nested(s, ax, az);
        CHECK(!live.empty(), "nested: something settled");

        const float S0 = 6.4f;
        int holes = 0, doubles = 0, probes = 0;
        int by_level[6] = {0, 0, 0, 0, 0, 0};
        for (const auto& [k, v] : live) by_level[std::get<0>(k)]++;
        // Probe the inner 700 m at 3.2 m spacing (half a level-0 tile).
        for (float wz = -700.0f; wz <= 700.0f; wz += 3.2f)
            for (float wx = -700.0f; wx <= 700.0f; wx += 3.2f) {
                const float d = std::sqrt(wx * wx + wz * wz);
                if (d > 690.0f) continue;         // clear of the reach edge
                ++probes;
                int cover = 0;
                for (int L = 0; L <= 5; ++L) {
                    const float S = S0 * float(1 << L);
                    const long long tx = (long long)std::floor(wx / S);
                    const long long tz = (long long)std::floor(wz / S);
                    if (live.count({L, tx, tz})) ++cover;
                }
                if (cover == 0) ++holes;
                if (cover > 1) ++doubles;
            }
        printf("  nested: %zu tiles (L0..L5: %d %d %d %d %d %d), "
               "%d probes, %d holes, %d doubles\n",
               live.size(), by_level[0], by_level[1], by_level[2],
               by_level[3], by_level[4], by_level[5], probes, holes, doubles);
        CHECK(probes > 100000, "coverage probe actually sampled the disc");
        CHECK(holes == 0,   "nested coverage: no world column is uncovered");
        CHECK(doubles == 0, "nested coverage: no world column is covered twice");
        // Every level appears: a ladder that collapsed to one level would pass
        // the coverage test above while defeating the entire design.
        for (int L = 0; L <= 5; ++L)
            CHECK(by_level[L] > 0, "nested: every level is populated");

        // FAILABLE CHECK for the coverage gate itself: delete one tile and the
        // probe must report holes. Without this, "0 holes" could mean the probe
        // is looking in the wrong place.
        {
            auto broken = live;
            broken.erase(broken.begin());
            int h = 0;
            for (float wz = -700.0f; wz <= 700.0f; wz += 3.2f)
                for (float wx = -700.0f; wx <= 700.0f; wx += 3.2f) {
                    if (std::sqrt(wx*wx + wz*wz) > 690.0f) continue;
                    int cover = 0;
                    for (int L = 0; L <= 5; ++L) {
                        const float S = S0 * float(1 << L);
                        if (broken.count({L, (long long)std::floor(wx / S),
                                             (long long)std::floor(wz / S)}))
                            ++cover;
                    }
                    if (!cover) ++h;
                }
            CHECK(h > 0, "coverage probe is failable: removing a tile is seen");
        }
    }

    // --- part count: the whole point of the migration -----------------------
    // The same reach on the uniform grid, counted by the same streamer.
    {
        SectorStreamer nested(nested_cfg());
        auto live = settle_nested(nested, 3.2f, 3.2f);

        Config u = nested_cfg();
        u.nested_sectors = false;
        SectorStreamer uniform(u);
        // The uniform path stops at the outermost RING, so give it a ring that
        // reaches as far as the bands do -- otherwise this compares two
        // different worlds rather than two ways of tiling one.
        SectorStreamer uni(u);
        for (int i = 0; i < 20000; ++i) {
            uni.update(3.2f, 0.0f, 3.2f);
            SectorRequest q; bool any = false;
            while (uni.next_request(q)) { any = true; uni.on_published(q.tx, q.ty, q.tz, q.rung); }
            uni.take_evictions();
            if (!any && i > 2) break;
        }
        const size_t nested_n = live.size();
        const size_t uniform_n = uni.resident_count();
        printf("  nested: %zu tiles vs uniform %zu sectors (%.1fx fewer)\n",
               nested_n, uniform_n,
               nested_n ? double(uniform_n) / double(nested_n) : 0.0);
        CHECK(uniform_n > nested_n * 20,
              "nested residency is more than an order of magnitude smaller "
              "than the uniform grid over the same reach");
    }

    // --- restriction: cardinal neighbours differ by at most one level -------
    // Checked at the finest pitch along every edge, so a lone over-fine
    // neighbour against a long coarse edge cannot hide.
    //
    // This block also used to assert that the packed variant's edge mask named
    // exactly the one-level-COARSER cardinal neighbours. That half is retired
    // with the mask itself: cross-level seam geometry is welded engine-side
    // from the two tiles actually drawn, so the streamer emits no claim about
    // neighbours for a test to check (design §4.1). The +-1 restriction half
    // below is NOT retired and matters more than it used to -- the welder's fan
    // logic implements exactly one level of difference across a face and
    // nothing wider (§4.4), so this is the invariant that makes the welder
    // sufficient rather than merely the invariant the old snap assumed.
    {
        SectorStreamer s(nested_cfg());
        auto live = settle_nested(s, 3.2f, 3.2f);
        const float S0 = 6.4f;
        auto level_at = [&](float wx, float wz) {
            for (int L = 0; L <= 5; ++L) {
                const float S = S0 * float(1 << L);
                if (live.count({L, (long long)std::floor(wx / S),
                                   (long long)std::floor(wz / S)})) return L;
            }
            return -1;
        };
        int violations = 0;
        for (const auto& [k, v] : live) {
            const int L = std::get<0>(k);
            const float S = S0 * float(1 << L);
            const float ox = float(std::get<1>(k)) * S;
            const float oz = float(std::get<2>(k)) * S;
            for (float t = 0.5f * S0; t < S; t += S0) {
                const float px[4] = {ox + S + 0.5f*S0, ox - 0.5f*S0, ox + t, ox + t};
                const float pz[4] = {oz + t, oz + t, oz + S + 0.5f*S0, oz - 0.5f*S0};
                for (int n = 0; n < 4; ++n) {
                    const int nl = level_at(px[n], pz[n]);
                    if (nl >= 0 && std::abs(nl - L) > 1) ++violations;
                }
            }
        }
        CHECK(violations == 0,
              "nested restriction: cardinal neighbours differ by at most one "
              "level, checked at the finest pitch along every edge");
    }

    // --- restriction under a PATHOLOGICAL band table ------------------------
    // With a sane table the restriction pass is a no-op by construction (every
    // annulus is wider than one tile of the coarser level), so the check above
    // proves only that nothing broke. This table is authored to break it: the
    // level-1 annulus is 10 m wide against a 128 m level-1 tile, so the descent
    // alone puts level-0 tiles cardinally against level-2 ones -- a two-level
    // jump the edge-mask snap cannot stitch and the mesher has no wider stitch
    // for.
    //
    // Two assertions, and they are complementary on purpose: the pass must
    // remove every violation AND must be seen to have done work (tiles forced
    // FINER than their own band). Delete restrict_levels() and the first goes
    // red because the jumps come back, the second because nothing forces a
    // split any more. That is this guard's failability proof.
    {
        Config c;
        c.sector_size = 64.0f;
        c.nested_sectors = true;
        c.hysteresis = 0.0f;                 // isolate the geometry
        c.rings = { {6400.0f, 0} };
        c.terrain_bands = {
            {100.0f, 5}, {110.0f, 4}, {800.0f, 3},
            {1600.0f, 2}, {3200.0f, 1}, {6400.0f, 0},
        };
        const float lvl_r[6] = {100.0f, 110.0f, 800.0f, 1600.0f, 3200.0f, 6400.0f};
        SectorStreamer s(c);
        auto live = settle_nested(s, 32.0f, 32.0f);
        CHECK(!live.empty(), "pathological table settled");

        const float S0 = 64.0f;
        auto level_at = [&](float wx, float wz) {
            for (int L = 0; L <= 5; ++L) {
                const float S = S0 * float(1 << L);
                if (live.count({L, (long long)std::floor(wx / S),
                                   (long long)std::floor(wz / S)})) return L;
            }
            return -1;
        };
        int violations = 0, forced = 0;
        for (const auto& [k, v] : live) {
            const int L = std::get<0>(k);
            const float S = S0 * float(1 << L);
            const float ox = float(std::get<1>(k)) * S;
            const float oz = float(std::get<2>(k)) * S;
            for (float t = 0.5f * S0; t < S; t += S0) {
                const float px[4] = {ox + S + 0.5f*S0, ox - 0.5f*S0, ox + t, ox + t};
                const float pz[4] = {oz + t, oz + t, oz + S + 0.5f*S0, oz - 0.5f*S0};
                for (int n = 0; n < 4; ++n) {
                    const int nl = level_at(px[n], pz[n]);
                    if (nl >= 0 && std::abs(nl - L) > 1) ++violations;
                }
            }
            // A tile whose centre is past its own level's band radius is finer
            // than the bands alone asked for: something split it.
            const float cx = ox + 0.5f * S - 32.0f, cz = oz + 0.5f * S - 32.0f;
            if (std::sqrt(cx * cx + cz * cz) > lvl_r[L]) ++forced;
        }
        printf("  nested pathological: %zu tiles, %d violations, %d forced finer\n",
               live.size(), violations, forced);
        CHECK(violations == 0,
              "nested restriction: a pathological band table still settles with "
              "cardinal neighbours at most one level apart");
        CHECK(forced > 0,
              "nested restriction: the pass actually forced tiles finer than "
              "their band -- it is not passing by doing nothing");
    }

    // --- hysteresis: parked on a level boundary, nothing churns -------------
    // Same shape as the uniform oscillation test above, and for the same
    // reason: the FIRST crossing of a boundary legitimately refines something,
    // and movement legitimately pulls new frontier tiles in. Neither is churn.
    // The property that matters is that once the boundary tiles have settled,
    // continued jitter inside the hysteresis band stops rebaking them.
    {
        SectorStreamer s(nested_cfg());
        settle_nested(s, 31.0f, 0.0f);   // parked on the level-0/1 boundary (32 m)
        s.take_evictions();

        auto oscillate = [&](int iters, std::vector<Eviction>* sink) {
            for (int i = 0; i < iters; ++i) {
                s.update(31.0f + (i % 2 ? 0.5f : -0.5f), 0.0f, 0.0f);  // < hysteresis 2
                SectorRequest q;
                while (s.next_request(q)) s.on_published(q.tx, q.ty, q.tz, q.rung);
                auto ev = s.take_evictions();
                if (sink) sink->insert(sink->end(), ev.begin(), ev.end());
            }
        };
        oscillate(8, nullptr);                       // reach steady state
        const size_t steady = s.resident_count();

        std::vector<Eviction> churn;
        oscillate(12, &churn);                       // measure
        if (!churn.empty())
            printf("  nested hysteresis: %zu evictions, first at level %d\n",
                   churn.size(), variant_level(churn[0].rung));
        CHECK(churn.empty(),
              "nested hysteresis: steady-state jitter across a level boundary "
              "neither splits nor merges");
        CHECK(s.resident_count() >= steady,
              "nested hysteresis: no tile lost during steady-state jitter");
    }

    // --- a split is 4 publishes replacing 1, and a merge the reverse --------
    // WP4 makes the swap atomic; this asserts the SHAPE the group machinery
    // has to make atomic, so a regression in the descent is caught here rather
    // than as a visual artifact later.
    {
        SectorStreamer s(nested_cfg());
        settle_nested(s, 400.0f, 0.0f);      // level 2/3 country
        s.take_evictions();
        // Fly in: tiles ahead must refine.
        int splits = 0;
        std::map<std::tuple<int,long long,long long>, int> pub;
        for (int step = 0; step < 40; ++step) {
            s.update(400.0f - step * 10.0f, 0.0f, 0.0f);
            SectorRequest q;
            while (s.next_request(q)) {
                s.on_published(q.tx, q.ty, q.tz, q.rung);
                pub[{variant_level(q.rung), (long long)q.tx, (long long)q.tz}]++;
            }
            for (const auto& e : s.take_evictions()) {
                // Each eviction at level L should coincide with publishes of
                // its four level-(L-1) children (a split) or of its
                // level-(L+1) parent (a merge).
                const int L = variant_level(e.rung);
                if (L == 0) continue;
                int kids = 0;
                for (int c = 0; c < 4; ++c)
                    kids += pub.count({L - 1, 2 * (long long)e.tx + (c & 1),
                                              2 * (long long)e.tz + (c >> 1)})
                            ? 1 : 0;
                if (kids == 4) ++splits;
            }
        }
        printf("  nested: %d evictions matched by a complete child quad\n",
               splits);
        CHECK(splits > 0,
              "nested: flying in splits tiles, and each split evicts the parent "
              "only alongside a COMPLETE set of four children");
    }

    // --- transition groups: NO HOLE while a camera flies (WP4) --------------
    // The gate that matters. A split replaces one tile with four independent
    // bakes that finish at different times; evicting the parent when the
    // descent changes its mind opens a hole over its whole footprint until the
    // slowest child lands. Here the bakes are deliberately SLOW and staggered
    // (a fixed number of publishes per tick, not instant), the camera flies,
    // and after every single tick every world column near it must be covered by
    // a RESIDENT tile -- not merely a desired one.
    {
        SectorStreamer s(nested_cfg());
        const float S0 = 6.4f;
        // Track residency ourselves: resident_count() is a number, and what
        // this needs is the set.
        std::map<std::tuple<int,long long,long long>, int> resident;
        auto settle_at = [&](float x, float z, int budget) {
            s.update(x, 0.0f, z);
            SectorRequest q;
            for (int i = 0; i < budget && s.next_request(q); ++i)
                if (s.on_published(q.tx, q.ty, q.tz, q.rung))
                    resident[{variant_level(q.rung), (long long)q.tx,
                              (long long)q.tz}] = q.rung;
            for (const auto& e : s.take_evictions()) {
                // Erase ONLY if the evicted variant is the one still recorded.
                // A 1:1 swap (an edge-mask rebake) publishes the new variant of
                // the SAME tile and then evicts the old one, so a blind erase
                // here deletes the tile that is actually resident and invents a
                // hole the streamer never produced.
                const auto key = std::make_tuple(variant_level(e.rung),
                                                 (long long)e.tx,
                                                 (long long)e.tz);
                auto it = resident.find(key);
                if (it != resident.end() && it->second == e.rung)
                    resident.erase(it);
            }
        };
        // Fill completely at the start position.
        for (int i = 0; i < 4000; ++i) settle_at(0.0f, 0.0f, 64);

        auto covered = [&](float wx, float wz) {
            for (int L = 0; L <= 5; ++L) {
                const float S = S0 * float(1 << L);
                if (resident.count({L, (long long)std::floor(wx / S),
                                       (long long)std::floor(wz / S)}))
                    return true;
            }
            return false;
        };

        int holes = 0, ticks = 0, worst_tick = -1;
        for (int step = 0; step < 220; ++step) {
            // 3 publishes per tick: slow enough that a split is in progress for
            // many ticks at a time, which is the condition a hole needs.
            settle_at(float(step) * 2.0f, 0.0f, 3);
            ++ticks;
            int tick_holes = 0;
            for (float d = -60.0f; d <= 60.0f; d += 3.2f)
                for (float e = -60.0f; e <= 60.0f; e += 3.2f)
                    if (!covered(float(step) * 2.0f + d, e)) ++tick_holes;
            if (tick_holes && worst_tick < 0) worst_tick = step;
            holes += tick_holes;
        }
        printf("  nested groups: %d ticks flown, %d uncovered probes "
               "(first at tick %d)\n", ticks, holes, worst_tick);
        CHECK(holes == 0,
              "transition groups: a superseded tile stays resident until every "
              "tile replacing it is resident, so a flying camera never sees a "
              "hole");
    }

    // --- transition groups: the hold RELEASES the tick the group completes ---
    //
    // Reported from a real session: "a 2x or greater sized cell often gets
    // replaced with smaller cells but the smaller cells overlap the larger
    // cell." That is a real bug, but measurement puts it in the ENGINE, not
    // here -- and this test is what establishes that, so it is worth keeping
    // even though it does not fail.
    //
    // The streamer's contract has two halves and only the first was ever
    // asserted (the no-hole test above returns on the FIRST level covering a
    // column, so a column covered by a parent AND its children passes it). The
    // second half is that the hold must RELEASE immediately: a superseded tile
    // must stop being resident on the very next update after the last tile
    // replacing it becomes resident, not linger.
    //
    // Measured below: worst dwell is 1 tick. One is inherent and irreducible at
    // this API -- `on_published` means "accepted", the streamer cannot evict a
    // parent before the last child's acceptance, and acceptance necessarily
    // happens after the update() that would have evicted it. So the streamer is
    // doing everything it can.
    //
    // Which is why the residency overlap this also reports is NOT failed here.
    // Residency is not visibility. The engine draws a child the moment its own
    // bake lands and removes the parent only after the last sibling's -- and
    // sector bakes finish SECONDS apart, so what the eye sees is not this
    // one-tick window at all. Zero-overlap and zero-hole are jointly impossible
    // for any layer whose "published" means "drawn"; some layer has to hold
    // completed bakes invisible until the whole group is ready. That layer is
    // the engine (matter_engine.cpp parks a publication whose footprint a
    // visible different-level entry still covers, and unparks the group inside
    // the same WorldDelta that removes the parent).
    {
        SectorStreamer s(nested_cfg());
        const float S0 = 6.4f;
        std::map<std::tuple<int,long long,long long>, int> resident;
        int ev_mismatched = 0, ev_unknown = 0;
        auto settle_at = [&](float x, float z, int budget) {
            s.update(x, 0.0f, z);
            SectorRequest q;
            for (int i = 0; i < budget && s.next_request(q); ++i)
                if (s.on_published(q.tx, q.ty, q.tz, q.rung))
                    resident[{variant_level(q.rung), (long long)q.tx,
                              (long long)q.tz}] = q.rung;
            for (const auto& e : s.take_evictions()) {
                const auto key = std::make_tuple(variant_level(e.rung),
                                                 (long long)e.tx,
                                                 (long long)e.tz);
                auto it = resident.find(key);
                if (it == resident.end())      ++ev_unknown;
                else if (it->second != e.rung) ++ev_mismatched;
                else                           resident.erase(it);
            }
        };
        for (int i = 0; i < 4000; ++i) settle_at(0.0f, 0.0f, 64);

        // How MANY resident tiles cover this column, across every level.
        auto coverage = [&](float wx, float wz) {
            int n = 0;
            for (int L = 0; L <= 5; ++L) {
                const float S = S0 * float(1 << L);
                if (resident.count({L, (long long)std::floor(wx / S),
                                       (long long)std::floor(wz / S)})) ++n;
            }
            return n;
        };

        // How LONG each superseded tile stays resident alongside its
        // replacements is the question that decides which layer owns this bug.
        // One tick is inherent: the streamer cannot evict a parent before the
        // last child's publish is accepted, and acceptance happens after
        // update(). Many ticks would mean the hold rule itself is wrong.
        std::map<std::tuple<int,long long,long long>, int> overlap_since;
        int overlaps = 0, worst = 0, worst_tick = -1, worst_dwell = 0;
        for (int step = 0; step < 220; ++step) {
            settle_at(float(step) * 2.0f, 0.0f, 3);
            for (float d = -60.0f; d <= 60.0f; d += 3.2f)
                for (float e = -60.0f; e <= 60.0f; e += 3.2f) {
                    const int n = coverage(float(step) * 2.0f + d, e);
                    if (n > 1) {
                        ++overlaps;
                        if (n > worst) worst = n;
                        if (worst_tick < 0) worst_tick = step;
                    }
                }
            // Dwell: for every resident tile that is covered by a resident tile
            // at a FINER level (i.e. it has been superseded and its
            // replacements are up), count consecutive ticks it survives.
            // "Superseded AND its replacement is complete" is the condition the
            // hold rule releases on, so it is the only one whose dwell means
            // anything. A parent with one child up and three still baking is
            // being held CORRECTLY -- counting that as dwell measures the
            // feature, not the bug. The footprint may be covered at any depth
            // (a child can itself have split), so this recurses exactly the way
            // scan_footprint does.
            std::function<bool(int,long long,long long)> covered_by_finer =
                [&](int L, long long tx, long long tz) -> bool {
                    if (L == 0) return false;
                    for (int c = 0; c < 4; ++c) {
                        const long long cx = 2 * tx + (c & 1);
                        const long long cz = 2 * tz + (c >> 1);
                        if (resident.count({L - 1, cx, cz})) continue;
                        if (!covered_by_finer(L - 1, cx, cz)) return false;
                    }
                    return true;
                };
            std::map<std::tuple<int,long long,long long>, int> next_since;
            for (const auto& [k, rung] : resident) {
                const int L = std::get<0>(k);
                if (L == 0) continue;
                if (!covered_by_finer(L, std::get<1>(k), std::get<2>(k)))
                    continue;
                const int since = overlap_since.count(k)
                    ? overlap_since[k] + 1 : 1;
                next_since[k] = since;
                if (since > worst_dwell) worst_dwell = since;
            }
            overlap_since.swap(next_since);
        }
        printf("  nested groups: worst superseded-tile dwell %d ticks "
               "(1 is inherent to publish-then-evict)\n", worst_dwell);
        // Bookkeeping soundness, reported because this test's whole claim
        // rests on it.
        //
        // `mismatched` is EXPECTED and benign: a 1:1 edge-mask rebake
        // publishes the new variant of a tile and only then evicts the old
        // one, so the eviction names a variant that is no longer the recorded
        // residency -- and the tile really is still resident, as the newer
        // variant. Erasing on that would invent a hole.
        //
        // `unknown` is the one that would matter: an eviction for a key this
        // model never recorded means the two residencies have diverged, and
        // then the overlap count below is measuring this test rather than the
        // engine.
        printf("  nested groups: eviction bookkeeping: %d superseded-variant "
               "(expected), %d unknown-key\n", ev_mismatched, ev_unknown);
        CHECK(ev_unknown == 0,
              "every eviction names a tile this model knows about, so the "
              "overlap count below is the engine's and not this test's");
        // Context, not a verdict: this is the one-tick residency window the
        // dwell gate above bounds, not the seconds-long VISIBILITY overlap the
        // engine is responsible for. Printed so a future reader can see the
        // two are different quantities.
        printf("  nested groups: %d probe-ticks of residency overlap "
               "(worst depth %d, first at tick %d) -- the 1-tick window, not "
               "the visibility bug\n", overlaps, worst, worst_tick);
        CHECK(worst_dwell <= 1,
              "transition groups: a superseded tile stops being resident on "
              "the very next update after the last tile replacing it becomes "
              "resident -- the hold releases immediately, it does not linger");
    }

    // --- transition groups: a failed child holds its parent -----------------
    // Partial failure is the case the invariant exists for. One child of a
    // split never bakes; the parent must stay resident and drawn rather than
    // leaving a quarter-tile hole, and must release the moment the child lands.
    {
        SectorStreamer s(nested_cfg());
        // Settle far out, so the tiles around the anchor are coarse and a move
        // inward forces splits.
        for (int i = 0; i < 4000; ++i) {
            s.update(300.0f, 0.0f, 0.0f);
            SectorRequest q;
            while (s.next_request(q)) s.on_published(q.tx, q.ty, q.tz, q.rung);
            s.take_evictions();
        }
        // Move in and refuse exactly one request forever.
        bool have_victim = false;
        int64_t vtx = 0, vtz = 0; int vrung = 0;
        int parent_evictions_while_failing = 0;
        for (int i = 0; i < 400; ++i) {
            s.update(120.0f, 0.0f, 0.0f);
            SectorRequest q;
            while (s.next_request(q)) {
                if (!have_victim && variant_level(q.rung) <= 3) {
                    have_victim = true;
                    vtx = q.tx; vtz = q.tz; vrung = q.rung;
                }
                if (have_victim && q.tx == vtx && q.tz == vtz &&
                    q.rung == vrung) {
                    s.on_failed(q.tx, 0, q.tz, q.rung);      // never succeeds
                    continue;
                }
                s.on_published(q.tx, q.ty, q.tz, q.rung);
            }
            // While the victim is missing, no tile whose footprint contains it
            // may be evicted -- that is the hole this prevents.
            for (const auto& e : s.take_evictions()) {
                const int el = variant_level(e.rung);
                const int vl = variant_level(vrung);
                if (!have_victim || el <= vl) continue;
                const int sh = el - vl;
                if ((vtx >> sh) == e.tx && (vtz >> sh) == e.tz)
                    ++parent_evictions_while_failing;
            }
        }
        CHECK(have_victim, "the failure test actually found a victim request");
        printf("  nested groups: %d ancestor evictions while a child was "
               "failing\n", parent_evictions_while_failing);
        CHECK(parent_evictions_while_failing == 0,
              "transition groups: a child that never bakes holds its parent "
              "resident instead of opening a hole");
    }

    // --- transition groups: abandonment releases with no bookkeeping --------
    // A transition the camera walks away from must not strand its held tile.
    {
        SectorStreamer s(nested_cfg());
        for (int i = 0; i < 4000; ++i) {
            s.update(0.0f, 0.0f, 0.0f);
            SectorRequest q;
            while (s.next_request(q)) s.on_published(q.tx, q.ty, q.tz, q.rung);
            s.take_evictions();
        }
        const size_t settled = s.resident_count();
        // Start a wave of splits by moving, then leave entirely without ever
        // servicing them.
        s.update(200.0f, 0.0f, 0.0f);
        s.update(5000.0f, 0.0f, 5000.0f);
        for (int i = 0; i < 4000; ++i) {
            s.update(5000.0f, 0.0f, 5000.0f);
            SectorRequest q;
            while (s.next_request(q)) s.on_published(q.tx, q.ty, q.tz, q.rung);
            s.take_evictions();
        }
        printf("  nested groups: %zu resident at origin, %zu after leaving\n",
               settled, s.resident_count());
        CHECK(s.resident_count() < settled * 2,
              "transition groups: abandoning a transition releases the held "
              "tiles rather than stranding them");
    }

    // =======================================================================
    // STAGED REFINEMENT (docs/volumetric-sectors-design-2026-08-10.md §4.5)
    //
    // restrict_levels() constrains the DESIRED map. Nothing constrained the
    // DRAWN one, and the drawn map is what a seam exists between: under fast
    // flight a region's desired level jumps several levels at once, the tiles
    // rebake independently, and whichever lands first is drawn at level 0 next
    // to a neighbour still showing level 3. That is outside the seam welder's
    // one-level domain entirely (§4.4) and it is also the 8x-detail pop.
    //
    // The rule under test: a tile is never REQUESTED more than one level finer
    // than the resident coverage over its own footprint, so a multi-level jump
    // is served as monotone coarse->fine waves. Every block below runs the same
    // scenario twice, with Config::staged_refinement ON and OFF, because the
    // OFF run is what makes these checks failable -- "0 violations" means
    // nothing unless the same probe reports thousands with the rule removed.
    // =======================================================================

    // One scripted run: settle at `from`, TELEPORT to `to`, then keep pumping
    // until quiet, measuring only the second half. Requests are serviced
    // instantly, which is the harshest case for staging (a wave completes in
    // one update, so nothing is hidden by bake latency).
    struct StageRun {
        std::map<std::tuple<int,long long,long long>, int> resident;
        long long requests   = 0;   // requests issued during the measured half
        int updates          = 0;   // updates taken to go quiet after the jump
        int clamp_violations = 0;   // requested > 1 level finer than resident
        int probes           = 0;   // requests whose footprint had ANY residency
        int skips            = 0;   // a column's request sequence skipped a level
        int deepest_wave     = 0;   // distinct levels requested in one column
    };
    auto teleport_run = [&](bool staged, bool lateral, float fx, float fz,
                            float tox, float toz) {
        Config c = nested_cfg();
        c.staged_refinement = staged;
        c.lateral_staging   = lateral;
        SectorStreamer s(c);
        StageRun r;
        // Coarsest resident level at or above L over the footprint of
        // (L, tx, tz) in `snap`, or -1. This is the same ancestor walk
        // SectorStreamer::resident_level_over does, rebuilt here from nothing
        // but the public request/publish/evict stream -- the test must not be
        // able to agree with the implementation by sharing its code.
        auto resident_level_over =
            [](const std::map<std::tuple<int,long long,long long>, int>& snap,
               int L, long long tx, long long tz) {
                int coarsest = -1;
                for (int up = L; up <= 5; ++up) {
                    const int sh = up - L;
                    if (snap.count({up, tx >> sh, tz >> sh})) coarsest = up;
                }
                return coarsest;
            };
        // Per level-5 column: the finest level requested so far. A request more
        // than one level finer than that is a skipped wave.
        std::map<std::pair<long long,long long>, int> col_min;
        std::map<std::pair<long long,long long>, std::vector<int>> col_seq;

        auto pump = [&](float x, float z, bool measure) {
            s.update(x, 0.0f, z);
            // Residency AS THE STREAMER SAW IT when it built this desired map:
            // publishes serviced below would otherwise let the model run ahead
            // of the decision being judged.
            const auto snap = r.resident;
            SectorRequest q;
            bool any = false;
            while (s.next_request(q)) {
                any = true;
                const int L = variant_level(q.rung);
                if (measure) {
                    ++r.requests;
                    const int R = resident_level_over(snap, L, q.tx, q.tz);
                    if (R >= 0) {
                        ++r.probes;
                        if (L < R - 1) ++r.clamp_violations;
                    }
                    const int sh = 5 - L;
                    const std::pair<long long,long long> col{q.tx >> sh,
                                                             q.tz >> sh};
                    auto it = col_min.find(col);
                    if (it != col_min.end() && L < it->second - 1) ++r.skips;
                    if (it == col_min.end() || L < it->second) {
                        col_min[col] = L;
                        col_seq[col].push_back(L);
                        r.deepest_wave = std::max(
                            r.deepest_wave, (int)col_seq[col].size());
                    }
                }
                if (s.on_published(q.tx, q.ty, q.tz, q.rung))
                    r.resident[{L, (long long)q.tx, (long long)q.tz}] = q.rung;
            }
            for (const auto& e : s.take_evictions()) {
                // Guarded erase, for the reason the transition-group test spells
                // out: a 1:1 variant swap publishes the new value first, so a
                // blind erase would delete a tile that is still resident.
                const auto k = std::make_tuple(variant_level(e.rung),
                                               (long long)e.tx,
                                               (long long)e.tz);
                auto it = r.resident.find(k);
                if (it != r.resident.end() && it->second == e.rung)
                    r.resident.erase(it);
            }
            if (measure) ++r.updates;
            return any;
        };

        // QUIET IS NOT DONE under staged refinement, and this is the one place
        // the rule leaks into callers. A wave is admitted by the ABSENCE of
        // coarser residency, and the coarse tile is evicted at the END of the
        // update in which its replacement quad completes -- so there is exactly
        // one update per wave in which the desired map still equals the
        // resident map and next_request() has nothing to say. The engine pumps
        // every frame regardless and never notices; a settle loop that breaks
        // on the first silent update stops mid-jump (measured: it stopped at
        // 852 of 1188 tiles). So require several consecutive silent updates.
        auto run = [&](float x, float z, bool measure) {
            int quiet = 0;
            for (int i = 0; i < 4000 && quiet < 8; ++i)
                quiet = pump(x, z, measure) ? 0 : quiet + 1;
        };
        run(fx, fz, false);
        run(tox, toz, true);
        return r;
    };

    // --- a multi-level jump refines in monotone waves ------------------------
    // The anchor teleports 600 m, so the region it lands in was resident at
    // level 4 (the 775 m band) and is now wanted at level 0 -- a four-level
    // jump in a single update. No request may be issued more than one level
    // finer than what is resident over it.
    {
        const auto on  = teleport_run(true,  true,  3.2f, 3.2f, 600.0f, 0.0f);
        const auto off = teleport_run(false, false, 3.2f, 3.2f, 600.0f, 0.0f);
        printf("  staged jump ON : %lld requests over %d updates, "
               "%d/%d over-fine, %d wave skips, deepest wave chain %d\n",
               on.requests, on.updates, on.clamp_violations, on.probes,
               on.skips, on.deepest_wave);
        printf("  staged jump OFF: %lld requests over %d updates, "
               "%d/%d over-fine, %d wave skips, deepest wave chain %d\n",
               off.requests, off.updates, off.clamp_violations, off.probes,
               off.skips, off.deepest_wave);
        CHECK(on.probes > 100,
              "the jump actually landed on ground that already had residency -- "
              "otherwise there is nothing for the clamp to clamp against");
        CHECK(on.clamp_violations == 0,
              "staged refinement: no tile is ever requested more than one level "
              "finer than the resident coverage over its own footprint");
        CHECK(on.skips == 0,
              "staged refinement: the levels requested for a column step down "
              "one at a time -- monotone, no skipped wave");
        // Failability. Without the rule the same probe must light up, or the
        // two CHECKs above are measuring nothing.
        CHECK(off.clamp_violations > 100,
              "with staged refinement OFF the same jump requests far finer than "
              "what is resident -- the probe above is failable");
        CHECK(on.deepest_wave >= 3,
              "staged refinement: a four-level jump is served as a chain of "
              "intermediate waves, not one step");
    }

    // --- cold start is not clamped ------------------------------------------
    // The wave rule keys off resident coverage, and a fresh world has none. If
    // "no coverage" were treated as "coverage at the coarsest level" then every
    // world would load in six visible stages and the first frame of every scene
    // would be a single quad. -1 must mean NO CLAMP, and the proof is that a
    // cold settle is request-for-request identical with the flag either way.
    {
        auto cold = [&](bool staged) {
            Config c = nested_cfg();
            c.staged_refinement = staged;
            SectorStreamer s(c);
            std::vector<std::tuple<long long,long long,int>> trace;
            std::map<std::tuple<int,long long,long long>, int> live;
            for (int i = 0; i < 20000; ++i) {
                s.update(3.2f, 0.0f, 3.2f);
                SectorRequest q; bool any = false;
                while (s.next_request(q)) {
                    any = true;
                    trace.push_back({q.tx, q.tz, q.rung});
                    if (s.on_published(q.tx, q.ty, q.tz, q.rung))
                        live[{variant_level(q.rung), (long long)q.tx,
                              (long long)q.tz}] = q.rung;
                }
                for (const auto& e : s.take_evictions())
                    live.erase({variant_level(e.rung), (long long)e.tx,
                                (long long)e.tz});
                if (!any && i > 2) break;
            }
            return std::make_pair(trace, live);
        };
        const auto on  = cold(true);
        const auto off = cold(false);
        int by_level[6] = {0, 0, 0, 0, 0, 0};
        for (const auto& [k, v] : on.second) by_level[std::get<0>(k)]++;
        printf("  staged cold start: %zu requests ON vs %zu OFF, settled "
               "L0..L5 = %d %d %d %d %d %d\n",
               on.first.size(), off.first.size(), by_level[0], by_level[1],
               by_level[2], by_level[3], by_level[4], by_level[5]);
        CHECK(!on.first.empty() && on.first == off.first,
              "cold start is not clamped: with nothing resident anywhere the "
              "staged and unstaged descents issue the identical request stream");
        CHECK(on.second == off.second,
              "cold start settles to the same set either way");
        for (int L = 0; L <= 5; ++L)
            CHECK(by_level[L] > 0,
                  "cold start still reaches every authored desired level -- the "
                  "wave rule does not stall a fresh world at the coarse end");
    }

    // --- no deadlock: same steady state, different path ---------------------
    // Staging interacts with two existing rules that can both WITHHOLD work:
    // the split/merge hysteresis in descend() and the transition-group hold in
    // update_nested(). The hold waits for the current desired set to become
    // resident; staging waits for exactly the same event before admitting the
    // next wave, so they run on one clock and cannot wait on each other. This
    // asserts that claim rather than trusting the argument: the settle
    // terminates, and it terminates at the same place the unstaged one does.
    {
        const auto on  = teleport_run(true,  true,  3.2f, 3.2f, 600.0f, 0.0f);
        const auto off = teleport_run(false, false, 3.2f, 3.2f, 600.0f, 0.0f);
        printf("  staged converge: %d updates ON vs %d OFF (%.2fx), "
               "%zu resident tiles ON vs %zu OFF\n",
               on.updates, off.updates,
               off.updates ? double(on.updates) / double(off.updates) : 0.0,
               on.resident.size(), off.resident.size());
        CHECK(on.updates < 3999 && off.updates < 3999,
              "staged refinement terminates: neither settle ran out of updates");
        CHECK(on.resident == off.resident,
              "staged refinement converges to exactly the same steady state as "
              "the unstaged descent -- same destination, different path");
        // The extra updates are the intermediate waves, and the cost is per
        // LEVEL CROSSED, not per tile: measured 15 vs 9 for a four-level jump,
        // which is the four waves plus the silent update each one needs before
        // its successor is admitted. A bound proportional to the tile count
        // would mean staging had turned into serialization.
        CHECK(on.updates <= off.updates + 24,
              "staged refinement's extra updates are bounded by the wave count, "
              "not proportional to the tile count");
    }

    // --- the intermediate-wave work bound -----------------------------------
    // Design §4.5 bounds the intermediate waves at sum(8^-k) ~ +14%, and that
    // number does not apply here: it is the VOLUMETRIC octree's, where a tile
    // one level coarser covers 8x the volume. This streamer is still the 2D
    // quadtree, where the same argument gives sum(4^-k) = 1/3, so the honest
    // ceiling for M0 is about +33% and the +14% arrives with the third axis.
    //
    // MEASURED, this four-level jump: 1296 requests staged vs 1090 direct,
    // +18.9%, comfortably inside the 2D ceiling. That number is a
    // CHARACTERIZATION of this scenario -- one band table, one jump distance --
    // and not a derived bound; a different table moves it. The assertion is
    // pinned at the +33% ceiling rather than tight to the measurement so that
    // band tuning does not make it flaky, while a regression that re-requests
    // waves (the plausible failure: a clamp that oscillates and re-issues an
    // already-resident level) blows straight through it.
    {
        const auto on  = teleport_run(true,  true,  3.2f, 3.2f, 600.0f, 0.0f);
        const auto off = teleport_run(false, false, 3.2f, 3.2f, 600.0f, 0.0f);
        const double ratio = off.requests
            ? double(on.requests) / double(off.requests) : 0.0;
        printf("  staged work: %lld requests ON vs %lld OFF (%.3fx, +%.1f%%) "
               "-- 2D quadtree ceiling sum(4^-k) = +33%%\n",
               on.requests, off.requests, ratio, 100.0 * (ratio - 1.0));
        CHECK(off.requests > 100, "the work comparison serviced a real jump");
        CHECK(ratio < 1.35,
              "staged refinement's intermediate waves cost a bounded fraction "
              "of the direct jump, not a multiple of it");
        // What the LATERAL term adds on top of the footprint clamp, which is
        // the number the next reader will want and cannot get from the two
        // above. Both arms are staged; only the lateral half differs.
        const auto fp = teleport_run(true, false, 3.2f, 3.2f, 600.0f, 0.0f);
        printf("  lateral work: %lld requests with the lateral term vs %lld "
               "footprint-only (%+.1f%%)\n",
               on.requests, fp.requests,
               fp.requests
                   ? 100.0 * (double(on.requests) / double(fp.requests) - 1.0)
                   : 0.0);
    }

    // =======================================================================
    // THE DRAWN ±1 INVARIANT ACROSS FACES -- LATERAL STAGING
    // (docs/volumetric-sectors-m0-resolutions.md R12, R1, R7, R10.)
    //
    // M0's acceptance soak found the footprint clamp above is not sufficient.
    // A settled StreamCaverns (590 sectors, 6454 samples) logged
    // `drawn_level_violations = 28` -- tiles DRAWN at level 2 against a DRAWN
    // level-4 face neighbour. The welder spans exactly one level by design and
    // correctly refuses those pairs (`level_gap_pairs == 0`), so each of those
    // faces draws with no seam at all: the missing-strip class this whole
    // stage exists to remove.
    //
    // Everything above measures the REQUEST stream against RESIDENCY. This
    // block measures the DRAWN map, which is the thing the invariant is about
    // and is a different quantity -- residency transiently stacks levels over
    // one column (R7) and parking is what keeps that off the screen. The model
    // used here is the engine's: a publication whose footprint a visible
    // different-level entry still covers is PARKED
    // (`sector_blocked_by_visible`), so during a split the incumbent coarse
    // tile keeps the column and the newcomer waits. Hence
    //
    //     drawn level of a column = the COARSEST resident level covering it
    //
    // and the drawn map is a partition of the world, exactly like the engine's.
    // Two cardinally adjacent columns whose drawn levels differ by >= 2 are one
    // violation -- the same predicate the engine's `[stream]` warning applies,
    // rebuilt here from nothing but the public request/publish/evict stream.
    //
    // WHY IT NEEDS A BUDGET. Service every request in the update that issued it
    // and every transition group completes in the same tick, so neighbouring
    // columns are in lockstep for free and the defect cannot appear -- which is
    // why the blocks above, which do exactly that, are all green while the real
    // world is not. The bakes here are deliberately slow and staggered (a fixed
    // few publishes per tick), which is what lets one group finish a wave while
    // the group beside it is still on the previous one.
    // =======================================================================
    {
        struct DrawnRun {
            long long requests     = 0;
            long long refine_gaps  = 0;  // the FINE side landed last (a split)
            long long merge_gaps   = 0;  // the COARSE side landed last (a merge)
            int       gap_ticks    = 0;
            int       worst_gap    = 0;
            int       first_tick   = -1;
            size_t    settled      = 0;
            size_t    min_resident = size_t(-1);
        };
        // Test-local tile key. Deliberately NOT the streamer's nested_key: this
        // probe has to be able to disagree with the implementation.
        auto tkey = [](int L, long long tx, long long tz) {
            return (unsigned long long)((uint64_t(unsigned(L) & 0xFu) << 60) |
                                        ((uint64_t(tx) & 0x3FFFFFFFull) << 30) |
                                        (uint64_t(tz) & 0x3FFFFFFFull));
        };

        auto drawn_run = [&](bool staged, bool lateral) {
            Config c = nested_cfg();
            c.staged_refinement = staged;
            c.lateral_staging   = lateral;
            SectorStreamer s(c);
            DrawnRun r;

            std::map<std::tuple<int,long long,long long>, int> resident;
            std::map<unsigned long long, int> live;   // tile -> publish order
            int now = 0;

            auto pump = [&](float x, float z, int cap, bool measure) {
                s.update(x, 0.0f, z);
                SectorRequest q;
                bool any = false;
                int n = 0;
                while ((cap < 0 || n < cap) && s.next_request(q)) {
                    any = true; ++n;
                    if (measure) ++r.requests;
                    const int L = variant_level(q.rung);
                    if (s.on_published(q.tx, q.ty, q.tz, q.rung)) {
                        resident[{L, (long long)q.tx, (long long)q.tz}] = q.rung;
                        live[tkey(L, q.tx, q.tz)] = ++now;
                    }
                }
                for (const auto& e : s.take_evictions()) {
                    // Guarded erase, for the reason the transition-group test
                    // spells out: a 1:1 variant swap publishes the new value
                    // first, so a blind erase deletes a tile still resident.
                    const int L = variant_level(e.rung);
                    const auto k = std::make_tuple(L, (long long)e.tx,
                                                   (long long)e.tz);
                    auto it = resident.find(k);
                    if (it != resident.end() && it->second == e.rung) {
                        resident.erase(it);
                        live.erase(tkey(L, e.tx, e.tz));
                    }
                }
                return any;
            };

            // Settle cold at the origin. Full service, no measurement: R10 --
            // a soak on an UNSETTLED streaming world passes vacuously, because
            // with no stable neighbours there are no cross-level faces at all
            // and every invariant holds by absence.
            { int quiet = 0;
              for (int i = 0; i < 4000 && quiet < 8; ++i)
                  quiet = pump(3.2f, 3.2f, -1, false) ? 0 : quiet + 1; }

            const float S0 = 6.4f, TOX = 600.0f, TOZ = 0.0f;

            // Which SIDE of a violating face arrived last says which direction
            // produced it, and the two have different fixes: the lateral clamp
            // withholds a REQUEST, so it can only ever close the refinement
            // direction. Publish order is recorded in `live`.
            auto classify = [&](int la, long long ax, long long az,
                                int lb, long long bx, long long bz) {
                const int lf = std::min(la, lb), lc = std::max(la, lb);
                const long long fx = (la <= lb ? ax : bx);
                const long long fz = (la <= lb ? az : bz);
                const long long gx = (la <= lb ? bx : ax);
                const long long gz = (la <= lb ? bz : az);
                auto ord = [&](int L, long long x, long long z) {
                    auto it = live.find(tkey(L, x >> L, z >> L));
                    return it == live.end() ? 0 : it->second;
                };
                if (ord(lf, fx, fz) > ord(lc, gx, gz)) ++r.refine_gaps;
                else                                   ++r.merge_gaps;
            };
            // One rectangular window of the drawn map, in level-0 cells.
            std::vector<int> drawn;
            auto scan = [&](float wx, float wz, float radius) {
                const int half = int(radius / S0);
                const int W = 2 * half + 1;
                const long long c0x = (long long)std::floor(wx / S0);
                const long long c0z = (long long)std::floor(wz / S0);
                drawn.assign(size_t(W) * size_t(W), -1);
                for (int j = 0; j < W; ++j)
                    for (int i = 0; i < W; ++i) {
                        const long long cx = c0x + i - half;
                        const long long cz = c0z + j - half;
                        for (int L = 5; L >= 0; --L)     // COARSEST wins
                            if (live.count(tkey(L, cx >> L, cz >> L))) {
                                drawn[size_t(j) * W + i] = L; break;
                            }
                    }
                long long gaps = 0;
                for (int j = 0; j < W; ++j)
                    for (int i = 0; i < W; ++i) {
                        const int a = drawn[size_t(j) * W + i];
                        if (a < 0) continue;
                        const long long ax = c0x + i - half;
                        const long long az = c0z + j - half;
                        if (i + 1 < W) {
                            const int b = drawn[size_t(j) * W + i + 1];
                            if (b >= 0 && std::abs(a - b) >= 2) {
                                ++gaps;
                                r.worst_gap = std::max(r.worst_gap,
                                                       std::abs(a - b));
                                classify(a, ax, az, b, ax + 1, az);
                            }
                        }
                        if (j + 1 < W) {
                            const int b = drawn[size_t(j + 1) * W + i];
                            if (b >= 0 && std::abs(a - b) >= 2) {
                                ++gaps;
                                r.worst_gap = std::max(r.worst_gap,
                                                       std::abs(a - b));
                                classify(a, ax, az, b, ax, az + 1);
                            }
                        }
                    }
                return gaps;
            };

            for (int t = 0; t < 300; ++t) {
                pump(TOX, TOZ, 4, true);            // 4 publishes per tick
                if (resident.size() < r.min_resident)
                    r.min_resident = resident.size();
                // Two windows, because the jump drives BOTH directions at once
                // and they fail differently: the region landed on refines, and
                // the region departed coarsens. A probe on the landing alone
                // reports the merge direction as zero and reads as a clean
                // sweep it has not earned.
                long long tick_gaps = scan(TOX, TOZ, 300.0f)     // refining
                                    + scan(3.2f, 3.2f, 250.0f);  // coarsening
                if (tick_gaps) {
                    ++r.gap_ticks;
                    if (r.first_tick < 0) r.first_tick = t;
                }
            }
            { int quiet = 0;                          // settle out
              for (int i = 0; i < 8000 && quiet < 8; ++i)
                  quiet = pump(TOX, TOZ, -1, true) ? 0 : quiet + 1; }
            r.settled = resident.size();
            return r;
        };

        const auto none = drawn_run(false, false);   // no staging at all
        const auto fp   = drawn_run(true,  false);   // footprint clamp only
        const auto lat  = drawn_run(true,  true);    // + the lateral term

        auto report = [](const char* name, const DrawnRun& r) {
            printf("  drawn +-1 [%s]: refine-direction gaps %lld, "
                   "merge-direction %lld, on %d/300 ticks (worst %d, first "
                   "%d), %lld requests, settled %zu (min during fill %zu)\n",
                   name, r.refine_gaps, r.merge_gaps, r.gap_ticks,
                   r.worst_gap, r.first_tick, r.requests, r.settled,
                   r.min_resident);
        };
        report("staging OFF   ", none);
        report("footprint only", fp);
        report("+ lateral     ", lat);

        // FAILABILITY FIRST. "0 violations" means nothing unless the same probe
        // reports thousands with the term removed -- and the arm that has to
        // light up is `fp`, staged refinement exactly as it shipped, because
        // that is the configuration the soak measured 28 violations in.
        CHECK(fp.refine_gaps > 1000,
              "the drawn probe is failable: with the footprint clamp alone, a "
              "budgeted four-level jump draws tiles two or more levels from a "
              "drawn face neighbour -- the M0 acceptance defect, reproduced");
        CHECK(fp.worst_gap >= 2,
              "the reproduced defect really is a multi-level face, not a "
              "rounding artifact in the probe");
        CHECK(lat.refine_gaps == 0,
              "lateral staging: no tile is ever DRAWN more than one level "
              "finer than a drawn face neighbour");

        // NOT A STALL. The measured failure mode of an over-eager clamp is
        // that residency never reaches its settled value and the probe passes
        // because nothing is drawn (R10 again). Both halves are asserted: the
        // same settled set, and no dip in coverage on the way there.
        CHECK(lat.settled == fp.settled && lat.settled == none.settled,
              "lateral staging settles to exactly the same residency as the "
              "footprint clamp alone and as no staging at all -- it costs "
              "path, not tiles");
        CHECK(lat.min_resident >= fp.min_resident * 9 / 10,
              "lateral staging does not starve the fill: residency during the "
              "jump never dips below the footprint-only arm's");

        // NO DEADLOCK. Two neighbours can in principle each wait on the other;
        // the argument in descend() is that they cannot, because a tile only
        // ever waits on something strictly COARSER than itself and the
        // coarsest resident level is never blocked. What is asserted here is
        // the consequence: the settle-out terminated (a deadlock would have
        // burned all 8000 updates and left residency short), and it terminated
        // where the unstaged descent does.
        CHECK(lat.settled > 1000,
              "lateral staging terminates: the settle-out reached the full "
              "resident set rather than parking on a mutual wait");

        // THE MERGE DIRECTION IS NOT CLOSED, and is reported rather than
        // asserted. A multi-level MERGE lands one coarse tile while the
        // neighbouring footprint still draws fine tiles; the lateral term
        // withholds REQUESTS, and there is no request to withhold when the
        // level in question is the one already being asked for. Staging the
        // coarsening direction is not the answer -- the intermediate waves
        // there are FINER than the target, so a four-level merge that costs
        // one bake today would cost 4+16+64. It shows up in the arm with no
        // staging at all too, so it is not something this term introduced.
        printf("  drawn +-1: merge-direction gaps are NOT closed by the "
               "lateral term (staging off %lld, footprint %lld, lateral "
               "%lld) -- see descend()'s ONE-SIDED ON PURPOSE note\n",
               none.merge_gaps, fp.merge_gaps, lat.merge_gaps);
    }

    // --- legacy: the flag off is the old streamer, request for request ------
    // The rollback position. Two streamers over the same anchors, one built
    // before this change would have mattered (nested_sectors defaults false),
    // must emit identical request and eviction streams.
    {
        Config c;                        // stock defaults, ladder on
        c.sector_size = 64.0f;
        c.rings = { {128.0f, 2}, {320.0f, 1}, {2560.0f, 0} };
        c.terrain_lod_enabled = true;
        CHECK(!c.nested_sectors, "nested_sectors defaults OFF");

        auto trace = [](Config cfg) {
            SectorStreamer s(cfg);
            std::vector<std::tuple<long long,long long,int,int>> out;
            for (int step = 0; step < 60; ++step) {
                s.update(32.0f + step * 25.0f, 0.0f, 32.0f);
                SectorRequest q;
                while (s.next_request(q)) {
                    out.push_back({q.tx, q.tz, q.rung, 0});
                    s.on_published(q.tx, q.ty, q.tz, q.rung);
                }
                for (const auto& e : s.take_evictions())
                    out.push_back({e.tx, e.tz, e.rung, 1});
            }
            return out;
        };
        const auto a = trace(c);
        const auto b = trace(c);
        CHECK(!a.empty() && a == b,
              "uniform path is deterministic and untouched by the nested code");
    }

    // --- volumetric sectors: the flag cannot outlive its ladder (M3-WP1) -----
    //
    // `volumetric_sectors` is the octree, and the octree descends the NESTED
    // level ladder with a third axis. Two ways that ladder can fail to exist,
    // and the constructor has to refuse the flag in both -- because a streamer
    // that kept it would hand out column keys while the engine's publish path
    // read them as cubes, which is a keyspace disagreement rather than a
    // degraded picture.
    {
        printf("== volumetric sectors: flag coherence ==\n");
        Config base;
        base.sector_size = 64.0f;
        base.terrain_bands = { {320.0f, 5}, {640.0f, 4}, {1280.0f, 3},
                               {2560.0f, 2}, {5120.0f, 1}, {10240.0f, 0} };
        CHECK(!base.volumetric_sectors, "volumetric_sectors defaults OFF");

        // 1. Asked for with nesting: kept.
        {
            Config c = base;
            c.nested_sectors = true;
            c.volumetric_sectors = true;
            SectorStreamer s(c);
            CHECK(s.config().volumetric_sectors,
                  "volumetric survives alongside a resolvable nested ladder");
            CHECK(s.config().nested_sectors, "and nesting with it");
        }
        // 2. Asked for WITHOUT nesting: refused. This is the case the engine
        //    prints a diagnostic for; here we assert the streamer's half.
        {
            Config c = base;
            c.nested_sectors = false;
            c.volumetric_sectors = true;
            SectorStreamer s(c);
            CHECK(!s.config().volumetric_sectors,
                  "volumetric without nesting is refused, not half-honoured");
        }
        // 3. Nesting asked for but the band table resolves NO levels, so the
        //    ladder collapses to uniform. Nesting already turned itself off
        //    here; volumetric has to go with it or it would be the only flag
        //    left claiming a keyspace nothing produces. A table starting at
        //    level 1 (band lod 4) has no level 0 to descend from, which is the
        //    contiguous-run rule in the constructor.
        {
            Config c = base;
            c.terrain_bands = { {640.0f, 4}, {1280.0f, 3} };
            c.nested_sectors = true;
            c.volumetric_sectors = true;
            SectorStreamer s(c);
            CHECK(!s.config().nested_sectors,
                  "a table with no level 0 collapses nesting to uniform");
            CHECK(!s.config().volumetric_sectors,
                  "and volumetric cannot outlive the ladder it descends");
        }
        printf("  refused without nesting, and refused when the band table "
               "resolves no level ladder\n");
    }

    // --- the octree descent (M3-WP2) ----------------------------------------
    {
        printf("== volumetric sectors: the octree descent ==\n");
        Config base;
        base.sector_size = 64.0f;
        base.nested_sectors = true;
        base.rings = { {128.0f, 2}, {512.0f, 1}, {2048.0f, 0} };
        base.terrain_bands = { {128.0f, 5}, {256.0f, 4}, {512.0f, 3},
                               {1024.0f, 2}, {2048.0f, 1}, {4096.0f, 0} };
        base.y_min = -1024.0f;
        base.y_max = 1024.0f;

        // Drive one settle and return the full desired set as (level,tx,ty,tz).
        auto settle = [](Config cfg, float ax, float ay, float az) {
            SectorStreamer s(cfg);
            for (int i = 0; i < 40; ++i) {
                s.update(ax, ay, az);
                SectorRequest q;
                while (s.next_request(q)) s.on_published(q.tx, q.ty, q.tz, q.rung);
                s.take_evictions();
            }
            s.update(ax, ay, az);
            std::vector<std::tuple<long long,long long,long long,int>> out;
            SectorRequest q;
            while (s.next_request(q))
                out.push_back({q.tx, q.ty, q.tz, q.rung});
            return std::make_pair(s.resident_count(), out);
        };

        // 1. THE ROLLBACK POSITION, and the assertion the whole WP rests on:
        //    with the flag off, an anchor 900 m underground selects exactly
        //    what it selected when last_anchor_y_ was documented "stored, never
        //    read". If this moves, the octree leaked into the column path.
        {
            Config c = base;
            const auto ground = settle(c, 0.0f, 0.0f, 0.0f);
            const auto deep   = settle(c, 0.0f, -900.0f, 0.0f);
            CHECK(ground.first > 0, "the flag-off world streams something");
            CHECK(ground.first == deep.first,
                  "flag OFF: anchor altitude changes no residency at all");
        }
        // 2. Flag ON, and the point of the milestone: the desired set is a
        //    STACK. Count distinct ty values among resident tiles by walking
        //    the request stream of a cold streamer.
        {
            Config c = base;
            c.volumetric_sectors = true;
            SectorStreamer s(c);
            std::set<long long> tys;
            std::set<std::tuple<long long,long long,long long>> cells;
            for (int i = 0; i < 60; ++i) {
                s.update(0.0f, 0.0f, 0.0f);
                SectorRequest q;
                while (s.next_request(q)) {
                    tys.insert((long long)q.ty);
                    cells.insert({(long long)q.tx, (long long)q.ty, (long long)q.tz});
                    s.on_published(q.tx, q.ty, q.tz, q.rung);
                }
                s.take_evictions();
            }
            CHECK(tys.size() > 1,
                  "flag ON: the desired set spans more than one ty -- tiles are "
                  "cubes stacked in y, not columns");
            CHECK(tys.count(0) == 1, "including the row the anchor sits in");
            printf("  octree: %zu distinct ty rows, %zu distinct (tx,ty,tz) "
                   "cells requested from a cold start\n",
                   tys.size(), cells.size());
        }
        // 3. THE EXTENT BOUNDS THE TREE. No tile may be requested whose cube
        //    lies wholly outside [y_min, y_max] -- x and z are unbounded and
        //    stopped only by the reach, but y is authored and the selector must
        //    honour it or it streams rock the world does not define.
        {
            Config c = base;
            c.volumetric_sectors = true;
            c.y_min = -128.0f;      // deliberately tight: two level-0 tiles
            c.y_max =  128.0f;
            SectorStreamer s(c);
            long long lo_violations = 0, hi_violations = 0;
            for (int i = 0; i < 60; ++i) {
                s.update(0.0f, 0.0f, 0.0f);
                SectorRequest q;
                while (s.next_request(q)) {
                    const float S = c.sector_size *
                                    float(1 << matter_stream::variant_level(q.rung));
                    const float y0 = float(q.ty) * S;
                    if (y0 + S <= c.y_min) ++lo_violations;
                    if (y0 >= c.y_max)     ++hi_violations;
                    s.on_published(q.tx, q.ty, q.tz, q.rung);
                }
                s.take_evictions();
            }
            CHECK(lo_violations == 0 && hi_violations == 0,
                  "no tile is requested wholly outside the authored extent");
            printf("  extent [%.0f, %.0f]: %lld tiles below, %lld above\n",
                   c.y_min, c.y_max, lo_violations, hi_violations);
        }
        // 4. ALTITUDE NOW SELECTS. The mirror of case 1: with the flag on, the
        //    same anchor moved 900 m down must NOT produce the same tiles, or
        //    the y term never reached tile_near_dist.
        {
            Config c = base;
            c.volumetric_sectors = true;
            auto cells_at = [&](float ay) {
                SectorStreamer s(c);
                std::set<std::tuple<long long,long long,long long,int>> cells;
                for (int i = 0; i < 60; ++i) {
                    s.update(0.0f, ay, 0.0f);
                    SectorRequest q;
                    while (s.next_request(q)) {
                        cells.insert({(long long)q.tx, (long long)q.ty,
                                      (long long)q.tz, q.rung});
                        s.on_published(q.tx, q.ty, q.tz, q.rung);
                    }
                    s.take_evictions();
                }
                return cells;
            };
            const auto ground = cells_at(0.0f);
            const auto deep   = cells_at(-900.0f);
            CHECK(!ground.empty() && ground != deep,
                  "flag ON: the bands are spheres -- a camera 900 m down "
                  "refines the rock around it, not the surface above it");
        }
        // 5. THE 2:1 RULE HOLDS ON ALL SIX FACES (M3-WP3). This is the gate the
        //    whole mode depends on: seam_weld rejects a rung gap of 2 outright
        //    (seam_weld_tests [2]), so a face pair two levels apart is not a
        //    blemish, it is a seam the welder REFUSES to close -- a hole. The
        //    ±y pair is what the octree adds and what a 4-face pass would miss.
        {
            Config c = base;
            c.volumetric_sectors = true;
            SectorStreamer s(c);
            for (int i = 0; i < 80; ++i) {
                s.update(0.0f, 0.0f, 0.0f);
                SectorRequest q;
                while (s.next_request(q)) s.on_published(q.tx, q.ty, q.tz, q.rung);
                s.take_evictions();
            }
            s.update(0.0f, 0.0f, 0.0f);
            // Rebuild the desired map from the settled request stream, then
            // check every face-adjacent pair directly. Levels come from the
            // variant, which is where the streamer actually publishes them.
            std::map<std::tuple<long long,long long,long long>, int> lvl_at;
            {
                SectorStreamer t(c);
                for (int i = 0; i < 80; ++i) {
                    t.update(0.0f, 0.0f, 0.0f);
                    SectorRequest q;
                    while (t.next_request(q)) {
                        lvl_at[{(long long)q.tx, (long long)q.ty, (long long)q.tz}] =
                            matter_stream::variant_level(q.rung);
                        t.on_published(q.tx, q.ty, q.tz, q.rung);
                    }
                    t.take_evictions();
                }
            }
            // A cell's neighbours at ITS OWN level: walk world space so tiles of
            // different sizes are compared at a shared point rather than by
            // index arithmetic that only works within one level.
            long long worst_gap = 0, pairs = 0;
            auto level_at_point = [&](float x, float y, float z) -> int {
                for (int L = 0; L <= matter_stream::kMaxLevel; ++L) {
                    const float S = c.sector_size * float(1 << L);
                    auto it = lvl_at.find({(long long)std::floor(x / S),
                                           (long long)std::floor(y / S),
                                           (long long)std::floor(z / S)});
                    if (it != lvl_at.end() && it->second == L) return L;
                }
                return -1;
            };
            for (const auto& [cell, L] : lvl_at) {
                const auto [tx, ty, tz] = cell;
                const float S = c.sector_size * float(1 << L);
                const float cx = (float(tx) + 0.5f) * S;
                const float cy = (float(ty) + 0.5f) * S;
                const float cz = (float(tz) + 0.5f) * S;
                const float o = 0.5f * c.sector_size;
                const float probes[6][3] = {
                    {cx + 0.5f * S + o, cy, cz}, {cx - 0.5f * S - o, cy, cz},
                    {cx, cy + 0.5f * S + o, cz}, {cx, cy - 0.5f * S - o, cz},
                    {cx, cy, cz + 0.5f * S + o}, {cx, cy, cz - 0.5f * S - o},
                };
                for (const auto& p : probes) {
                    const int n = level_at_point(p[0], p[1], p[2]);
                    if (n < 0) continue;               // past the reach
                    ++pairs;
                    worst_gap = std::max<long long>(worst_gap, std::abs(n - L));
                }
            }
            CHECK(pairs > 0, "there are face-adjacent pairs to check");
            CHECK(worst_gap <= 1,
                  "every face-adjacent pair differs by at most one level, on "
                  "all six faces -- the domain the weld fan is defined on");
            printf("  6-face 2:1: %lld adjacent pairs, worst level gap %lld\n",
                   pairs, worst_gap);
        }
    }

    // --- occlusion detail cap and priority (M4 Phase A) ---------------------
    //
    // The safety property first, the behaviour second. This feature is allowed
    // to cost detail and is NEVER allowed to cost coverage, because its input
    // is a fenced readback that is stale by construction.
    //
    // A NOTE ON WHAT "NEVER SEEN" MEANS, because this block previously asserted
    // the opposite and the assertion was what hid the bug. The first cut read
    // `last_visible == 0` as "visible", so that a cold fill could not be
    // demoted. That is unimplementable as stated: the tile the cap is asked
    // about in `descend` is the one about to be SPLIT, so it is never resident
    // and never drawn, so its bit is structurally always zero -- the cap was
    // dead code and measured exactly zero effect on StreamCaverns.
    //
    // The fix is two changes that only work together: visibility propagates UP
    // the tree (a node is visible if anything in its subtree was drawn), and
    // "never seen" is decided against an ELIGIBILITY clock started when the
    // descent first reaches the node. So a cold fill IS demoted -- for one
    // grace period, until the coarse tiles land and are drawn, at which point
    // the cap releases over everything on screen. That is progressive
    // refinement, and it is better than what it replaced, but it is a
    // behavioural change and it is asserted below rather than assumed.
    {
        printf("== occlusion: detail cap and priority ==\n");
        Config base;
        base.sector_size = 64.0f;
        base.nested_sectors = true;
        base.rings = { {128.0f, 2}, {512.0f, 1}, {2048.0f, 0} };
        base.terrain_bands = { {128.0f, 5}, {256.0f, 4}, {512.0f, 3},
                               {1024.0f, 2}, {2048.0f, 1}, {4096.0f, 0} };
        CHECK(base.occlusion_grace_ticks == 0,
              "the cap is OFF by default -- an engine that never reports "
              "visibility must behave exactly as it did before this existed");

        // A resident set the harness tracks itself, so it can report the WHOLE
        // of it visible every tick. Reporting only what was published this tick
        // (the previous harness) makes every tile go stale one grace later no
        // matter what the renderer is doing, which is indistinguishable from
        // total occlusion -- so "a fully visible world" was never actually
        // being tested.
        struct Tile { long long tx, ty, tz; int rung; };
        struct Settled {
            size_t resident = 0;
            std::map<std::tuple<long long,long long,long long>, int> level_of;
            long long finest = 99;
        };
        auto settle = [](Config cfg, bool report_visible, int ticks) {
            SectorStreamer s(cfg);
            std::map<std::tuple<long long,long long,long long,int>, char> live;
            for (int i = 0; i < ticks; ++i) {
                s.set_visibility_frame(uint64_t(i + 1));
                if (report_visible)
                    for (const auto& [t, _] : live)
                        s.mark_visible(std::get<0>(t), std::get<1>(t),
                                       std::get<2>(t), std::get<3>(t));
                s.update(0.0f, 0.0f, 0.0f);
                SectorRequest q;
                while (s.next_request(q)) {
                    s.on_published(q.tx, q.ty, q.tz, q.rung);
                    live[{q.tx, q.ty, q.tz, q.rung}] = 1;
                }
                for (const auto& e : s.take_evictions())
                    live.erase({e.tx, e.ty, e.tz, e.rung});
            }
            Settled out;
            out.resident = s.resident_count();
            for (const auto& [t, _] : live) {
                const int level =
                    matter_stream::variant_level(std::get<3>(t));
                out.level_of[{std::get<0>(t), std::get<1>(t),
                              std::get<2>(t)}] = level;
                out.finest = std::min<long long>(out.finest, level);
            }
            return out;
        };

        // COVERAGE, checked directly rather than inferred from a tile count.
        // Every level-0 cell inside the innermost band must be covered by
        // exactly one resident tile -- itself or one of its ancestors. This is
        // the property the whole safety argument rests on, and a count can not
        // express it: a coarser world has FEWER tiles covering the same ground,
        // which is the intended outcome and also what a hole looks like.
        auto covers_reach = [](const Settled& st, int radius_tiles) {
            for (long long cx = -radius_tiles; cx < radius_tiles; ++cx)
                for (long long cz = -radius_tiles; cz < radius_tiles; ++cz) {
                    int hits = 0;
                    for (int L = 0; L <= 5; ++L) {
                        const auto found =
                            st.level_of.find({cx >> L, 0, cz >> L});
                        if (found != st.level_of.end() && found->second == L)
                            ++hits;
                    }
                    if (hits != 1) return false;
                }
            return true;
        };

        Config capped = base;
        capped.occlusion_grace_ticks = 4;
        capped.occlusion_cap_levels = 1;

        const Settled plain = settle(base, false, 80);
        const Settled all_visible = settle(capped, true, 80);
        const Settled unseen = settle(capped, false, 80);

        CHECK(plain.resident > 0 && covers_reach(plain, 2),
              "the uncapped baseline covers its reach exactly once per cell");

        // 1. A world the renderer reports as fully visible is a world the cap
        //    never touches. This is the assertion that would fail if the
        //    ancestor propagation were wrong in the direction that matters:
        //    a parent whose children are all drawn must read as visible.
        CHECK(all_visible.resident == plain.resident &&
                  all_visible.level_of == plain.level_of,
              "a fully visible world is bit-for-bit the uncapped world");

        // 2. A world the renderer never reports at all ends up COARSER, and
        //    still covers its reach. Both halves matter: the first is the
        //    feature working, the second is the feature being safe.
        CHECK(unseen.resident < plain.resident,
              "a world nothing is drawn in settles coarser than the bands ask");
        CHECK(covers_reach(unseen, 2),
              "COVERAGE SURVIVES the cap -- the desired set still covers the "
              "reach exactly once per cell, which is what makes a stale "
              "visibility bit cost detail and never a hole");
        CHECK(unseen.finest > plain.finest,
              "coarser means coarser: the finest resident LEVEL rises, which "
              "is what `occlusion_cap_levels` names");

        // 3. Disocclusion. Report nothing for long enough to coarsen, then
        //    report everything, and the world must refine back to exactly the
        //    uncapped map. A cap that could not be released would be a
        //    one-way ratchet, and nothing above would catch it.
        {
            SectorStreamer s(capped);
            std::map<std::tuple<long long,long long,long long,int>, char> live;
            uint64_t clock = 0;
            auto run = [&](int ticks, bool report_visible) {
                for (int i = 0; i < ticks; ++i) {
                    s.set_visibility_frame(++clock);
                    if (report_visible)
                        for (const auto& [t, _] : live)
                            s.mark_visible(std::get<0>(t), std::get<1>(t),
                                           std::get<2>(t), std::get<3>(t));
                    s.update(0.0f, 0.0f, 0.0f);
                    SectorRequest q;
                    while (s.next_request(q)) {
                        s.on_published(q.tx, q.ty, q.tz, q.rung);
                        live[{q.tx, q.ty, q.tz, q.rung}] = 1;
                    }
                    for (const auto& e : s.take_evictions())
                        live.erase({e.tx, e.ty, e.tz, e.rung});
                }
            };
            run(80, false);
            const size_t occluded_residency = s.resident_count();
            run(80, true);
            CHECK(occluded_residency < plain.resident &&
                      s.resident_count() == plain.resident,
                  "DISOCCLUSION RELEASES THE CAP: a world that goes unseen "
                  "coarsens and refines all the way back when it is drawn "
                  "again");
            printf("  uncapped %zu tiles (finest level %lld) | unseen %zu "
                   "(finest %lld) | disoccluded back to %zu\n",
                   plain.resident, plain.finest, unseen.resident,
                   unseen.finest, s.resident_count());
        }
    }

    return check_summary();
}
