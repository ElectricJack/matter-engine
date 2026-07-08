#pragma once
#include "file_watcher.h"
#include "live_edit_interfaces.h"
#include <set>
#include <vector>

namespace live_edit {

struct LiveEditConfig {
    long long debounce_ms = 150;   // coalesce saves within this window into one rebuild
    long long bake_budget_ms = 2000; // dev time budget per bake (SP-2); <=0 = unbounded
};

// Result of one processed rebuild pass (for test instrumentation).
struct RebuildReport {
    std::vector<PartId> rebaked;       // exactly the upward cone, topo order
    std::vector<PartId> reflattened;   // affected roots
    bool succeeded = true;             // false => fail-closed, last-good kept
    std::vector<LiveEditError> errors; // structured errors surfaced this pass
};

// Owns the dev live-edit loop. Pulls debounced events from the watcher, maps
// each changed file to its parts (SP-3 reverse map), computes the upward cone,
// rebakes it in topo order under the dev budget (SP-2), then re-flattens each
// affected root subtree (SP-4). Fail-closed: a failed bake/flatten keeps the
// last-good artifact and surfaces a structured error; retried on next event.
class LiveEditSession {
public:
    LiveEditSession(FileWatcher& w, GraphResolver& g, Baker& b, Flattener& f,
                    ErrorSink& sink, LiveEditConfig cfg)
        : w_(w), g_(g), b_(b), f_(f), sink_(sink), cfg_(cfg) {}

    // Drain ready events, apply debounce, and run at most one rebuild pass for
    // the coalesced change set whose quiet window has elapsed. Returns the
    // report for the pass that ran (succeeded defaults true with empty sets if
    // nothing was ready). Call once per host tick.
    RebuildReport tick();

    // Pure scoping helper (exposed for unit tests): the upward cone of a set of
    // directly-changed parts = the changed parts + all their transitive
    // ancestors, returned as a set.
    std::set<PartId> upward_cone(const std::vector<PartId>& changed) const;

    // Map a set of changed file paths to the directly-changed parts (SP-3
    // reverse map). A shared-lib module fans out to all importers; duplicates
    // across files are de-duplicated.
    std::set<PartId> changed_parts(const std::set<std::string>& paths) const;

private:
    FileWatcher&  w_;
    GraphResolver& g_;
    Baker&        b_;
    Flattener&    f_;
    ErrorSink&    sink_;
    LiveEditConfig cfg_;

    // Pending debounce state: paths seen and the last event time among them.
    std::set<std::string> pending_paths_;
    long long last_event_ms_ = 0;
    bool have_pending_ = false;

    RebuildReport run_rebuild(const std::set<std::string>& paths);
};

} // namespace live_edit
