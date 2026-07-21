#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Graph snapshot recorded by PartGraph::install (Task 9).
// Consumed by live_edit production seams (ProdGraphResolver) and Task 10's
// RebakeCone. Recorded once per install, updated in-place by reresolve().
//
// All methods run on the WORKER thread (the sole graph mutator via
// PartGraph::install under the async session); no extra locking needed.

namespace part_graph_snapshot {

struct Node {
    std::string module;                       // live_edit::PartId
    std::string source_path;                  // absolute <schemas_dir>/<module>.js
    std::string params_json;                  // canonical params at install
    std::vector<std::string> children;        // child module names (deduped, insert order)
    std::vector<std::string> shared_imports;  // shared-lib module names found in source
    std::vector<std::string> shared_source_paths; // selected direct/transitive files
    uint64_t resolved_hash = 0;
    bool is_root = false;
};

struct Snapshot {
    std::map<std::string, Node> nodes;                           // by module
    std::map<std::string, std::vector<std::string>> by_file;     // abs source path -> modules
    std::map<std::string, std::vector<std::string>> by_import;   // shared-lib module -> importer modules

    // Reverse-edge helper: all direct parents of `module`.
    std::vector<std::string> parents_of(const std::string& module) const;
};

} // namespace part_graph_snapshot
