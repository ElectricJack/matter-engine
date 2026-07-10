// One-off profiling harness: bake individual Rock variants cold and report
// wall time + triangle counts per (seed, size). Fresh /tmp sandbox each run
// so every install is a cache-miss bake.
#include "part_graph.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

using namespace part_graph;

static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

static size_t load_tri_count(uint64_t h) {
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    if (!part_asset::load_v2(part_asset::cache_path_resolved(h), h, blas, tlas, children, lods))
        return SIZE_MAX;
    size_t n = 0;
    for (const auto& e : blas.get_entries()) n += e->triangles.size();
    return n;
}

int main(int argc, char** argv) {
    const std::string schemas    = abspath("../examples/world_demo/schemas");
    const std::string shared_lib = abspath("../shared-lib");

    const std::string sandbox = "/tmp/me3_rock_profile";
    system(("rm -rf " + sandbox).c_str());
    system(("mkdir -p " + sandbox + "/parts").c_str());
    if (chdir(sandbox.c_str()) != 0) { printf("FAIL: chdir sandbox\n"); return 1; }

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib);
    FileModuleResolver resolver(host, schemas);
    HostBaker baker(host, ".");

    struct Case { int seed; double size; };
    std::vector<Case> cases;
    if (argc > 1) {
        // args: seed size [seed size ...]
        for (int i = 1; i + 1 < argc; i += 2)
            cases.push_back({ atoi(argv[i]), atof(argv[i+1]) });
    } else {
        cases = { {0,1.0}, {2,1.0}, {2,0.12}, {2,0.35}, {2,2.5}, {2,6.0} };
    }

    for (const Case& c : cases) {
        Params p;
        p["seed"] = ParamValue::number(c.seed);
        if (c.size != 1.0) p["size"] = ParamValue::number(c.size);

        PartGraph graph(resolver, baker);
        auto t0 = std::chrono::steady_clock::now();
        InstallResult ir = graph.install({ ChildRequest{ "Rock", p } });
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
        if (!ir.ok) {
            printf("[rock seed=%d size=%.2f] FAILED: %s\n", c.seed, c.size, ir.error.c_str());
            continue;
        }
        size_t tris = load_tri_count(ir.root_hashes[0]);
        printf("[rock seed=%d size=%.2f] %8.1f ms  baked=%zu hits=%d tris=%zu\n",
               c.seed, c.size, ms, ir.baked.size(), ir.hits, tris);
        fflush(stdout);
    }
    return 0;
}
