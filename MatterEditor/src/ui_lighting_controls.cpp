// MatterEditor/src/ui_lighting_controls.cpp
//
// The world-scope reset seams: dropping every Scope::World property group's
// backing struct in ViewerStats back to its compiled default (property-system
// design S4, layer 1). Called at the world reload and world switch boundaries
// by main.cpp, so that EditorProps::on_world_connected captures the INCOMING
// world's authored values as layer 2 with no residue from the outgoing one.
//
// WHY ITS OWN TRANSLATION UNIT. This is not a panel and there is no ImGui here
// on purpose: MatterEditor/Makefile compiles this file directly into
// build/windows/vulkan_smoke_tests.exe, which cannot link ui.cpp. Adding an
// ImGui or panel dependency here breaks that build, not this one — keep it to
// plain struct assignment.
//
// Also note that MatterEngine3/tests/property_editor_tests.cpp asserts on this
// file's SOURCE TEXT (it greps for the individual `stats.<field> = ...{};`
// lines to prove no lighting lane was forgotten). Adding a lane means adding
// the literal assignment here, not a loop or a helper that hides it.
#include "ui.h"

namespace viewer {

// Lighting-only reset, used where the atmosphere presentation lanes must go
// back to their defaults without touching fog / POM / VT.
void reset_lighting_controls(ViewerStats& stats) {
    // Whole-struct reset is intentional: it restores all four atmosphere
    // presentation lanes atomically with their backwards-compatible defaults.
    stats.lighting = matter::VulkanLightingOverrides{};
    stats.atmosphere = matter::AtmosphereSettings{};
    stats.volumetrics = matter::VulkanVolumetricsSettings{};
    stats.cloud_shadows = matter::CloudShadowSettings{};
}

// The reload / world-switch seams drop every Scope::World group's backing
// struct to its compiled default (property-system design S4 layer 1). Without
// this the outgoing world's live edits would still be sitting in the struct
// when EditorProps::on_world_connected captures the incoming world's baseline,
// and they would be indistinguishable from authored values from then on.
void reset_world_scope_controls(ViewerStats& stats) {
    stats.lighting = matter::VulkanLightingOverrides{};
    stats.atmosphere = matter::AtmosphereSettings{};
    stats.volumetrics = matter::VulkanVolumetricsSettings{};
    stats.cloud_shadows = matter::CloudShadowSettings{};
    stats.tileset_pom = matter::TilesetPomSettings{};
    stats.vt_near_band = matter::VtNearBandSettings{};
    // render.fog. Reseeded from the INCOMING world's authored fog at the next
    // BakeFinished (main.cpp), before on_world_connected captures the baseline
    // — dropping it to the compiled default here is what guarantees the
    // outgoing world's fog edits cannot be mistaken for authored values.
    stats.fog = matter::FogSettings{};
}

// The two seam wrappers. They are deliberately named for the seam rather than
// the action, so main.cpp's call sites read as "what happened" — and so the
// switch case can decline the reset: a FAILED world switch leaves the current
// world's live edits alone, because that world is still the one on screen.
void prepare_world_reload(ViewerStats& stats) {
    reset_world_scope_controls(stats);
}

void complete_world_switch(ViewerStats& stats, bool succeeded) {
    if (succeeded) reset_world_scope_controls(stats);
}

}  // namespace viewer
