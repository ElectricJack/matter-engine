// One-off diagnostic harness: bake an arbitrary part module cold and report
// triangle counts per seed. Built to root-cause the AlpineSlab zero-triangle
// bakes that block the alpine detail tilesets (AlpineRockDetail et al).
//
// Usage: slab_bake_probe <Module> [seed ...]        (default seeds: 0 1 2 3)
//
// Unlike rock_bake_profile this uses a std::filesystem sandbox (no /tmp, no
// system("rm -rf")), so it runs on Windows where those suites are red.
#include "part_graph.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "portable_realpath.h"

using namespace part_graph;

static size_t load_tri_count(uint64_t h, float* out_min_y, float* out_max_y) {
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    if (!part_asset::load_v2(part_asset::cache_path_resolved(h), h, blas, tlas, children, lods))
        return SIZE_MAX;
    size_t n = 0;
    float mn = 1e9f, mx = -1e9f;
    for (const auto& e : blas.get_entries()) {
        n += e->triangles.size();
        for (const auto& t : e->triangles) {
            for (const auto* v : { &t.vertex0, &t.vertex1, &t.vertex2 }) {
                if (v->y < mn) mn = v->y;
                if (v->y > mx) mx = v->y;
            }
        }
    }
    if (out_min_y) *out_min_y = mn;
    if (out_max_y) *out_max_y = mx;
    return n;
}

int main(int argc, char** argv) {
    const std::string schemas    = abspath("../../projects/world_demo/objects");
    const std::string shared_lib = abspath("../shared-lib");

    namespace fs = std::filesystem;
    const fs::path sandbox = fs::absolute("build/slab_probe_sandbox");
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox / "parts", ec);
    if (ec) { printf("FAIL: sandbox mkdir: %s\n", ec.message().c_str()); return 1; }
    fs::current_path(sandbox, ec);
    if (ec) { printf("FAIL: chdir sandbox: %s\n", ec.message().c_str()); return 1; }

    const std::string module = (argc > 1) ? argv[1] : "AlpineSlab";
    std::vector<int> seeds;
    int stage = -1;   // -1: don't pass a stage param
    for (int i = 2; i < argc; ++i) {
        if (strncmp(argv[i], "--stage=", 8) == 0) { stage = atoi(argv[i] + 8); continue; }
        seeds.push_back(atoi(argv[i]));
    }
    if (seeds.empty()) seeds = { 0, 1, 2, 3 };

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib);
    FileModuleResolver resolver(host, schemas);
    HostBaker baker(host, ".");

    for (int seed : seeds) {
        Params p;
        p["seed"] = ParamValue::number(seed);
        if (stage >= 0) p["stage"] = ParamValue::number(stage);

        PartGraph graph(resolver, baker);
        auto t0 = std::chrono::steady_clock::now();
        InstallResult ir = graph.install({ ChildRequest{ module, p } });
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
        if (!ir.ok) {
            printf("[%s seed=%d] FAILED: %s\n", module.c_str(), seed, ir.error.c_str());
            continue;
        }
        if (ir.root_hashes.empty() || ir.root_hashes[0] == 0) {
            printf("[%s seed=%d] FAILED: root hash 0", module.c_str(), seed);
            for (const auto& fp : ir.failed)
                printf(" (%s: %s)", fp.module.c_str(), fp.error.c_str());
            printf("\n");
            continue;
        }
        float mn = 0, mx = 0;
        size_t tris = load_tri_count(ir.root_hashes[0], &mn, &mx);
        printf("[%s seed=%d] %8.1f ms  hash=%016llx tris=%zu y=[%.3f, %.3f]\n",
               module.c_str(), seed, ms,
               (unsigned long long)ir.root_hashes[0], tris,
               (double)mn, (double)mx);
        fflush(stdout);
    }
    return 0;
}
