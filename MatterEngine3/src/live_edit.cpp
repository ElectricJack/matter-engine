#include "live_edit.h"

namespace live_edit {

std::set<PartId> LiveEditSession::upward_cone(const std::vector<PartId>& changed) const {
    std::set<PartId> cone(changed.begin(), changed.end());
    for (const auto& p : changed) {
        for (const auto& a : g_.ancestors(p)) cone.insert(a);
    }
    return cone;
}

std::set<PartId> LiveEditSession::changed_parts(const std::set<std::string>& paths) const {
    std::set<PartId> out;
    for (const auto& path : paths)
        for (const auto& p : g_.parts_for_file(path)) out.insert(p);
    return out;
}

RebuildReport LiveEditSession::run_rebuild(const std::set<std::string>& paths) {
    RebuildReport rep;
    // 1. Map changed files -> directly-changed parts (SP-3 reverse map).
    std::set<PartId> changed = changed_parts(paths);
    if (changed.empty()) return rep;  // unmapped file (e.g. non-part) -> no-op

    // 2. Upward cone: changed parts + all their transitive ancestors.
    std::vector<PartId> changed_v(changed.begin(), changed.end());
    std::set<PartId> cone = upward_cone(changed_v);

    // 3. Topo order (children-before-parents) over exactly the cone.
    std::vector<PartId> order = g_.topo_order(cone);

    // 4. Re-resolve + bake each in order under the dev budget (SP-2).
    for (const auto& p : order) {
        ResolvedHash h = g_.reresolve(p);
        BakeOutcome o = b_.bake(p, h, cfg_.bake_budget_ms);
        if (!o.ok) {                       // fail-closed
            rep.succeeded = false;
            rep.errors.push_back(o.error);
            sink_.report(o.error);
            return rep;                    // stop; last-good kept downstream
        }
        rep.rebaked.push_back(p);
    }

    // 5. Re-flatten each affected root's subtree (SP-4).
    for (const auto& root : g_.roots_over(changed)) {
        BakeOutcome o = f_.reflatten(root);
        if (!o.ok) { rep.succeeded = false; rep.errors.push_back(o.error); sink_.report(o.error); return rep; }
        rep.reflattened.push_back(root);
    }
    return rep;
}

RebuildReport LiveEditSession::rebuild(const std::set<std::string>& paths) {
    return run_rebuild(paths);
}

RebuildReport LiveEditSession::tick() {
    // 1. Drain newly observed events into the pending debounce set.
    std::vector<FileEvent> evs;
    w_.poll(evs);
    for (const auto& e : evs) {
        pending_paths_.insert(e.path);
        last_event_ms_ = (e.t_ms > last_event_ms_) ? e.t_ms : last_event_ms_;
        have_pending_ = true;
    }
    // 2. If nothing pending, nothing to do.
    if (!have_pending_) return RebuildReport{};
    // 3. Only fire once the quiet window has elapsed since the last event.
    if (w_.now_ms() - last_event_ms_ < cfg_.debounce_ms) return RebuildReport{};
    // 4. Quiet window elapsed: run ONE rebuild for the whole coalesced set.
    std::set<std::string> paths;
    paths.swap(pending_paths_);
    have_pending_ = false;
    last_event_ms_ = 0;
    return run_rebuild(paths);
}

} // namespace live_edit
