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

// Invoked when an author drags a target gizmo. The panel only ever calls this
// for a row whose gizmo_enabled is true; a controller-driven target is drawn
// disabled, because one-driver arbitration would reject the write.
//
// The panel does not touch AnimationService itself -- the host (main.cpp) owns
// the session and performs the set_transform, so this file stays free of any
// engine-mutation surface.
using AnimationTargetEdit =
    std::function<void(uint64_t resolved_hash, uint16_t target_index,
                       const matter::AnimationTransform& desired)>;

// Draws the six observational tabs: Rig, Skin, Clips, Graph, Targets, Render.
// `overlay` is the same options struct the viewport overlay draws with, so the
// Render tab can toggle visualization without a second source of truth.
void draw_animation_panel(AnimationPanelModel& model,
                          AnimationDebugOverlayOptions& overlay,
                          const AnimationTargetEdit& edit_target = {});

} // namespace viewer
