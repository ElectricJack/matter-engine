#pragma once

// Sector-streaming RUNTIME settings — the MATTER_STREAM_* vars that are
// consumed once per PROCESS rather than once per world connect, consolidated
// the same way matter/vt_budgets.h consolidated the VT residency budgets.
//
// WHY THESE TWO ARE NOT IN StreamingLodPrefs. The editor's stream.lod override
// (MatterEditor/src/streaming_lod_prefs.h) is a per-WORLD, RequiresReload
// group: everything in it is read by make_streaming_profile at the next
// connect, so an Apply & Reload genuinely re-applies it. The two knobs here
// are not:
//
//   * workers  — WorldSession::Impl::ensure_bake_pool_started spawns the
//                executor pool ONCE and never resizes it. A world reload
//                quiesces the pool (worker_loop) but does not shut it down, so
//                a reload would NOT re-read this. Promising RequiresReload
//                would be a lie; it is ReadOnly, displayed with its env source.
//   * prebuild — latched into a function-local static at the first sector bake
//                (matter_engine.cpp bake_and_stage_sector) and then read from
//                several executor threads per sector. Also ReadOnly, which is
//                what keeps that concurrent read race-free: nothing writes the
//                struct after the single-threaded env pass below.
//
// Both therefore follow the vt_budgets.h ReadOnly discipline: the value is
// visible in Tunables with its env var named, and changing it means relaunching
// with the env var set. Everything else about them is unchanged.
//
// The remaining MATTER_STREAM_* vars are deliberately NOT here — they are
// profiling counters and A/B path switches (FILL_PROFILE, PUBLISH_PROFILE,
// BAKE_PROFILE, STAGE_FROM_MEMORY, STAGE_VERIFY, PREBUILD_VERIFY, FIRST_RUNG,
// NO_EVICT, SKIP_PART_WRITE), not settings anyone tunes from a panel.

#include "matter/props.h"

#include <algorithm>
#include <cstdint>
#include <thread>

namespace matter {

struct StreamRuntimeSettings {
    // Sector-bake executor threads. 0 means "not resolved yet"; the effective
    // machine-scaled default is written by ensure_stream_runtime_env_applied
    // BEFORE the env layer, so the struct always shows the count the pool will
    // actually use rather than a sentinel.
    uint32_t workers = 0;
    // False restores the old build-the-VkScenePart-inside-the-GL-job path, so
    // the two can be diffed pixel-for-pixel from one binary.
    bool prebuild = true;
};

inline StreamRuntimeSettings& stream_runtime_settings() {
    static StreamRuntimeSettings s;
    return s;
}

// cores/2, capped at 12 — the 2026-07-30 measurement (see the long note at
// ensure_bake_pool_started). Kept here so the schema's displayed default and
// the pool's actual default are one expression.
inline uint32_t stream_default_workers() {
    const unsigned hw = std::thread::hardware_concurrency();
    return static_cast<uint32_t>(
        std::max(1, std::min(12, static_cast<int>(hw > 0 ? hw / 2 : 1))));
}

inline const props::Group& stream_runtime_group() {
    using props::prop;
    static const auto def = props::group<StreamRuntimeSettings>(
        "stream.runtime", "Streaming Runtime",
        prop(&StreamRuntimeSettings::workers, "workers")
            .label("Bake workers").range(1.0f, 32.0f)
            .env("MATTER_STREAM_WORKERS").read_only()
            .doc("Sector-bake executor threads. The pool is spawned once per "
                 "process and never resized, so this is set at launch via "
                 "MATTER_STREAM_WORKERS. Default is cores/2 capped at 12."),
        prop(&StreamRuntimeSettings::prebuild, "prebuild")
            .label("Prebuild scene parts")
            .env("MATTER_STREAM_PREBUILD").read_only()
            .doc("Off builds each streamed sector's VkScenePart inside the GL "
                 "publish job instead of on the bake executor. Latched at the "
                 "first sector bake — set MATTER_STREAM_PREBUILD=0 to A/B it."));
    return def.group();
}

// Layer 5 for engine-standalone paths, and the seeding of `workers`. Idempotent
// and safe to repeat after the editor's own apply_env — both write the same
// values from the same environment. MUST run before the first read of either
// field; the editor calls it before binding, the engine before its own use.
inline void ensure_stream_runtime_env_applied() {
    static const bool once = [] {
        StreamRuntimeSettings& s = stream_runtime_settings();
        if (s.workers == 0) s.workers = stream_default_workers();
        props::apply_env(&s, stream_runtime_group());
        return true;
    }();
    (void)once;
}

}  // namespace matter
