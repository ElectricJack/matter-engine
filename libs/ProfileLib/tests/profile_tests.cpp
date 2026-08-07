// ProfileLib P0 unit tests: zone interning, scope accumulation, the frame ring
// (order + wrap), disable gating, and stats shape. The compile-OUT proof is a
// separate build-level check (see the Makefile `compileout` target and
// compileout_probe.cpp) because it asserts on emitted object code, not runtime.

#include "profile.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace matter::profile;

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL: %s\n", msg);                                  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// Spin until the monotonic clock advances, so a Scope over this span is
// guaranteed a nonzero duration regardless of clock resolution.
static void spin_one_tick() {
    const uint64_t t0 = now_ns();
    while (now_ns() == t0) { /* busy */ }
}

static uint64_t last_zone_ns(int zone) {
    FrameRecord recent[4];
    const int n = copy_recent(recent, 4);
    if (n == 0) return 0;
    return recent[n - 1].zone_ns[zone];
}

int main() {
    set_enabled(true);

    // --- zone interning -------------------------------------------------
    const int a = register_zone("alpha");
    const int b = register_zone("beta");
    const int a2 = register_zone("alpha");
    CHECK(a != b, "distinct names get distinct ids");
    CHECK(a == a2, "same name interns to the same id");
    CHECK(std::string(zone_name(a)) == "alpha", "zone_name round-trips");
    CHECK(zone_count() >= 2, "zone_count reflects registrations");

    // --- direct deposit + frame sweep -----------------------------------
    add_ns(a, 1000);
    add_ns(a, 500);
    add_ns(b, 250);
    frame_mark();
    {
        FrameRecord r[1];
        CHECK(copy_recent(r, 1) == 1, "one record after one frame_mark");
        CHECK(r[0].zone_ns[a] == 1500, "zone a accumulated 1000+500");
        CHECK(r[0].zone_ns[b] == 250, "zone b accumulated 250");
    }
    // Accumulators must reset after the sweep.
    frame_mark();
    CHECK(last_zone_ns(a) == 0, "accumulators cleared after sweep");

    // --- Scope actually deposits when enabled ---------------------------
    const int s = register_zone("scoped");
    {
        Scope sc(s);
        spin_one_tick();
    }
    frame_mark();
    CHECK(last_zone_ns(s) > 0, "enabled Scope deposits nonzero time");

    // --- Scope deposits nothing when disabled ---------------------------
    const int d = register_zone("disabled");
    set_enabled(false);
    {
        Scope sc(d);
        spin_one_tick();
    }
    frame_mark();
    CHECK(last_zone_ns(d) == 0, "disabled Scope deposits nothing");
    set_enabled(true);

    // --- ring order + wrap ----------------------------------------------
    const uint64_t base = frame_index();
    for (int i = 0; i < 5; ++i) frame_mark();
    {
        FrameRecord r[5];
        const int n = copy_recent(r, 5);
        CHECK(n == 5, "copy_recent returns requested count when available");
        for (int i = 1; i < n; ++i)
            CHECK(r[i].frame_index > r[i - 1].frame_index,
                  "records are oldest-to-newest");
        CHECK(r[n - 1].frame_index == frame_index() - 1,
              "newest record is the last frame marked");
        CHECK(r[n - 1].frame_index == base + 4,
              "frame indices advanced by the marks issued");
    }
    // Overfill the ring and confirm it wraps to exactly kFrameHistory.
    for (int i = 0; i < kFrameHistory + 32; ++i) frame_mark();
    {
        std::vector<FrameRecord> r(kFrameHistory + 8);
        const int n = copy_recent(r.data(), kFrameHistory + 8);
        CHECK(n == kFrameHistory, "ring caps at kFrameHistory after overfill");
        for (int i = 1; i < n; ++i)
            CHECK(r[i].frame_index > r[i - 1].frame_index,
                  "wrapped ring stays monotonic");
    }

    // --- stats shape ----------------------------------------------------
    {
        const FrameStats st = frame_stats(16.6);
        CHECK(st.samples == kFrameHistory, "stats sample the full window");
        CHECK(st.min_ms >= 0.0 && st.max_ms >= st.min_ms, "min <= max");
        CHECK(st.mean_ms >= st.min_ms && st.mean_ms <= st.max_ms,
              "mean within [min,max]");
        CHECK(st.stddev_ms >= 0.0, "stddev non-negative");
        CHECK(st.smoothness >= 0.0 && st.smoothness <= 1.0,
              "smoothness in [0,1]");
    }

    if (g_failures == 0)
        std::printf("ALL PASS (ProfileLib P0)\n");
    else
        std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
