#include "local_provider.h"

#include "part_graph.h"        // -DMATTER_HAVE_SCRIPT_HOST pulls in script_host.h
#include "part_asset_v2.h"     // cache_path_resolved

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
    bool manifest_ok = PartGraph::read_manifest(abs_world_data, cfg_.world_name, roots, err);
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
    std::map<std::string, uint64_t> hash_of;
    for (size_t i = 0; i < roots.size(); ++i)
        hash_of[roots[i].module] = ir.root_hashes[i];

    // Scatter the example world (identical layout to example_world.cpp).
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

    const int kTileGrid = 3; const float kTile = 8.0f;
    const float kSpan = kTileGrid * kTile;
    for (int i = 0; i < kTileGrid; ++i)
        for (int j = 0; j < kTileGrid; ++j)
            place(hash_of["Terrain"], i * kTile, 0.0f, j * kTile);

    Rng64 rng(0xC0FFEEu);
    const int kTrees = 24, kGrass = 120;
    for (int n = 0; n < kTrees; ++n)
        place(hash_of["Tree"],  rng.range(0, kSpan), 1.0f, rng.range(0, kSpan));
    for (int n = 0; n < kGrass; ++n)
        place(hash_of["Grass"], rng.range(0, kSpan), 0.6f, rng.range(0, kSpan));

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

} // namespace viewer
