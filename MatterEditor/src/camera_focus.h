#pragma once

// Task 13 — "Focus" camera routine: frames the camera on the merged
// world-space AABB of the current selection. Instant snap (no animation) for
// this first pass; see camera_focus.cpp for the AABB/FOV math.

#include <functional>

#include "matter/camera.h"
#include "matter/math_types.h"
#include "selection_set.h"
#include "selection_bounds.h"  // SelectionBounds for the baked-root provider
#include "properties_panel.h"  // for FieldCommands

namespace viewer {

// Bounds source for BakedRoot selections. A std::function rather than a
// WorldSession* keeps this TU session-free (headless-testable); the live
// caller wraps bounds_for_object over the production session.
using BakedRootBoundsFn =
    std::function<bool(uint64_t part_hash, SelectionBounds& out)>;

// Frames `camera` on the merged world-space AABB of the selection.
//
// Entity selections contribute a default 1m cube (0.5m half-extent) centered
// on LocalTransform.translation — part-aware bounds are future work, since
// FieldCommands has no part-AABB accessor yet. BakedRoot selections
// contribute their instance's real OBB when `baked_bounds` is supplied
// (viewer.reveal_part passes the session-backed provider) and are skipped
// otherwise. If the selection resolves to no bounds at all, the camera is
// left unchanged.
void focus_camera_on_selection(matter::CameraDesc& camera,
                               const SelectionSet& selection,
                               const FieldCommands& fields,
                               const BakedRootBoundsFn& baked_bounds = {});

// The centre of that same merged AABB, plus the radius of its bounding sphere.
// Returns false when the selection is empty or nothing in it resolves to
// bounds (exactly the cases where focus_camera_on_selection leaves the camera
// alone).
//
// focus_camera_on_selection is now a thin wrapper over this, which is the
// point: the "Orbit selection" pivot (issue a4203d22 part 1) and the F-key
// focus must never disagree about where the selection is, and the only way to
// guarantee that is for both to read the same merge rather than for the orbit
// to recompute a centroid of its own.
bool selection_focus_point(const SelectionSet& selection,
                           const FieldCommands& fields,
                           const BakedRootBoundsFn& baked_bounds,
                           matter::Float3& out_center, float& out_radius);

} // namespace viewer
