#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// MatterEngine3/src/part_graph_snapshot.h
//
// Graph snapshot recorded by PartGraph::install (Task 9).
// Consumed by live_edit production seams (ProdGraphResolver) and Task 10's
// RebakeCone. Recorded once per install, updated in-place by reresolve().
//
// All methods run on the WORKER thread (the sole graph mutator via
// PartGraph::install under the async session); no extra locking needed.

namespace part_graph_snapshot {

// One entry per MODULE NAME — not one per resolved node. When the same module
// is instantiated at several different params the install keeps the FIRST one
// it walked, so `params_json` and `resolved_hash` describe one representative
// instantiation, not all of them. That is the module-identity contract
// live-edit works in: an edit to a file invalidates the module, and the rebake
// cone is expanded from `children` / `by_import`, never from a hash.
struct Node {
    std::string module;                       // live_edit::PartId
    std::string source_path;                  // absolute <schemas_dir>/<module>.js
    std::string params_json;                  // params the PLACEMENT passed at install
    // The parameter object the resolved hash was actually folded from: the
    // module's `static params` defaults overlaid with `params_json`. Only the
    // script host can read those defaults, so the graph takes them from the
    // Baker alongside the hash (part_graph.h's Baker::resolve_hash
    // `merged_params_out`) instead of re-deriving them here. Empty when no
    // host answered -- host-free logic tests, or a node whose hash resolve
    // failed -- so read it through effective_params() below, not directly.
    std::string effective_params_json;
    std::vector<std::string> children;        // child module names (deduped, insert order)
    std::vector<std::string> shared_imports;  // shared-lib module names found in source
    std::vector<std::string> shared_source_paths; // selected direct/transitive files
    uint64_t resolved_hash = 0;
    bool is_root = false;
};

// The parameters a reader should attribute this node's generated content to.
// One spelling for every consumer: `scene.trace_provenance`'s
// `generation_inputs`, `scene.list_objects`' root rows and
// `procedural.parameters` all answer about the SAME parameter object, and a
// surface that reported the bare placement params would omit every default the
// module declared and never overrode -- which is exactly what made
// `procedural.parameters` return an empty set for a root placed with no
// overrides. Falls back to the placement params when no host merged them.
inline const std::string& effective_params(const Node& node) {
    return node.effective_params_json.empty() ? node.params_json
                                              : node.effective_params_json;
}

// The whole recorded graph plus two reverse indices, both built at install
// time: `by_file` answers "an editor saved this path — which modules changed?"
// (a module's own .js and every shared-lib file its import closure selected),
// and `by_import` answers the same question in shared-lib specifier terms.
// Rebuilt wholesale by each install and updated in place by reresolve(); no
// entry is ever removed by an edit, so a module that disappears from the graph
// lingers until the next full install.
struct Snapshot {
    std::map<std::string, Node> nodes;                           // by module
    std::map<std::string, std::vector<std::string>> by_file;     // abs source path -> modules
    std::map<std::string, std::vector<std::string>> by_import;   // shared-lib module -> importer modules

    // Reverse-edge helper: all direct parents of `module`.
    // O(nodes x children per node): there is no stored reverse edge, this scans
    // every node's child list on each call. Fine for one lookup, quadratic if
    // you call it for every module in the snapshot.
    std::vector<std::string> parents_of(const std::string& module) const;
};

} // namespace part_graph_snapshot
