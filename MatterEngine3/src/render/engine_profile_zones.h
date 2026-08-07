#pragma once

// Engine zone ids for the renderer build region, mapped onto libs/ProfileLib.
// This replaces the render-thread-only viewer::vk_build_profile enum: the zone
// NAMES are identical (so existing analysis and the [vk.build]-style output read
// the same), but they now resolve to ProfileLib zone ids and flow through the
// one shared, frame-correlated profiler.
//
// See docs/superpowers/specs/2026-08-07-engine-profiler-design.md.
//
// Usage mirrors the old code -- a named RAII scope, optionally .stop()'d early:
//   matter::profile::Scope s(engine_prof::id(engine_prof::kUiLayout));
//
// When MATTER_PROFILE_ENABLED == 0, id() returns 0 and references no zone-name
// string, so the whole thing compiles out with the macros.

#include "profile.h"

namespace engine_prof {

enum Zone {
    kTemporalAlign = 0,
    kTemporalFill,
    kTemporalMap,
    kTemporalHistory,
    kSetTemporalCompare,
    kSetTemporalCopy,
    kUiFastPath,
    kUiBuildLoop,
    kUiCompare,
    kUiLayout,
    kUiSnapshot,
    kPfFlushTemplate,
    kPfDynamic,
    kPfAnimBounds,
    kPfVtSlots,
    kPfUploadStatic,
    kPfUploadInstances,
    kPfUploadCommands,
    kPfUploadOther,
    kPfStats,
    kPfRetain,
    // P1: the previously-unmeasured per-sector executor cost -- stage_load's LOD
    // ladder ("rebuilding closer clusters") and the VkScenePart prebuild. These
    // run on the bake worker; ProfileLib attributes their time to whichever
    // frame is current when they end.
    kStageLoad,
    kScenePartPrebuild,
    kZoneCount
};

#if MATTER_PROFILE_ENABLED
// Register all zone names once, in enum order, and return stable ids. The names
// match the former vk_build_profile::zone_name table exactly.
inline int id(Zone z) {
    static const int ids[kZoneCount] = {
        ::matter::profile::register_zone("temporal.align"),
        ::matter::profile::register_zone("temporal.fill"),
        ::matter::profile::register_zone("temporal.map"),
        ::matter::profile::register_zone("temporal.hist"),
        ::matter::profile::register_zone("settemporal.cmp"),
        ::matter::profile::register_zone("settemporal.copy"),
        ::matter::profile::register_zone("ui.fastpath"),
        ::matter::profile::register_zone("ui.loop"),
        ::matter::profile::register_zone("ui.compare"),
        ::matter::profile::register_zone("ui.layout"),
        ::matter::profile::register_zone("ui.snapshot"),
        ::matter::profile::register_zone("pf.flushtmpl"),
        ::matter::profile::register_zone("pf.dynamic"),
        ::matter::profile::register_zone("pf.animbounds"),
        ::matter::profile::register_zone("pf.vtslots"),
        ::matter::profile::register_zone("pf.static"),
        ::matter::profile::register_zone("pf.instances"),
        ::matter::profile::register_zone("pf.commands"),
        ::matter::profile::register_zone("pf.other"),
        ::matter::profile::register_zone("pf.stats"),
        ::matter::profile::register_zone("pf.retain"),
        ::matter::profile::register_zone("bake.stageload"),
        ::matter::profile::register_zone("bake.prebuild"),
    };
    return ids[z];
}
#else
inline int id(Zone) { return 0; }
#endif

}  // namespace engine_prof
