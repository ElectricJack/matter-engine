// bake_trace.h — hierarchical span + counter tracing for the bake pipeline.
// Bake Lab foundation; see docs/bake-lab.md §II.1.
//
// One Collector per bake run. The thread running the bake is the single
// writer (begin/count/end); readers (viewer UI, tests) call snapshot(),
// which deep-copies the span tree under the collector's mutex, so a
// snapshot taken while the writer is mid-append is always a consistent
// tree. Writer calls take the same mutex — uncontended, this is a handful
// of nanoseconds and keeps the reader/writer contract trivially correct.
//
// Deep call sites need no collector plumbing: the bake entry point calls
// set_current(&collector) (thread-local), and BAKE_SPAN / BAKE_COUNT pick
// up whatever collector is current on this thread. Every API here is a
// safe no-op when no collector is current, so instrumented code costs
// (nearly) nothing outside bakes.
//
// Span names must come from bake_trace_names.h constants, never string
// literals at call sites — names are the diff / variant-table key.

#pragma once

#include <chrono>
#include <mutex>
#include <vector>

namespace bake_trace {

struct Counter {
    const char* name;
    double value;
};

// end_ms value of a span that is still open (in a snapshot taken mid-write).
constexpr double kOpenEndMs = -1.0;

// Name of the implicit root span returned by Collector::snapshot().
constexpr const char* kRootName = "root";

// One node of the span tree. Times are milliseconds on steady_clock,
// relative to the collector's start (construction or last reset()).
struct Span {
    const char* name = nullptr;   // from bake_trace_names.h constants
    double begin_ms = 0.0;
    double end_ms = kOpenEndMs;
    std::vector<Counter> counters;
    std::vector<Span> children;
};

class Collector {
public:
    Collector();

    // Writer API — single-writer discipline: all begin/count/end calls come
    // from one thread (the thread running the bake).
    void begin(const char* name);            // open a child of the open span
    void count(const char* name, double v);  // attach a counter to the open
                                             // span (root if none is open)
    // Close the innermost open span (safe no-op when none is open). Returns
    // the closed span's duration in ms — exactly end_ms - begin_ms as stored
    // on the span — so writers can record phase timings without re-walking
    // the tree via snapshot(); 0.0 on the no-op path.
    double end();

    // Reader API — deep copy of the whole tree under the mutex; safe to call
    // from any thread while the writer is appending. The returned root span
    // (name kRootName, begin_ms 0) has end_ms = time of the snapshot; spans
    // still open at snapshot time keep end_ms == kOpenEndMs.
    Span snapshot() const;

    // Writer-side: drop all spans/counters and restart the clock.
    void reset();

private:
    double now_ms() const;

    mutable std::mutex mutex_;
    Span root_;
    // Open-span chain, innermost last; empty means root_ is the open span.
    // Pointers stay valid because begin() only ever appends to the innermost
    // open span's children, and no element of that vector is itself open.
    std::vector<Span*> open_;
    std::chrono::steady_clock::time_point start_;
};

// Thread-local "current collector" so deep call sites (lod_bake,
// part_flatten, script_host) need no plumbing: set_current(&c) at bake
// entry; spans nest into whatever is current; nullptr (the default on
// every thread) makes all macro sites no-ops.
void       set_current(Collector* c);
Collector* current();

// RAII span. Two forms: Scope(name) uses the thread-local current collector
// (spec §I.6 usage); Scope(collector, name) targets an explicit collector
// (spec §II.1 usage). A null collector makes the scope a no-op.
class Scope {
public:
    explicit Scope(const char* name) : Scope(current(), name) {}
    Scope(Collector* c, const char* name) : c_(c) {
        if (c_) c_->begin(name);
    }
    ~Scope() {
        if (c_) c_->end();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Collector* c_;
};

// Counter helpers backing BAKE_COUNT; both forms are null/unset-safe.
inline void count(Collector* c, const char* name, double v) {
    if (c) c->count(name, v);
}
inline void count(const char* name, double v) {
    count(current(), name, v);
}

}  // namespace bake_trace

// Macros. Variadic pass-through supports both call forms:
//   BAKE_SPAN(kSpanMesh);            BAKE_SPAN(&collector, kSpanMesh);
//   BAKE_COUNT("tris", n);           BAKE_COUNT(&collector, "tris", n);
#define BAKE_TRACE_CONCAT2(a, b) a##b
#define BAKE_TRACE_CONCAT(a, b) BAKE_TRACE_CONCAT2(a, b)
#define BAKE_SPAN(...) \
    ::bake_trace::Scope BAKE_TRACE_CONCAT(_bt_scope_, __LINE__)(__VA_ARGS__)
#define BAKE_COUNT(...) ::bake_trace::count(__VA_ARGS__)
