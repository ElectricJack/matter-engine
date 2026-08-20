// MatterEditor/src/reveal_part.cpp
//
// Implementation of the one rule behind Asset Browser "Reveal" /
// `viewer.reveal_part` — the contract is in reveal_part.h.
//
// Deliberately narrow: this writes the SelectionSet and nothing else.
// Everything else the Reveal command performs — clearing the EditorModel's
// entity selection, marking the Scene tree's baked-root row
// (`ui.select_baked_root`), framing the camera via
// `focus_camera_on_selection`, and turning a 0 return into a Console message —
// is composed by the `viewer::ViewerRevealPart` handler in
// MatterEditor/src/main.cpp. Keeping those out of here is what lets
// MatterEditor/tests/test_workbench_actions.cpp assert the module -> selection
// rule with no editor, no session and no ImGui.

#include "reveal_part.h"

namespace viewer {

uint64_t reveal_part_in_world(const part_graph_snapshot::Snapshot& snapshot,
                              const std::string& module,
                              SelectionSet& selection) {
    const auto it = snapshot.nodes.find(module);
    if (it == snapshot.nodes.end()) return 0;
    const part_graph_snapshot::Node& node = it->second;
    // Two ways there is nothing to reveal, both reported as 0: a non-root node
    // only ever appears composed inside other parts, so it has no world
    // instance of its own to outline or focus; and a node with no resolved
    // hash has nothing addressable to select. The caller tells these apart
    // from "module not in this world at all" by re-testing `snapshot.nodes`
    // itself — see the ViewerRevealPart handler in main.cpp.
    if (!node.is_root || node.resolved_hash == 0) return 0;
    selection.replace(SelectedObject{SelectedObject::BakedRoot, node.resolved_hash});
    return node.resolved_hash;
}

}  // namespace viewer
