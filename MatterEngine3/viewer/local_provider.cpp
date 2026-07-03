#include "local_provider.h"

#include "part_graph.h"        // -DMATTER_HAVE_SCRIPT_HOST pulls in script_host.h
#include "part_asset_v2.h"     // cache_path_resolved, cache_path_flat
#include "part_flatten.h"      // bake-time subtree flattening
#include "world_lights.h"
#include "probe_volume.h"
#include "world_tracer.h"
#include "probe_bake.h"
#include "part_asset.h"   // fnv1a64

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
#ifdef _WIN32
#include <direct.h>      // _mkdir, _getcwd, _chdir
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
bool fs_getcwd(char* buf, size_t n)                { return _getcwd(buf, (int)n) != nullptr; }
int  fs_chdir(const char* p)                       { return _chdir(p); }
#else
int  fs_mkdir(const char* p)                       { return ::mkdir(p, 0755); }
bool fs_realpath(const char* in, char* out)        { return realpath(in, out) != nullptr; }
bool fs_getcwd(char* buf, size_t n)                { return getcwd(buf, n) != nullptr; }
int  fs_chdir(const char* p)                       { return chdir(p); }
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
} // namespace

LocalProvider::LocalProvider(LocalProviderConfig cfg) : cfg_(std::move(cfg)) {}

bool LocalProvider::connect(WorldManifest& out, std::string& err) {
    baked_count_ = 0;
    hit_count_   = 0;
    baked_hashes_.clear();

    // Ensure the persistent cache dir exists.
    // bake_source writes "parts/<hash>.part" relative to cwd, so we temporarily
    // chdir into cache_root (saving and restoring the caller's cwd). All other
    // paths are resolved to absolute BEFORE the chdir so they remain valid.
    fs_mkdir(cfg_.cache_root.c_str());
    std::string parts_subdir = cfg_.cache_root + "/parts";
    fs_mkdir(parts_subdir.c_str());

    // Resolve all relative paths to absolute now, while cwd is still the caller's.
    std::string abs_schemas    = abspath(cfg_.schemas_dir);
    std::string abs_world_data = abspath(cfg_.world_data_dir);
    std::string abs_shared_lib = abspath(cfg_.shared_lib_dir);
    std::string abs_cache_root = abspath(cfg_.cache_root);

    // Save caller's cwd, chdir to cache_root so bake_source writes parts/ there.
    char orig_cwd[PATH_MAX];
    if (!fs_getcwd(orig_cwd, sizeof(orig_cwd))) {
        err = "getcwd failed";
        return false;
    }
    if (fs_chdir(abs_cache_root.c_str()) != 0) {
        err = "cache_root does not exist or could not be created: " + cfg_.cache_root;
        return false;
    }

    // SP-2/SP-3/SP-7 wiring. HostBaker's second arg is the PARENT of parts/ (== ".").
    // bake_source writes "parts/<hash>.part" relative to cwd (== abs_cache_root).
    script_host::ScriptHost host;
    host.set_shared_lib_root(abs_shared_lib);
    FileModuleResolver resolver(host, abs_schemas);
    HostBaker baker(host, ".");
    PartGraph graph(resolver, baker);

    std::vector<ChildRequest> roots;
    std::vector<bool> expand_flags;
    bool manifest_ok = PartGraph::read_manifest(abs_world_data, cfg_.world_name,
                                                roots, err, &expand_flags);
    if (!manifest_ok) {
        fs_chdir(orig_cwd);
        return false;
    }

    InstallResult ir = graph.install(roots);
    if (!ir.ok) {
        err = ir.error;
        fs_chdir(orig_cwd);
        return false;
    }
    baked_count_ = (int)ir.baked.size();
    hit_count_   = ir.hits;
    baked_hashes_.insert(ir.baked.begin(), ir.baked.end());

    // Restore caller's cwd before doing anything further.
    fs_chdir(orig_cwd);

    // Map each root module to the child-FOLDED resolved hash the graph baked it under.
    // install() returns root_hashes parallel to `roots`; using them (instead of an
    // unfolded resolve_hash recompute) keeps manifest instances pointing at the .part
    // that actually exists on disk — critical once a root has children (e.g. Tree->Leaf).
    if (ir.root_hashes.size() != roots.size()) {
        err = "install did not return a hash for every root";
        return false;
    }
    // Place each manifest root at the origin; `expand`-flagged roots are instead
    // replaced by their baked child-instance table (one world instance per child).
    out.world_root_hash = 1;
    out.instances.clear();
    uint32_t next_id = 1;
    auto place = [&](uint64_t h, float x, float y, float z) {
        WorldManifestEntry e;
        e.instance_id = next_id++;
        e.part_hash   = h;
        set_translate(e.transform, x, y, z);
        out.instances.push_back(e);
    };

    // Bake-time flattening of every placed root: merge its whole child subtree
    // into per-cluster meshes with per-cluster LOD ladders (<hash>.flat.part),
    // so the viewer renders flat cluster instances per root instead of
    // re-expanding hundreds of child instances each frame.
    // Content-addressed AND version-sniffed: regenerate when the flat artifact
    // is missing OR is an old v2 file (peek_format_version != 3).
    auto flatten_placed = [&]() {
        std::set<uint64_t> done;
        for (const auto& e : out.instances) {
            if (!done.insert(e.part_hash).second) continue;
            const std::string flat_abs_path =
                abs_cache_root + "/" + part_asset::cache_path_flat(e.part_hash);
            if (part_asset::peek_format_version(flat_abs_path) == 3) continue;
            part_flatten::FlattenResult fr =
                part_flatten::flatten_part(abs_cache_root, e.part_hash);
            if (fr.ok) {
                printf("LocalProvider: flattened %016llx (%zu clusters, %zu levels, %zu -> %zu tris)\n",
                       (unsigned long long)e.part_hash, fr.clusters, fr.levels,
                       fr.full_tris, fr.coarsest_tris);
            } else {
                // Non-fatal: the viewer falls back to compositional rendering.
                printf("LocalProvider: flatten failed for %016llx: %s\n",
                       (unsigned long long)e.part_hash, fr.error.c_str());
            }
        }
    };

    // Generic placement: every manifest root is placed at the origin, except
    // roots flagged `expand`, whose baked child-instance table is promoted to
    // individual world instances (per-child LOD, culling, and instanced
    // batching downstream). No per-world special cases.
    for (size_t i = 0; i < roots.size(); ++i) {
        if (expand_flags[i]) {
            if (!append_expanded_children(abs_cache_root, ir.root_hashes[i],
                                          next_id, out.instances, err))
                return false;
        } else {
            place(ir.root_hashes[i], 0.0f, 0.0f, 0.0f);
        }
    }
    flatten_placed();

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
    for (uint64_t h : want) {
        if (!store.get_or_load(h)) { err = "load failed for part " + std::to_string(h); return false; }
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
