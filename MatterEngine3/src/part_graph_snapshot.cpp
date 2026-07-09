// Implementation of non-trivial Snapshot methods (Task 9).
#include "part_graph_snapshot.h"

namespace part_graph_snapshot {

// Reverse-edge helper: scan all nodes for those whose children list contains `module`.
std::vector<std::string> Snapshot::parents_of(const std::string& module) const {
    std::vector<std::string> result;
    for (const auto& kv : nodes) {
        for (const auto& child : kv.second.children) {
            if (child == module) { result.push_back(kv.first); break; }
        }
    }
    return result;
}

} // namespace part_graph_snapshot
