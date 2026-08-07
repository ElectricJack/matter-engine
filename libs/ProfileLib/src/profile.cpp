// ProfileLib core implementation. See include/profile.h and
// docs/superpowers/specs/2026-08-07-engine-profiler-design.md.
//
// When MATTER_PROFILE_ENABLED == 0 the macros expand to nothing, so nothing here
// is CALLED in a compiled-out build -- but the TU still compiles cleanly, and
// the header's Scope has an empty inline body, so a dist build links none of
// this in via the macros. (The library archive may still be built; the point of
// the switch is that call sites emit no references to it.)

#include "profile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace matter {
namespace profile {

namespace {

struct Registry {
    // Zone names, copied in so callers may pass transient strings. Fixed pool;
    // register_zone clamps to the last slot if exhausted rather than growing.
    char names[kMaxZones][64] = {};
    std::atomic<int> count{0};
    std::mutex register_mutex;

    // Per-zone accumulators for the frame in progress. Relaxed atomics: any
    // thread (render or bake worker) may fetch_add on scope exit; frame_mark
    // exchanges to 0 on the render thread.
    std::atomic<uint64_t> zone_ns[kMaxZones] = {};

    // FrameRecord history ring, written only by frame_mark. Guarded so a report
    // dump / editor read from another thread is safe.
    std::mutex ring_mutex;
    FrameRecord ring[kFrameHistory];
    int ring_head = 0;   // next write slot
    int ring_count = 0;  // valid entries, capped at kFrameHistory
    uint64_t frame_counter = 0;
    uint64_t last_mark_ns = 0;
    bool have_last_mark = false;

    // Scene-scale tags staged for the next frame_mark.
    uint64_t pending_instances = 0;
    uint64_t pending_clusters = 0;
    uint64_t pending_parts = 0;
    uint64_t pending_commands = 0;

    std::atomic<bool> enabled_flag{true};
};

Registry& reg() {
    static Registry* r = [] {
        Registry* fresh = new Registry();
        const char* env = std::getenv("MATTER_PROFILE");
        const bool on = env == nullptr || env[0] == '\0' || env[0] != '0';
        fresh->enabled_flag.store(on, std::memory_order_relaxed);
        return fresh;
    }();
    return *r;
}

}  // namespace

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool enabled() { return reg().enabled_flag.load(std::memory_order_relaxed); }
void set_enabled(bool on) {
    reg().enabled_flag.store(on, std::memory_order_relaxed);
}

int register_zone(const char* name) {
    if (name == nullptr) name = "?";
    Registry& r = reg();
    std::lock_guard<std::mutex> guard(r.register_mutex);
    const int have = r.count.load(std::memory_order_relaxed);
    for (int i = 0; i < have; ++i) {
        if (std::strcmp(r.names[i], name) == 0) return i;
    }
    if (have >= kMaxZones) return kMaxZones - 1;  // clamp; never grow unbounded
    std::strncpy(r.names[have], name, sizeof(r.names[have]) - 1);
    r.names[have][sizeof(r.names[have]) - 1] = '\0';
    r.count.store(have + 1, std::memory_order_release);
    return have;
}

const char* zone_name(int zone) {
    Registry& r = reg();
    if (zone < 0 || zone >= r.count.load(std::memory_order_acquire)) return "?";
    return r.names[zone];
}

int zone_count() { return reg().count.load(std::memory_order_acquire); }

void add_ns(int zone, uint64_t ns) {
    if (zone < 0 || zone >= kMaxZones) return;
    reg().zone_ns[zone].fetch_add(ns, std::memory_order_relaxed);
}

void set_frame_counts(uint64_t instances, uint64_t clusters, uint64_t parts,
                      uint64_t commands) {
    Registry& r = reg();
    r.pending_instances = instances;
    r.pending_clusters = clusters;
    r.pending_parts = parts;
    r.pending_commands = commands;
}

void frame_mark() {
    Registry& r = reg();
    const uint64_t t = now_ns();
    FrameRecord record;
    if (r.have_last_mark) record.wall_ns = t - r.last_mark_ns;
    r.last_mark_ns = t;
    r.have_last_mark = true;

    // Sweep the accumulators regardless of enabled state so a disable->enable
    // toggle never carries a stale partial sum into the first live frame.
    const int zones = r.count.load(std::memory_order_acquire);
    for (int i = 0; i < zones && i < kMaxZones; ++i) {
        record.zone_ns[i] = r.zone_ns[i].exchange(0, std::memory_order_relaxed);
    }
    record.frame_index = r.frame_counter;
    record.instances = r.pending_instances;
    record.clusters = r.pending_clusters;
    record.parts = r.pending_parts;
    record.commands = r.pending_commands;

    {
        std::lock_guard<std::mutex> guard(r.ring_mutex);
        r.ring[r.ring_head] = record;
        r.ring_head = (r.ring_head + 1) % kFrameHistory;
        if (r.ring_count < kFrameHistory) ++r.ring_count;
    }
    ++r.frame_counter;
}

uint64_t frame_index() { return reg().frame_counter; }

int copy_recent(FrameRecord* out, int max) {
    if (out == nullptr || max <= 0) return 0;
    Registry& r = reg();
    std::lock_guard<std::mutex> guard(r.ring_mutex);
    const int n = std::min(max, r.ring_count);
    // Oldest-to-newest ending at the most recent write. ring_head points at the
    // next (empty) slot, so the newest valid entry is ring_head-1.
    for (int i = 0; i < n; ++i) {
        const int slot =
            (r.ring_head - n + i + 2 * kFrameHistory) % kFrameHistory;
        out[i] = r.ring[slot];
    }
    return n;
}

FrameStats frame_stats(double budget_ms) {
    FrameStats stats;
    Registry& r = reg();
    std::vector<double> ms;
    {
        std::lock_guard<std::mutex> guard(r.ring_mutex);
        if (r.ring_count == 0) return stats;
        ms.reserve(r.ring_count);
        for (int i = 0; i < r.ring_count; ++i) {
            const int slot =
                (r.ring_head - r.ring_count + i + 2 * kFrameHistory) %
                kFrameHistory;
            ms.push_back(static_cast<double>(r.ring[slot].wall_ns) / 1.0e6);
        }
    }
    stats.samples = static_cast<int>(ms.size());
    stats.last_ms = ms.back();
    double sum = 0.0;
    stats.min_ms = ms[0];
    stats.max_ms = ms[0];
    for (double v : ms) {
        sum += v;
        stats.min_ms = std::min(stats.min_ms, v);
        stats.max_ms = std::max(stats.max_ms, v);
        if (budget_ms > 0.0 && v > budget_ms) ++stats.over_budget;
    }
    stats.mean_ms = sum / stats.samples;
    double var = 0.0;
    for (double v : ms) var += (v - stats.mean_ms) * (v - stats.mean_ms);
    stats.stddev_ms = std::sqrt(var / stats.samples);
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    const auto pct = [&](double p) {
        if (sorted.empty()) return 0.0;
        int idx = static_cast<int>(p * (sorted.size() - 1) + 0.5);
        idx = std::max(0, std::min(idx, static_cast<int>(sorted.size()) - 1));
        return sorted[idx];
    };
    stats.p99_ms = pct(0.99);
    const double median = pct(0.5);
    // Smoothness: 1.0 when p99 == median (perfectly even), falling as the tail
    // stretches past the middle. Clamped to [0,1].
    if (median > 0.0) {
        const double ratio = stats.p99_ms / median - 1.0;
        stats.smoothness = std::max(0.0, std::min(1.0, 1.0 - ratio));
    }
    return stats;
}

}  // namespace matter::profile
}  // namespace matter
