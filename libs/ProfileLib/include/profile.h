#pragma once

// ProfileLib -- one shared, frame-correlated profiler for the engine.
//
// Design: docs/superpowers/specs/2026-08-07-engine-profiler-design.md
//
// This is the P0 core: a shared clock, a zone registry, RAII scopes that
// accumulate per-zone time, a per-frame sweep into a bounded FrameRecord ring,
// and the compile-out switch. It subsumes the render-thread-only
// viewer::vk_build_profile (folded in at P1) and adds cross-thread attribution
// (a bake-worker scope's time lands in whichever frame is current when it ends)
// plus the FrameRecord history the editor window and the report tail read.
//
// COMPILE-OUT (the load-bearing requirement). When MATTER_PROFILE_ENABLED == 0
// every macro below expands to `((void)0)`: no Scope object, no zone-name string
// literal, no timer, nothing to link. The perf build keeps it on (see the spec's
// build-topology note); only `make dist` compiles it out.
//
// OBSERVER EFFECT. The hot path is a monotonic clock read plus one relaxed
// atomic fetch_add per scope EXIT -- no I/O, no lock, no formatting. All
// serialisation happens on the frame sweep / dump only. This preserves the
// discipline that vk_build_profile established after per-item stderr logging was
// measured to halve fill throughput (docs/sector-bake-time-findings-2026-07-30).

#ifndef MATTER_PROFILE_ENABLED
#define MATTER_PROFILE_ENABLED 1
#endif

#include <atomic>
#include <cstdint>

namespace matter {
namespace profile {

// ---------------------------------------------------------------------------
// Capacities. MAX_ZONES bounds builtin enum zones + interned string zones.
// FRAME_HISTORY is the FrameRecord ring depth (~512 frames ~= 8-17 s), the tail
// persisted with reports and the window the stats are computed over.
// ---------------------------------------------------------------------------
constexpr int kMaxZones = 128;
constexpr int kMaxCounters = 32;
constexpr int kFrameHistory = 512;

// One monotonic source for CPU scopes, the frame clock, and (P2) GPU/worker
// event stamps, so every lane lines up on the same timeline.
uint64_t now_ns();

// Runtime enable: only meaningful when compiled in. Starts from the
// MATTER_PROFILE env var (unset/"1" => on, "0" => off) and is togglable from the
// editor window. When off, Scope construction reads one cached bool and stores
// nothing -- vk_build_profile's cost model, preserved.
bool enabled();
void set_enabled(bool on);

// Register (or look up) a zone by name; returns its stable id. Interned under a
// mutex on FIRST sight only -- call sites cache the id in a function-local
// static (see PROFILE_SCOPE), so steady state is a plain integer. Returns a
// clamped fallback id if the table is full rather than growing unboundedly.
int register_zone(const char* name);
const char* zone_name(int zone);
int zone_count();

// Add nanoseconds to a zone's accumulator for the frame in progress. Public so
// GPU-zone readback (resolved a frame or two late) and worker jobs can deposit
// directly; the RAII Scope is the common path.
void add_ns(int zone, uint64_t ns);

// Counters: per-frame event tallies (layout rebuilds, draw calls, jobs run),
// distinct from time zones. Same intern-once model as zones.
int register_counter(const char* name);
const char* counter_name(int counter);
int counter_count();
void add_count(int counter, uint64_t n);

// ---------------------------------------------------------------------------
// FrameRecord: what one frame cost, produced by frame_mark() from the swept
// accumulators. zone_ns is indexed by zone id. wall_ns is the real time between
// this frame_mark and the previous one (the true frame time, independent of the
// attributed sum).
// ---------------------------------------------------------------------------
struct FrameRecord {
    uint64_t frame_index = 0;
    uint64_t wall_ns = 0;
    uint64_t zone_ns[kMaxZones] = {};
    uint64_t counter[kMaxCounters] = {};
    // Scene scale tags (optional; set via set_frame_counts).
    uint64_t instances = 0;
    uint64_t clusters = 0;
    uint64_t parts = 0;
    uint64_t commands = 0;
};

// Close the current frame: compute wall_ns from the shared clock, sweep every
// zone accumulator (atomic exchange to 0) into a fresh FrameRecord, push it to
// the ring, advance the frame index. Call once per rendered frame on the render
// thread. No-op (beyond advancing nothing) when disabled.
void frame_mark();

// Optional per-frame scene-scale tags, folded into the NEXT frame_mark's record.
void set_frame_counts(uint64_t instances, uint64_t clusters, uint64_t parts,
                      uint64_t commands);

// Ring access for the editor window / report tail. copy_recent fills `out` (up
// to `max`) with the most recent records, newest last, and returns the count.
uint64_t frame_index();
int copy_recent(FrameRecord* out, int max);

// Whole-frame wall-time stats over the resident history, in milliseconds.
struct FrameStats {
    int samples = 0;
    double last_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
    double mean_ms = 0.0;
    double stddev_ms = 0.0;   // jitter
    double p99_ms = 0.0;
    int over_budget = 0;      // frames whose wall_ms exceeded budget_ms
    double smoothness = 1.0;  // 1 - clamp(p99/median - 1): 1.0 = perfectly even
};
FrameStats frame_stats(double budget_ms);

// Serialize the resident FrameRecord history to a Chrome-trace / Perfetto JSON
// file (loads directly in chrome://tracing). Zones are laid out per frame on a
// synthetic timeline built from wall_ns, split across a "render" lane and a
// "bake" lane (by zone-name prefix), with frame_ms and each counter emitted as
// counter tracks so jitter/spikes are visible. Returns false if the file cannot
// be opened. Safe when compiled out / never enabled -- it just writes an empty
// trace. This is the tail persisted with each issue-report screenshot.
bool dump_chrome_trace(const char* path);

// ---------------------------------------------------------------------------
// RAII scope. Construction snapshots the clock iff enabled; destruction adds the
// elapsed ns to the zone. Non-copyable, non-movable.
// ---------------------------------------------------------------------------
class Scope {
public:
#if MATTER_PROFILE_ENABLED
    explicit Scope(int zone) : zone_(zone), active_(enabled()) {
        if (active_) start_ = now_ns();
    }
    ~Scope() { stop(); }
    void stop() {
        if (!active_) return;
        active_ = false;
        add_ns(zone_, now_ns() - start_);
    }
#else
    explicit Scope(int) {}
    void stop() {}
#endif
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
#if MATTER_PROFILE_ENABLED
    int zone_ = 0;
    bool active_ = false;
    uint64_t start_ = 0;
#endif
};

}  // namespace profile
}  // namespace matter

// ---------------------------------------------------------------------------
// Macros -- the only surface call sites use. Compile to nothing when disabled.
// PROFILE_SCOPE caches the interned zone id in a function-local static so the
// hash/intern runs once per call site; PROFILE_SCOPE_ID takes a pre-declared
// enum id and skips even that.
// ---------------------------------------------------------------------------
#define MATTER_PROFILE_CONCAT_(a, b) a##b
#define MATTER_PROFILE_CONCAT(a, b) MATTER_PROFILE_CONCAT_(a, b)

#if MATTER_PROFILE_ENABLED

#define PROFILE_SCOPE(name)                                              \
    static const int MATTER_PROFILE_CONCAT(_prof_zone_, __LINE__) =      \
        ::matter::profile::register_zone(name);                         \
    ::matter::profile::Scope MATTER_PROFILE_CONCAT(_prof_scope_, __LINE__)( \
        MATTER_PROFILE_CONCAT(_prof_zone_, __LINE__))

#define PROFILE_SCOPE_ID(zone)                                          \
    ::matter::profile::Scope MATTER_PROFILE_CONCAT(_prof_scope_, __LINE__)( \
        static_cast<int>(zone))

#define PROFILE_FRAME() ::matter::profile::frame_mark()

#define PROFILE_COUNT(name, n)                                            \
    do {                                                                  \
        static const int MATTER_PROFILE_CONCAT(_prof_ctr_, __LINE__) =    \
            ::matter::profile::register_counter(name);                    \
        ::matter::profile::add_count(                                     \
            MATTER_PROFILE_CONCAT(_prof_ctr_, __LINE__),                  \
            static_cast<uint64_t>(n));                                    \
    } while (0)

#else

#define PROFILE_SCOPE(name) ((void)0)
#define PROFILE_SCOPE_ID(zone) ((void)0)
#define PROFILE_FRAME() ((void)0)
#define PROFILE_COUNT(name, n) ((void)0)

#endif
