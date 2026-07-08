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
#include "mesh_retopo.hpp"     // retopo() TBB warm-up (see connect() below)
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
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>
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
// Filesystem portability shim: MinGW lacks the POSIX mkdir(mode)/realpath and
// spells getcwd/chdir with leading underscores.
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
} // namespace

LocalProvider::LocalProvider(LocalProviderConfig cfg) : cfg_(std::move(cfg)) {}

bool LocalProvider::connect(WorldManifest& out, std::string& err) {
    // Reset all mutable state at entry so repeated connect() calls are
    // idempotent. Unload any previously-loaded tileset slots so a re-connect
    // for a different world doesn't inherit stale atlases. Also reset the
    // material-16 (DIRT) binding so the shader sees -1 (unbound) until the
    // new bake installs a fresh slot.
    viewer::tileset_provider::unload_all();
    MaterialRegistrySetGroundTilesetSlot(16, -1);
    baked_tileset_count_ = 0;

    baked_count_ = 0;
    hit_count_   = 0;
    baked_hashes_.clear();

    // Ensure the persistent cache dir exists.
    // All artifact writes are absolute-path (Task 3 Phase B: HostBaker::bake
    // passes parts_dir to bake_source via BakeOptions so no chdir is needed).
    fs_mkdir(cfg_.cache_root.c_str());
    std::string parts_subdir = cfg_.cache_root + "/parts";
    fs_mkdir(parts_subdir.c_str());

    // Resolve all relative paths to absolute now (cache_root may itself be relative).
    std::string abs_schemas    = abspath(cfg_.schemas_dir);
    std::string abs_world_data = abspath(cfg_.world_data_dir);
    std::string abs_shared_lib = abspath(cfg_.shared_lib_dir);
    std::string abs_cache_root = abspath(cfg_.cache_root);

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
    matter_engine3::retopo_blacklist::init(abs_cache_root);
#endif

    // SP-2/SP-3/SP-7 wiring. HostBaker receives the absolute cache root; it passes
    // it through BakeOptions.parts_dir so bake_source writes artifacts to absolute
    // paths (Task 3 Phase B: no chdir required).
    script_host::ScriptHost host;
    host.set_shared_lib_root(abs_shared_lib);
    FileModuleResolver resolver(host, abs_schemas);

    // Phase 5 autoremesher (Task 15): record each part's retopo settings by
    // hash so flatten_one() below can thread them into flatten_part().
    // Decorator over HostBaker: PartGraph::install calls resolve_hash() for
    // EVERY node in the reachable graph (memoized by module+params), which
    // gives us the source + resolved hash for every part regardless of cache
    // state — cold OR warm. Recording on resolve_hash (not bake) means a
    // repeat run with fully cached parts still populates the map, so
    // flatten_one always sees the correct retopo settings. Schemas without
    // `static retopo` land on the default (enabled=false), so the map entry
    // is byte-identical to "not opted in".
    std::unordered_map<uint64_t, part_asset::RetopoSettings> retopo_by_hash;
    struct RecordingBaker : public Baker {
        HostBaker inner;
        script_host::ScriptHost& host;
        std::unordered_map<uint64_t, part_asset::RetopoSettings>* map;
        RecordingBaker(script_host::ScriptHost& h, std::string parts_dir,
                       std::unordered_map<uint64_t, part_asset::RetopoSettings>* m)
            : inner(h, std::move(parts_dir)), host(h), map(m) {}
        uint64_t resolve_hash(const std::string& source, const Params& params,
                              const std::vector<uint64_t>& child_hashes) override {
            uint64_t h = inner.resolve_hash(source, params, child_hashes);
            if (map && h != 0 && !map->count(h)) {
                // Fail-closed: eval_retopo_settings returns defaults on any error.
                // Guard the double-map lookup so re-visits of the same (source, params)
                // don't re-parse (eval_retopo_settings spins up a QuickJS context).
                (*map)[h] = host.eval_retopo_settings(source);
            }
            return h;
        }
        bool cached(uint64_t resolved_hash) override { return inner.cached(resolved_hash); }
        bool bake(const std::string& source, const Params& params,
                  const std::vector<uint64_t>& child_hashes,
                  const std::vector<std::string>& child_modules,
                  const std::vector<std::string>& child_params,
                  uint64_t resolved_hash) override {
            return inner.bake(source, params, child_hashes, child_modules,
                              child_params, resolved_hash);
        }
        bool bake_lod_variants(const std::string& source, const Params& params,
                               const std::vector<uint64_t>& child_hashes,
                               uint64_t resolved_hash) override {
            return inner.bake_lod_variants(source, params, child_hashes, resolved_hash);
        }
    };
    RecordingBaker baker(host, abs_cache_root, &retopo_by_hash);
    PartGraph graph(resolver, baker);

    std::vector<ChildRequest> roots;
    std::vector<bool> expand_flags;
    std::vector<bool> tileset_flags;
    bool manifest_ok = PartGraph::read_manifest(abs_world_data, cfg_.world_name,
                                                roots, err, &expand_flags, &tileset_flags);
    if (!manifest_ok) {
        return false;
    }

    // Tileset roots are installed by run_tileset_phase (it calls install() itself
    // on the tileset script's `static requires` children). Split them out here so
    // PartGraph::install() only sees the non-tileset roots.
    std::vector<ChildRequest> roots_for_install;
    std::vector<size_t> install_to_orig;
    std::vector<size_t> tileset_indices;
    for (size_t i = 0; i < roots.size(); ++i) {
        if (tileset_flags[i]) tileset_indices.push_back(i);
        else { roots_for_install.push_back(roots[i]); install_to_orig.push_back(i); }
    }

    InstallResult ir = graph.install(roots_for_install);
    if (!ir.ok) {
        err = ir.error;
        return false;
    }
    baked_count_ = (int)ir.baked.size();
    hit_count_   = ir.hits;
    baked_hashes_.insert(ir.baked.begin(), ir.baked.end());

    // Build hash -> module name map for use in fetch_parts()'s on_part callback.
    module_by_hash_.clear();
    for (size_t j = 0; j < ir.root_hashes.size(); ++j)
        if (ir.root_hashes[j] != 0)
            module_by_hash_[ir.root_hashes[j]] = roots_for_install[j].module;

    // Map each root module to the child-FOLDED resolved hash the graph baked it under.
    // install() returns root_hashes parallel to `roots_for_install`; using them (instead
    // of an unfolded resolve_hash recompute) keeps manifest instances pointing at the .part
    // that actually exists on disk — critical once a root has children (e.g. Tree->Leaf).
    if (ir.root_hashes.size() != roots_for_install.size()) {
        err = "install did not return a hash for every root";
        return false;
    }
    // Place each manifest root at the origin; `expand`-flagged roots are instead
    // replaced by their baked child-instance table (one world instance per child).
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

    // Bake-time flattening of every placed root: merge its whole child subtree
    // into per-cluster meshes with per-cluster LOD ladders (<hash>.flat.part),
    // so the viewer renders flat cluster instances per root instead of
    // re-expanding hundreds of child instances each frame.
    // Content-addressed AND version-sniffed: regenerate when the flat artifact
    // is missing OR from an older bake version (peek_format_version != kFormatVersionFlat).
    auto flatten_one = [&](uint64_t part_hash) -> bool {
        const std::string flat_abs_path =
            abs_cache_root + "/" + part_asset::cache_path_flat(part_hash);
        if (part_asset::peek_format_version(flat_abs_path) == part_asset::kFormatVersionFlat)
            return true;
        // Phase 5 (Task 15): thread the per-schema retopo settings recorded
        // during bake into the flatten call. Absent-entry lookup returns the
        // default (enabled=false), so parts with no `static retopo` in their
        // schema behave exactly as before — no retopo, no .retopo.part sidecar.
        part_flatten::FlattenTargets targets;
        auto rit = retopo_by_hash.find(part_hash);
        if (rit != retopo_by_hash.end()) targets.retopo = rit->second;
        part_flatten::FlattenResult fr =
            part_flatten::flatten_part(abs_cache_root, part_hash, targets);
        if (fr.ok) {
            printf("LocalProvider: flattened %016llx (%zu clusters, %zu levels, %zu -> %zu tris, %zu instance_refs%s)\n",
                   (unsigned long long)part_hash, fr.clusters, fr.levels,
                   fr.full_tris, fr.coarsest_tris, fr.instance_refs,
                   targets.retopo.enabled ? ", retopo=on" : "");
            return true;
        } else {
            // Non-fatal: the viewer falls back to compositional rendering.
            printf("LocalProvider: flatten failed for %016llx: %s\n",
                   (unsigned long long)part_hash, fr.error.c_str());
            return false;
        }
    };
    auto flatten_placed = [&]() {
        std::set<uint64_t> done;
        for (const auto& e : out.instances) {
            if (!done.insert(e.part_hash).second) continue;
            flatten_one(e.part_hash);
        }
    };

    // Bake-hardening #2: expand FlatInstanceRefs recorded in each placed
    // root's .flat.part into world manifest entries. A pure-scatter root
    // (StressForest) that busts the flatten budget lands on the BOUNDARY
    // path — its .flat.part contains N refs, one per placement, which the
    // GpuCuller consumes exactly like any other resolved instance. Runs
    // AFTER flatten_placed so every referenced part is guaranteed to have
    // been baked and (recursively) flattened. Iterates to a fixed point so
    // nested boundaries are expanded too.
    auto append_instance_refs = [&]() {
        // Snapshot the set of already-emitted part hashes so we don't
        // double-expand a part that appears both as a manifest root and as
        // an instance ref of another root.
        std::set<uint64_t> visited;
        // Deferred expansion: read every unique part's flat.part refs and
        // append world entries; repeat until no new part hashes appear.
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
                // Make sure this part's flat.part exists (some added-mid-loop
                // parts may not have been flattened yet). Non-fatal on failure.
                flatten_one(ph);
                const std::string flat_abs_path =
                    abs_cache_root + "/" + part_asset::cache_path_flat(ph);
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
            if (out.instances.size() == before) break;   // fixed point
        }
    };

    // Generic placement: every non-tileset manifest root is placed at the origin, except
    // roots flagged `expand`, whose baked child-instance table is promoted to
    // individual world instances (per-child LOD, culling, and instanced
    // batching downstream). Tileset roots are handled separately below.
    for (size_t i = 0; i < roots.size(); ++i) {
        if (tileset_flags[i]) {
            // Handled below via run_tileset_phase; not placed as a world instance.
            continue;
        }
        // Map back to the install index for this original root.
        size_t k = 0; bool found = false;
        for (size_t j = 0; j < install_to_orig.size(); ++j)
            if (install_to_orig[j] == i) { k = j; found = true; break; }
        if (!found) continue;  // (unreachable — every non-tileset root was installed)
        if (expand_flags[i]) {
            if (!append_expanded_children(abs_cache_root, ir.root_hashes[k],
                                          next_id, out.instances, err))
                return false;
        } else {
            place(ir.root_hashes[k], 0.0f, 0.0f, 0.0f, roots[i].module);
        }
    }
    flatten_placed();
    append_instance_refs();

    // ---- Tileset roots: GPU bake + .gtex load into a viewer slot -----------
    // run_tileset_phase reads world.manifest and .js from abs_world_data.
    // The gtex is written to abs_world_data/<root_module>.gtex (the GPU overload
    // constructs the path as world_data_dir + "/" + root_module + ".gtex").
    //
    // run_tileset_phase passes abs_cache_root to HostBaker; HostBaker::bake now
    // propagates it via BakeOptions.parts_dir so bake_source writes absolute paths
    // (Task 3 Phase B: no chdir needed here).
    // (baked_tileset_count_ already reset at connect() entry.)

    // Guard: fail-closed BEFORE any GL/disk work if the manifest declares more
    // tileset roots than we have sampler-array slots. Checking inside the loop
    // left 0..N-1 slots loaded with stale atlases on error.
    if ((int)tileset_indices.size() > viewer::tileset_provider::max_slots()) {
        err = "LocalProvider: manifest declares " +
              std::to_string(tileset_indices.size()) +
              " tileset roots but only " +
              std::to_string(viewer::tileset_provider::max_slots()) +
              " slots are available";
        return false;
    }

    for (size_t ti : tileset_indices) {
        const std::string root_module = roots[ti].module;
        tileset::SettledTorus settled;
        tileset::TilesetPhaseOpts opts;
        opts.force_rebake = false;   // let content_hash decide
        // Dev hook: MATTER_TILESET_DUMP_PNG=1 dumps loose <root>-albedo.png /
        // -normal.png / -orm.png / -height.png next to the .gtex so an operator
        // can eyeball the baked atlas without launching the viewer.
        opts.dump_png     = std::getenv("MATTER_TILESET_DUMP_PNG") != nullptr;
        std::string te;
        if (!tileset::run_tileset_phase(abs_world_data, cfg_.world_name, root_module,
                                        abs_cache_root, settled, opts, te,
                                        abs_shared_lib)) {
            err = "LocalProvider: run_tileset_phase(" + root_module + "): " + te +
                  " (if a GL error: set GALLIUM_DRIVER=d3d12 on WSLg)";
            return false;
        }
        // The GPU overload writes: world_data_dir + "/" + root_module + ".gtex"
        const std::string gtex_path = abs_world_data + "/" + root_module + ".gtex";
        std::string le;
        if (!viewer::tileset_provider::load_slot(baked_tileset_count_, gtex_path, le)) {
            err = "LocalProvider: tileset_provider::load_slot(" + gtex_path + "): " + le;
            return false;
        }
        // Bind material DIRT (16) to this slot by default — the world script may
        // override later. Non-DIRT materials keep groundTilesetSlot = -1.
        MaterialRegistrySetGroundTilesetSlot(16, baked_tileset_count_);
        ++baked_tileset_count_;
        printf("LocalProvider: tileset '%s' -> slot %d (%s)\n",
               root_module.c_str(), baked_tileset_count_ - 1, gtex_path.c_str());
    }

    // --- Parse world lights ---
    {
        const std::string manifest_path = abs_world_data + "/" + cfg_.world_name + "/world.manifest";
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
        const std::string cache_subdir = abs_cache_root + "/cache";
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
            if (!tracer.build(abs_cache_root, trace_instances, tracer_err)) {
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
    for (size_t i = 0; i < want.size(); ++i) {
        uint64_t h = want[i];
        if (!store.get_or_load(h)) { err = "load failed for part " + std::to_string(h); return false; }
        if (cfg_.on_part) {
            auto it = module_by_hash_.find(h);
            const char* mod = (it != module_by_hash_.end() && !it->second.empty())
                              ? it->second.c_str() : nullptr;
            cfg_.on_part(mod, (int)(i + 1), (int)want.size());
        }
    }
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

} // namespace viewer
