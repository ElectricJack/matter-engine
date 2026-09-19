// ProfileLib P0 unit tests: zone interning, scope accumulation, the frame ring
// (order + wrap), disable gating, and stats shape. The compile-OUT proof is a
// separate build-level check (see the Makefile `compileout` target and
// compileout_probe.cpp) because it asserts on emitted object code, not runtime.

#include "profile.h"

#include <cstdio>
#include <string>
#include <thread>
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

    // --- scope nesting (parent/child) -----------------------------------
    {
        const int parent = register_zone("outer");
        const int child = register_zone("inner");
        const int sibling = register_zone("after");
        {
            Scope p(parent);
            spin_one_tick();
            {
                Scope c(child);
                spin_one_tick();
            }
        }
        {
            Scope s2(sibling);  // opened at top level, not under a parent
            spin_one_tick();
        }
        CHECK(zone_parent(parent) == -1, "top-level scope has no parent");
        CHECK(zone_parent(child) == parent, "nested scope records its parent");
        CHECK(zone_parent(sibling) == -1, "a later top-level scope stays root");
    }

    // --- lane attribution -----------------------------------------------
    {
        const int render_zone = register_zone("render.only");
        const int worker_zone = register_zone("worker.only");
        add_ns(render_zone, 1000);  // deposited on this (default render) thread
        std::thread worker([&] {
            set_thread_lane(kLaneWorker);
            add_ns(worker_zone, 1000);
        });
        worker.join();
        frame_mark();
        CHECK(zone_lane(render_zone) == kLaneRender,
              "a zone first seen on the render thread is render-lane");
        CHECK(zone_lane(worker_zone) == kLaneWorker,
              "a zone first seen on a worker thread is worker-lane");
        CHECK(lane_thread_count(kLaneWorker) >= 1,
              "worker-lane thread count reflects the tagged thread");
        // Lane is sticky: a later deposit from the render thread must not move an
        // already-worker zone back to render (first-sight wins, like the parent).
        add_ns(worker_zone, 500);
        frame_mark();
        CHECK(zone_lane(worker_zone) == kLaneWorker,
              "recorded lane is sticky (first-sight wins)");
    }

    // --- chrome trace dump ----------------------------------------------
    {
        // Record a render zone, a bake zone, and a counter, then a frame. The
        // bake zone is deposited from a WORKER-tagged thread so the trace places
        // it on the worker lane by recorded thread, not by its name prefix.
        const int rz = register_zone("ui.loop");
        const int bz = register_zone("bake.stagemem");
        add_ns(rz, 2000);
        std::thread([&] {
            set_thread_lane(kLaneWorker);
            add_ns(bz, 8000);
        }).join();
        add_count(register_counter("layout_rebuilds"), 3);
        frame_mark();
        const char* path = "profile_trace_test.json";
        CHECK(dump_chrome_trace(path), "dump_chrome_trace writes a file");
        std::FILE* rf = std::fopen(path, "rb");
        CHECK(rf != nullptr, "trace file is readable");
        if (rf) {
            std::string body;
            char buf[4096];
            size_t got;
            while ((got = std::fread(buf, 1, sizeof(buf), rf)) > 0)
                body.append(buf, got);
            std::fclose(rf);
            CHECK(body.find("\"traceEvents\"") != std::string::npos,
                  "trace has traceEvents array");
            CHECK(body.find("bake.stagemem") != std::string::npos,
                  "trace includes the bake zone");
            CHECK(body.find("\"tid\":2") != std::string::npos,
                  "bake zone lands on the bake lane (tid 2)");
            CHECK(body.find("frame_ms") != std::string::npos,
                  "trace emits the frame_ms counter track");
            CHECK(body.find("layout_rebuilds") != std::string::npos,
                  "trace emits the layout_rebuilds counter");
            CHECK(!body.empty() && body[0] == '{' &&
                      body.find_last_of('}') != std::string::npos,
                  "trace is brace-delimited JSON");
        }
        std::remove(path);
    }

    // Exhaustion must not attribute unrelated work to the final valid name.
    // This occurs in real editor captures when late VT counters exceed the
    // registry budget. Existing names must still resolve after saturation.
    {
        const int overflow_parent = register_zone("capacity.parent");
        const int overflow_child = register_zone("capacity.child");
        while (zone_count() < kMaxZones - 1)
            register_zone(("capacity.zone." + std::to_string(zone_count())).c_str());
        while (counter_count() < kMaxCounters - 1)
            register_counter(("capacity.counter." + std::to_string(counter_count())).c_str());
        const int last_zone = register_zone("capacity.last.zone");
        const int last_counter = register_counter("capacity.last.counter");
        const int rejected_zone = register_zone("capacity.excess.zone");
        const int rejected_counter = register_counter("capacity.excess.counter");
        CHECK(rejected_zone == -1, "zone overflow returns an invalid id");
        CHECK(rejected_counter == -1, "counter overflow returns an invalid id");
        CHECK(register_zone("alpha") == a, "existing zone survives full registry");
        CHECK(register_counter("capacity.last.counter") == last_counter,
              "existing counter survives full registry");
        CHECK(zone_count() == kMaxZones && counter_count() == kMaxCounters,
              "registries stay bounded after overflow");
        frame_mark();
        add_ns(last_zone, 7);
        add_ns(rejected_zone, 9000);
        add_count(last_counter, 11);
        add_count(rejected_counter, 9000);
        frame_mark();
        FrameRecord r[1];
        CHECK(copy_recent(r, 1) == 1, "overflow frame is available");
        CHECK(r[0].zone_ns[last_zone] == 7, "overflow cannot contaminate a valid zone");
        CHECK(r[0].counter[last_counter] == 11, "overflow cannot contaminate a valid counter");
        {
            Scope parent(overflow_parent);
            Scope ignored(rejected_zone);
            Scope child(overflow_child);
        }
        CHECK(zone_parent(overflow_child) == overflow_parent,
              "rejected scope leaves valid nesting intact");
        const char* path = "profile_overflow_test.json";
        CHECK(dump_chrome_trace(path), "overflow trace writes a file");
        std::FILE* rf = std::fopen(path, "rb");
        CHECK(rf != nullptr, "overflow trace is readable");
        if (rf) {
            std::string body;
            char buf[4096];
            size_t got;
            while ((got = std::fread(buf, 1, sizeof(buf), rf)) > 0) body.append(buf, got);
            std::fclose(rf);
            CHECK(body.find("\"rejected_zone_registrations\":1") != std::string::npos,
                  "trace discloses missing zone registrations");
            CHECK(body.find("\"rejected_counter_registrations\":1") != std::string::npos,
                  "trace discloses missing counter registrations");
        }
        std::remove(path);
    }

    if (g_failures == 0)
        std::printf("ALL PASS (ProfileLib P0)\n");
    else
        std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
