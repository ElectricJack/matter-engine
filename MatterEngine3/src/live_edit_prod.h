#pragma once
// Production live-edit seam implementations over the snapshot + ScriptHost.
// All methods run on the WORKER thread (the sole graph mutator).
// Consumed by Task 10's RebakeCone.

#include "live_edit_interfaces.h"
#include "part_graph_snapshot.h"
#include "part_flatten.h"
#include "script_host.h"

#include <set>
#include <string>
#include <vector>

namespace live_edit_prod {

// SP-3 production seam: resolve + reverse-map + ancestors + topo + affected roots.
// Reads from the snapshot recorded at install time; reresolve() also updates it
// in-place so subsequent calls see current hashes.
class ProdGraphResolver : public live_edit::GraphResolver {
public:
    ProdGraphResolver(part_graph_snapshot::Snapshot& snap,
                      script_host::ScriptHost& host,
                      std::string schemas_dir,
                      std::string shared_lib_dir);

    // The part(s) defined by, or importing, this script / shared-lib file.
    // by_file lookup first (normal schema); if that misses, check if the path is
    // inside shared_lib_dir and look up by_import (shared-lib fans out to all importers).
    std::vector<live_edit::PartId> parts_for_file(const std::string& path) override;

    // All transitive parents of `p`, up to the root(s) — reverse-edge BFS.
    std::vector<live_edit::PartId> ancestors(const live_edit::PartId& p) override;

    // Children-before-parents DFS post-order over exactly the given subset.
    std::vector<live_edit::PartId> topo_order(const std::set<live_edit::PartId>& subset) override;

    // The root part(s) whose subtree must re-flatten given the changed set —
    // walks up to is_root nodes.
    std::vector<live_edit::PartId> roots_over(const std::set<live_edit::PartId>& changed) override;

    // Reload source from disk, compute the new resolved hash folding children's
    // CURRENT snapshot hashes, update the snapshot node, return as decimal string
    // ("" on failure).
    live_edit::ResolvedHash reresolve(const live_edit::PartId& p) override;

private:
    part_graph_snapshot::Snapshot& snap_;
    script_host::ScriptHost&       host_;
    std::string                    schemas_dir_;
    std::string                    shared_lib_dir_;
};

// SP-2 production seam: bake one part under its new resolved hash.
// Reads source from disk (schemas_dir / module.js), gathers children's current
// hashes from the snapshot, delegates to ScriptHost::bake_source.
// Fail-closed: on failure the existing artifact is left untouched.
// budget_ms <= 0 is always passed by the live-edit loop (retired / unbounded).
class ProdBaker : public live_edit::Baker {
public:
    ProdBaker(part_graph_snapshot::Snapshot& snap,
              script_host::ScriptHost& host,
              std::string parts_dir);

    live_edit::BakeOutcome bake(const live_edit::PartId& p,
                                const live_edit::ResolvedHash& h,
                                long long budget_ms) override;

private:
    part_graph_snapshot::Snapshot& snap_;
    script_host::ScriptHost&       host_;
    std::string                    parts_dir_;
};

// SP-4 production seam: re-flatten one affected root's subtree.
// Calls part_flatten::flatten_part with the root's current resolved hash.
// Retopo now happens at part bake time via modifier regions (beginModifier/
// endModifier), not at flatten time, so no per-part settings need to be
// threaded through here.
class ProdFlattener : public live_edit::Flattener {
public:
    ProdFlattener(part_graph_snapshot::Snapshot& snap,
                  script_host::ScriptHost& host,
                  std::string abs_cache_root);

    live_edit::BakeOutcome reflatten(const live_edit::PartId& root) override;

private:
    part_graph_snapshot::Snapshot& snap_;
    script_host::ScriptHost&       host_;
    std::string                    abs_cache_root_;
};

} // namespace live_edit_prod
