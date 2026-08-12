#pragma once
#include "matter/camera.h"
#include "selection_set.h"

namespace matter { class WorldSession; }

namespace viewer {

// Draw wireframe AABB outlines for all selected objects using ImGui draw lists.
// Call this AFTER ImGui::NewFrame() and BEFORE ImGui::Render(), so lines appear
// in the foreground over the 3D viewport.
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
