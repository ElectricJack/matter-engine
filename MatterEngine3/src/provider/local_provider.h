#ifndef VIEWER_LOCAL_PROVIDER_H
#define VIEWER_LOCAL_PROVIDER_H

#include "world_source.h"
#include "part_store.h"
#include "part_graph.h"           // PartGraph, InstallResult, ChildRequest
#include "part_graph_snapshot.h"  // Task 9: live-edit graph snapshot

#if defined(MATTER_HAVE_SCRIPT_HOST)
#include "script_host.h"
#include "part_asset_v2.h"  // part_asset::RetopoSettings
#endif

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer {

struct LocalProviderConfig {
    std::string schemas_dir;      // ../examples/world_demo/schemas
    std::string world_data_dir;   // ../examples/world_demo/WorldData
    std::string world_name;       // "Demo"
    std::string shared_lib_dir;   // ../shared-lib
    std::string cache_root;       // persistent parts/ cache (NOT a /tmp throwaway)

    // Invoked during fetch_parts once per part processed (bake or cache hit):
    // module = part module name, done/total = progress through the want list.
    // Also fired during install_graph() for freshly-baked nodes with total == 0
    // (indeterminate count during install phase).
    std::function<void(const char* module, int done, int total)> on_part;

    // Phase B: run `fn` on the GL thread and wait for completion. Null => run
    // inline on the calling thread (synchronous callers, tests).
    std::function<bool(const char* name,
                       std::function<bool(std::string& err)> fn,
                       std::string& err)> gpu_run;

    // True when GLAD function pointers are loaded (i.e., a window was created).
    // False in headless tests (no window, no GL context): the tileset phase
    // runs physics-settle only; the .gtex is generated later when a viewer
    // with a live GL context opens the world (GL 4.6 required by GPU path).
    // Note: does NOT imply GL 4.6 — contexts with allow_gl_lt_46=true have
    // GLAD loaded and set this true; gl46_available() then decides success/error.
    bool gl_available = false;

    // Task 7: OOM/error injection hook for testing skip-and-continue.
    // Fired once per part processed (install bake + fetch/load); `part_index` is the
    // 0-based index within the current phase's part list. May throw to inject an error
    // (std::bad_alloc → OutOfMemory, any other exception → ScriptError/Internal).
    // Null in production (kernel-internal test seam; not part of the public stable API).
    std::function<void(int part_index)> test_fault_hook;

    // Phase C Task 7: optional root-params override JSON object, e.g. {"worldSeed": 2}.
    // When non-empty, merged (overrides win) into every manifest root's params before
    // merge_params_canonical, so the resolved hash changes with the seed value.
    // Empty string = no override (today's default behavior).
    // Populated by WorldSession::regenerate(); not used by execute_rebake_cone (cone
    // rebuilds operate on a diff of changed files, not a full root-params change).
    std::string root_params_json;
};

// Drives the SP-3 install path over a persistent content-addressed cache and
// scatters the example world (terrain/trees/grass) into a WorldManifest. Same
// interface as a future NetworkProvider.
class LocalProvider : public WorldProvider {
public:
    explicit LocalProvider(LocalProviderConfig cfg);

    // connect() == install_graph() + compose_world() with unchanged external behavior.
    bool connect(WorldManifest& out, std::string& err) override;

    // Heavy phase: ScriptHost + PartGraph::install (script eval, mesh, per-part bake).
    // Must be called before compose_world(). Idempotent state reset happens here.
    //
    // policy=BakePolicy::All (default, sync/connect() path): bakes every node eagerly.
    // policy=BakePolicy::RootsOnly (async execute_bake path, Phase C Task 14):
    //   only bakes root nodes; the retained bake_plan covers all nodes so
    //   ensure_part_baked() can bake individual subtrees on demand from the publish loop.
    bool install_graph(std::string& err,
                       part_graph::BakePolicy policy = part_graph::BakePolicy::All);

    // Task 13 (Phase C): on-demand bake primitives.
    // Bake one part (and, post-order, any unbaked children in its subtree) using
    // the retained bake_plan from the last install_graph(). cached() short-circuits
    // per node; also runs bake_lod_variants for freshly-baked nodes. Safe to call
    // from the worker thread after install_graph (host_ is idle post-install).
    // Returns false (with err set) on bake failure; true on success or already cached.
    bool ensure_part_baked(uint64_t part_hash, std::string& err);

    // Flatten one baked part to .flat.part (moved from compose_world's flatten_one
    // lambda into a member; identical logic incl. retopo_by_hash_ threading +
    // version sniff). Requires the part to have been baked (ensure_part_baked first).
    // Returns false (non-fatal) if flatten_part fails; caller falls back to
    // compositional rendering.
    bool ensure_part_flattened(uint64_t part_hash);

    // Post-install: scatter/place, expand, per-root flatten, instance refs,
    // tileset phase (via gpu_run), probe bake. Reusable after a cone rebake.
    // Requires install_graph() to have succeeded first.
    bool compose_world(WorldManifest& out, std::string& err);

    std::vector<uint64_t> reconcile(const WorldManifest& manifest,
                                    const PartStore& store) override;
    bool fetch_parts(const std::vector<uint64_t>& want,
                     PartStore& store, std::string& err) override;
    bool poll_deltas(WorldDelta& out) override;   // LocalProvider: always false (static world)

    int baked_count() const { return baked_count_; }
    int hit_count()   const { return hit_count_; }
    int baked_tileset_count() const { return baked_tileset_count_; }

    // Task 7: access install-phase partial failures (populated after install_graph()).
    const part_graph::InstallResult& install_result() const { return ir_; }

    // Task 9: access the graph snapshot recorded by the last install_graph().
    // Valid after a successful install_graph() call. Updated by reresolve() in
    // ProdGraphResolver for live-edit cascade tracking.
    part_graph_snapshot::Snapshot& graph_snapshot() { return graph_snapshot_; }

    // Task 7 fix: per-part load failures recorded during fetch_parts() when
    // get_or_load returns null (skip-and-continue; returns true even with failures).
    struct FetchFailed { std::string module; std::string error; };
    const std::vector<FetchFailed>& fetch_failed() const { return fetch_failed_; }

private:
    LocalProviderConfig  cfg_;
    int                  baked_count_ = 0;
    int                  hit_count_   = 0;
    int                  baked_tileset_count_ = 0;
    int                  install_bake_count_ = 0; // counter for install-phase on_part callbacks
    std::set<uint64_t>   baked_hashes_;  // hashes freshly baked by last install_graph()
    std::map<uint64_t, std::string> module_by_hash_; // hash -> module name (from manifest roots)
    std::vector<FetchFailed> fetch_failed_; // Task 7 fix: per-part load failures from fetch_parts()
    part_graph_snapshot::Snapshot graph_snapshot_;  // Task 9: live-edit graph snapshot

    // State produced by install_graph(), consumed by compose_world().
    // Valid only after a successful install_graph() call.
    std::string abs_schemas_;
    std::string abs_world_data_;
    std::string abs_shared_lib_;
    std::string abs_cache_root_;

    // install_graph() output: roots split by kind, install result
    std::vector<part_graph::ChildRequest> roots_;           // all manifest roots
    std::vector<bool>                     expand_flags_;
    std::vector<bool>                     tileset_flags_;
    std::vector<part_graph::ChildRequest> roots_for_install_;
    std::vector<size_t>                   install_to_orig_;
    std::vector<size_t>                   tileset_indices_;
    part_graph::InstallResult             ir_;

#if defined(MATTER_HAVE_SCRIPT_HOST)
    // Owned objects that span both phases.
    std::unique_ptr<script_host::ScriptHost>            host_;
    std::unique_ptr<part_graph::FileModuleResolver>     resolver_;
    std::unique_ptr<part_graph::HostBaker>              host_baker_;  // Task 13: shared baker
    std::unordered_map<uint64_t, part_asset::RetopoSettings> retopo_by_hash_;
#endif
};

// Expand an assembly root's baked child-instance table (from its .part in
// cache_root) into individual world-manifest instances — one per child, with
// the child's stored transform. Generic: used for any manifest root flagged
// `expand`. Fails closed if the root artifact is missing or has no children.
bool append_expanded_children(const std::string& cache_root, uint64_t root_hash,
                              uint32_t& next_id,
                              std::vector<WorldManifestEntry>& out_instances,
                              std::string& err);

} // namespace viewer

#endif // VIEWER_LOCAL_PROVIDER_H
