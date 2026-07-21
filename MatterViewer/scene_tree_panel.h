#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>

#include "editor_model.h"
#include "part_graph_snapshot.h"
#include "properties_panel.h"  // FieldCommands
#include "selection_set.h"
#include "console_panel.h"     // ConsoleLog
#include "matter/camera.h"
#include "matter/scene.h"      // matter::scene::SimulationMode

namespace matter { class WorldSession; }

namespace viewer {

struct SceneTreeState {
    char filter_text[256] = {};
    int filter_mode = 0;  // 0=All, 1=Entities, 2=Roots
    uint64_t cached_graph_gen = 0;
    part_graph_snapshot::Snapshot cached_snapshot;
    // Selected baked-root hash (0 = no root selected)
    uint64_t selected_root_hash = 0;
};

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
