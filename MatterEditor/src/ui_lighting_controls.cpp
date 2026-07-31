#include "ui.h"

namespace viewer {

void reset_lighting_controls(ViewerStats& stats) {
    stats.lighting = matter::VulkanLightingOverrides{};
}

// The reload / world-switch seams drop every Scope::World group's backing
// struct to its compiled default (property-system design S4 layer 1). Without
// this the outgoing world's live edits would still be sitting in the struct
// when EditorProps::on_world_connected captures the incoming world's baseline,
// and they would be indistinguishable from authored values from then on.
void reset_world_scope_controls(ViewerStats& stats) {
    stats.lighting = matter::VulkanLightingOverrides{};
    stats.volumetrics = matter::VulkanVolumetricsSettings{};
    stats.tileset_pom = matter::TilesetPomSettings{};
    // render.fog. Reseeded from the INCOMING world's authored fog at the next
    // BakeFinished (main.cpp), before on_world_connected captures the baseline
    // — dropping it to the compiled default here is what guarantees the
    // outgoing world's fog edits cannot be mistaken for authored values.
    stats.fog = matter::FogSettings{};
}

void prepare_world_reload(ViewerStats& stats) {
    reset_world_scope_controls(stats);
}

void complete_world_switch(ViewerStats& stats, bool succeeded) {
    if (succeeded) reset_world_scope_controls(stats);
}

}  // namespace viewer
