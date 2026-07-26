#pragma once

// Task 13 — "Focus" camera routine: frames the camera on the merged
// world-space AABB of the current selection. Instant snap (no animation) for
// this first pass; see camera_focus.cpp for the AABB/FOV math.

#include "matter/camera.h"
#include "matter/math_types.h"
#include "selection_set.h"
#include "properties_panel.h"  // for FieldCommands

namespace viewer {

// Frames `camera` on the merged AABB of `selection`'s entities. Baked-root
// selections contribute no bounds (no transform data is available for them
// yet) and are skipped; if the selection is empty or resolves to zero
// entities with a known LocalTransform, the camera is left unchanged.
//
// Each entity contributes a default 1m cube (0.5m half-extent) centered on
// its LocalTransform.translation — part-aware bounds are future work, since
// FieldCommands has no part-AABB accessor yet.
void focus_camera_on_selection(matter::CameraDesc& camera,
                               const SelectionSet& selection,
                               const FieldCommands& fields);

} // namespace viewer
