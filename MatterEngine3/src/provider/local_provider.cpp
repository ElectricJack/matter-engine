#include "local_provider.h"

#include "part_graph.h"        // -DMATTER_HAVE_SCRIPT_HOST pulls in script_host.h
#include "part_asset_v2.h"     // cache_path_resolved, cache_path_flat, FlatInstanceRef
#include "part_flatten.h"      // bake-time subtree flattening
#include "world_lights.h"
#include "probe_volume.h"
#include "world_tracer.h"
#include "probe_bake.h"
#include "part_asset.h"   // fnv1a64
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "tileset_phase.h"
#include "tileset_bake_gpu.h"  // TilesetPhaseOpts
#include "tileset_provider.h"
#include "material_registry.h"

#if defined(MATTER_HAVE_AUTOREMESHER)
#include "mesh_retopo.hpp"     // retopo() TBB warm-up (see install_graph() below)
#include "mesh_indexed.hpp"
#include "mesh_transform.hpp"  // from_tri
#include "bvh.h"               // Tri / TriEx / float3
#include "retopo_blacklist.h"  // load persistent crash-recovery journal
#include <cmath>               // std::sqrt for warm-up mesh construction
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>         // std::bad_alloc (Task 7 fix: fetch_parts skip-and-continue)
#include <set>
#include <sstream>
#include <stdexcept>   // std::exception (Task 7 fix: fetch_parts skip-and-continue)
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#ifdef _WIN32
#include <direct.h>      // _mkdir
#include <stdlib.h>      // _fullpath, _MAX_PATH
#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif
#else
#include <limits.h>
#include <unistd.h>
#endif

using namespace part_graph;

namespace viewer {

// Deterministic splitmix64 (matches example_world's scatter exactly).
namespace {
// Filesystem portability shim: MinGW mkdir and realpath have different names.
#ifdef _WIN32
int  fs_mkdir(const char* p)                       { return _mkdir(p); }
bool fs_realpath(const char* in, char* out)        { return _fullpath(out, in, PATH_MAX) != nullptr; }
#else
int  fs_mkdir(const char* p)                       { return ::mkdir(p, 0755); }
bool fs_realpath(const char* in, char* out)        { return realpath(in, out) != nullptr; }
#endif
struct Rng64 {
    uint64_t s;
    explicit Rng64(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s += 0x9e3779b97f4a7c15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    float range(float a, float b) {
        return a + (float)((next() >> 11) * (1.0 / 9007199254740992.0)) * (b - a);
    }
};
void set_translate(float m[16], float x, float y, float z) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[3] = x; m[7] = y; m[11] = z;
}
// Resolve a path (possibly relative to cwd) to an absolute path.
std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (fs_realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

#if defined(MATTER_HAVE_AUTOREMESHER)
// Phase 5 autoremesher (Task 15): one-shot TBB / geogram warm-up.
// See MatterEngine3/tests/retopo_integration_tests.cpp for the empirical
// motivation: on WSL2, the first retopo() call segfaults during TBB
// "Multithreading enabled" init if it happens AFTER any heap-heavy work like
// part_asset::save_v2 (which HostBaker::bake calls for every part). Calling
// retopo() FIRST on a small valid mesh initializes the TBB scheduler in a
// clean state and every subsequent retopo() succeeds. The N=4 spherified cube
// is the same fixture used by the integration test — small (~96 tris) but
// non-degenerate for the cross-field parameterizer.
void tbb_warmup_retopo() {
    std::vector<Tri> tris;
    tris.reserve(6 * 4 * 4 * 2);
    struct Face { float o[3], u[3], v[3]; };
    const Face faces[6] = {
        { {-1,-1,-1}, {2,0,0}, {0,2,0} },
        { {-1,-1, 1}, {0,2,0}, {2,0,0} },
        { {-1,-1,-1}, {0,0,2}, {2,0,0} },
        { {-1, 1,-1}, {2,0,0}, {0,0,2} },
        { {-1,-1,-1}, {0,2,0}, {0,0,2} },
        { { 1,-1,-1}, {0,0,2}, {0,2,0} },
    };
    const int N = 4;
    auto project = [](float x, float y, float z) {
        float r = std::sqrt(x*x + y*y + z*z);
        return make_float3(x / r, y / r, z / r);
    };
    for (const auto& f : faces) {
        std::vector<float3> grid((N + 1) * (N + 1));
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                float s = static_cast<float>(i) / N;
                float t = static_cast<float>(j) / N;
                grid[j * (N + 1) + i] = project(
                    f.o[0] + s * f.u[0] + t * f.v[0],
                    f.o[1] + s * f.u[1] + t * f.v[1],
                    f.o[2] + s * f.u[2] + t * f.v[2]);
            }
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) {
                float3 a = grid[j * (N + 1) + i];
                float3 b = grid[j * (N + 1) + i + 1];
                float3 c = grid[(j + 1) * (N + 1) + i];
                float3 d = grid[(j + 1) * (N + 1) + i + 1];
                Tri t1, t2;
                t1.vertex0 = a; t1.vertex1 = b; t1.vertex2 = d;
                t1.centroid = make_float3((a.x+b.x+d.x)/3, (a.y+b.y+d.y)/3, (a.z+b.z+d.z)/3);
                t2.vertex0 = a; t2.vertex1 = d; t2.vertex2 = c;
                t2.centroid = make_float3((a.x+d.x+c.x)/3, (a.y+d.y+c.y)/3, (a.z+d.z+c.z)/3);
                tris.push_back(t1);
                tris.push_back(t2);
            }
    }
    MeshIndexed warm = from_tri(tris, nullptr);
    RetopoOptions opts;
    opts.threads = 1;
    RetopoResult wr = retopo(warm, opts);
    printf("LocalProvider: TBB warm-up retopo ok=%d elapsed=%.3fs\n",
           (int)wr.ok, wr.elapsed_seconds);
}
#endif

// Extract the class name from a JS schema source so install-phase on_part
// callbacks can report a human-readable module name. Schemas follow the pattern:
//   class ClassName extends Part { ... }
// Scan for "class " followed by an identifier, stopping at whitespace or '{'.
// Returns empty string on parse failure (callback receives null module).
std::string class_name_from_source(const std::string& source) {
    const char* kw = "class ";
    const size_t kwlen = 6;
    size_t pos = source.find(kw);
    while (pos != std::string::npos) {
        size_t name_start = pos + kwlen;
        // Skip whitespace between 'class' and the identifier
        while (name_start < source.size() &&
               (source[name_start] == ' ' || source[name_start] == '\t'))
            ++name_start;
        if (name_start >= source.size()) break;
        // Read the identifier
        size_t name_end = name_start;
        while (name_end < source.size() &&
               (std::isalnum((unsigned char)source[name_end]) || source[name_end] == '_' || source[name_end] == '$'))
            ++name_end;
        if (name_end > name_start) {
            // Accept it if followed by whitespace or '{' (not a method/variable name collision)
            if (name_end < source.size() &&
                (source[name_end] == ' ' || source[name_end] == '\t' ||
                 source[name_end] == '\n' || source[name_end] == '\r' ||
                 source[name_end] == '{'))
                return source.substr(name_start, name_end - name_start);
        }
        pos = source.find(kw, pos + 1);
    }
    return {};
}

} // namespace

LocalProvider::LocalProvider(LocalProviderConfig cfg) : cfg_(std::move(cfg)) {}

bool LocalProvider::install_graph(std::string& err, part_graph::BakePolicy policy) {
    // Reset all mutable state at entry so repeated install_graph() calls are
    // idempotent. Unload any previously-loaded tileset slots so a re-connect
    // for a different world doesn't inherit stale atlases. Also reset the
    // material-16 (DIRT) binding so the shader sees -1 (unbound) until the
    // new bake installs a fresh slot.
    viewer::tileset_provider::unload_all();
    MaterialRegistrySetGroundTilesetSlot(16, -1);
    baked_tileset_count_ = 0;

    baked_count_ = 0;
    hit_count_   = 0;
    install_bake_count_ = 0;
    baked_hashes_.clear();

    // Clear cross-phase state
    roots_.clear();
    expand_flags_.clear();
    tileset_flags_.clear();
    roots_for_install_.clear();
    install_to_orig_.clear();
    tileset_indices_.clear();
    ir_ = part_graph::InstallResult{};
    graph_snapshot_ = part_graph_snapshot::Snapshot{};  // Task 9: reset snapshot

    // Ensure the persistent cache dir exists.
    // All artifact writes are absolute-path (Task 3 Phase B: HostBaker::bake
    // passes parts_dir to bake_source via BakeOptions so no chdir is needed).
    fs_mkdir(cfg_.cache_root.c_str());
    std::string parts_subdir = cfg_.cache_root + "/parts";
    fs_mkdir(parts_subdir.c_str());

    // Resolve all relative paths to absolute now (cache_root may itself be relative).
    abs_schemas_    = abspath(cfg_.schemas_dir);
    abs_world_data_ = abspath(cfg_.world_data_dir);
    abs_shared_lib_ = abspath(cfg_.shared_lib_dir);
    abs_cache_root_ = abspath(cfg_.cache_root);

#if defined(MATTER_HAVE_AUTOREMESHER)
    // Warm the TBB scheduler BEFORE any heap-heavy work (install() calls
    // save_v2 for every fresh bake). Per the retopo_integration_tests docstring:
    // on WSL2 the first retopo() call segfaults during TBB init if it happens
    // AFTER save_v2 activity. This warms it once per process into a clean
    // state. Runs unconditionally: cost is <50 ms on a tiny cube, and it
    // avoids a conditional-execution hazard for worlds that DO have opt-ins.
    tbb_warmup_retopo();

    // Load the retopo blacklist journal. Any hash present in the .retopo_pending
    // journal without a matching .retopo_success entry crashed autoremesher on
    // a previous run and will be skipped in this session. See
    // MatterEngine3/include/retopo_blacklist.h for the mechanism.
    matter_engine3::retopo_blacklist::init(abs_cache_root_);
#endif

#if defined(MATTER_HAVE_SCRIPT_HOST)
    // SP-2/SP-3/SP-7 wiring. HostBaker receives the absolute cache root; it passes
    // it through BakeOptions.parts_dir so bake_source writes artifacts to absolute
    // paths (Task 3 Phase B: no chdir required).
    host_ = std::make_unique<script_host::ScriptHost>();
    host_->set_shared_lib_root(abs_shared_lib_);
    resolver_ = std::make_unique<part_graph::FileModuleResolver>(*host_, abs_schemas_);
    // Task 13 (Phase C): create a shared HostBaker that persists beyond install_graph()
    // so ensure_part_baked() can reuse it without reconstructing a ScriptHost.
    host_baker_ = std::make_unique<part_graph::HostBaker>(*host_, abs_cache_root_);

    // Task 2: apply transient settings to the baker (if set_transient_modules was called)
    if (!transient_modules_.empty()) {
        host_baker_->set_transient(&transient_modules_, transient_dir_);
    }

    // Capture cfg_ pointer for the RecordingBaker lambda (install-phase on_part).
    LocalProviderConfig* cfg_ptr = &cfg_;
    int* install_bake_count_ptr  = &install_bake_count_;

    // Task 5 (Phase B): RecordingBaker::bake() fires cfg_.on_part for
    // each freshly-baked part during install, with total==0 (indeterminate).
    // Task 13: RecordingBaker delegates to *host_baker_ (the shared HostBaker
    // member) instead of an inline inner, so ensure_part_baked() reuses it.
    part_graph::HostBaker* shared_baker_ptr = host_baker_.get();
    struct RecordingBaker : public Baker {
        part_graph::HostBaker& inner;
        LocalProviderConfig* cfg;
        int* install_bake_count;
        RecordingBaker(part_graph::HostBaker& b,
                       LocalProviderConfig* c, int* ibc)
            : inner(b), cfg(c), install_bake_count(ibc) {}
        uint64_t resolve_hash(const std::string& source, const Params& params,
                              const std::vector<uint64_t>& child_hashes) override {
            return inner.resolve_hash(source, params, child_hashes);
        }
        bool cached(uint64_t resolved_hash) override { return inner.cached(resolved_hash); }
        bool bake(const std::string& source, const Params& params,
                  const std::vector<uint64_t>& child_hashes,
                  const std::vector<std::string>& child_modules,
                  const std::vector<std::string>& child_params,
                  uint64_t resolved_hash) override {
            // Task 7: fire test_fault_hook (0-based index = current count BEFORE increment).
            // Exceptions propagate to PartGraph::install's per-node try-catch if present,
            // or to the worker's top-level catch; they serve as the OOM injection point.
            if (cfg && cfg->test_fault_hook)
                cfg->test_fault_hook(*install_bake_count);
            // Task 5 (Phase B): fire install-phase on_part before delegating.
            // total == 0 signals indeterminate count (install phase).
            if (cfg && cfg->on_part) {
                std::string class_name = class_name_from_source(source);
                const char* mod = class_name.empty() ? nullptr : class_name.c_str();
                cfg->on_part(mod, ++(*install_bake_count), 0);
            }
            return inner.bake(source, params, child_hashes, child_modules,
                              child_params, resolved_hash);
        }
        bool bake_lod_variants(const std::string& source, const Params& params,
                               const std::vector<uint64_t>& child_hashes,
                               uint64_t resolved_hash) override {
            return inner.bake_lod_variants(source, params, child_hashes, resolved_hash);
        }
        // Task 2: forward module notification to HostBaker for transient routing.
        void set_baking_module(const std::string& module) override {
            inner.set_baking_module(module);
        }
    };
    RecordingBaker baker(*shared_baker_ptr, cfg_ptr, install_bake_count_ptr);
    PartGraph graph(*resolver_, baker);

    bool manifest_ok = PartGraph::read_manifest(abs_world_data_, cfg_.world_name,
                                                roots_, err, &expand_flags_, &tileset_flags_);
    if (!manifest_ok) {
        return false;
    }

    // Tileset roots are installed by run_tileset_phase (it calls install() itself
    // on the tileset script's `static requires` children). Split them out here so
    // PartGraph::install() only sees the non-tileset roots.
    for (size_t i = 0; i < roots_.size(); ++i) {
        if (tileset_flags_[i]) tileset_indices_.push_back(i);
        else { roots_for_install_.push_back(roots_[i]); install_to_orig_.push_back(i); }
    }

    // Phase C Task 7: if a root_params_json override is set (e.g. {"worldSeed": 2}
    // from WorldSession::regenerate()), merge it into every root's params before
    // calling install() so merge_params_canonical (and hence the resolved hash)
    // reflects the override. Override keys win; keys absent from the override are
    // unchanged. Only manifest roots receive the override; child parts (scatter
    // schemas such as Rock/Grass) get their params exclusively from the parent's
    // `static requires` function, which intentionally does NOT forward worldSeed to
    // scatter children — so their hashes are seed-free and hit cache on a reroll.
    if (!cfg_.root_params_json.empty()) {
        Params override_params = params_from_json(cfg_.root_params_json);
        if (!override_params.empty()) {
            for (auto& root : roots_for_install_)
                for (const auto& kv : override_params)
                    root.params[kv.first] = kv.second;
        }
    }

    ir_ = graph.install(roots_for_install_, &graph_snapshot_, policy);
    if (!ir_.ok) {
        err = ir_.error;
        return false;
    }
    // Task 9: source_path is set by FileModuleResolver::source_path_for (called
    // from install's snapshot recording). Re-build the by_file index now that
    // source_path entries are present (they were set during install for FileModuleResolver).
    // The by_import index is already built in install; by_file is also built there.
    // Nothing extra needed here: install fills both indices directly.
    baked_count_ = (int)ir_.baked.size();
    hit_count_   = ir_.hits;
    baked_hashes_.insert(ir_.baked.begin(), ir_.baked.end());

    // Build hash -> module name map for use in fetch_parts()'s on_part callback.
    // Populate from roots first, then augment with all graph snapshot nodes so
    // that expanded children (e.g. BoxA placed inside a World expand root) also
    // get a module name in BakePartDone events and publish sort operations.
    module_by_hash_.clear();
    for (size_t j = 0; j < ir_.root_hashes.size(); ++j)
        if (ir_.root_hashes[j] != 0)
            module_by_hash_[ir_.root_hashes[j]] = roots_for_install_[j].module;
    for (const auto& kv : graph_snapshot_.nodes)
        if (kv.second.resolved_hash != 0)
            module_by_hash_.emplace(kv.second.resolved_hash, kv.second.module);

    // Map each root module to the child-FOLDED resolved hash the graph baked it under.
    // install() returns root_hashes parallel to `roots_for_install`; using them (instead
    // of an unfolded resolve_hash recompute) keeps manifest instances pointing at the .part
    // that actually exists on disk — critical once a root has children (e.g. Tree->Leaf).
    if (ir_.root_hashes.size() != roots_for_install_.size()) {
        err = "install did not return a hash for every root";
        return false;
    }

    return true;
#else
    // Without MATTER_HAVE_SCRIPT_HOST, the install phase can't do real baking.
    err = "install_graph: MATTER_HAVE_SCRIPT_HOST not defined";
    return false;
#endif
}

bool LocalProvider::ensure_part_baked(uint64_t part_hash, std::string& err) {
#if defined(MATTER_HAVE_SCRIPT_HOST)
    if (!host_baker_) {
        err = "ensure_part_baked: install_graph() has not been called";
        return false;
    }

    // Top-level entry guard: bake_plan covers every node of the installed graph.
    // An absent hash at the top level means the caller passed a stale or garbage
    // hash — fail fast rather than silently succeeding with no bake output.
    if (ir_.bake_plan.find(part_hash) == ir_.bake_plan.end()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)part_hash);
        err = std::string("ensure_part_baked: hash ") + buf + " not in bake plan";
        return false;
    }

    // Post-order DFS over bake_plan children so children are baked before parents.
    // Each node: cached() short-circuits; otherwise bake + bake_lod_variants.
    // Visited set prevents double-visiting in a DAG (shared children).
    std::set<uint64_t> visited;
    std::string bake_err;

    std::function<bool(uint64_t)> bake_subtree = [&](uint64_t hash) -> bool {
        if (visited.count(hash)) return true;
        visited.insert(hash);

        auto it = ir_.bake_plan.find(hash);
        if (it == ir_.bake_plan.end()) {
            // Not in bake_plan — already baked by install (BakePolicy::All path)
            // or not a known node. Treat as cached/ok for recursive child lookups.
            return true;
        }
        const part_graph::BakeInputs& bi = it->second;

        // Recurse into children first (post-order).
        for (uint64_t child_hash : bi.child_hashes) {
            if (!bake_subtree(child_hash)) return false;
        }

        // cached() short-circuits — counts as a demand-phase cache hit.
        if (host_baker_->cached(hash)) {
            ++hit_count_;
            return true;
        }

        // Fire on_part callback (demand phase, total==0 signals indeterminate count).
        if (cfg_.on_part) {
            const char* mod = bi.module.empty() ? nullptr : bi.module.c_str();
            cfg_.on_part(mod, ++install_bake_count_, 0);
        }

        // Set the baking module for transient routing (must precede bake call)
        host_baker_->set_baking_module(bi.module);

        // Bake
        bool bake_ok = false;
        try {
            bake_ok = host_baker_->bake(bi.source, bi.params, bi.child_hashes,
                                        bi.child_modules, bi.child_params, hash);
        } catch (std::bad_alloc&) {
            bake_err = "out of memory baking part: " + bi.module;
            return false;
        } catch (std::exception& e) {
            bake_err = std::string("exception baking part: ") + bi.module + ": " + e.what();
            return false;
        } catch (...) {
            bake_err = "unknown exception baking part: " + bi.module;
            return false;
        }
        if (!bake_ok) {
            bake_err = "bake failed for part: " + bi.module;
            return false;
        }

        // bake_lod_variants (mirrors install's per-node call)
        if (!host_baker_->bake_lod_variants(bi.source, bi.params, bi.child_hashes, hash)) {
            bake_err = "lod-variant bake failed for part: " + bi.module;
            return false;
        }

        // Track freshly demand-baked parts in baked_count_ and baked_hashes_ so
        // frame_stats().parts_baked reflects demand-phase activity and future
        // reconcile() calls know this hash is freshly written to disk.
        ++baked_count_;
        baked_hashes_.insert(hash);

        return true;
    };

    bool ok = bake_subtree(part_hash);
    if (!ok) err = bake_err;
    return ok;
#else
    err = "ensure_part_baked: MATTER_HAVE_SCRIPT_HOST not defined";
    return false;
#endif
}

bool LocalProvider::ensure_part_flattened(uint64_t part_hash) {
    // Identical logic to compose_world's flatten_one lambda (moved here verbatim
    // as a member; compose_world delegates to this function).
    // Transient parts live in scratch; their flats belong there too (never the cache).
    std::string root = abs_cache_root_;
    if (!transient_dir_.empty()) {
        const std::string scratch_part =
            transient_dir_ + "/" + part_asset::cache_path_resolved(part_hash);
        if (part_asset::peek_format_version(scratch_part) != 0)
            root = transient_dir_;
    }
    const std::string flat_abs_path =
        root + "/" + part_asset::cache_path_flat(part_hash);
    if (part_asset::peek_format_version(flat_abs_path) == part_asset::kFormatVersionFlat)
        return true;
    part_flatten::FlattenResult fr =
        part_flatten::flatten_part(root, part_hash);
    if (fr.ok) {
        printf("LocalProvider: flattened %016llx (%zu clusters, %zu levels, %zu -> %zu tris, %zu instance_refs)\n",
               (unsigned long long)part_hash, fr.clusters, fr.levels,
               fr.full_tris, fr.coarsest_tris, fr.instance_refs);
        return true;
    } else {
        printf("LocalProvider: flatten failed for %016llx: %s\n",
               (unsigned long long)part_hash, fr.error.c_str());
        return false;
    }
}

bool LocalProvider::compose_world(WorldManifest& out, std::string& err) {
    // Requires install_graph() to have succeeded.
    // All absolute paths (abs_*_) are set by install_graph().

    // Reset tileset count (may be called again for cone rebake).
    baked_tileset_count_ = 0;

    out.world_root_hash = 1;
    out.instances.clear();
    uint32_t next_id = 1;
    auto place = [&](uint64_t h, float x, float y, float z, const std::string& mod = {}) {
        WorldManifestEntry e;
        e.instance_id = next_id++;
        e.part_hash   = h;
        e.module      = mod;
        set_translate(e.transform, x, y, z);
        out.instances.push_back(e);
    };

    // Note: flatten_placed() and append_instance_refs() have moved to the publish
    // pipeline (matter_engine.cpp::publish_pipeline per-part bake+flatten+streaming).
    // compose_world() now only places roots and runs the tileset phase; per-part
    // flatten and FlatInstanceRef expansion happen on-demand in the publish loop.
    // connect() (sync API) still runs flatten+refs eagerly via its own compose path.

    // Generic placement: every non-tileset manifest root is placed at the origin, except
    // roots flagged `expand`, whose baked child-instance table is promoted to
    // individual world instances (per-child LOD, culling, and instanced
    // batching downstream). Tileset roots are handled separately below.
    for (size_t i = 0; i < roots_.size(); ++i) {
        if (tileset_flags_[i]) {
            // Handled below via run_tileset_phase; not placed as a world instance.
            continue;
        }
        // Map back to the install index for this original root.
        size_t k = 0; bool found = false;
        for (size_t j = 0; j < install_to_orig_.size(); ++j)
            if (install_to_orig_[j] == i) { k = j; found = true; break; }
        if (!found) continue;  // (unreachable — every non-tileset root was installed)
        // Task 7: skip roots that failed during install (root_hash == 0 → failed).
        if (ir_.root_hashes[k] == 0) continue;
        if (expand_flags_[i]) {
            if (!append_expanded_children(abs_cache_root_, ir_.root_hashes[k],
                                          next_id, out.instances, err))
                return false;
        } else {
            place(ir_.root_hashes[k], 0.0f, 0.0f, 0.0f, roots_[i].module);
        }
    }
    // Backfill module names for expanded children: append_expanded_children does
    // not know the module name of each child hash, but module_by_hash_ (built at
    // install time from graph_snapshot_.nodes) covers the full graph. Fill in any
    // empty module fields so BakePartDone events and publish-sort operations have
    // correct labels for all placed parts (roots and expanded children alike).
    for (auto& e : out.instances) {
        if (e.module.empty()) {
            auto it = module_by_hash_.find(e.part_hash);
            if (it != module_by_hash_.end()) e.module = it->second;
        }
    }

    // ---- Tileset roots: deferred (Task 15) -----------------------------------
    // Tileset roots are no longer run in compose_world. They run after BakeFinished
    // in the deferred tileset phase (publish_pipeline tail in matter_engine.cpp)
    // via run_tileset_deferred(). This removes the ~350s box3d settle wall from
    // the silhouette critical path.
    //
    // connect() (synchronous API) still runs them eagerly via run_tileset_deferred
    // called immediately after compose_world().
    //
    // Guard: fail-closed BEFORE any GL/disk work if the manifest declares more
    // tileset roots than we have sampler-array slots.
    if ((int)tileset_indices_.size() > viewer::tileset_provider::max_slots()) {
        err = "LocalProvider: manifest declares " +
              std::to_string(tileset_indices_.size()) +
              " tileset roots but only " +
              std::to_string(viewer::tileset_provider::max_slots()) +
              " slots are available";
        return false;
    }
    // tileset roots placed/slotted later; baked_tileset_count_ stays 0 until deferred phase.

    // --- Parse world lights ---
    {
        const std::string manifest_path = abs_world_data_ + "/" + cfg_.world_name + "/world.manifest";
        std::string lights_err;
        if (!world_lights::parse_lights(manifest_path, out.lights, lights_err)) {
            printf("LocalProvider: warning: light parse failed: %s\n", lights_err.c_str());
            out.lights = world_lights::WorldLights{};  // keep defaults
        }
    }

    // --- Compute probe fingerprint ---
    // Fold: each instance (part_hash, transform[16]) in manifest order,
    // then bake grid constants packed as a struct, then lights_fingerprint(out.lights).
    {
        probe_bake::BakeParams bake_params;   // default BakeParams
        std::vector<uint8_t> fp_buf;

        // 1. Instances (part_hash u64 + transform[16] floats)
        for (const auto& e : out.instances) {
            const size_t off = fp_buf.size();
            fp_buf.resize(off + sizeof(uint64_t) + 16 * sizeof(float));
            std::memcpy(fp_buf.data() + off, &e.part_hash, sizeof(uint64_t));
            std::memcpy(fp_buf.data() + off + sizeof(uint64_t), e.transform, 16 * sizeof(float));
        }

        // 2. Bake grid constants (packed struct of raw bytes)
        struct BakeGridKey {
            float cell;
            int   max_cells_axis;
            int   pad_cells;
            int   rays_per_cell;
            int   sun_rays;
            float sun_cone_deg;
        } gk;
        static_assert(sizeof(BakeGridKey) == 24, "fingerprinted byte-for-byte; no padding allowed");
        gk.cell           = bake_params.cell;
        gk.max_cells_axis = bake_params.max_cells_axis;
        gk.pad_cells      = bake_params.pad_cells;
        gk.rays_per_cell  = bake_params.rays_per_cell;
        gk.sun_rays       = bake_params.sun_rays;
        gk.sun_cone_deg   = bake_params.sun_cone_deg;
        {
            const size_t off = fp_buf.size();
            fp_buf.resize(off + sizeof(BakeGridKey));
            std::memcpy(fp_buf.data() + off, &gk, sizeof(BakeGridKey));
        }

        // 3. Lights fingerprint (u64 appended as bytes)
        uint64_t lf = world_lights::lights_fingerprint(out.lights);
        {
            const size_t off = fp_buf.size();
            fp_buf.resize(off + sizeof(uint64_t));
            std::memcpy(fp_buf.data() + off, &lf, sizeof(uint64_t));
        }

        const uint64_t probe_fingerprint = part_asset::fnv1a64(fp_buf.data(), fp_buf.size());

        // --- Probe cache path: <cache_root>/cache/<world_name>.probes ---
        const std::string cache_subdir = abs_cache_root_ + "/cache";
        {
            int r = fs_mkdir(cache_subdir.c_str());
            (void)r;   // EEXIST is fine; mirrors the parts/ mkdir above
        }
        const std::string probes_path = cache_subdir + "/" + cfg_.world_name + ".probes";

        // --- Try to load cached probes ---
        probe_volume::ProbeVolume vol;
        bool loaded = probe_volume::load_probes(probes_path, vol, probe_fingerprint);
        if (loaded) {
            out.probes = std::make_shared<probe_volume::ProbeVolume>(std::move(vol));
        } else {
            // Cache miss or stale -> re-bake
            // Build TraceInstance list from manifest instances.
            std::vector<world_tracer::TraceInstance> trace_instances;
            trace_instances.reserve(out.instances.size());
            for (const auto& e : out.instances) {
                world_tracer::TraceInstance ti;
                ti.part_hash = e.part_hash;
                std::memcpy(ti.transform, e.transform, sizeof(ti.transform));
                trace_instances.push_back(ti);
            }

            // Build tracer (non-fatal on failure).
            world_tracer::WorldTracer tracer;
            std::string tracer_err;
            if (!tracer.build(abs_cache_root_, trace_instances, tracer_err)) {
                printf("probe bake failed: %s\n", tracer_err.c_str());
                // out.probes stays null -> fallback shading
            } else {
                // Bake probes and measure wall time.
                auto t0 = std::chrono::steady_clock::now();
                probe_volume::ProbeVolume baked = probe_bake::bake_probes(tracer, out.lights, bake_params);
                auto t1 = std::chrono::steady_clock::now();
                double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

                if (!baked.valid()) {
                    printf("probe bake failed: baked volume is invalid\n");
                    // out.probes stays null -> fallback shading
                } else {
                    printf("probes: %dx%dx%d baked in %.1fs\n",
                           baked.grid.nx, baked.grid.ny, baked.grid.nz, elapsed_s);

                    // Save to cache (non-fatal on failure).
                    if (!probe_volume::save_probes(probes_path, baked, probe_fingerprint)) {
                        printf("probe bake failed: could not save probes to %s\n",
                               probes_path.c_str());
                        // Still assign probes even if save failed (runtime will work, no cache).
                    }
                    out.probes = std::make_shared<probe_volume::ProbeVolume>(std::move(baked));
                }
            }
        }
    }

    return true;
}

bool LocalProvider::run_tileset_deferred(
    std::function<void(int done, int total, const char* module)> on_tileset_part,
    std::function<bool()> is_cancelled,
    std::string& err)
{
#if defined(MATTER_HAVE_SCRIPT_HOST)
    // Guard: max slots checked at compose_world time; indices already validated.
    const int total = (int)tileset_indices_.size();

    // Phase B: route GL work through gpu_run when set; fall back to inline.
    auto run_gl = [&](const char* name, std::function<bool(std::string&)> fn,
                      std::string& e) -> bool {
        if (cfg_.gpu_run) return cfg_.gpu_run(name, std::move(fn), e);
        return fn(e);
    };

    for (int idx = 0; idx < total; ++idx) {
        if (is_cancelled && is_cancelled()) {
            err = "tileset deferred phase cancelled";
            return false;
        }

        const size_t ti = tileset_indices_[(size_t)idx];
        const std::string root_module = roots_[ti].module;

        if (on_tileset_part)
            on_tileset_part(idx, total, root_module.c_str());

        const int slot_idx = baked_tileset_count_;

        if (!cfg_.gl_available) {
            // Headless: settle-only (no GPU atlas).
            fprintf(stderr, "[local_provider] headless deferred tileset: '%s'\n",
                    root_module.c_str());
            fflush(stderr);
            std::string se;
            tileset::SettledTorus settled;
            if (!tileset::run_tileset_phase(abs_world_data_, cfg_.world_name, root_module,
                                            abs_cache_root_, settled, se,
                                            abs_shared_lib_)) {
                err = "LocalProvider: tileset '" + root_module +
                      "' settle failed (headless): " + se;
                return false;
            }
            printf("LocalProvider: tileset '%s' settle ok (deferred headless)\n",
                   root_module.c_str());

            if (on_tileset_part)
                on_tileset_part(idx + 1, total, root_module.c_str());
            continue;
        }

        // GL available: settle on the worker (CPU-only, cache-wired), then GPU bake on GL thread.
        const bool dump_png = std::getenv("MATTER_TILESET_DUMP_PNG") != nullptr;

        // Step 1 (worker thread): physics settle + cache load/save — no GL needed.
        tileset::SettledTorus settled;
        {
            std::string se;
            if (!tileset::run_tileset_phase(abs_world_data_, cfg_.world_name, root_module,
                                            abs_cache_root_, settled, se, abs_shared_lib_)) {
                err = "LocalProvider: tileset '" + root_module + "' settle failed: " + se;
                return false;
            }
        }

        // Compute script source hash on the worker (needed by bake_tileset_gpu for .gtex cache key).
        const std::string root_js_path =
            abs_world_data_ + "/../schemas/" + root_module + ".js";
        uint64_t script_source_hash = 0;
        {
            std::ifstream jf(root_js_path, std::ios::binary);
            if (jf) {
                std::ostringstream ss; ss << jf.rdbuf();
                const std::string src = ss.str();
                script_source_hash = part_asset::fnv1a64(src.data(), src.size());
            }
            // If the file can't be read, hash stays 0 — bake_tileset_gpu will force-rebake.
        }
        const std::string gtex_path = abs_world_data_ + "/" + root_module + ".gtex";

        // Step 2 (GL thread): GPU atlas bake + slot upload — GL required.
        std::string te;
        bool ok = run_gl(root_module.c_str(), [&](std::string& ge) -> bool {
            tileset::BakeInputs bi; bi.parts_cache_dir = abs_cache_root_;
            std::string be;
            if (!tileset::bake_tileset_gpu(settled, script_source_hash, gtex_path,
                                           bi, false, dump_png, be)) {
                ge = "bake_tileset_gpu(" + root_module + "): " + be +
                     " (if a GL error: set GALLIUM_DRIVER=d3d12 on WSLg)";
                return false;
            }
            std::string le;
            if (!viewer::tileset_provider::load_slot(slot_idx, gtex_path, le)) {
                ge = "tileset_provider::load_slot(" + gtex_path + "): " + le;
                return false;
            }
            MaterialRegistrySetGroundTilesetSlot(16, slot_idx);
            printf("LocalProvider: tileset '%s' -> slot %d (%s) [deferred]\n",
                   root_module.c_str(), slot_idx, gtex_path.c_str());
            return true;
        }, te);

        if (!ok) {
            err = "LocalProvider: tileset '" + root_module + "': " + te;
            return false;
        }
        ++baked_tileset_count_;

        if (on_tileset_part)
            on_tileset_part(idx + 1, total, root_module.c_str());
    }
    return true;
#else
    (void)on_tileset_part;
    (void)is_cancelled;
    err = "run_tileset_deferred: MATTER_HAVE_SCRIPT_HOST not defined";
    return false;
#endif
}

bool LocalProvider::connect(WorldManifest& out, std::string& err) {
    // Sync API: keep eager behavior — BakePolicy::All bakes every node at install.
    // After compose_world (which now skips flatten/refs in the async path),
    // eagerly flatten all placed roots and expand FlatInstanceRefs here.
    if (!install_graph(err, part_graph::BakePolicy::All)) return false;
    if (!compose_world(out, err)) return false;

    // Eager flatten: every placed root gets a .flat.part so synchronous callers
    // (tests, gallery_bake, viewer_logic_tests) see the flat artifacts immediately.
    {
        std::set<uint64_t> done;
        for (const auto& e : out.instances) {
            if (!done.insert(e.part_hash).second) continue;
            ensure_part_flattened(e.part_hash);
        }
    }

    // Eager FlatInstanceRef expansion: read each placed root's .flat.part and
    // append world entries for any instance refs (BOUNDARY-path roots like
    // StressForest). Iterates to a fixed point so nested boundaries expand too.
    {
        uint32_t next_id = out.instances.empty() ? 1u :
            (out.instances.back().instance_id + 1u);
        std::set<uint64_t> visited;
        while (true) {
            std::vector<uint64_t> to_process;
            std::set<uint64_t> seen;
            for (const auto& e : out.instances) {
                if (!seen.insert(e.part_hash).second) continue;
                if (visited.count(e.part_hash)) continue;
                to_process.push_back(e.part_hash);
            }
            if (to_process.empty()) break;
            const size_t before = out.instances.size();
            for (uint64_t ph : to_process) {
                visited.insert(ph);
                ensure_part_flattened(ph);
                const std::string flat_abs_path =
                    abs_cache_root_ + "/" + part_asset::cache_path_flat(ph);
                if (part_asset::peek_format_version(flat_abs_path) !=
                    part_asset::kFormatVersionFlat) continue;
                BLASManager scratch_blas;
                TLASManager scratch_tlas(4);
                std::vector<part_asset::FlatCluster> clusters_ignored;
                std::vector<part_asset::FlatInstanceRef> refs;
                if (!part_asset::load_flat_v3(flat_abs_path, ph, scratch_blas,
                                              scratch_tlas, clusters_ignored, refs))
                    continue;
                if (refs.empty()) continue;
                out.instances.reserve(out.instances.size() + refs.size());
                for (const auto& r : refs) {
                    WorldManifestEntry we;
                    we.instance_id = next_id++;
                    we.part_hash   = r.child_resolved_hash;
                    std::memcpy(we.transform, r.transform, sizeof(we.transform));
                    out.instances.push_back(we);
                }
            }
            if (out.instances.size() == before) break;  // fixed point
        }
    }

    // Task 15: sync API eagerly runs the deferred tileset phase (headless or GL).
    // Async path runs this after BakeFinished via publish_pipeline step 9.
    // Null callbacks: no progress reporting, no cancellation on the sync path.
    // FATAL on this sync path (pre-Task-15 behavior): connect() callers (tests,
    // gallery_bake, viewer_logic_tests) expect a fully prepared world on success.
    if (!tileset_indices_.empty()) {
        std::string te;
        if (!run_tileset_deferred(nullptr, nullptr, te)) {
            err = "LocalProvider::connect: tileset phase failed: " + te;
            return false;
        }
    }
    return true;
}

std::vector<uint64_t>
LocalProvider::reconcile(const WorldManifest& manifest, const PartStore& store) {
    // Return unique hashes that need to be fetched/loaded:
    //  - Newly baked this session (baked_hashes_): just written to disk, not yet
    //    loaded into the store's memory.
    //  - Not found on disk at all (store.has() covers both in-memory and disk):
    //    handles the case of a partially populated cache.
    std::vector<uint64_t> want;
    std::set<uint64_t> seen;
    for (const auto& e : manifest.instances) {
        if (!seen.insert(e.part_hash).second) continue;
        if (baked_hashes_.count(e.part_hash) || !store.has(e.part_hash))
            want.push_back(e.part_hash);
    }
    return want;
}

bool LocalProvider::fetch_parts(const std::vector<uint64_t>& want,
                                PartStore& store, std::string& err) {
    // LocalProvider already wrote the .part blobs to the shared cache during
    // connect()'s install; "fetching" is just loading them into the store.
    //
    // Task 7 fix: null get_or_load no longer hard-aborts (old: return false).
    // Instead, record the failure into fetch_failed_ and continue to the next
    // part. Return true if the loop completed, even with failures; callers that
    // care about partial failures inspect fetch_failed(). This matches the plan:
    // "a get_or_load null return appends to a failed list instead of returning false".
    //
    // Remaining callers of fetch_parts():
    //   - viewer_logic_tests.cpp:182  — direct unit test; now gets true-with-failures
    //     and can inspect fetch_failed() for assertions (expected by plan).
    //   - viewer_logic_tests.cpp:1268 — install_phase_progress test; same behavior.
    // The async path (execute_bake) does NOT call fetch_parts; it calls
    // store->get_or_load() directly in publish jobs. Both callers now see
    // skip-and-continue, which is the intended behavior per the plan.
    fetch_failed_.clear();
    for (size_t i = 0; i < want.size(); ++i) {
        uint64_t h = want[i];
        // Task 7: fire test_fault_hook (0-based index) with per-part exception catch.
        std::string module_name;
        {
            auto it = module_by_hash_.find(h);
            if (it != module_by_hash_.end()) module_name = it->second;
        }

        bool part_failed = false;
        try {
            if (cfg_.test_fault_hook) cfg_.test_fault_hook((int)i);
            if (!store.get_or_load(h)) {
                FetchFailed ff;
                ff.module = module_name;
                ff.error  = "load failed for part " + std::to_string(h);
                fetch_failed_.push_back(std::move(ff));
                part_failed = true;
            }
        } catch (std::bad_alloc&) {
            FetchFailed ff;
            ff.module = module_name;
            ff.error  = "std::bad_alloc loading part " + std::to_string(h);
            fetch_failed_.push_back(std::move(ff));
            part_failed = true;
        } catch (std::exception& ex) {
            FetchFailed ff;
            ff.module = module_name;
            ff.error  = ex.what();
            fetch_failed_.push_back(std::move(ff));
            part_failed = true;
        }

        if (part_failed) continue;  // skip-and-continue

        if (cfg_.on_part) {
            const char* mod = module_name.empty() ? nullptr : module_name.c_str();
            cfg_.on_part(mod, (int)(i + 1), (int)want.size());
        }
    }
    // Return true (loop completed) even with partial failures. Report empty err
    // string on partial failure — callers inspect fetch_failed() for details.
    // Return false only if there is a fatal structural failure (none currently
    // possible in this implementation).
    (void)err;  // no fatal error path in this implementation
    return true;
}

bool LocalProvider::poll_deltas(WorldDelta&) { return false; }  // static world

bool append_expanded_children(const std::string& cache_root, uint64_t root_hash,
                              uint32_t& next_id,
                              std::vector<WorldManifestEntry>& out_instances,
                              std::string& err) {
    const std::string path = cache_root + "/" + part_asset::cache_path_resolved(root_hash);
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    if (!part_asset::load_v2(path, root_hash, blas, tlas, children, lods)) {
        err = "expand: failed to load root part " + path;
        return false;
    }
    if (children.empty()) {
        err = "expand: root has no children (nothing to expand)";
        return false;
    }
    out_instances.reserve(out_instances.size() + children.size());
    for (const auto& c : children) {
        WorldManifestEntry e;
        e.instance_id = next_id++;
        e.part_hash   = c.child_resolved_hash;
        std::memcpy(e.transform, c.transform, sizeof(e.transform));
        out_instances.push_back(e);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Phase C Task 17 — resolve cache restore hook
// ---------------------------------------------------------------------------

bool LocalProvider::restore_from_cache(
    const part_graph_snapshot::Snapshot&              snapshot,
    const std::unordered_map<uint64_t, part_graph::BakeInputs>& bake_plan,
    const std::vector<uint64_t>&                      root_hashes,
    std::string& err)
{
#if defined(MATTER_HAVE_SCRIPT_HOST)
    // Reset mutable state (mirrors the preamble of install_graph()).
    viewer::tileset_provider::unload_all();
    MaterialRegistrySetGroundTilesetSlot(16, -1);
    baked_tileset_count_ = 0;
    baked_count_  = 0;
    hit_count_    = 0;
    install_bake_count_ = 0;
    baked_hashes_.clear();
    roots_.clear();
    expand_flags_.clear();
    tileset_flags_.clear();
    roots_for_install_.clear();
    install_to_orig_.clear();
    tileset_indices_.clear();
    ir_ = part_graph::InstallResult{};
    graph_snapshot_ = part_graph_snapshot::Snapshot{};

    // Resolve absolute paths.
    fs_mkdir(cfg_.cache_root.c_str());
    std::string parts_subdir = cfg_.cache_root + "/parts";
    fs_mkdir(parts_subdir.c_str());
    abs_schemas_    = abspath(cfg_.schemas_dir);
    abs_world_data_ = abspath(cfg_.world_data_dir);
    abs_shared_lib_ = abspath(cfg_.shared_lib_dir);
    abs_cache_root_ = abspath(cfg_.cache_root);

    // Initialise ScriptHost + HostBaker so ensure_part_baked() can re-bake
    // individual cache-miss parts without running a global resolve.
    host_ = std::make_unique<script_host::ScriptHost>();
    host_->set_shared_lib_root(abs_shared_lib_);
    resolver_ = std::make_unique<part_graph::FileModuleResolver>(*host_, abs_schemas_);
    host_baker_ = std::make_unique<part_graph::HostBaker>(*host_, abs_cache_root_);

    // Task 2: apply transient settings to the baker (if set_transient_modules was called)
    if (!transient_modules_.empty()) {
        host_baker_->set_transient(&transient_modules_, transient_dir_);
    }

    // Read the world manifest to populate roots_/expand_flags_/tileset_flags_/
    // tileset_indices_ so run_tileset_deferred() still operates correctly.
    {
        std::string merr;
        if (!PartGraph::read_manifest(abs_world_data_, cfg_.world_name,
                                      roots_, merr, &expand_flags_, &tileset_flags_)) {
            err = "restore_from_cache: failed to read manifest: " + merr;
            return false;
        }
        for (size_t i = 0; i < roots_.size(); ++i) {
            if (tileset_flags_[i]) tileset_indices_.push_back(i);
            else { roots_for_install_.push_back(roots_[i]); install_to_orig_.push_back(i); }
        }
    }

    // Apply root_params_json override (mirrors install_graph's merge step).
    if (!cfg_.root_params_json.empty()) {
        // We don't actually need to merge params into roots_ here because we're
        // not re-resolving the graph — the cached root_hashes already reflect the
        // override. We still populate roots_ for tileset phase which will re-eval
        // its own script; tileset scripts don't use root_params_json.
        (void)cfg_.root_params_json;
    }

    // Restore cache payload into ir_.
    ir_.ok          = true;
    ir_.root_hashes = root_hashes;
    ir_.bake_plan   = bake_plan;

    // Restore graph snapshot.
    graph_snapshot_ = snapshot;

    // Build module_by_hash_ from snapshot nodes (for diagnostics / module labels).
    module_by_hash_.clear();
    for (const auto& kv : graph_snapshot_.nodes)
        module_by_hash_[kv.second.resolved_hash] = kv.second.module;

    return true;
#else
    err = "restore_from_cache: MATTER_HAVE_SCRIPT_HOST not defined";
    return false;
#endif
}

bool LocalProvider::try_load_cached_probes(WorldManifest& m) {
    // Replicates the probe fingerprint computation from compose_world() so the
    // same .probes cache file is used on a warm (cache-hit) launch.
    probe_bake::BakeParams bake_params;  // default BakeParams (same as compose_world)
    std::vector<uint8_t> fp_buf;

    // 1. Instances (part_hash u64 + transform[16] floats)
    for (const auto& e : m.instances) {
        const size_t off = fp_buf.size();
        fp_buf.resize(off + sizeof(uint64_t) + 16 * sizeof(float));
        std::memcpy(fp_buf.data() + off, &e.part_hash, sizeof(uint64_t));
        std::memcpy(fp_buf.data() + off + sizeof(uint64_t), e.transform, 16 * sizeof(float));
    }

    // 2. Bake grid constants (packed struct, same layout as compose_world)
    struct BakeGridKey {
        float cell;
        int   max_cells_axis;
        int   pad_cells;
        int   rays_per_cell;
        int   sun_rays;
        float sun_cone_deg;
    } gk;
    static_assert(sizeof(BakeGridKey) == 24, "probe fingerprint struct size mismatch");
    gk.cell           = bake_params.cell;
    gk.max_cells_axis = bake_params.max_cells_axis;
    gk.pad_cells      = bake_params.pad_cells;
    gk.rays_per_cell  = bake_params.rays_per_cell;
    gk.sun_rays       = bake_params.sun_rays;
    gk.sun_cone_deg   = bake_params.sun_cone_deg;
    {
        const size_t off = fp_buf.size();
        fp_buf.resize(off + sizeof(BakeGridKey));
        std::memcpy(fp_buf.data() + off, &gk, sizeof(BakeGridKey));
    }

    // 3. Lights fingerprint
    uint64_t lf = world_lights::lights_fingerprint(m.lights);
    {
        const size_t off = fp_buf.size();
        fp_buf.resize(off + sizeof(uint64_t));
        std::memcpy(fp_buf.data() + off, &lf, sizeof(uint64_t));
    }

    const uint64_t probe_fingerprint = part_asset::fnv1a64(fp_buf.data(), fp_buf.size());

    // Try to load from the probe cache.
    const std::string cache_subdir = abs_cache_root_ + "/cache";
    const std::string probes_path  = cache_subdir + "/" + cfg_.world_name + ".probes";
    probe_volume::ProbeVolume vol;
    if (!probe_volume::load_probes(probes_path, vol, probe_fingerprint))
        return false;
    m.probes = std::make_shared<probe_volume::ProbeVolume>(std::move(vol));
    return true;
}

// Task 2: transient artifact routing
void LocalProvider::set_transient_modules(std::set<std::string> modules) {
    transient_modules_ = std::move(modules);

    // Create the scratch dir: /tmp/matter_transient/<pid>/
    // Use mkdir -p semantics (safe no-op if already exists).
    pid_t pid = ::getpid();
    transient_dir_ = "/tmp/matter_transient/" + std::to_string(pid);

    // mkdir -p: ensure parent dirs exist
    std::system(("mkdir -p " + transient_dir_).c_str());

    // Note: baker and store configuration happens in install_graph() once they're created.
    // PartStore is owned at a higher level, so callers must call
    // store.set_scratch_dir(prov->transient_dir()) independently.
}

void LocalProvider::release_transient(uint64_t hash) {
    if (transient_dir_.empty()) return;  // not configured

    // Unlink .part and .flat.part from scratch
    const std::string part_path = transient_dir_ + "/" + part_asset::cache_path_resolved(hash);
    const std::string flat_path = transient_dir_ + "/" + part_asset::cache_path_flat(hash);

    ::unlink(part_path.c_str());
    ::unlink(flat_path.c_str());
}

} // namespace viewer
