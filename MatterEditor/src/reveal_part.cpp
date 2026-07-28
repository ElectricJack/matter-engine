#include "reveal_part.h"

namespace viewer {

uint64_t reveal_part_in_world(const part_graph_snapshot::Snapshot& snapshot,
                              const std::string& module,
                              SelectionSet& selection) {
    const auto it = snapshot.nodes.find(module);
    if (it == snapshot.nodes.end()) return 0;
    const part_graph_snapshot::Node& node = it->second;
    if (!node.is_root || node.resolved_hash == 0) return 0;
    selection.replace(SelectedObject{SelectedObject::BakedRoot, node.resolved_hash});
    return node.resolved_hash;
}

}  // namespace viewer
