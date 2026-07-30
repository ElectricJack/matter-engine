#pragma once

// Fine-grained CPU attribution for the Vulkan renderer's per-FRAME "build"
// region -- the span matter_engine.cpp reports as `stats.build_ms` and the
// [stream.frame] telemetry line prints as `build=`. During a StreamMountain
// sector fill that region measured 32-87 ms per frame over ~90k resolved
// instances, which is the throughput ceiling of the whole fill (publishes are
// pumped once per frame). See docs/sector-bake-time-findings-2026-07-30.md.
//
// Everything here is OFF unless MATTER_VK_BUILD_PROFILE=1 is set in the
// environment. The env var is read exactly once into a function-local static.
// When enabled, counters accumulate into a plain per-process struct and are
// rendered as ONE aggregate line every kPrintIntervalMs -- deliberately never
// per frame and never per instance: this codebase has a measured case where
// per-item stderr logging halved fill throughput and inverted a conclusion
// (see the same findings doc, "Per-sector stderr logging perturbs the run").
//
// Threading: the build region runs entirely on the render thread, and so does
// every writer below. No synchronisation, no atomics -- the cost when disabled
// is one predictable branch on a cached bool.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace viewer {

namespace vk_build_profile {

enum Zone : int {
    // viewer::TemporalState::begin() -- history bookkeeping over every
    // instance, called from matter_engine's begin_temporal lambda.
    kTemporalAlign = 0,   // the "is the id sequence unchanged" prefix compare
    kTemporalFill,        // (ids, values) table fill for the next presented set
    kTemporalMap,         // TransformTable::build_map()
    kTemporalHistory,     // TemporalFrame::instances fill + per-id history find
    // VkSceneRenderer::set_temporal_frame()
    kSetTemporalCompare,  // per-entry "did the history the build loop reads change"
    kSetTemporalCopy,     // temporal_frame_ = frame (deep copy of the above)
    // VkSceneRenderer::update_instances()
    kUiFastPath,          // unchanged-input snapshot compare
    kUiBuildLoop,         // the per-instance candidate build (slot lookup + pack)
    kUiCompare,           // candidate vs staging equality / layout compare
    kUiLayout,            // rebuild_command_template()
    kUiSnapshot,          // snapshot_instance_inputs()
    // VkSceneRenderer::prepare_frame()
    kPfFlushTemplate,     // deferred rebuild_command_template for new parts
    kPfDynamic,           // dynamic-instance merge + apply_dynamic_command_layout
    kPfAnimBounds,        // animation bounds gather + upload
    kPfVtSlots,           // rebuild_vt_draw_slots + its upload
    kPfUploadStatic,      // cluster/vertex/index (append or full rewrite)
    kPfUploadInstances,   // the GpuInstance buffer memcpy
    kPfUploadCommands,    // the DrawCommand template memcpy (+ mirror copies)
    kPfUploadOther,       // materials, frame constants, buffer sizing
    kPfStats,             // stats readback / clear
    kPfRetain,            // retain_for_frame lifetime bookkeeping
    kZoneCount
};

inline const char* zone_name(int zone) {
    static const char* kNames[kZoneCount] = {
        "temporal.align",  "temporal.fill",   "temporal.map",
        "temporal.hist",   "settemporal.cmp", "settemporal.copy",
        "ui.fastpath",     "ui.loop",         "ui.compare",
        "ui.layout",       "ui.snapshot",     "pf.flushtmpl",
        "pf.dynamic",      "pf.animbounds",   "pf.vtslots",
        "pf.static",       "pf.instances",    "pf.commands",
        "pf.other",        "pf.stats",        "pf.retain"};
    return (zone >= 0 && zone < kZoneCount) ? kNames[zone] : "?";
}

// Aggregate window: one stderr line at most this often.
constexpr int64_t kPrintIntervalMs = 2000;

struct State {
    bool enabled = false;
    uint64_t frames = 0;
    uint64_t zone_us[kZoneCount]{};
    uint64_t instances = 0;
    uint64_t clusters = 0;
    uint64_t parts = 0;
    uint64_t commands = 0;
    uint64_t layout_rebuilds = 0;
    std::chrono::steady_clock::time_point last_print{};
    bool last_print_valid = false;
};

inline State& state() {
    static State s = [] {
        State fresh;
        const char* value = std::getenv("MATTER_VK_BUILD_PROFILE");
        fresh.enabled = value != nullptr && value[0] != '\0' && value[0] != '0';
        return fresh;
    }();
    return s;
}

inline bool enabled() { return state().enabled; }

inline void add_us(int zone, uint64_t microseconds) {
    State& s = state();
    if (!s.enabled || zone < 0 || zone >= kZoneCount) return;
    s.zone_us[zone] += microseconds;
}

// RAII timer. Constructing one while disabled reads a bool and stores nothing.
class Scope {
public:
    explicit Scope(int zone) : zone_(zone), active_(enabled()) {
        if (active_) start_ = std::chrono::steady_clock::now();
    }
    ~Scope() { stop(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void stop() {
        if (!active_) return;
        active_ = false;
        add_us(zone_, static_cast<uint64_t>(
                          std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - start_)
                              .count()));
    }

private:
    int zone_;
    bool active_;
    std::chrono::steady_clock::time_point start_{};
};

// Called once per rendered frame at the end of the build region (the tail of
// VkSceneRenderer::prepare_frame). Renders at most one line per interval.
inline void frame_end(uint64_t instances, uint64_t clusters, uint64_t parts,
                      uint64_t commands) {
    State& s = state();
    if (!s.enabled) return;
    ++s.frames;
    s.instances += instances;
    s.clusters += clusters;
    s.parts += parts;
    s.commands += commands;
    const auto now = std::chrono::steady_clock::now();
    if (!s.last_print_valid) {
        s.last_print = now;
        s.last_print_valid = true;
        return;
    }
    const int64_t elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - s.last_print)
            .count();
    if (elapsed_ms < kPrintIntervalMs || s.frames == 0) return;
    const double frames = static_cast<double>(s.frames);
    double total_ms = 0.0;
    for (int zone = 0; zone < kZoneCount; ++zone)
        total_ms += static_cast<double>(s.zone_us[zone]) / 1000.0 / frames;
    std::fprintf(stderr,
                 "[vk.build] frames=%llu instances=%llu clusters=%llu "
                 "parts=%llu commands=%llu layout=%llu attributed=%.1f ms/frame\n",
                 (unsigned long long)s.frames,
                 (unsigned long long)(s.instances / s.frames),
                 (unsigned long long)(s.clusters / s.frames),
                 (unsigned long long)(s.parts / s.frames),
                 (unsigned long long)(s.commands / s.frames),
                 (unsigned long long)s.layout_rebuilds, total_ms);
    for (int zone = 0; zone < kZoneCount; ++zone) {
        const double ms = static_cast<double>(s.zone_us[zone]) / 1000.0 / frames;
        if (ms < 0.005) continue;
        std::fprintf(stderr, "[vk.build]   %-16s %6.2f ms  %4.1f%%\n",
                     zone_name(zone), ms,
                     total_ms > 0.0 ? 100.0 * ms / total_ms : 0.0);
    }
    s.frames = 0;
    s.instances = 0;
    s.clusters = 0;
    s.parts = 0;
    s.commands = 0;
    s.layout_rebuilds = 0;
    for (int zone = 0; zone < kZoneCount; ++zone) s.zone_us[zone] = 0;
    s.last_print = now;
}

inline void note_layout_rebuild() {
    State& s = state();
    if (s.enabled) ++s.layout_rebuilds;
}

}  // namespace vk_build_profile

namespace vk_perf {

// Geometric pre-growth for the large vectors the build region refills every
// frame.
//
// std::vector's copy-assign, assign(), and reserve() all allocate EXACTLY the
// requested element count when they have to grow. A streaming fill adds
// instances every frame, so `v.reserve(n)` / `v = other` with an n that ticks
// upward reallocates on every single frame: capacity lands exactly on size,
// and the next frame is one element too large again. At ~90k instances those
// are 8-15 MB blocks, which on Windows go to the large-block allocator and get
// decommitted on free -- so each frame also eats a soft page fault per 4 KB of
// the fresh block. push_back's own doubling never kicks in because reserve()
// already sized the buffer to the exact final count.
//
// Rounding the request up to at least twice the current capacity restores the
// amortised-O(1) growth these vectors are supposed to have: O(log N)
// reallocations over a fill instead of O(N).
//
// This only ever changes a vector's CAPACITY. Sizes, contents, iteration order
// and every observable value are untouched, so it cannot move a pixel.
// MATTER_VK_VECTOR_GROWTH=0 falls back to the exact-size requests.
inline bool geometric_growth_enabled() {
    static const bool value = [] {
        const char* env = std::getenv("MATTER_VK_VECTOR_GROWTH");
        return env == nullptr || env[0] == '\0' || env[0] != '0';
    }();
    return value;
}

template <typename T, typename A>
inline void reserve_geometric(std::vector<T, A>& target, std::size_t count) {
    if (count <= target.capacity()) return;
    if (!geometric_growth_enabled()) {
        target.reserve(count);
        return;
    }
    const std::size_t doubled = target.capacity() * 2;
    target.reserve(count > doubled ? count : doubled);
}

// resize() that grows geometrically. Same contract as reserve_geometric: only
// capacity behaviour differs from a plain resize().
template <typename T, typename A>
inline void resize_geometric(std::vector<T, A>& target, std::size_t count) {
    reserve_geometric(target, count);
    target.resize(count);
}

}  // namespace vk_perf

}  // namespace viewer
