#pragma once
#include "matter/scene.h"

namespace viewer {

struct ToolbarState {
};

struct ToolbarActions {
    bool play_clicked = false;
    bool pause_clicked = false;
    bool step_clicked = false;
    bool stop_clicked = false;
};

// Draw the toolbar and return which actions were clicked.
ToolbarActions draw_toolbar_contents(ToolbarState& state,
                                     matter::scene::SimulationMode mode);

// Draw the viewport border tint overlay.
void draw_viewport_border_tint(matter::scene::SimulationMode mode);

} // namespace viewer
