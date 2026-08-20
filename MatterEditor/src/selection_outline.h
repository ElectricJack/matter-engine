#pragma once

// MatterEditor/src/selection_outline.h
//
// The editor's wireframe overlays: the selection boxes, and the frozen-cull
// frustum debug sketch. Two different transports, and the call ordering below
// is not interchangeable —
//
//  - `submit_selection_overlay_lines` stages world-space lines for the ENGINE
//    to draw depth-tested during the render pass, so it goes BEFORE
//    `WorldSession::render()`. The session clears its overlay buffer after
//    each render, so this must be resubmitted every frame it should be
//    visible.
//  - `draw_frozen_cull_frustum` (and the now-empty `draw_selection_outlines`)
//    paint straight onto ImGui's foreground draw list, so they go between
//    ImGui::NewFrame and ImGui::Render and always sit on top.
//
// Geometry comes from selection_bounds.h, which the viewport pick raycast also
// uses, so the drawn box and the clickable box are the same box. UI thread
// only; implementation in selection_outline.cpp.

#include "matter/camera.h"
#include "selection_set.h"

namespace matter { class WorldSession; }

namespace viewer {

// Submit world-space selection wireframe lines to the session's overlay buffer.
// Call BEFORE WorldSession::render() — the lines are drawn depth-tested into
// the HDR composite during the render pass.
void submit_selection_overlay_lines(const SelectionSet& selection,
                                    matter::WorldSession& session);

// Legacy ImGui path kept for the frozen-cull frustum and any future 2D-only
// overlays. Call AFTER ImGui::NewFrame() and BEFORE ImGui::Render().
//
// The body is currently EMPTY — it voids every argument and returns. Selection
// boxes are drawn by submit_selection_overlay_lines above, and the frozen-cull
// frustum has its own entry point below; this remains only as the hook for a
// future 2D-only overlay. main.cpp still calls it each frame, which costs
// nothing.
void draw_selection_outlines(const SelectionSet& selection,
                             const matter::CameraDesc& camera,
                             int fb_width, int fb_height,
                             matter::WorldSession& session,
                             float offset_x = 0.0f,
                             float offset_y = 0.0f);

// Outline the frozen cull frustum (ViewerStats::freeze_cull_camera) as seen
// from the live camera. Same call ordering as draw_selection_outlines.
//
// `depth_limit` truncates the drawn far face; the camera's real far plane is
// kilometres away and projects to a shape that reads as two parallel lines
// rather than as a frustum.
void draw_frozen_cull_frustum(const matter::CameraDesc& frozen,
                              const matter::CameraDesc& live,
                              int fb_width, int fb_height,
                              float depth_limit,
                              float offset_x = 0.0f,
                              float offset_y = 0.0f);

} // namespace viewer
