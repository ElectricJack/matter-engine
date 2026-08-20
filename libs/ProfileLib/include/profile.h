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

// libs/ProfileLib/include/profile.h
//
// Dependencies: none. ProfileLib is a leaf — <atomic> and <cstdint> here,
// <chrono>/<mutex>/<vector>/<cstdio> in the .cpp, and nothing from the repo.
// Both MatterEngine3 and MatterEditor compile it in directly.
//
// Integration points (the whole wiring, in four lines):
//   - `MatterEngine3/src/render/vk_scene_renderer.cpp` calls PROFILE_FRAME()
//     once per rendered frame. Nothing else may call it — the frame boundary is
//     what every accumulator is swept into.
//   - `MatterEngine3/src/matter_engine.cpp` calls set_thread_lane(kLaneWorker)
//     on each bake worker as it starts. Everything else is the render lane by
//     default.
//   - MatterEditor reads the ring: the Performance/Memory panels via
//     copy_recent/frame_stats, `src/main.cpp` dumps a trace on exit when
//     MATTER_PROFILE_TRACE=<path> is set, and `src/issue_reporter.cpp` attaches
//     one to every filed issue.
//   - Call sites anywhere else only ever use the PROFILE_* macros at the bottom.
//
// Environment:
//   MATTER_PROFILE=0        start disabled (anything else, or unset, is on)
//   MATTER_PROFILE_LOG=1    periodic aggregate stderr report, ~every 2 s
//   MATTER_PROFILE_TRACE=P  MatterEditor dumps a Chrome trace to P on exit
// All three are read exactly once, at first use of any profile function. Setting
// them later in the process has no effect.
//
// There is no object to construct or own. Every function below acts on one
// process-global registry created lazily on first use and intentionally never
// destroyed, so a scope closing during static destruction is still safe. Zone
// and counter ids are stable for the process lifetime and are not portable
// between runs (they are assigned in first-seen order).
//
// Capacity: at most kMaxZones distinct zone names and kMaxCounters counters.
// Registration past either cap does not grow and does not fail — it returns the
// LAST slot, so every further name silently merges into one zone. If a profile
// looks like one huge mystery zone, check zone_count() against kMaxZones first.
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
// Monotonic (steady_clock); the epoch is arbitrary, so only differences are
// meaningful. Never wall-clock time, and never affected by system clock changes.
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
// `name` is COPIED into a fixed 64-byte slot, so transient strings are fine —
// but names are truncated at 63 characters and two names agreeing in their
// first 63 collapse into one zone. Every call takes a global mutex and does a
// linear strcmp scan, so registering inside a loop instead of caching the id
// serializes threads on a lock; the PROFILE_* macros cache for you.
// `zone_name` returns "?" for an id outside the registered range.
int register_zone(const char* name);
const char* zone_name(int zone);
int zone_count();

// Add nanoseconds to a zone's accumulator for the frame in progress. Public so
// GPU-zone readback (resolved a frame or two late) and worker jobs can deposit
// directly; the RAII Scope is the common path.
// The single sink every timing path funnels through, and therefore also where a
// zone's LANE is fixed (see below). Time lands in whichever frame is in
// progress at the moment of the call, which is what makes cross-thread
// attribution work — and also means a scope straddling a frame boundary is
// attributed entirely to the later frame, and a GPU zone resolved a frame or
// two late lands on the frame it was deposited in, not the frame it ran in.
// Out-of-range zone ids are dropped silently.
void add_ns(int zone, uint64_t ns);

// Scope nesting. scope_enter records `zone`'s parent as whatever scope is
// currently open on THIS thread (tracked in a thread-local stack) and returns
// the previous open zone so scope_exit can restore it. A zone's parent is
// recorded once, on first sight. zone_parent returns -1 for a root zone. This
// is what makes a PROFILE_SCOPE opened inside another render as its child.
// Call these only through `Scope` — they must be paired, and `scope_exit` takes
// the token `scope_enter` returned, not a zone id.
//
// The parent map is display metadata, recorded once and never revised: a zone
// entered from two different call sites keeps whichever parent it saw first, so
// the tree is one plausible nesting rather than the full call graph. A zone
// entered recursively is not recorded as its own parent, but its time IS added
// once per level, so a recursive zone's total exceeds its wall time.
int scope_enter(int zone);
void scope_exit(int previous);
int zone_parent(int zone);

// ---------------------------------------------------------------------------
// Lane attribution -- which THREAD a zone's time was spent on.
//
// Every thread that records scopes belongs to a lane: the render/critical-path
// lane (the frame's real cost) or the background worker lane (the bake pool --
// up to a dozen threads whose SUMMED time is emphatically not frame time). A
// thread declares its lane once at startup with set_thread_lane; an untagged
// thread defaults to the render lane, so the app/render thread needs no call.
//
// A zone's lane is recorded once, on first sight, from the lane of the thread
// that runs it -- exactly like zone_parent. Because scope nesting is
// thread-local, a zone and its parent always share a lane, so the display can
// cleanly split the frame tree (render) from the background tree (workers)
// instead of guessing from the "bake." name prefix. This is what lets the panel
// stop reporting 12 parallel worker threads' summed CPU as ">100% of a frame".
// Three properties of this model that determine how the numbers must be read:
//
//  - First deposit wins. A zone's lane is fixed by the FIRST thread ever to
//    deposit time into it and is never revised. A zone genuinely run on both
//    lanes lands wholly on whichever ran first, so the same helper called from
//    the render thread and from a bake worker will be misfiled for one of them.
//    Give such a helper two zone names if the split matters.
//  - The worker lane is a SUM ACROSS THREADS, not an elapsed time. Add up a
//    dozen bake workers' zones and the total can exceed the frame's wall time
//    several times over; that is correct, not a bug. Use
//    `lane_thread_count(kLaneWorker)` to say "spread over N threads" rather
//    than presenting it as frame cost. Only the render lane is comparable to
//    `FrameStats::wall_ms`.
//  - `lane_thread_count` counts a thread on its FIRST `set_thread_lane` call
//    only. Re-tagging a thread to a different lane moves its future zones but
//    does not move the count, and the count never decreases when a thread
//    exits, so a pool that churns threads inflates it.
enum Lane { kLaneRender = 0, kLaneWorker = 1, kLaneCount = 2 };
void set_thread_lane(int lane);
int zone_lane(int zone);          // recorded lane; kLaneRender if never seen
int lane_thread_count(int lane);  // distinct threads that tagged this lane

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
    uint64_t frame_index = 0;   // monotonic; 0 is the first frame ever marked
    uint64_t wall_ns = 0;       // real time since the previous frame_mark; 0 on the first
    uint64_t zone_ns[kMaxZones] = {};       // indexed by zone id; sums BOTH lanes
    uint64_t counter[kMaxCounters] = {};    // indexed by counter id; per-frame tallies
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
// `copy_recent` takes the ring lock and memcpys whole records — sizeof
// (FrameRecord) is over a kilobyte, so asking for the full kFrameHistory copies
// roughly 0.7 MB. Fine for a panel refresh or a report dump, not for a hot
// path. Returns fewer than `max` (possibly 0) before the ring has filled.
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
// Computed over the WHOLE resident ring, not a fixed time window — at 8 ms a
// frame that is roughly the last 4 seconds, at 33 ms roughly the last 17, so
// the averaging window silently stretches as the frame time gets worse.
// Allocates and sorts (O(n log n) over up to kFrameHistory samples), so it is a
// panel-refresh call, not a per-frame one. `budget_ms <= 0` disables the
// `over_budget` tally; an empty ring returns the all-zero default with
// `samples == 0`.
FrameStats frame_stats(double budget_ms);

// Serialize the resident FrameRecord history to a Chrome-trace / Perfetto JSON
// file (loads directly in chrome://tracing). Zones are laid out per frame on a
// synthetic timeline built from wall_ns, split across a "render" lane and a
// "bake" lane (by each zone's RECORDED LANE -- see zone_lane above -- NOT by a
// name prefix; the prefix heuristic is exactly what the lane model replaced),
// with frame_ms and each counter emitted as
// counter tracks so jitter/spikes are visible. Returns false if the file cannot
// be opened. Safe when compiled out / never enabled -- it just writes an empty
// trace. This is the tail persisted with each issue-report screenshot.
bool dump_chrome_trace(const char* path);

// ---------------------------------------------------------------------------
// RAII scope. Construction snapshots the clock iff enabled; destruction adds the
// elapsed ns to the zone. Non-copyable, non-movable.
// ---------------------------------------------------------------------------
// Cost when enabled: one clock read and one thread-local store on construction,
// one clock read plus one relaxed fetch_add and one thread-local store on exit.
// Cost when `enabled()` is false at construction: one relaxed atomic load, and
// the scope stays inert for its whole life — toggling the profiler on mid-scope
// does not retroactively start it.
//
// `stop()` is idempotent and is what `PROFILE_SCOPE_NAMED` uses to close a
// region early; the destructor calls it again harmlessly. Non-copyable and
// non-movable, so a Scope cannot outlive or escape the block it times.
class Scope {
public:
#if MATTER_PROFILE_ENABLED
    explicit Scope(int zone) : zone_(zone), active_(enabled()) {
        if (active_) {
            start_ = now_ns();
            prev_ = scope_enter(zone);
        }
    }
    ~Scope() { stop(); }
    void stop() {
        if (!active_) return;
        active_ = false;
        add_ns(zone_, now_ns() - start_);
        scope_exit(prev_);
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
    int prev_ = -1;
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

// A named scope for sequential regions where a plain block won't do (variables
// declared in one region are used in the next). Declare it, then call
// `var.stop()` at the region boundary; the next PROFILE_SCOPE_NAMED opens as a
// sibling under the same parent. On an early return/throw the RAII dtor stops it
// anyway. Compiles out to an empty Scope (no zone string, no register_zone).
#define PROFILE_SCOPE_NAMED(var, name)                                    \
    static const int MATTER_PROFILE_CONCAT(_prof_nz_, var) =              \
        ::matter::profile::register_zone(name);                           \
    ::matter::profile::Scope var(MATTER_PROFILE_CONCAT(_prof_nz_, var))

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
#define PROFILE_SCOPE_NAMED(var, name) ::matter::profile::Scope var(0)
#define PROFILE_FRAME() ((void)0)
#define PROFILE_COUNT(name, n) ((void)0)

#endif
