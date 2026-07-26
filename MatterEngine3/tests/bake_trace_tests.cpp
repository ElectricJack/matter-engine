// bake_trace_tests.cpp — unit tests for the BakeTrace span/counter collector
// (Bake Lab task 1.1; docs/bake-lab.md §II.1, §II.6).
//
// Coverage: span nesting, counters attach to the open span, snapshot taken
// mid-write is consistent (deterministic single-thread + concurrent
// writer/reader), thread-local current, all APIs no-op when no collector is
// set, and an overhead micro-benchmark (10k spans; the <0.1%-of-bake target
// is judged by the printed number, the hard assert is deliberately loose).
//
// Headless, pure CPU — no raylib/GL. Run via `make run-baketrace`.

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "bake_trace.h"
#include "bake_trace_names.h"
#include "check.h"

using bake_trace::Collector;
using bake_trace::Span;
using bake_trace::kOpenEndMs;

// ---------------------------------------------------------------------------
// Consistency validator: every invariant a snapshot must satisfy no matter
// when it was taken relative to the writer.
//   - every span has a name;
//   - begin_ms is within the parent's interval (>= parent begin);
//   - a closed span has end_ms >= begin_ms, and <= parent's end when the
//     parent is closed;
//   - an open span (end_ms == kOpenEndMs) can only be the LAST child of its
//     parent, and only if the parent itself is open-or-root (the open chain
//     is a single rightmost path).
// ---------------------------------------------------------------------------
static bool span_consistent(const Span& s, double parent_begin,
                            double parent_end /* kOpenEndMs = open */,
                            bool parent_on_open_chain) {
    if (s.name == nullptr) return false;
    if (s.begin_ms + 1e-9 < parent_begin) return false;
    const bool open = (s.end_ms == kOpenEndMs);
    if (open && !parent_on_open_chain) return false;
    if (!open) {
        if (s.end_ms + 1e-9 < s.begin_ms) return false;
        if (parent_end != kOpenEndMs && s.end_ms > parent_end + 1e-9) return false;
    }
    for (const bake_trace::Counter& c : s.counters)
        if (c.name == nullptr) return false;
    for (size_t i = 0; i < s.children.size(); ++i) {
        const Span& child = s.children[i];
        const bool child_may_be_open = open && (i + 1 == s.children.size());
        if (child.end_ms == kOpenEndMs && !child_may_be_open) return false;
        if (!span_consistent(child, s.begin_ms, s.end_ms, child_may_be_open))
            return false;
    }
    return true;
}

// Root wrapper: the snapshot root has end_ms = snapshot time and is treated
// as the head of the open chain.
static bool snapshot_consistent(const Span& root) {
    if (root.name == nullptr || root.begin_ms != 0.0) return false;
    if (root.end_ms == kOpenEndMs) return false;  // root always gets a time
    for (size_t i = 0; i < root.children.size(); ++i) {
        const Span& child = root.children[i];
        const bool may_be_open = (i + 1 == root.children.size());
        if (child.end_ms == kOpenEndMs && !may_be_open) return false;
        if (!span_consistent(child, 0.0, kOpenEndMs, may_be_open)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 1. Span nesting + reset.
// ---------------------------------------------------------------------------
static void test_nesting() {
    Collector c;
    c.begin(bake_trace::kSpanInstall);
    c.begin(bake_trace::kSpanPartBake);
    c.end();
    c.begin(bake_trace::kSpanMesh);
    c.end();
    c.end();
    c.end();  // unbalanced: must be a safe no-op

    Span s = c.snapshot();
    CHECK(snapshot_consistent(s), "nesting: snapshot consistent");
    CHECK(s.children.size() == 1, "nesting: one top-level span");
    if (s.children.size() == 1) {
        const Span& install = s.children[0];
        CHECK(install.name == bake_trace::kSpanInstall,
              "nesting: top span is kSpanInstall (pointer identity)");
        CHECK(install.end_ms != kOpenEndMs, "nesting: install closed");
        CHECK(install.children.size() == 2, "nesting: install has two children");
        if (install.children.size() == 2) {
            const Span& pb = install.children[0];
            const Span& mesh = install.children[1];
            CHECK(pb.name == bake_trace::kSpanPartBake,
                  "nesting: first child is kSpanPartBake");
            CHECK(mesh.name == bake_trace::kSpanMesh,
                  "nesting: second child is kSpanMesh");
            CHECK(install.begin_ms <= pb.begin_ms &&
                      pb.end_ms <= mesh.begin_ms &&
                      mesh.end_ms <= install.end_ms,
                  "nesting: sibling/parent times monotone");
        }
    }

    c.reset();
    Span after = c.snapshot();
    CHECK(after.children.empty() && after.counters.empty(),
          "nesting: reset drops all spans and counters");
}

// ---------------------------------------------------------------------------
// 2. Counters attach to the open span (root when none is open).
// ---------------------------------------------------------------------------
static void test_counters() {
    Collector c;
    c.count("pre", 1.0);  // no span open -> attaches to root
    c.begin(bake_trace::kSpanLod);
    c.count("tris_in", 100.0);
    c.begin(bake_trace::kSpanLodRung);
    c.count("tris_out", 40.0);
    c.count("keep_ratio", 0.4);
    c.end();
    c.count("tris_in", 200.0);  // back on the lod span after child closed
    c.end();

    Span s = c.snapshot();
    CHECK(snapshot_consistent(s), "counters: snapshot consistent");
    CHECK(s.counters.size() == 1 && std::strcmp(s.counters[0].name, "pre") == 0,
          "counters: rootless count lands on root span");
    CHECK(s.children.size() == 1, "counters: one top-level span");
    if (s.children.size() == 1) {
        const Span& lod = s.children[0];
        CHECK(lod.counters.size() == 2, "counters: lod span has two counters");
        if (lod.counters.size() == 2) {
            CHECK(std::strcmp(lod.counters[0].name, "tris_in") == 0 &&
                      lod.counters[0].value == 100.0,
                  "counters: first lod counter is tris_in=100");
            CHECK(lod.counters[1].value == 200.0,
                  "counters: post-child count re-attaches to reopened parent");
        }
        CHECK(lod.children.size() == 1 && lod.children[0].counters.size() == 2,
              "counters: rung span carries its own two counters");
        if (lod.children.size() == 1 && lod.children[0].counters.size() == 2) {
            CHECK(lod.children[0].counters[1].value == 0.4,
                  "counters: rung keep_ratio value preserved");
        }
    }
}

// ---------------------------------------------------------------------------
// 3a. Snapshot mid-write, deterministic: spans still open.
// ---------------------------------------------------------------------------
static void test_snapshot_open_spans() {
    Collector c;
    c.begin(bake_trace::kSpanInstall);
    c.begin(bake_trace::kSpanEval);
    c.count("n", 1.0);

    Span s = c.snapshot();  // writer "mid-bake": two spans open
    CHECK(snapshot_consistent(s), "open-snapshot: consistent");
    CHECK(s.children.size() == 1 && s.children[0].end_ms == kOpenEndMs,
          "open-snapshot: top span reported open");
    CHECK(s.children.size() == 1 && s.children[0].children.size() == 1 &&
              s.children[0].children[0].end_ms == kOpenEndMs,
          "open-snapshot: inner span reported open");

    c.end();
    c.end();
    Span done = c.snapshot();
    CHECK(snapshot_consistent(done), "open-snapshot: consistent after close");
    CHECK(done.children.size() == 1 && done.children[0].end_ms != kOpenEndMs,
          "open-snapshot: span closed after end()");
    // The earlier snapshot is an independent deep copy — still open in it.
    CHECK(s.children[0].end_ms == kOpenEndMs,
          "open-snapshot: earlier snapshot unaffected by later writes");
}

// ---------------------------------------------------------------------------
// 3b. Snapshot mid-write, concurrent: reader snapshots while the writer
//     appends; every snapshot must be a consistent tree.
// ---------------------------------------------------------------------------
static void test_snapshot_concurrent() {
    Collector c;
    std::atomic<bool> writer_done{false};
    std::atomic<int> iterations{0};

    std::thread writer([&] {
        bake_trace::set_current(&c);
        for (int i = 0; i < 3000; ++i) {
            BAKE_SPAN(bake_trace::kSpanPartBake);
            BAKE_COUNT("i", static_cast<double>(i));
            {
                BAKE_SPAN(bake_trace::kSpanMesh);
                BAKE_COUNT("tris", i * 3.0);
            }
            iterations.fetch_add(1, std::memory_order_relaxed);
        }
        bake_trace::set_current(nullptr);
        writer_done.store(true, std::memory_order_release);
    });

    int snaps = 0;
    int bad = 0;
    while (!writer_done.load(std::memory_order_acquire) || snaps < 50) {
        Span s = c.snapshot();
        if (!snapshot_consistent(s)) ++bad;
        ++snaps;
        if (snaps > 100000) break;  // safety valve
    }
    writer.join();

    CHECK(bad == 0, "concurrent: every mid-write snapshot consistent");
    Span final_snap = c.snapshot();
    CHECK(snapshot_consistent(final_snap), "concurrent: final snapshot consistent");
    CHECK(static_cast<int>(final_snap.children.size()) == iterations.load(),
          "concurrent: final snapshot has all writer spans");
    printf("  (concurrent: %d snapshots raced against 3000 spans)\n", snaps);
}

// ---------------------------------------------------------------------------
// 4. Thread-local current: per-thread, defaults to null on new threads.
// ---------------------------------------------------------------------------
static void test_thread_local_current() {
    Collector main_c;
    bake_trace::set_current(&main_c);
    CHECK(bake_trace::current() == &main_c, "tls: current set on main thread");

    Collector worker_c;
    bool worker_started_null = false;
    std::thread t([&] {
        worker_started_null = (bake_trace::current() == nullptr);
        bake_trace::set_current(&worker_c);
        {
            BAKE_SPAN(bake_trace::kSpanBuild);
            BAKE_COUNT("ops", 7.0);
        }
        bake_trace::set_current(nullptr);
    });
    t.join();

    CHECK(worker_started_null, "tls: new thread starts with null current");
    CHECK(bake_trace::current() == &main_c,
          "tls: worker's set_current did not leak into main thread");

    Span ws = worker_c.snapshot();
    CHECK(ws.children.size() == 1 &&
              ws.children[0].name == bake_trace::kSpanBuild &&
              ws.children[0].counters.size() == 1,
          "tls: worker spans landed in the worker's collector");
    Span ms = main_c.snapshot();
    CHECK(ms.children.empty(), "tls: main collector untouched by worker");
    bake_trace::set_current(nullptr);
}

// ---------------------------------------------------------------------------
// 5. No-op when no collector is current (and with explicit null collector).
// ---------------------------------------------------------------------------
static void test_noop_when_unset() {
    bake_trace::set_current(nullptr);
    CHECK(bake_trace::current() == nullptr, "noop: current is null");
    {
        BAKE_SPAN(bake_trace::kSpanInstall);   // must not crash
        BAKE_COUNT("tris", 42.0);              // must not crash
        BAKE_SPAN(static_cast<Collector*>(nullptr), bake_trace::kSpanMesh);
        BAKE_COUNT(static_cast<Collector*>(nullptr), "x", 1.0);
        bake_trace::Scope manual(bake_trace::kSpanEval);
    }
    Collector untouched;
    Span s = untouched.snapshot();
    CHECK(s.children.empty() && s.counters.empty(),
          "noop: nothing recorded anywhere while unset");
}

// ---------------------------------------------------------------------------
// 6. Overhead micro-benchmark: 10k spans (+1 counter each) with a collector
//    current, and the same loop with no collector (the production no-op
//    path). The <0.1%-of-bake target is judged by the printed numbers; the
//    asserts are loose so slow CI machines don't flake.
// ---------------------------------------------------------------------------
static void test_overhead() {
    using clock = std::chrono::steady_clock;
    constexpr int kSpans = 10000;

    Collector c;
    bake_trace::set_current(&c);
    auto t0 = clock::now();
    for (int i = 0; i < kSpans; ++i) {
        BAKE_SPAN(bake_trace::kSpanPartBake);
        BAKE_COUNT("tris", static_cast<double>(i));
    }
    double active_ms =
        std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    bake_trace::set_current(nullptr);

    t0 = clock::now();
    for (int i = 0; i < kSpans; ++i) {
        BAKE_SPAN(bake_trace::kSpanPartBake);
        BAKE_COUNT("tris", static_cast<double>(i));
    }
    double noop_ms =
        std::chrono::duration<double, std::milli>(clock::now() - t0).count();

    printf("  overhead: %d spans+counters active: %.3f ms (%.0f ns/span); "
           "no-collector: %.3f ms (%.1f ns/span)\n",
           kSpans, active_ms, active_ms * 1e6 / kSpans, noop_ms,
           noop_ms * 1e6 / kSpans);

    Span s = c.snapshot();
    CHECK(static_cast<int>(s.children.size()) == kSpans,
          "overhead: all 10k spans recorded");
    // Loose bounds: typical numbers are ~1-5 ms active, ~0.05 ms no-op.
    CHECK(active_ms < 250.0, "overhead: 10k active spans under 250 ms");
    CHECK(noop_ms < 50.0, "overhead: 10k no-op spans under 50 ms");
}

int main() {
    printf("bake_trace tests\n");
    test_nesting();
    test_counters();
    test_snapshot_open_spans();
    test_snapshot_concurrent();
    test_thread_local_current();
    test_noop_when_unset();
    test_overhead();
    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
