// End-to-end MatterEngine3 example world: terrain, trees, and grass.
//
// Drives the WHOLE pipeline on committed assets under
// ../examples/world_demo (objects + worlds/Demo.js) and the shared script
// library under ../shared-lib:
//
//   SP-3  load_world_definition -> PartGraph::install (walk + dedup + cache)
//   SP-2  ScriptHost bakes each part (voxel-CSG build) via HostBaker
//   SP-7  Tree.js `import {rng} from 'shared-lib/rng'` (module resolution + fold)
//   SP-1  load_v2 reads each baked .part back (geometry + LOD round-trip shape)
//   SP-4  lod_bake (decimate to LOD levels) -> world_flatten (compose a world by
//         scattering instances across a terrain grid) -> sector_grid (bin) ->
//         lod_select (choose per-sector LOD for a near and a far camera)
//
// The DSL cannot place children at transforms (only `static requires` declares
// child *kinds*), so world layout is built here in C++: a synthetic root part
// whose world_flatten child rows scatter terrain tiles, trees, and grass.
//
// Headless and GL-free for the host/CPU steps (raylib is linked only for the
// Tri<->mesh bridge). Bakes into a fresh /tmp sandbox so the repo stays clean.

#include "part_graph.h"        // -DMATTER_HAVE_SCRIPT_HOST pulls in script_host.h
#include "part_asset_v2.h"     // cache_path_resolved, load_v2, ChildInstance
#include "lod_bake.h"
#include "world_flatten.h"
#include "sector_grid.h"
#include "lod_select.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "script/world_definition_loader.h"
#include "provider/local_provider.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <limits.h>
#include <unistd.h>

using namespace part_graph;

// Deterministic splitmix64 so the scatter is reproducible across runs/platforms.
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

static std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

// Row-major translate matrix into a part_asset/world_flatten transform[16].
static void set_translate(float m[16], float x, float y, float z) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[3] = x; m[7] = y; m[11] = z;
}

// Absolute path of `rel` resolved against the current working directory.
static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;   // best-effort; caller will fail loudly if it doesn't exist
}

int main() {
    // --- Resolve committed asset locations (run from MatterEngine3/tests). ---
    const std::string project    = abspath("../examples/world_demo");
    const std::string objects    = abspath("../examples/world_demo/objects");
    const std::string shared_lib = abspath("../shared-lib");
    printf("objects:    %s\n", objects.c_str());
    printf("shared-lib: %s\n", shared_lib.c_str());

    // --- Fresh sandbox; bake writes the RELATIVE "parts/<hash>.part", so chdir. ---
    const std::string sandbox = "/tmp/me3_example_world";
    system(("rm -rf " + sandbox).c_str());
    system(("mkdir -p " + sandbox + "/parts").c_str());
    if (chdir(sandbox.c_str()) != 0) { printf("FAIL: chdir sandbox\n"); return 1; }

    // --- SP-2/SP-3/SP-7 wiring. set_shared_lib_root enables `import` resolution. ---
    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib);
    FileModuleResolver resolver(host, objects);
    HostBaker baker(host, ".");            // parts_dir_ is PARENT of parts/ (== cwd)
    PartGraph graph(resolver, baker);

    // --- SP-3: load world definition into root parts, then install (bake) them. ---
    matter::WorldLoadDesc load_desc;
    load_desc.world_path = project + "/worlds/Demo.js";
    load_desc.objects_dir = objects;
    load_desc.engine_shared_lib_dir = shared_lib;
    matter::WorldDefinition definition;
    matter::WorldLoadError load_error;
    if (!matter::load_world_definition(load_desc, definition, load_error)) {
        printf("FAIL: load_world_definition: %s\n", load_error.message.c_str());
        return 1;
    }
    viewer::ProviderWorldDefinition adapted =
        viewer::adapt_world_definition(definition);
    std::vector<ChildRequest> roots = std::move(adapted.roots);
    std::string err;
    printf("\n[install] manifest roots: ");
    for (auto& r : roots) printf("%s ", r.module.c_str());
    printf("\n");

    InstallResult ir = graph.install(roots);
    if (!ir.ok) { printf("FAIL: install: %s\n", ir.error.c_str()); return 1; }
    printf("[install] baked %zu artifact(s), %d cache hit(s)\n", ir.baked.size(), ir.hits);

    // --- Resolve each module's content hash (the host is the hash authority). ---
    std::map<std::string, uint64_t> hash_of;
    for (auto& r : roots) {
        std::string src = read_file(objects + "/" + r.module + ".js");
        if (src.empty()) { printf("FAIL: missing schema %s\n", r.module.c_str()); return 1; }
        uint64_t h = host.resolve_hash(src, "{}");
        if (h == 0) { printf("FAIL: resolve_hash %s\n", r.module.c_str()); return 1; }
        hash_of[r.module] = h;
        printf("[hash] %-8s -> %016llx\n", r.module.c_str(), (unsigned long long)h);
    }

    // --- SP-1 + SP-4: load each baked part, derive LOD levels + a bound radius. ---
    lod_select::PartLodTable lod_table;
    for (auto& kv : hash_of) {
        const std::string& mod = kv.first;
        uint64_t h = kv.second;
        std::string path = part_asset::cache_path_resolved(h);   // "parts/<hash>.part"

        BLASManager blas; TLASManager tlas(256);
        std::vector<part_asset::ChildInstance> children;
        part_asset::LodLevels lods_in;
        if (!part_asset::load_v2(path, h, blas, tlas, children, lods_in)) {
            printf("FAIL: load_v2 %s (%s)\n", mod.c_str(), path.c_str());
            return 1;
        }

        // Gather the full-resolution geometry the bake produced.
        std::vector<Tri> tris;
        for (const auto& e : blas.get_entries())
            tris.insert(tris.end(), e->triangles.begin(), e->triangles.end());

        // Bound radius = half the AABB diagonal (drives projected-size LOD math).
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        auto acc = [&](const float3& v) {
            mn[0] = std::fmin(mn[0], v.x); mx[0] = std::fmax(mx[0], v.x);
            mn[1] = std::fmin(mn[1], v.y); mx[1] = std::fmax(mx[1], v.y);
            mn[2] = std::fmin(mn[2], v.z); mx[2] = std::fmax(mx[2], v.z);
        };
        for (const auto& t : tris) { acc(t.vertex0); acc(t.vertex1); acc(t.vertex2); }
        float radius = 0.0f;
        if (!tris.empty()) {
            float dx = mx[0]-mn[0], dy = mx[1]-mn[1], dz = mx[2]-mn[2];
            radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
        }

        // SP-4 lod_bake: decimate the geometry into 3 selectable LOD levels.
        BLASManager lod_blas;
        lod_bake::LodLevels lods = lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, lod_blas);
        std::vector<float> thresholds;
        printf("[lod]  %-8s radius=%.3f tris=%zu  levels:", mod.c_str(), radius, tris.size());
        for (const auto& L : lods) {
            thresholds.push_back(L.screen_size_threshold);
            size_t n = lod_blas.get_entries()[L.blas_indices[0]]->triangles.size();
            printf(" [thr=%.4f tris=%zu]", L.screen_size_threshold, n);
        }
        printf("\n");
        lod_table[h] = lod_select::PartLod{ radius, thresholds };
    }

    // --- SP-4 world_flatten: build a world by scattering instances in C++. ---
    // Synthetic root part (hash 1) holds the placed child rows; the real parts
    // are leaves (no entry => emit a FlatInstance per placement).
    world_flatten::PartGraph wg;
    const uint64_t kWorldRoot = 1;
    auto place = [&](uint64_t h, float x, float y, float z) {
        world_flatten::ChildInstance c;
        c.child_resolved_hash = h;
        set_translate(c.transform, x, y, z);
        wg[kWorldRoot].push_back(c);
    };

    const int kTileGrid = 3;        // 3x3 terrain tiles
    const float kTile   = 8.0f;     // one tile spans 8 world units
    const float kSpan   = kTileGrid * kTile;

    // Terrain grid.
    for (int i = 0; i < kTileGrid; ++i)
        for (int j = 0; j < kTileGrid; ++j)
            place(hash_of["Terrain"], i * kTile, 0.0f, j * kTile);

    // Scatter trees and grass over the terrain footprint (deterministic seed).
    Rng64 rng(0xC0FFEEu);
    const int kTrees = 24, kGrass = 120;
    for (int n = 0; n < kTrees; ++n)
        place(hash_of["Tree"], rng.range(0, kSpan), 1.0f, rng.range(0, kSpan));
    for (int n = 0; n < kGrass; ++n)
        place(hash_of["Grass"], rng.range(0, kSpan), 0.6f, rng.range(0, kSpan));

    world_flatten::FlattenLimits lim;
    std::vector<world_flatten::FlatInstance> flat;
    std::string ferr;
    if (!world_flatten::flatten(wg, kWorldRoot, lim, flat, ferr)) {
        printf("FAIL: flatten: %s\n", ferr.c_str());
        return 1;
    }
    printf("\n[world] flattened %zu instances (%d terrain + %d trees + %d grass)\n",
           flat.size(), kTileGrid * kTileGrid, kTrees, kGrass);

    // --- SP-4 sector_grid: bin instances into a fixed-pitch grid. ---
    sector_grid::SectorGrid grid(16.0f);
    sector_grid::Sectors sectors = sector_grid::bin_instances(flat, grid);
    printf("[sectors] %zu occupied (pitch %.1f):", sectors.size(), grid.pitch());
    for (const auto& s : sectors)
        printf(" (%d,%d,%d):%zu", s.first.x, s.first.y, s.first.z, s.second.size());
    printf("\n");

    // --- SP-4 lod_select: per-sector LOD for a near and a far camera. ---
    auto report = [&](const char* label, float3 cam) {
        auto chosen = lod_select::select_sector_lods(sectors, lod_table, cam);
        printf("[lod-select] %s camera (%.0f,%.0f,%.0f):\n", label, cam.x, cam.y, cam.z);
        for (const auto& sk : chosen) {
            printf("    sector (%d,%d,%d):", sk.first.x, sk.first.y, sk.first.z);
            for (const auto& pl : sk.second)
                printf(" %016llx=L%d", (unsigned long long)pl.first, pl.second);
            printf("\n");
        }
    };
    printf("\n");
    report("near", make_float3(kSpan * 0.5f, 4.0f, -4.0f));
    report("far",  make_float3(kSpan * 0.5f, 80.0f, -200.0f));

    // --- Demonstrate incremental cache: a second install bakes nothing. ---
    InstallResult ir2 = graph.install(roots);
    printf("\n[install-2] baked %zu, hits %d (incremental cache hit)\n",
           ir2.baked.size(), ir2.hits);

    bool ok = ir2.ok && ir2.baked.empty() && !flat.empty() && !sectors.empty();
    printf("\n%s\n", ok ? "Example world OK" : "Example world FAILED");
    return ok ? 0 : 1;
}
