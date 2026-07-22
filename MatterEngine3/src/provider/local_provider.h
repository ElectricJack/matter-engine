#ifndef VIEWER_LOCAL_PROVIDER_H
#define VIEWER_LOCAL_PROVIDER_H

#include "world_source.h"
#include "world_lights.h"
#include "part_store.h"
#include "part_graph.h"           // PartGraph, InstallResult, ChildRequest
#include "part_graph_snapshot.h"  // Task 9: live-edit graph snapshot
#include "matter/world_definition.h"

#if defined(MATTER_HAVE_SCRIPT_HOST)
#include "script_host.h"
#endif

#include <cmath>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer {

struct LocalProviderConfig {
    std::string project_dir;
    std::string objects_dir;
    std::string worlds_dir;
    std::string world_path;
    std::string project_shared_lib_dir;
    std::string engine_shared_lib_dir;

    static LocalProviderConfig for_project(
        const std::string& project_dir,
        const std::string& world_name,
        const std::string& engine_shared_lib_dir);

    const std::string& object_sources_dir() const { return objects_dir; }
    std::vector<std::string> shared_lib_roots() const {
        std::vector<std::string> roots;
        if (!project_shared_lib_dir.empty())
            roots.push_back(project_shared_lib_dir);
        if (!engine_shared_lib_dir.empty())
            roots.push_back(engine_shared_lib_dir);
        return roots;
    }

    std::string world_name;
    std::string cache_root;

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

    // Tileset Vulkan port (spec "Phase 1 - Vulkan tileset consumption",
    // Task 6): invoked once per tileset root right after the GL bake +
    // viewer::tileset_provider::load_slot succeed in run_tileset_deferred, so
    // the Vulkan renderer can mirror the same .gtex atlas into its own
    // texture-array slot. Null when no Vulkan renderer is active (GL-only
    // viewer, headless tests, MATTER_VULKAN_ONLY builds that never reach the
    // GL bake branch below). A false return is logged and otherwise ignored —
    // the world load still succeeds; ground rendering for that slot just
    // stays untextured on the Vulkan side (fail-closed, matches the GL path's
    // own failure handling for a single tileset root).
    std::function<bool(int slot, const std::string& gtex_path,
                       std::string& err)> vk_tileset_load;

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

inline LocalProviderConfig LocalProviderConfig::for_project(
    const std::string& project_dir_value,
    const std::string& world_name_value,
    const std::string& engine_shared_lib_dir_value) {
    namespace fs = std::filesystem;
    LocalProviderConfig cfg;
    const fs::path project(project_dir_value);
    cfg.project_dir = project.string();
    cfg.objects_dir = (project / "objects").string();
    cfg.worlds_dir = (project / "worlds").string();
    cfg.world_name = world_name_value;
    cfg.world_path = (project / "worlds" / (world_name_value + ".js")).string();
    const fs::path project_shared = project / "shared-lib";
    std::error_code ec;
    if (fs::is_directory(project_shared, ec))
        cfg.project_shared_lib_dir = project_shared.string();
    cfg.engine_shared_lib_dir = engine_shared_lib_dir_value;
    cfg.cache_root = (project / ".cache" / world_name_value).string();
    return cfg;
}

struct ProviderWorldDefinition {
    std::vector<part_graph::ChildRequest> roots;
    std::vector<matter::Mat4f> root_transforms;
    std::vector<bool> expand_flags;
    std::vector<bool> tileset_flags;
    world_lights::WorldLights lights;
    matter::WorldSettings settings;
};

struct ProceduralWorldProfile {
    float sector_size = 16.0f;
    float y_min = -64.0f;
    float y_max = 192.0f;

    template <typename WorldBinding>
    void apply(WorldBinding& binding) const {
        binding.sector_size = sector_size;
        binding.y_min = y_min;
        binding.y_max = y_max;
    }
};

inline ProceduralWorldProfile select_procedural_world_profile(
    bool project_layout,
    const matter::WorldSettings& authored,
    const matter::WorldSettings& legacy) {
    const matter::WorldSettings& selected = project_layout ? authored : legacy;
    return {selected.sector_size, selected.y_min, selected.y_max};
}

inline ProviderWorldDefinition adapt_world_definition(
    const matter::WorldDefinition& definition) {
    ProviderWorldDefinition out;
    out.roots.reserve(definition.roots.size());
    out.root_transforms.reserve(definition.roots.size());
    out.expand_flags.reserve(definition.roots.size());
    out.tileset_flags.reserve(definition.roots.size());
    for (const matter::WorldRoot& root : definition.roots) {
        out.roots.push_back({root.module,
                             part_graph::params_from_json(root.params_json)});
        out.root_transforms.push_back(root.transform);
        out.expand_flags.push_back(root.expand);
        out.tileset_flags.push_back(root.tileset);
    }

    out.settings = definition.settings;
    out.lights.sun_dir[0] = definition.settings.sun_direction.x;
    out.lights.sun_dir[1] = definition.settings.sun_direction.y;
    out.lights.sun_dir[2] = definition.settings.sun_direction.z;
    out.lights.sun_color[0] = definition.settings.sun_color.x;
    out.lights.sun_color[1] = definition.settings.sun_color.y;
    out.lights.sun_color[2] = definition.settings.sun_color.z;
    out.lights.sky_color[0] = definition.settings.sky_color.x;
    out.lights.sky_color[1] = definition.settings.sky_color.y;
    out.lights.sky_color[2] = definition.settings.sky_color.z;
    out.lights.spots.reserve(definition.lights.size());
    constexpr float kPiOver180 = 3.14159265358979323846f / 180.0f;
    for (const matter::WorldLight& light : definition.lights) {
        world_lights::SpotLight runtime{};
        runtime.pos[0] = light.position.x;
        runtime.pos[1] = light.position.y;
        runtime.pos[2] = light.position.z;
        runtime.dir[0] = light.direction.x;
        runtime.dir[1] = light.direction.y;
        runtime.dir[2] = light.direction.z;
        const float length = std::sqrt(runtime.dir[0] * runtime.dir[0] +
                                       runtime.dir[1] * runtime.dir[1] +
                                       runtime.dir[2] * runtime.dir[2]);
        if (length > 1e-8f) {
            runtime.dir[0] /= length;
            runtime.dir[1] /= length;
            runtime.dir[2] /= length;
        }
        runtime.color[0] = light.color.x * light.intensity;
        runtime.color[1] = light.color.y * light.intensity;
        runtime.color[2] = light.color.z * light.intensity;
        runtime.range = light.range;
        runtime.cos_inner = std::cos(light.inner_cone_degrees * kPiOver180);
        runtime.cos_outer = std::cos(light.outer_cone_degrees * kPiOver180);
        out.lights.spots.push_back(runtime);
    }
    return out;
}

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
    // lambda into a member; identical logic incl. version sniff).
    // Requires the part to have been baked (ensure_part_baked first).
    // Returns false (non-fatal) if flatten_part fails; caller falls back to
    // compositional rendering.
    bool ensure_part_flattened(uint64_t part_hash);

    // Post-install: scatter/place, expand, per-root flatten, instance refs,
    // probe bake. Tileset roots are skipped here; call run_tileset_deferred
    // after BakeFinished (Task 15: tileset off the critical path).
    // Reusable after a cone rebake. Requires install_graph() to have succeeded.
    bool compose_world(WorldManifest& out, std::string& err);

    // Run the deferred tileset phase for all tileset roots after BakeFinished.
    // settle_cache_load → on miss: ensure_part_baked children + settle_tileset
    // + settle_cache_save; then bake_tileset_gpu (if gl_available).
    // Emits progress via on_tileset_part (done, total, root_module) for each
    // tileset root processed. Returns false on hard failure (sets err).
    bool run_tileset_deferred(
        std::function<void(int done, int total, const char* module)> on_tileset_part,
        std::function<bool()> is_cancelled,
        std::string& err);

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

    // Phase C Task 17 — resolve cache restore hook.
    // Called by execute_bake on a resolve-cache hit INSTEAD of install_graph().
    // Restores bake_plan, root_hashes, graph_snapshot from the cache payload and
    // initialises the ScriptHost + HostBaker so ensure_part_baked() can re-bake
    // individual cache-miss parts on a warm run without global re-resolve.
    // Also reads the world manifest to populate roots_/tileset_indices_ so the
    // deferred tileset phase (run_tileset_deferred) still runs correctly.
    // Requires: MATTER_HAVE_SCRIPT_HOST (returns false otherwise).
    // Fail-closed: any setup error returns false; caller falls through to full resolve.
    bool restore_from_cache(
        const part_graph_snapshot::Snapshot&              snapshot,
        const std::unordered_map<uint64_t, part_graph::BakeInputs>& bake_plan,
        const std::vector<uint64_t>&                      root_hashes,
        std::string& err);

    // Task 7 fix: per-part load failures recorded during fetch_parts() when
    // get_or_load returns null (skip-and-continue; returns true even with failures).
    struct FetchFailed { std::string module; std::string error; };
    const std::vector<FetchFailed>& fetch_failed() const { return fetch_failed_; }

    // Phase C Task 2: transient artifact routing (tmpfs scratch dir).
    // Configure the baker and store to route bakes of the listed modules to
    // a per-process scratch dir (/tmp/matter_transient/<pid>/). The scratch
    // dir is created with mkdir -p semantics.
    void set_transient_modules(std::set<std::string> modules);

    // Release a transient part: unlink its .part and .flat.part from scratch.
    // Safe no-op if the artifact is absent or not transient.
    void release_transient(uint64_t hash);

    // Access the transient scratch dir (for test assertions).
    const std::string& transient_dir() const { return transient_dir_; }
    const std::vector<std::string>& shared_lib_roots() const {
        return abs_shared_lib_roots_;
    }
    const matter::WorldSettings& world_settings() const {
        return world_settings_;
    }

    // Phase C Task 4: name of the module tagged `world` in world.manifest (empty
    // if no world-kind entry). Populated after install_graph(). Task 9 consumes it.
    const std::string& world_module() const { return world_module_; }

    // Task: authored entities parsed from `static entities` / buildEntities()
    // in the world script, populated by load_authored_world() and consumed
    // by the ECS runtime to bootstrap Flecs entities.
    const std::vector<matter::RawEntityRecipe>& authored_entities() const {
        return authored_entities_;
    }

    bool resolve_module_hash(const std::string& module_name, uint64_t& out_hash) const {
        for (const auto& kv : module_by_hash_) {
            if (kv.second == module_name) { out_hash = kv.first; return true; }
        }
        return false;
    }

    std::map<std::string, uint64_t> entity_part_hashes() const {
        std::map<std::string, uint64_t> out;
        for (const auto& kv : graph_snapshot_.nodes)
            if (kv.second.resolved_hash != 0)
                out[kv.first] = kv.second.resolved_hash;
        return out;
    }

#if defined(MATTER_HAVE_SCRIPT_HOST)
    // Phase C Task 9: expose the shared HostBaker so install_world can set the
    // world binding and bake sector child assets through the same baker instance.
    part_graph::HostBaker& host_baker() { return *host_baker_; }
#endif

private:
    LocalProviderConfig  cfg_;
    int                  baked_count_ = 0;
    int                  hit_count_   = 0;
    int                  baked_tileset_count_ = 0;
    int                  install_bake_count_ = 0; // counter for install-phase on_part callbacks
    std::set<uint64_t>   baked_hashes_;  // hashes freshly baked by last install_graph()
    std::map<uint64_t, std::string> module_by_hash_; // hash -> module name (from manifest roots)
    std::vector<matter::RawEntityRecipe> authored_entities_; // authored entity recipes from world script
    std::vector<FetchFailed> fetch_failed_; // Task 7 fix: per-part load failures from fetch_parts()
    part_graph_snapshot::Snapshot graph_snapshot_;  // Task 9: live-edit graph snapshot

    // State produced by install_graph(), consumed by compose_world().
    // Valid only after a successful install_graph() call.
    std::string abs_schemas_;
    std::string abs_world_path_;
    std::string abs_project_shared_lib_;
    std::string abs_engine_shared_lib_;
    std::vector<std::string> abs_shared_lib_roots_;
    std::string abs_cache_root_;

    // Select scratch for a transient hash only when its current .part is
    // compatible; all flat probe/load/expansion paths must reuse this decision.
    std::string artifact_root(uint64_t part_hash) const;

    // install_graph() output: roots split by kind, install result
    std::vector<part_graph::ChildRequest> roots_;           // all manifest roots
    std::vector<matter::Mat4f>            root_transforms_;
    std::vector<bool>                     expand_flags_;
    std::vector<bool>                     tileset_flags_;
    std::vector<part_graph::ChildRequest> roots_for_install_;
    std::vector<size_t>                   install_to_orig_;
    std::vector<size_t>                   tileset_indices_;
    size_t                                entity_part_root_start_ = 0;
    part_graph::InstallResult             ir_;
    static std::set<std::string> collect_entity_part_modules(
        const std::vector<matter::RawEntityRecipe>& entities);

#if defined(MATTER_HAVE_SCRIPT_HOST)
    // Owned objects that span both phases.
    std::unique_ptr<script_host::ScriptHost>            host_;
    std::unique_ptr<part_graph::FileModuleResolver>     resolver_;
    std::unique_ptr<part_graph::HostBaker>              host_baker_;  // Task 13: shared baker
#endif

    // Task 2: transient module routing state
    std::set<std::string> transient_modules_;
    std::string transient_dir_;

    // Task 4: world-kind module name from manifest (empty if none).
    std::string world_module_;
    world_lights::WorldLights authored_lights_;
    matter::WorldSettings world_settings_;

    bool prepare_paths(std::string& err);
    bool load_authored_world(std::string& err);
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
