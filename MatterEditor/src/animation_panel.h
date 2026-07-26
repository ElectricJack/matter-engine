#pragma once

// Part Workbench animation tabs — ImGui drawing over AnimationPanelModel.
//
// All presentation logic lives in animation_panel_model.{h,cpp}, which has no
// ImGui dependency and is covered by tests/test_animation_panel_model.cpp. This
// file is deliberately thin: it renders the model's rows and forwards the one
// interactive affordance (a target gizmo edit) to the caller.

#include "animation_panel_model.h"
#include "animation_debug_overlay.h"

#include <cstdint>
#include <functional>

namespace viewer {

// The write path for the Targets tab. The panel only ever calls these for a row
// whose gizmo_enabled is true; a controller-driven target is drawn disabled,
// because one-driver arbitration rejects the write anyway.
//
// The panel resolves nothing itself -- the host owns the session and forwards to
// WorldSession::set_animation_target_transform / snap_animation_target, so this
// file stays free of any engine-mutation surface. Both return the engine's
// verdict so the panel can show a rejection instead of pretending it landed.
struct AnimationTargetWriter {
    std::function<bool(matter::AnimatorInstanceHandle, const char* target_name,
                       const matter::AnimationTransform& desired)> set_transform;
    std::function<bool(matter::AnimatorInstanceHandle, const char* target_name)> snap;

    bool bound() const { return static_cast<bool>(set_transform); }
};

// Draws the six observational tabs: Rig, Skin, Clips, Graph, Targets, Render.
// `overlay` is the same options struct the viewport overlay draws with, so the
// Render tab can toggle visualization without a second source of truth.
void draw_animation_panel(AnimationPanelModel& model,
                          AnimationDebugOverlayOptions& overlay,
                          const AnimationTargetWriter& writer = {});

} // namespace viewer
