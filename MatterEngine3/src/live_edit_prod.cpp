// MatterEngine3/src/live_edit_prod.cpp
//
// The production live-edit seams (declared in live_edit_prod.h). Everything
// here is graph bookkeeping over `part_graph_snapshot::Snapshot` plus two
// delegations: ScriptHost for resolve/bake, part_flatten for the root
// subtree.
//
// Conventions worth knowing before reading
//   - live_edit::PartId is a MODULE NAME, and live_edit::ResolvedHash is the
//     uint64 hash rendered as a DECIMAL STRING. ProdBaker parses it back with
//     strtoull, so "" (every failure path in reresolve) parses to 0 and is
//     rejected there as an invalid hash -- failures propagate as a failed
//     bake, never as a silent skip.
//   - Sources are re-read from disk on every call; nothing is cached between
//     passes, which is the point (the file just changed).
//   - Errors are returned, not thrown or logged, except for two fprintf
//     diagnostics that accompany a returned failure.
//
// Task 9: production live-edit seam implementations.
// ProdGraphResolver / ProdBaker / ProdFlattener wrap the snapshot recorded at
// install time and drive the real ScriptHost + part_flatten pipeline.
//
// All methods run on the WORKER thread (sole graph mutator via async session).

#include "live_edit_prod.h"
#include "part_graph.h"       // params_to_json, params_from_json (MATTER_HAVE_SCRIPT_HOST section)
#include "part_asset_v2.h"    // cache_path_resolved, cache_path_flat
#include "script_host.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace live_edit_prod {

// ---------------------------------------------------------------------------
// ProdGraphResolver
// ---------------------------------------------------------------------------

ProdGraphResolver::ProdGraphResolver(part_graph_snapshot::Snapshot& snap,
                                     script_host::ScriptHost& host,
                                     std::string schemas_dir,
                                     std::string shared_lib_dir)
    : snap_(snap)
    , host_(host)
    , schemas_dir_(std::move(schemas_dir))
    , shared_lib_dirs_{std::move(shared_lib_dir)}
{}

ProdGraphResolver::ProdGraphResolver(part_graph_snapshot::Snapshot& snap,
                                     script_host::ScriptHost& host,
                                     std::string schemas_dir,
                                     std::vector<std::string> shared_lib_dirs)
    : snap_(snap)
    , host_(host)
    , schemas_dir_(std::move(schemas_dir))
    , shared_lib_dirs_(std::move(shared_lib_dirs))
    , exact_shared_paths_(true)
{}

// `path` must match the snapshot's by_file keys byte for byte -- there is no
// normalization of separators or case here. Returns empty for a file the graph
// does not know, which the session treats as "not a part, nothing to do".
std::vector<live_edit::PartId>
ProdGraphResolver::parts_for_file(const std::string& path) {
    // 1. Direct by-file lookup.
    {
        auto it = snap_.by_file.find(path);
        if (it != snap_.by_file.end()) return it->second;
    }
    // 2. Shared-lib lookup: if path is inside shared_lib_dir, strip the dir
    //    prefix to get a module name, then look it up in by_import.
    if (exact_shared_paths_) return {};
    for (const std::string& shared_lib_dir : shared_lib_dirs_) {
      if (!shared_lib_dir.empty() &&
        path.size() > shared_lib_dir.size() &&
        path.compare(0, shared_lib_dir.size(), shared_lib_dir) == 0) {
        // path = <shared_lib_dir>/<module>.js
        std::string rel = path.substr(shared_lib_dir.size());
        // strip leading slash
        if (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel = rel.substr(1);
        // strip .js suffix
        if (rel.size() > 3 && rel.compare(rel.size() - 3, 3, ".js") == 0)
            rel = rel.substr(0, rel.size() - 3);
        auto it2 = snap_.by_import.find(rel);
        if (it2 != snap_.by_import.end()) return it2->second;
      }
    }
    return {};
}

// Reverse-edge traversal, de-duplicated. `p` itself is NOT included. The
// result is in traversal order, not topological order -- callers that need
// child-before-parent ordering run topo_order over the resulting set.
std::vector<live_edit::PartId>
ProdGraphResolver::ancestors(const live_edit::PartId& p) {
    // BFS over reverse edges (parents_of).
    std::vector<live_edit::PartId> result;
    std::set<live_edit::PartId>    seen;
    std::vector<live_edit::PartId> queue;
    queue.push_back(p);
    while (!queue.empty()) {
        live_edit::PartId cur = queue.back(); queue.pop_back();
        for (const auto& par : snap_.parents_of(cur)) {
            if (seen.insert(par).second) {
                result.push_back(par);
                queue.push_back(par);
            }
        }
    }
    return result;
}

std::vector<live_edit::PartId>
ProdGraphResolver::topo_order(const std::set<live_edit::PartId>& subset) {
    // Children-before-parents: DFS post-order over snapshot edges, restricted to `subset`.
    std::vector<live_edit::PartId> result;
    std::set<live_edit::PartId>    visited;

    std::function<void(const std::string&)> dfs = [&](const std::string& m) {
        if (!subset.count(m) || visited.count(m)) return;
        visited.insert(m);
        // Visit children first
        auto it = snap_.nodes.find(m);
        if (it != snap_.nodes.end()) {
            for (const auto& child : it->second.children) {
                if (subset.count(child)) dfs(child);
            }
        }
        result.push_back(m);
    };

    for (const auto& m : subset) dfs(m);
    return result;
}

// Walks upward from every changed part and collects the nodes flagged
// is_root, stopping there (a root is not walked past). A changed part with no
// root above it contributes nothing, so nothing re-flattens for it -- which is
// correct: geometry that no root subtree contains is not on screen.
std::vector<live_edit::PartId>
ProdGraphResolver::roots_over(const std::set<live_edit::PartId>& changed) {
    // Walk up from each changed part until we hit is_root nodes.
    std::set<live_edit::PartId>    roots_set;
    std::set<live_edit::PartId>    visited;
    std::vector<live_edit::PartId> queue(changed.begin(), changed.end());

    while (!queue.empty()) {
        live_edit::PartId cur = queue.back(); queue.pop_back();
        if (!visited.insert(cur).second) continue;

        auto it = snap_.nodes.find(cur);
        if (it != snap_.nodes.end() && it->second.is_root) {
            roots_set.insert(cur);
            continue;  // don't walk further up from a root
        }
        // Push parents
        for (const auto& par : snap_.parents_of(cur)) {
            queue.push_back(par);
        }
    }
    return std::vector<live_edit::PartId>(roots_set.begin(), roots_set.end());
}

// MUTATES THE SNAPSHOT. Re-reads the part's source from disk, folds it with
// the children's CURRENT snapshot hashes through ScriptHost, writes the new
// hash back onto the node, and returns it as a decimal string.
//
// Returns "" for every failure -- unknown module, unreadable source, a child
// that is not resolved yet, or a host hash of 0 -- and in that case the node
// keeps its old hash. Because the caller rebuilds children before parents,
// the write-back is what lets a parent fold the child hash produced moments
// earlier in the same pass.
live_edit::ResolvedHash
ProdGraphResolver::reresolve(const live_edit::PartId& p) {
    auto it = snap_.nodes.find(p);
    if (it == snap_.nodes.end()) return "";

    // Copy fields we need before potentially modifying the node.
    std::string source_path = it->second.source_path;
    std::string params_json = it->second.params_json;
    std::vector<std::string> children = it->second.children;

    // Load source from disk.
    std::ifstream fin(source_path, std::ios::binary);
    if (!fin) {
        std::fprintf(stderr, "ProdGraphResolver::reresolve: cannot open %s\n",
                     source_path.c_str());
        return "";
    }
    std::ostringstream ss; ss << fin.rdbuf();
    std::string source = ss.str();

    // Gather children's CURRENT snapshot hashes.
    std::vector<uint64_t> child_hashes;
    for (const auto& child_mod : children) {
        auto cit = snap_.nodes.find(child_mod);
        if (cit == snap_.nodes.end() || cit->second.resolved_hash == 0) {
            // Child not resolved yet (failed node); treat as failure.
            return "";
        }
        child_hashes.push_back(cit->second.resolved_hash);
    }

    // Ask ScriptHost for the new hash (SP-2 is the hash authority, master C-2).
    uint64_t new_hash = host_.resolve_hash(source, params_json,
                                           child_hashes.data(), child_hashes.size());
    if (new_hash == 0) return "";

    // Update the snapshot in-place so subsequent reresolve() calls for
    // ancestors see the current child hash.
    it->second.resolved_hash = new_hash;

    // Return as decimal string (Task 9 spec: std::to_string / strtoull).
    return std::to_string(new_hash);
}

// ---------------------------------------------------------------------------
// ProdBaker
// ---------------------------------------------------------------------------

ProdBaker::ProdBaker(part_graph_snapshot::Snapshot& snap,
                     script_host::ScriptHost& host,
                     std::string parts_dir)
    : snap_(snap)
    , host_(host)
    , parts_dir_(std::move(parts_dir))
{}

// `budget_ms` is accepted and IGNORED: the dev time budget was retired, so
// BakeOptions.time_budget_ms stays 0 (unbounded) and Cause::BudgetExceeded is
// never produced here.
//
// After the bake it re-checks that ScriptHost agreed with the hash the caller
// resolved (`h`); a mismatch means the resolve and the bake read different
// source or different children, and is reported as a failure rather than
// letting an artifact be published under a hash nothing will look up.
live_edit::BakeOutcome
ProdBaker::bake(const live_edit::PartId& p,
                const live_edit::ResolvedHash& h,
                long long /*budget_ms*/) {
    // budget_ms <= 0 = unbounded (retired); BakeOptions.time_budget_ms = 0.

    auto it = snap_.nodes.find(p);
    if (it == snap_.nodes.end()) {
        return { false, live_edit::LiveEditError{
            live_edit::LiveEditError::Cause::ResolveFailed, p,
            "ProdBaker: unknown part " + p, ""} };
    }

    const part_graph_snapshot::Node& node = it->second;

    // Load source from disk.
    std::ifstream fin(node.source_path, std::ios::binary);
    if (!fin) {
        return { false, live_edit::LiveEditError{
            live_edit::LiveEditError::Cause::ResolveFailed, p,
            "ProdBaker: cannot open " + node.source_path, node.source_path} };
    }
    std::ostringstream ss; ss << fin.rdbuf();
    std::string source = ss.str();

    // Gather children's current hashes and module names + params.
    std::vector<uint64_t>    child_hashes;
    std::vector<std::string> child_modules;
    std::vector<std::string> child_params;
    for (const auto& child_mod : node.children) {
        auto cit = snap_.nodes.find(child_mod);
        if (cit == snap_.nodes.end() || cit->second.resolved_hash == 0) {
            return { false, live_edit::LiveEditError{
                live_edit::LiveEditError::Cause::ResolveFailed, p,
                "ProdBaker: child " + child_mod + " not resolved", ""} };
        }
        child_hashes.push_back(cit->second.resolved_hash);
        child_modules.push_back(child_mod);
        child_params.push_back(cit->second.params_json);
    }

    // Parse the resolved hash string back to uint64.
    uint64_t resolved_hash = std::strtoull(h.c_str(), nullptr, 10);
    if (resolved_hash == 0) {
        return { false, live_edit::LiveEditError{
            live_edit::LiveEditError::Cause::ResolveFailed, p,
            "ProdBaker: invalid resolved hash string '" + h + "'", ""} };
    }

    // Delegate to ScriptHost::bake_source (SP-2, HostBaker semantics).
    script_host::BakeOptions bopts;
    bopts.parts_dir = parts_dir_;
    // budget_ms <= 0 => unbounded (time_budget_ms = 0 default)

    script_host::BakeResult r = host_.bake_source(
        source, node.params_json, bopts,
        child_hashes.data(), child_hashes.size(),
        child_modules.data(), child_params.data());

    if (!r.error.ok) {
        return { false, live_edit::LiveEditError{
            live_edit::LiveEditError::Cause::Script, p,
            r.error.message, r.error.source_location} };
    }

    // Verify hash agreement (master C-2).
    if (r.resolved_hash != resolved_hash) {
        std::fprintf(stderr,
            "ProdBaker: hash mismatch for %s: expected %llu got %llu\n",
            p.c_str(), (unsigned long long)resolved_hash,
            (unsigned long long)r.resolved_hash);
        return { false, live_edit::LiveEditError{
            live_edit::LiveEditError::Cause::Script, p,
            "hash mismatch after bake", ""} };
    }

    return { true, {} };
}

// ---------------------------------------------------------------------------
// ProdFlattener
// ---------------------------------------------------------------------------

ProdFlattener::ProdFlattener(part_graph_snapshot::Snapshot& snap,
                             script_host::ScriptHost& host,
                             std::string abs_cache_root)
    : snap_(snap)
    , host_(host)
    , abs_cache_root_(std::move(abs_cache_root))
{}

// Flattens the root's subtree from the cache root, using the hash currently on
// the snapshot node -- so the caller must have re-resolved and re-baked the
// cone first, or this flattens the previous revision. A root missing from the
// snapshot, or one whose hash is still 0 (never resolved), is a FlattenFailed
// error rather than a silent no-op.
live_edit::BakeOutcome
ProdFlattener::reflatten(const live_edit::PartId& root) {
    auto it = snap_.nodes.find(root);
    if (it == snap_.nodes.end() || it->second.resolved_hash == 0) {
        return { false, live_edit::LiveEditError{
            live_edit::LiveEditError::Cause::FlattenFailed, root,
            "ProdFlattener: root " + root + " not in snapshot or hash=0", ""} };
    }

    uint64_t root_hash = it->second.resolved_hash;

    part_flatten::FlattenResult fr =
        part_flatten::flatten_part(abs_cache_root_, root_hash);

    if (!fr.ok) {
        return { false, live_edit::LiveEditError{
            live_edit::LiveEditError::Cause::FlattenFailed, root,
            fr.error, ""} };
    }
    return { true, {} };
}

} // namespace live_edit_prod
