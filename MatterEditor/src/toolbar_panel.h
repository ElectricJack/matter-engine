#pragma once
#include "matter/scene.h"

namespace viewer {

struct ToolbarState {
    // Slow-motion inspection. Scales the frame delta fed to the tick
    // accumulator, NOT TickDesc::fixed_delta_seconds: the fixed timestep stays
    // exactly 1/60 so physics and fixed-cadence animation keep their
    // deterministic step size and simply occur less often. Scaling the fixed
    // delta instead would change simulation behaviour, not its rate.
    float time_scale = 1.0f;
};

constexpr float kToolbarMinTimeScale = 0.05f;
constexpr float kToolbarMaxTimeScale = 2.0f;

struct ToolbarActions {
    bool play_clicked = false;
    bool pause_clicked = false;
    bool step_clicked = false;
    bool stop_clicked = false;
};

// Draw the toolbar and return which actions were clicked.
ToolbarActions draw_toolbar_contents(ToolbarState& state,
                                     matter::scene::SimulationMode mode);

// Draw the viewport border tint overlay around the given region.
void draw_viewport_border_tint(matter::scene::SimulationMode mode,
                               float vp_x, float vp_y, float vp_w, float vp_h);

} // namespace viewer
