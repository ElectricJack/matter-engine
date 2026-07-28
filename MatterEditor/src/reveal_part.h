#pragma once

// "Reveal" (Asset Browser -> viewer.reveal_part): select a module's baked
// root in the ACTIVE world. Kept as a free function over the graph snapshot
// so the module->selection rule is headless-testable and cannot drift from
// the Scene tree's baked-root rows, which record the identical
// SelectedObject{BakedRoot, resolved_hash} on click (scene_tree_panel.cpp).

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
