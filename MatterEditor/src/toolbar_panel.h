#pragma once

// MatterEditor/src/toolbar_panel.h
//
// The transport toolbar across the top of the editor, and the coloured border
// tint that marks a non-Edit viewport. Two immediate-mode draw helpers plus the
// small amount of state that has to survive between frames.
//
// This layer REPORTS, it does not decide: draw_toolbar_contents returns which
// buttons were clicked this frame and the caller applies them to the simulation
// mode. The mode is passed in only so the buttons can be enabled/disabled and
// the label drawn.
//
// Call site: `Ui::draw_toolbar` in `MatterEditor/src/ui.cpp` owns the
// ImGui::Begin/End for the toolbar window, calls draw_toolbar_contents inside
// it, then calls draw_viewport_border_tint with the viewport rect. Both
// functions must run inside an active ImGui frame, on the UI thread.
//
// Control surface: ToolbarState is bound as the `sim.time` property group
// (Scope::Session) in `MatterEditor/src/editor_props.cpp`, so headless QA can
// drive the slider over the FIFO with `set sim.time.time_scale 0.25`
// (docs/agent/control-surface.md). The slider and the property edit the same
// field; neither is a cache of the other.

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

// Slider clamp, as a multiplier on real time. editor_props.cpp reuses these
// two as the advertised range of the `sim.time.time_scale` property, so the
// panel and the FIFO/Tunables path clamp identically — change them here and
// both move together.
constexpr float kToolbarMinTimeScale = 0.05f;
constexpr float kToolbarMaxTimeScale = 2.0f;

// What the user asked for this frame. Edge-triggered: every flag is false in
// the common frame, and a set flag means "act once now" — the caller is
// expected to consume it immediately and never store the struct.
//
// A flag can be set with no button pressed: draw_toolbar_contents also handles
// the Space key and reports it as play_clicked / pause_clicked.
struct ToolbarActions {
    bool play_clicked = false;
    bool pause_clicked = false;
    bool step_clicked = false;
    bool stop_clicked = false;
};

// Draw the toolbar and return which actions were clicked.
//
// Must be called between ImGui::Begin/End for the toolbar window (the caller
// owns that). Edits `state.time_scale` in place as the slider moves. Also
// CONSUMES THE SPACE KEY as a play/pause toggle — suppressed while a text
// field has input focus, but otherwise global, so nothing else in the editor
// may claim Space.
ToolbarActions draw_toolbar_contents(ToolbarState& state,
                                     matter::scene::SimulationMode mode);

// Draw the viewport border tint overlay around the given region.
//
// No-op in Edit mode; green in Play, amber in Pause. The rect is in ImGui
// screen coordinates (pixels, origin top-left). Drawn on the FOREGROUND draw
// list, so it sits above every window including anything docked over the
// viewport, and it is not clipped to the toolbar's window.
void draw_viewport_border_tint(matter::scene::SimulationMode mode,
                               float vp_x, float vp_y, float vp_w, float vp_h);

} // namespace viewer
