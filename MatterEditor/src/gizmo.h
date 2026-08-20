#pragma once

// MatterEditor/src/gizmo.h
//
// Drawn over the 3D viewport by the editor's UI pass, once per frame, after
// ImGuizmo::BeginFrame(). All state that survives a frame is the one enum in
// GizmoState — the handle's position comes from the selection and the
// FieldCommands getters every frame, never from a cache here.
//
// Task 10 — Transform gizmo: renders an ImGuizmo translate/rotate/scale
// handle over the viewport for the primary selected ECS entity and writes
// manipulated values back through FieldCommands (LocalTransform.translation
// /rotation/scale). See gizmo.cpp for the matrix-convention notes.

#include "matter/camera.h"
#include "matter/math_types.h"
#include "matter/scene.h"
#include "selection_set.h"
#include "properties_panel.h"  // for FieldCommands

namespace viewer {

// Which handle the gizmo shows. Maps 1:1 onto ImGuizmo::TRANSLATE / ROTATE /
// SCALE; the mapping lives in gizmo.cpp so ImGuizmo.h stays out of this header.
enum class GizmoOperation { Translate, Rotate, Scale };

// The gizmo's entire persistent state. Owned by the editor's Ui and changed
// only by the toolbar buttons and update_gizmo_hotkeys().
struct GizmoState {
    GizmoOperation operation = GizmoOperation::Translate;
};

// Draws a transform gizmo for the primary selection, if any, and writes
// manipulated values back via `fields`. Returns true if a gizmo was drawn
// this frame (regardless of whether it was actively being dragged) — callers
// should feed this into Ui::gizmo_submitted_ so camera_input_allowed() can
// suppress camera input while ImGuizmo::IsOver()/IsUsing().
//
// No-op (returns false) when:
//   - mode == SimulationMode::Play
//   - the selection is empty
//   - the primary selection is a BakedRoot (gizmo only edits ECS entities)
//
// Must be called inside the ImGui frame, after ImGuizmo::BeginFrame().
//
// Also returns false when the primary entity has no readable
// LocalTransform.translation/rotation (a missing scale defaults to 1,1,1).
//
// Multi-selection: a TRANSLATE drag fans the primary's translation delta out
// to every other selected entity, added to each one's own
// LocalTransform.translation. Rotate and scale affect the primary only.
//
// The viewport_* arguments are the 3D view's rect in ImGui display
// coordinates, matching what ImGuizmo::SetRect expects.
bool draw_gizmo(GizmoState& state, const SelectionSet& selection,
                const FieldCommands& fields, const matter::CameraDesc& camera,
                matter::scene::SimulationMode mode, float viewport_x,
                float viewport_y, float viewport_w, float viewport_h);

// Handle gizmo mode hotkeys: G or T => Translate, R => Rotate, S => Scale.
// Call only when ImGui does not want text/keyboard capture (e.g.
// !io.WantTextInput && !io.WantCaptureKeyboard), so typing in a text field
// doesn't accidentally retarget the gizmo.
void update_gizmo_hotkeys(GizmoState& state);

} // namespace viewer
