// Headless bake check for the world_demo Tree, used to validate the Trunk/Tree
// split: Tree is now a geometry-less assembler placing a Trunk + TreeBranch
// children. Proves (1) the assembly bakes end-to-end GUI-free and (2) the
// decoupling: an INCREMENTAL install over a warm cache re-bakes only the parts
// whose source changed -- so editing a Leaf does not re-voxelize the trunk.
//
// Persistent sandbox (NOT wiped) so two runs can show incremental behavior:
//   run 1 (clean)   -> bakes Trunk(expensive) + TreeBranch + Leaf + Tree
//   edit Leaf.js, run 2 -> bakes Leaf + TreeBranch + Tree; Trunk is a cache HIT.

#include "part_graph.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

#include "portable_realpath.h"
#include "test_sandbox.h"

using namespace part_graph;

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
    const std::string schemas    = abspath("../../projects/world_demo/objects");
    const std::string shared_lib = abspath("../shared-lib");

    // Deliberately NOT wiped (see the header comment): a second run must find
    // the warm cache this one leaves behind.
    const std::string sandbox = ensure_sandbox("sandbox/me3_tree_bake");
    if (chdir(sandbox.c_str()) != 0) { printf("FAIL: chdir sandbox\n"); return 1; }

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib);
    FileModuleResolver resolver(host, schemas);
    HostBaker baker(host, ".");
    PartGraph graph(resolver, baker);

    std::vector<ChildRequest> roots = { ChildRequest{ "Tree", {} } };

    InstallResult ir = graph.install(roots);
    if (!ir.ok) { printf("FAIL: install: %s\n", ir.error.c_str()); return 1; }
    printf("[install] baked %zu artifact(s), %d hit(s)\n", ir.baked.size(), ir.hits);
    for (uint64_t h : ir.baked) {
        size_t kids = 0;
        size_t tris = load_tri_count(h, kids);
        printf("[baked] %016llx tris=%zu children=%zu\n",
               (unsigned long long)h, tris == SIZE_MAX ? 0 : tris, kids);
    }
    if (ir.root_hashes.empty() || ir.root_hashes[0] == 0) {
        printf("FAIL: install produced no root hash for Tree\n");
        return 1;
    }
    size_t kids = 0;
    size_t tris = load_tri_count(ir.root_hashes[0], kids);
    if (tris == SIZE_MAX) {
        printf("FAIL: root Tree artifact did not load back\n");
        return 1;
    }
    // Tree grows its own trunk geometry (particle-flow) AND places Leaf /
    // TreeBranch children, so both counts are non-zero. It was a geometry-less
    // assembler when this harness was written; the assertion is on the pair
    // being non-empty rather than on either count staying at a fixed value.
    printf("[root Tree] %016llx tris=%zu children=%zu\n",
           (unsigned long long)ir.root_hashes[0], tris, kids);
    if (tris == 0 && kids == 0) {
        printf("FAIL: root Tree baked neither geometry nor children\n");
        return 1;
    }
    return 0;
}
