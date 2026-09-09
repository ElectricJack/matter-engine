// End-to-end MatterEngine3 example world over the committed Demo scene.
//
// Drives the WHOLE pipeline on committed assets under
// ../../projects/world_demo (objects + scenes/Demo/Demo.js) and the shared script
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
// whose world_flatten child rows scatter every root the manifest names.
//
// Headless and GL-free for the host/CPU steps (raylib is linked only for the
// Tri<->mesh bridge). Bakes into a fresh scratch sandbox (see test_sandbox.h)
// so the repo cache stays clean.

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
#include <string>
#include <vector>
#include <map>
#include <limits.h>
#include <unistd.h>

#include "portable_realpath.h"
#include "test_sandbox.h"

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

// Row-major translate matrix into a part_asset/world_flatten transform[16].
static void set_translate(float m[16], float x, float y, float z) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[3] = x; m[7] = y; m[11] = z;
}

int main() {
    // --- Resolve committed asset locations (run from MatterEngine3/tests). ---
    const std::string project    = abspath("../../projects/world_demo");
    const std::string objects    = abspath("../../projects/world_demo/objects");
    const std::string shared_lib = abspath("../shared-lib");
    printf("objects:    %s\n", objects.c_str());
    printf("shared-lib: %s\n", shared_lib.c_str());

    // --- Fresh sandbox; bake writes the RELATIVE "parts/<hash>.part", so chdir. ---
    const std::string sandbox = make_sandbox("sandbox/me3_example_world");
    if (chdir(sandbox.c_str()) != 0) { printf("FAIL: chdir sandbox\n"); return 1; }

    // --- SP-2/SP-3/SP-7 wiring. set_shared_lib_root enables `import` resolution. ---
    // Object lookup is a SEARCH PATH, not a single directory: a scene's own
    // objects/ shadows the shared tier (Demo's roots live in
    // scenes/Demo/objects/). The hash pass below walks the same list.
    const std::vector<std::string> object_dirs{project + "/scenes/Demo/objects", objects};
    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib);
    FileModuleResolver resolver(host, object_dirs);
    HostBaker baker(host, ".");            // parts_dir_ is PARENT of parts/ (== cwd)
    PartGraph graph(resolver, baker);

    // --- SP-3: load world definition into root parts, then install (bake) them. ---
    matter::WorldLoadDesc load_desc;
    load_desc.world_path = project + "/scenes/Demo/Demo.js";
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

    // --- Resolve each module's content hash. The INSTALL is the authority:
    // root_hashes is child-folded, so an assembler root (TreeGallery declares
    // `static requires`) does not hash to what its own source alone hashes to.
    // Re-deriving it here with resolve_hash(source, "{}") only ever matched
    // leaf modules and missed the cache for every root with children. ---
    std::map<std::string, uint64_t> hash_of;
    for (size_t i = 0; i < roots.size(); ++i) {
        const uint64_t h = (i < ir.root_hashes.size()) ? ir.root_hashes[i] : 0;
        if (h == 0) {
            printf("FAIL: install produced no resolved hash for %s\n", roots[i].module.c_str());
            return 1;
        }
        hash_of[roots[i].module] = h;
        printf("[hash] %-14s -> %016llx\n", roots[i].module.c_str(), (unsigned long long)h);
    }

    // --- SP-1 + SP-4: load each baked part, derive LOD levels + a bound radius. ---
    lod_select::PartLodTable lod_table;
    size_t loaded_content = 0;
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

        // Demo's roots are either assemblers (TreeGallery places 8 Trees, so
        // its own triangle list is empty and the content is in the child rows)
        // or pure particle emitters (ChimneySmoke/WaterfallMist carry neither
        // triangles nor children by design), so this is counted across the
        // world instead of asserted per root -- but a run where NOTHING loads
        // back with geometry or children is a bake that produced nothing.
        loaded_content += tris.size() + children.size();

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
            // A geometry-less root (ChimneySmoke and WaterfallMist are particle
            // assemblers) bakes rungs with no BLAS entry at all, so the index
            // list can legitimately be empty — indexing it blindly aborts.
            size_t n = 0;
            if (!L.blas_indices.empty() &&
                L.blas_indices[0] < lod_blas.get_entries().size())
                n = lod_blas.get_entries()[L.blas_indices[0]]->triangles.size();
            printf(" [thr=%.4f tris=%zu]", L.screen_size_threshold, n);
        }
        printf("\n");
        lod_table[h] = lod_select::PartLod{ radius, thresholds };
    }

    if (loaded_content == 0) {
        printf("FAIL: every root loaded back empty (no triangles, no children)\n");
        return 1;
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

    const int kTileGrid = 3;        // 3x3 grid footprint
    const float kTile   = 8.0f;     // one tile spans 8 world units
    const float kSpan   = kTileGrid * kTile;

    // Scatter every root the manifest actually named. The Demo world's root
    // list is authored data and HAS changed (it used to be Terrain/Tree/Grass;
    // it is TreeGallery/ChimneySmoke/WaterfallMist today), so hardcoding module
    // names here silently placed hash 0 for each one and left the scatter
    // testing nothing.
    Rng64 rng(0xC0FFEEu);
    const int kPerRoot = 32;
    for (const auto& kv : hash_of) {
        for (int i = 0; i < kTileGrid; ++i)
            for (int j = 0; j < kTileGrid; ++j)
                place(kv.second, i * kTile, 0.0f, j * kTile);
        for (int n = 0; n < kPerRoot; ++n)
            place(kv.second, rng.range(0, kSpan), 1.0f, rng.range(0, kSpan));
    }
    if (hash_of.empty()) { printf("FAIL: manifest resolved no roots\n"); return 1; }

    world_flatten::FlattenLimits lim;
    std::vector<world_flatten::FlatInstance> flat;
    std::string ferr;
    if (!world_flatten::flatten(wg, kWorldRoot, lim, flat, ferr)) {
        printf("FAIL: flatten: %s\n", ferr.c_str());
        return 1;
    }
    printf("\n[world] flattened %zu instances (%zu roots x (%d grid + %d scattered))\n",
           flat.size(), hash_of.size(), kTileGrid * kTileGrid, kPerRoot);

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
