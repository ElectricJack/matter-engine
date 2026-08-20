// MatterEngine3/src/live_edit.cpp
//
// The dev live-edit rebuild pass (declared in live_edit.h). One tick does:
// drain the watcher -> debounce -> map changed FILES to changed PARTS via the
// graph's reverse map -> widen to the upward cone (those parts plus all their
// transitive ancestors, because a parent's bake embeds its children) -> topo
// order children-before-parents -> re-resolve and re-bake each under the dev
// time budget -> re-flatten every affected root's subtree.
//
// All the real work is delegated through the interfaces in
// live_edit_interfaces.h (GraphResolver, Baker, Flattener, ErrorSink), which
// is what lets the whole ordering be tested with no engine behind it.
//
// FAIL-CLOSED, AND NOT TRANSACTIONAL. The first failed bake or flatten stops
// the pass immediately and reports through the sink; the artifacts already
// rebuilt in this pass stay rebuilt and the ones after it keep their last-good
// output. Recovery is by retrying on the next file event, not by rollback.
//
// Everything here runs on whatever thread calls tick(); there is no locking.
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

// One rebuild pass over `paths`. An empty result is a normal outcome: paths
// that map to no part (a non-part file) produce a default report with
// `succeeded` still true. On failure the report is PARTIAL -- `rebaked` and
// `reflattened` list what completed before the stop, in the order it happened.
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

// The debounce window is measured from the LATEST event seen, not from the
// first, so a burst of saves keeps deferring the rebuild until the tree has
// been quiet for `cfg_.debounce_ms`. Pending paths accumulate across ticks and
// are consumed as one coalesced set, so a file touched several times inside
// the window is rebuilt once. Returns a default (empty, successful) report on
// every tick that does not fire.
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
