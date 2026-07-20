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
                             matter::WorldSession& session);

} // namespace viewer
