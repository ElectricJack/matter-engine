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

// Is `resolved_hash` a baked root that this world can still name?  The one
// liveness rule behind a BakedRoot selection entry, kept beside the rule that
// CREATES those entries so the two cannot disagree.
//
// The population is exactly the root nodes of the current graph snapshot --
// the same set `scene_tree_panel.cpp`'s "[Baked]" rows, `reveal_part_in_world`
// above and `inventory::find_object` (scene.list_objects / scene.get_object)
// enumerate.  PLACEMENT is deliberately not part of it: a root that is in the
// part graph but placed nowhere in this world is a real, selectable object
// that `scene.get_object` answers `found:true` for with
// `placement.available:false` (docs/agent/agent-protocol.md, "Availability is
// never faked").  Testing placement here instead is what made the editor
// silently drop such a root one frame after an agent's `selection.replace` --
// and after a user's click on its Scene-tree row -- while still reporting the
// mutation as `changed:true`.
//
// Staleness is still caught: a rebake republishes the graph with new resolved
// hashes and a world switch resets the snapshot, so an old hash stops matching.
bool baked_root_selectable(const part_graph_snapshot::Snapshot& snapshot,
                           uint64_t resolved_hash);

}  // namespace viewer
