#include "live_edit.h"

namespace live_edit {

std::set<PartId> LiveEditSession::upward_cone(const std::vector<PartId>& changed) const {
    std::set<PartId> cone(changed.begin(), changed.end());
    for (const auto& p : changed) {
        for (const auto& a : g_.ancestors(p)) cone.insert(a);
    }
    return cone;
}

RebuildReport LiveEditSession::run_rebuild(const std::set<std::string>&) {
    return RebuildReport{}; // filled in Task 5
}

RebuildReport LiveEditSession::tick() {
    return RebuildReport{}; // filled in Task 4
}

} // namespace live_edit
