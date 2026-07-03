// Headless full-world bake check for the Meadow assembly part. PERSISTENT
// sandbox (like tree_bake_check): the first run bakes 276 variants (minutes);
// later runs are warm and validate cache-hit determinism cheaply.
#include "part_graph.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

using namespace part_graph;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++failures; } \
    else         { printf("ok:   %s\n", (msg)); } } while (0)

static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

int main() {
    const std::string schemas    = abspath("../examples/world_demo/schemas");
    const std::string shared_lib = abspath("../shared-lib");

    const std::string sandbox = "/tmp/me3_meadow_bake";
    system(("mkdir -p " + sandbox + "/parts").c_str());
    if (chdir(sandbox.c_str()) != 0) { printf("FAIL: chdir sandbox\n"); return 1; }

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib);
    FileModuleResolver resolver(host, schemas);
    HostBaker baker(host, ".");
    PartGraph graph(resolver, baker);

    std::time_t t0 = std::time(nullptr);
    InstallResult ir = graph.install({ ChildRequest{ "Meadow", {} } });
    printf("[install] %lds, baked %zu artifact(s), %d hit(s)\n",
           (long)(std::time(nullptr) - t0), ir.baked.size(), ir.hits);
    CHECK(ir.ok, "meadow install ok");
    if (!ir.ok) { printf("  error: %s\n", ir.error.c_str()); return 1; }

    // Read back the root artifact: geometry-less assembly + full child table.
    uint64_t root = ir.root_hashes[0];
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    CHECK(part_asset::load_v2(part_asset::cache_path_resolved(root), root,
                              blas, tlas, children, lods),
          "meadow root artifact loads");

    size_t tris = 0;
    for (const auto& e : blas.get_entries()) tris += e->triangles.size();
    CHECK(tris == 0, "meadow root has zero own geometry (pure assembly)");

    printf("[root] children=%zu\n", children.size());
    CHECK(children.size() == 44896,
          "child table = 256 tiles + 600 rocks + 4000 pebbles + 40000 grass + 40 trees");

    std::set<uint64_t> uniq;
    for (const auto& c : children) uniq.insert(c.child_resolved_hash);
    printf("[root] unique variants=%zu\n", uniq.size());
    CHECK(uniq.size() == 276, "276 unique variants (256+8+6+5+1)");

    // Determinism: a fresh graph over the warm cache = same hash, zero bakes.
    PartGraph graph2(resolver, baker);
    InstallResult ir2 = graph2.install({ ChildRequest{ "Meadow", {} } });
    CHECK(ir2.ok && ir2.baked.empty(), "warm re-install bakes nothing");
    CHECK(!ir2.root_hashes.empty() && ir2.root_hashes[0] == root,
          "warm re-install resolves the identical root hash");

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
