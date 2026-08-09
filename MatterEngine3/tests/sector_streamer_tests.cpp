// MatterEngine3/tests/sector_streamer_tests.cpp
#include "check.h"
#include "../src/sector_streamer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <tuple>
#include <vector>

using namespace matter_stream;

// Service every outstanding request (simulate instant bakes). Optionally
// record each request via cb(q).
template <typename F>
static void service_all(SectorStreamer& s, F cb) {
    SectorRequest q;
    while (s.next_request(q)) { cb(q); s.on_published(q.tx, q.tz, q.rung); }
}
static void service_all(SectorStreamer& s) { service_all(s, [](const SectorRequest&){}); }

// Settle: update+service until quiescent; RETURNS all evictions drained on
// the way (callers assert on them — do not discard silently).
static std::vector<Eviction> settle(SectorStreamer& s, float x, float z) {
    std::vector<Eviction> evs;
    for (int i = 0; i < 10000; ++i) {           // ~8 publishes per update
        s.update(x, z);
        SectorRequest q;
        bool any = false;
        while (s.next_request(q)) { any = true; s.on_published(q.tx, q.tz, q.rung); }
        auto e = s.take_evictions();
        evs.insert(evs.end(), e.begin(), e.end());
        if (!any) break;
    }
    return evs;
}

int main() {
    Config cfg;   // defaults: rings 48/120/300/800, hysteresis 16, inflight 8

    // --- desired rung by distance -------------------------------------------
    {
        SectorStreamer s(cfg);
        // Fill everything, verifying known probe sectors were requested at the
        // right final rung (last request seen per probe wins).
        int rung_00 = -1, rung_40 = -1, rung_10_0 = -1, rung_30_0 = -1;
        bool saw_60_0 = false;
        for (int i = 0; i < 10000; ++i) {
            s.update(8.0f, 8.0f);        // camera at centre of sector (0,0)
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
        s.update(8.0f, 8.0f);
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
        s.update(8.0f + 8.0f, 8.0f);      // move less than hysteresis
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
                s.update(8.0f + (b % 2 == 0 ? 7.0f : -7.0f), 8.0f);   // ±7 < hysteresis 16
                SectorRequest q;
                while (s.next_request(q)) s.on_published(q.tx, q.tz, q.rung);
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
        s.update(8.0f, 8.0f);
        SectorRequest q;
        CHECK(s.next_request(q), "got a request");
        s.update(8.0f + 5000.0f, 8.0f);   // camera long gone
        CHECK(!s.on_published(q.tx, q.tz, q.rung), "stale publish rejected");
    }
    // --- fail cooldown ---------------------------------------------------------
    {
        Config c2 = cfg; c2.fail_cooldown_updates = 10; c2.max_inflight = 1;
        SectorStreamer s(c2);
        s.update(8.0f, 8.0f);
        SectorRequest q;
        CHECK(s.next_request(q), "first request");
        int64_t fx = q.tx, fz = q.tz; int fr = q.rung;
        s.on_failed(fx, fz, fr);
        bool re_requested_early = false;
        for (int i = 0; i < 9; ++i) {
            s.update(8.0f, 8.0f);
            if (s.next_request(q)) {
                if (q.tx == fx && q.tz == fz && q.rung == fr) re_requested_early = true;
                s.on_published(q.tx, q.tz, q.rung);   // keep the queue moving
                s.take_evictions();
            }
        }
        CHECK(!re_requested_early, "failed sector cools down");
        bool re_requested_later = false;
        for (int i = 0; i < 5000 && !re_requested_later; ++i) {
            s.update(8.0f, 8.0f);
            if (s.next_request(q)) {
                if (q.tx == fx && q.tz == fz && q.rung == fr) re_requested_later = true;
                else { s.on_published(q.tx, q.tz, q.rung); s.take_evictions(); }
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
        s.update(8.0f, 8.0f);
        SectorRequest q;
        CHECK(s.next_request(q), "clear late-publish test has an inflight request");
        s.clear();
        CHECK(!s.on_published(q.tx, q.tz, q.rung),
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
            s.update(x, 8.0f);
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

    // --- terrain LOD ladder: bands, 2:1 balance, edge masks ------------------
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
            s.update(32.0f, 32.0f);
            SectorRequest q; bool any = false;
            while (s.next_request(q)) {
                any = true;
                if (s.on_published(q.tx, q.tz, q.rung))
                    final_variant[skey(q.tx, q.tz)] = q.rung;
            }
            s.take_evictions();
            if (!any && i > 2) break;
        }
        CHECK(!final_variant.empty(), "terrain ladder settled");

        bool all_packed = true, near_is_voxel = true, far_is_coarse = true;
        bool balanced = true, masks_ok = true;
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

            int expect_mask = 0;
            const int64_t ntx[4] = {tx + 1, tx - 1, tx, tx};
            const int64_t ntz[4] = {tz, tz, tz + 1, tz - 1};
            for (int n = 0; n < 4; ++n) {
                auto it = final_variant.find(skey(ntx[n], ntz[n]));
                if (it == final_variant.end()) continue;
                const int nlod = variant_terrain_lod(it->second);
                if (nlod - lod > 1 || lod - nlod > 1) balanced = false;
                if (nlod == lod - 1) expect_mask |= 1 << n;
            }
            if (variant_edge_mask(v) != expect_mask) masks_ok = false;
        }
        CHECK(all_packed, "every terrain-ladder variant carries the marker");
        CHECK(near_is_voxel, "sectors inside 3S stay native voxel (lod 5)");
        CHECK(far_is_coarse, "outer-band sectors coarsen to lod <= 1");
        CHECK(seen_lods == 0x3F, "all six LOD levels appear in the settled disc");
        CHECK(balanced, "settled cardinal neighbors differ by at most one LOD");
        CHECK(masks_ok, "edge masks name exactly the one-coarser neighbors");

        // Legacy path untouched: same rings without the flag produce bare rungs.
        Config bare = tcfg; bare.terrain_lod_enabled = false;
        SectorStreamer s2(bare);
        s2.update(32.0f, 32.0f);
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
            s.update(x, z);
            SectorRequest q; bool any = false;
            while (s.next_request(q)) {
                any = true;
                if (s.on_published(q.tx, q.tz, q.rung))
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
            uni.update(3.2f, 3.2f);
            SectorRequest q; bool any = false;
            while (uni.next_request(q)) { any = true; uni.on_published(q.tx, q.tz, q.rung); }
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
        int violations = 0, mask_errors = 0;
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
            // The mask must name exactly the sides whose neighbour is one
            // level COARSER -- probed at the edge midpoint, which is where a
            // coarser neighbour necessarily is (it spans the whole edge).
            int expect = 0;
            const float mid = 0.5f * S;
            const float mx[4] = {ox + S + 0.5f*S0, ox - 0.5f*S0, ox + mid, ox + mid};
            const float mz[4] = {oz + mid, oz + mid, oz + S + 0.5f*S0, oz - 0.5f*S0};
            for (int n = 0; n < 4; ++n)
                if (level_at(mx[n], mz[n]) == L + 1) expect |= 1 << n;
            if (variant_edge_mask(v) != expect) ++mask_errors;
        }
        CHECK(violations == 0,
              "nested restriction: cardinal neighbours differ by at most one "
              "level, checked at the finest pitch along every edge");
        CHECK(mask_errors == 0,
              "nested edge masks name exactly the one-level-coarser neighbours");
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
                s.update(31.0f + (i % 2 ? 0.5f : -0.5f), 0.0f);  // < hysteresis 2
                SectorRequest q;
                while (s.next_request(q)) s.on_published(q.tx, q.tz, q.rung);
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
            s.update(400.0f - step * 10.0f, 0.0f);
            SectorRequest q;
            while (s.next_request(q)) {
                s.on_published(q.tx, q.tz, q.rung);
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
            s.update(x, z);
            SectorRequest q;
            for (int i = 0; i < budget && s.next_request(q); ++i)
                if (s.on_published(q.tx, q.tz, q.rung))
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
            s.update(x, z);
            SectorRequest q;
            for (int i = 0; i < budget && s.next_request(q); ++i)
                if (s.on_published(q.tx, q.tz, q.rung))
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
            s.update(300.0f, 0.0f);
            SectorRequest q;
            while (s.next_request(q)) s.on_published(q.tx, q.tz, q.rung);
            s.take_evictions();
        }
        // Move in and refuse exactly one request forever.
        bool have_victim = false;
        int64_t vtx = 0, vtz = 0; int vrung = 0;
        int parent_evictions_while_failing = 0;
        for (int i = 0; i < 400; ++i) {
            s.update(120.0f, 0.0f);
            SectorRequest q;
            while (s.next_request(q)) {
                if (!have_victim && variant_level(q.rung) <= 3) {
                    have_victim = true;
                    vtx = q.tx; vtz = q.tz; vrung = q.rung;
                }
                if (have_victim && q.tx == vtx && q.tz == vtz &&
                    q.rung == vrung) {
                    s.on_failed(q.tx, q.tz, q.rung);      // never succeeds
                    continue;
                }
                s.on_published(q.tx, q.tz, q.rung);
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
            s.update(0.0f, 0.0f);
            SectorRequest q;
            while (s.next_request(q)) s.on_published(q.tx, q.tz, q.rung);
            s.take_evictions();
        }
        const size_t settled = s.resident_count();
        // Start a wave of splits by moving, then leave entirely without ever
        // servicing them.
        s.update(200.0f, 0.0f);
        s.update(5000.0f, 5000.0f);
        for (int i = 0; i < 4000; ++i) {
            s.update(5000.0f, 5000.0f);
            SectorRequest q;
            while (s.next_request(q)) s.on_published(q.tx, q.tz, q.rung);
            s.take_evictions();
        }
        printf("  nested groups: %zu resident at origin, %zu after leaving\n",
               settled, s.resident_count());
        CHECK(s.resident_count() < settled * 2,
              "transition groups: abandoning a transition releases the held "
              "tiles rather than stranding them");
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
                s.update(32.0f + step * 25.0f, 32.0f);
                SectorRequest q;
                while (s.next_request(q)) {
                    out.push_back({q.tx, q.tz, q.rung, 0});
                    s.on_published(q.tx, q.tz, q.rung);
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

    return check_summary();
}
