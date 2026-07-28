#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>

#include "editor_model.h"
#include "scene_tree_model.h"  // SceneTreeState + graph-cache sync/reset
#include "properties_panel.h"  // FieldCommands
#include "selection_set.h"
#include "console_panel.h"     // ConsoleLog
#include "matter/camera.h"
#include "matter/scene.h"      // matter::scene::SimulationMode

namespace viewer {

// Draw the unified scene tree: baked roots from the cached graph snapshot,
// and ECS entities from `editor`. `authored_entity_ids`, when non-null, is
// the set of entity ids present in the last Edit-mode SimulationControl
// snapshot; entities absent from it are tagged [Runtime] (play-mode spawns).
// Null means "no simulation snapshot yet" -- every entity renders as [Entity].
//
// Task 13 additions: right-click on a row opens a context menu (Focus,
// Add Child Entity, Duplicate, Delete for entities; Focus, Open Source for
// baked roots). `commands` supplies the mutation callbacks (nullable — menu
// items that need it are simply omitted/disabled when null); `mode` disables
// destructive items (Delete, Duplicate) during Play; `camera`/`fields` drive
// the Focus action (see camera_focus.h); `selection` is updated so the
// Properties panel / gizmo stay in sync with context-menu-driven selection
// changes; `console_log`, when non-null, receives error messages from failed
// mutations.
void draw_scene_tree(SceneTreeState& state,
                     EditorModel& editor,
                     matter::WorldSession* session,
                     SceneCommands* commands,
                     matter::scene::SimulationMode mode,
                     matter::CameraDesc* camera,
                     SelectionSet* selection,
                     const FieldCommands* fields,
                     ConsoleLog* console_log,
                     const std::unordered_set<uint64_t>* authored_entity_ids = nullptr);

} // namespace viewer
