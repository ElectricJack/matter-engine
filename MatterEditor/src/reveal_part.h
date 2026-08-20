#pragma once

// MatterEditor/src/reveal_part.h
//
// "Reveal" (Asset Browser -> viewer.reveal_part): select a module's baked
// root in the ACTIVE world. Kept as a free function over the graph snapshot
// so the module->selection rule is headless-testable and cannot drift from
// the Scene tree's baked-root rows, which record the identical
// SelectedObject{BakedRoot, resolved_hash} on click (scene_tree_panel.cpp).
//
// Scope. This writes the SelectionSet and nothing else. The rest of the Reveal
// behaviour — clearing the ECS entity selection, highlighting the Scene tree
// row, framing the camera, and reporting a miss to the Console rather than as
// an error — is composed by the `viewer::ViewerRevealPart` command handler in
// MatterEditor/src/main.cpp.
//
// `snapshot` is the caller's cached copy of the session's part graph,
// refreshed on the session's graph generation. Nothing here reads the live
// session, so calling with no world connected is fine: an empty snapshot
// simply returns 0. Called on the app thread from the command handler.

#include <cstdint>
#include <string>

#include "part_graph_snapshot.h"  // MatterEngine3/src (on the app include path)
#include "selection_set.h"

namespace viewer {

// Selects `module`'s baked root in `selection` and returns its resolved
// hash. Returns 0 and leaves the selection untouched when the module has no
// selectable root in `snapshot` — either it isn't part of the current world
// at all, or it only appears composed inside other parts (a non-root node
// has no world instance of its own to outline or focus).
uint64_t reveal_part_in_world(const part_graph_snapshot::Snapshot& snapshot,
                              const std::string& module,
                              SelectionSet& selection);

}  // namespace viewer
