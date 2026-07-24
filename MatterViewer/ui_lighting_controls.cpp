#include "ui.h"

namespace viewer {

void reset_lighting_controls(ViewerStats& stats) {
    stats.lighting = matter::VulkanLightingOverrides{};
}

void prepare_world_reload(ViewerStats& stats) {
    reset_lighting_controls(stats);
}

void complete_world_switch(ViewerStats& stats, bool succeeded) {
    if (succeeded) reset_lighting_controls(stats);
}

}  // namespace viewer
