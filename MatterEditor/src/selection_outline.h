#pragma once
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
