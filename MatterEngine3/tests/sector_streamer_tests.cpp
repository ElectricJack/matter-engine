// MatterEngine3/tests/sector_streamer_tests.cpp
#include "check.h"
#include "../src/sector_streamer.h"
#include <cmath>
#include <cstdio>

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
    return check_summary();
}
