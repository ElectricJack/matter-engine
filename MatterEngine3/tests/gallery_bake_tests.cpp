// Headless smoke test for the primitive test world (examples/primitive_demo).
//
// The gallery schemas exercise the whole DSL surface (voxel + mesh primitives,
// postfix CSG, extrude, placeChild, lookAt, tint). This test proves the world
// BAKES end-to-end without a GUI: install the manifest, then load every baked
// .part back and assert it produced geometry. A verb that errored mid-build
// (e.g. extrude with a bad profile, or a primitive that fell through to a
// set_error stub) would surface here as a failed install or an empty BLAS.
//
// GL-free: raylib is linked only for the Tri<->mesh bridge, like example_world.
// Bakes into a fresh /tmp sandbox so the repo cache stays clean.

#include "part_graph.h"        // -DMATTER_HAVE_SCRIPT_HOST pulls in script_host.h
#include "part_asset_v2.h"     // cache_path_resolved, load_v2, ChildInstance
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "script/world_definition_loader.h"
#include "provider/local_provider.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

using namespace part_graph;

#include "check.h"

static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

// Count the full-resolution triangles a baked artifact loaded back with.
static size_t load_tri_count(uint64_t h, size_t& child_count) {
    std::string path = part_asset::cache_path_resolved(h);
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods_in;
    if (!part_asset::load_v2(path, h, blas, tlas, children, lods_in)) return SIZE_MAX;
    child_count = children.size();
    size_t n = 0;
    for (const auto& e : blas.get_entries()) n += e->triangles.size();
    return n;
}

int main() {
    const std::string project    = abspath("../examples/primitive_demo");
    const std::string objects    = abspath("../examples/primitive_demo/objects");
    const std::string shared_lib  = abspath("../shared-lib");

    const std::string sandbox = "/tmp/me3_gallery_bake";
    system(("rm -rf " + sandbox).c_str());
    system(("mkdir -p " + sandbox + "/parts").c_str());
    if (chdir(sandbox.c_str()) != 0) { printf("FAIL: chdir sandbox\n"); return 1; }

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib);
    FileModuleResolver resolver(host, objects);
    HostBaker baker(host, ".");
    PartGraph graph(resolver, baker);

    matter::WorldLoadDesc load_desc;
    load_desc.world_path = project + "/worlds/Primitives.js";
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
    CHECK(roots.size() == 1 && roots[0].module == "Gallery",
          "manifest should list exactly the Gallery root");

    InstallResult ir = graph.install(roots);
    if (!ir.ok) { printf("FAIL: install: %s\n", ir.error.c_str()); return 1; }
    // Gallery + 3 sub-galleries = 4 distinct artifacts on a clean cache.
    printf("[install] baked %zu artifact(s), %d hit(s)\n", ir.baked.size(), ir.hits);
    CHECK(ir.baked.size() == 4, "clean install should bake Gallery + 3 sub-galleries");
    CHECK(ir.root_hashes.size() == 1, "one root hash for the Gallery root");

    // Every baked artifact must round-trip with non-empty geometry.
    for (uint64_t h : ir.baked) {
        size_t kids = 0;
        size_t tris = load_tri_count(h, kids);
        CHECK(tris != SIZE_MAX, "load_v2 should succeed for every baked part");
        CHECK(tris > 0, "every gallery artifact should bake non-empty geometry");
        printf("[load] %016llx tris=%zu children=%zu\n",
               (unsigned long long)h, tris == SIZE_MAX ? 0 : tris, kids);
    }

    // The Gallery root must point at its three placed sub-galleries.
    {
        size_t kids = 0;
        size_t tris = load_tri_count(ir.root_hashes[0], kids);
        CHECK(tris != SIZE_MAX && tris > 0, "Gallery root has its own lookAt/tint mesh");
        CHECK(kids == 3, "Gallery should place 3 children (VoxelPrims/MeshPrims/Extrusions)");
    }

    // Incremental cache: a second install re-bakes nothing.
    InstallResult ir2 = graph.install(roots);
    CHECK(ir2.ok && ir2.baked.empty(), "second install should be a pure cache hit");

    if (g_failures == 0) printf("ALL PASS\n");
    else                 printf("%d CHECK(s) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
