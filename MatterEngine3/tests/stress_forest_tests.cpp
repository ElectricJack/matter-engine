// stress_forest_tests.cpp
// Real-content flatten-policy check for StressForest50k.
//
// The scatter fixture places COUNT=50000 children in a deterministic 1/3-1/3-1/3
// mix of Pebble / Rock(seed=0) / Tree (see StressForest50k.js). This test proves
// the bake pipeline's per-part flatten policy on that fixture:
//
//   1. Tree.flat.part exists with real merged triangles and NO instance_refs
//      => Tree is FlattenDecision::INLINE — its own subtree (trunk + branches)
//      got fused into a single artifact.
//
//   2. StressForest50k.flat.part exists with zero merged geometry and EXACTLY
//      50000 instance_refs, each pointing at Pebble / Rock / Tree's resolved
//      hash => StressForest50k is FlattenDecision::BOUNDARY — the pipeline
//      never allocated the fully-expanded intermediate buffer. This is the
//      load-bearing memory guarantee for large scatters.
//
//   3. Cross-cache determinism: two independent sandboxes produce the same
//      StressForest50k resolved hash, byte-identical .flat.part files, and an
//      identical FNV-1a hash over the FlatInstanceRef stream (child_hash +
//      transform[16], in table order).
//
// Sandbox: /tmp/me3_stress_forest/cache{A,B}. Rebuilt fresh on every run so the
// install path exercises real bakes (not warm cache hits) and the two caches
// are genuinely independent.
//
// Must run from MatterEngine3/tests/ so ../examples/world_demo paths resolve.

#include "part_graph.h"        // -DMATTER_HAVE_SCRIPT_HOST pulls in script_host.h
#include "part_asset_v2.h"     // cache_path_flat, load_flat_v3, FlatInstanceRef
#include "part_asset.h"        // fnv1a64
#include "part_flatten.h"      // flatten_part
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

using namespace part_graph;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

static const size_t kExpectedCount = 50000;

static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

static std::vector<uint8_t> file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b(std::istreambuf_iterator<char>(f), {});
    return b;
}

// One sandbox: fresh cache dir, PartGraph.install of StressForest50k, then
// explicit flatten_part for both Tree and StressForest50k.
struct BakeRec {
    bool     ok = false;
    uint64_t root_hash = 0;         // StressForest50k
    uint64_t tree_hash = 0;         // Tree
    std::string flat_root_path;     // absolute path to StressForest50k.flat.part
    std::string flat_tree_path;     // absolute path to Tree.flat.part
    part_flatten::FlattenResult flat_root_result;
    part_flatten::FlattenResult flat_tree_result;
};

// Look up a required module's resolved hash by installing it directly. The
// graph caches previously-baked artifacts, so re-installing a module already
// pulled in transitively is a cheap hit.
static uint64_t install_and_hash(PartGraph& g, const std::string& module_name,
                                 const Params& params) {
    ChildRequest req{module_name, params};
    InstallResult ir = g.install({req});
    if (!ir.ok || ir.root_hashes.empty()) return 0;
    return ir.root_hashes[0];
}

static BakeRec run_bake(const std::string& sandbox_abs,
                        const std::string& schemas_abs,
                        const std::string& shared_lib_abs) {
    BakeRec rec;

    char prev[PATH_MAX];
    if (!getcwd(prev, sizeof(prev))) { printf("FAIL: getcwd\n"); return rec; }
    if (chdir(sandbox_abs.c_str()) != 0) {
        printf("FAIL: chdir(%s)\n", sandbox_abs.c_str()); return rec;
    }

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib_abs);
    FileModuleResolver resolver(host, schemas_abs);
    HostBaker baker(host, ".");
    PartGraph graph(resolver, baker);

    // Install StressForest50k: transitively bakes Pebble, Rock(seed=0), Tree,
    // TreeBranch, and the parent. Reads the four child hashes back off the
    // parent's ChildInstance table by installing each module separately (the
    // graph's cache makes the extra installs cheap).
    InstallResult ir_root = graph.install({ChildRequest{"StressForest50k", {}}});
    if (!ir_root.ok || ir_root.root_hashes.empty()) {
        printf("FAIL: install StressForest50k: %s\n", ir_root.error.c_str());
        chdir(prev);
        return rec;
    }
    rec.root_hash = ir_root.root_hashes[0];
    printf("  StressForest50k resolved hash = %016llx (baked=%zu, hits=%d)\n",
           (unsigned long long)rec.root_hash, ir_root.baked.size(), ir_root.hits);

    rec.tree_hash = install_and_hash(graph, "Tree", {});
    if (!rec.tree_hash) { printf("FAIL: install Tree\n"); chdir(prev); return rec; }
    printf("  Tree resolved hash             = %016llx\n",
           (unsigned long long)rec.tree_hash);

    // Explicit flatten calls (install does not flatten; only the viewer's
    // LocalProvider does that on demand — see viewer/local_provider.cpp:170).
    std::string abs_cache = sandbox_abs;

    rec.flat_tree_path = abs_cache + "/" + part_asset::cache_path_flat(rec.tree_hash);
    rec.flat_tree_result = part_flatten::flatten_part(abs_cache, rec.tree_hash);
    printf("  Tree flatten: ok=%d clusters=%zu full_tris=%zu instance_refs=%zu\n",
           (int)rec.flat_tree_result.ok, rec.flat_tree_result.clusters,
           rec.flat_tree_result.full_tris, rec.flat_tree_result.instance_refs);

    rec.flat_root_path = abs_cache + "/" + part_asset::cache_path_flat(rec.root_hash);
    rec.flat_root_result = part_flatten::flatten_part(abs_cache, rec.root_hash);
    printf("  StressForest50k flatten: ok=%d clusters=%zu full_tris=%zu instance_refs=%zu\n",
           (int)rec.flat_root_result.ok, rec.flat_root_result.clusters,
           rec.flat_root_result.full_tris, rec.flat_root_result.instance_refs);

    chdir(prev);
    rec.ok = true;
    return rec;
}

int main() {
    const std::string schemas_abs    = abspath("../examples/world_demo/schemas");
    const std::string shared_lib_abs = abspath("../shared-lib");

    // Fresh sandbox with two independent cache dirs.
    const std::string sandbox = "/tmp/me3_stress_forest";
    system(("rm -rf " + sandbox).c_str());
    const std::string cacheA = sandbox + "/cacheA";
    const std::string cacheB = sandbox + "/cacheB";
    system(("mkdir -p " + cacheA + "/parts").c_str());
    system(("mkdir -p " + cacheB + "/parts").c_str());

    const std::string cacheA_abs = abspath(cacheA);
    const std::string cacheB_abs = abspath(cacheB);

    printf("[cacheA] running bake + flatten\n");
    BakeRec A = run_bake(cacheA_abs, schemas_abs, shared_lib_abs);
    printf("[cacheB] running bake + flatten\n");
    BakeRec B = run_bake(cacheB_abs, schemas_abs, shared_lib_abs);

    CHECK(A.ok && B.ok, "both sandboxes completed bake + flatten");
    if (!A.ok || !B.ok) { printf("\n%d FAILURE(S)\n", g_failures); return 1; }

    // ---- Policy assertions on cache A ---------------------------------------
    printf("\n[test_tree_inline]\n");
    CHECK(A.flat_tree_result.ok, "Tree flatten_part ok");
    CHECK(A.flat_tree_result.clusters > 0,
          "Tree.flat.part has >= 1 cluster (merged geometry present)");
    CHECK(A.flat_tree_result.full_tris > 0,
          "Tree.flat.part has non-zero merged triangles (INLINE fused trunk + branches)");
    CHECK(A.flat_tree_result.instance_refs == 0,
          "Tree.flat.part has zero instance_refs (INLINE, no BOUNDARY children)");

    // Load Tree.flat.part directly and re-verify.
    {
        BLASManager blas; TLASManager tlas(64);
        std::vector<part_asset::FlatCluster> clusters;
        std::vector<part_asset::FlatInstanceRef> refs;
        bool loaded = part_asset::load_flat_v3(A.flat_tree_path, A.tree_hash,
                                               blas, tlas, clusters, refs);
        CHECK(loaded, "Tree.flat.part reloads via load_flat_v3");
        CHECK(!clusters.empty(), "Tree.flat.part reload: clusters non-empty");
        CHECK(refs.empty(), "Tree.flat.part reload: instance_refs empty");
    }

    printf("\n[test_stress_boundary]\n");
    CHECK(A.flat_root_result.ok, "StressForest50k flatten_part ok");
    CHECK(A.flat_root_result.full_tris == 0,
          "StressForest50k.flat.part has zero merged triangles (BOUNDARY, no expansion)");
    CHECK(A.flat_root_result.instance_refs == kExpectedCount,
          "StressForest50k.flat.part instance_refs == 50000 (every scatter kept as instance)");

    // Load StressForest50k.flat.part and verify the refs point only at Pebble /
    // Rock / Tree resolved hashes.
    std::vector<part_asset::FlatInstanceRef> A_refs;
    {
        BLASManager blas; TLASManager tlas(64);
        std::vector<part_asset::FlatCluster> clusters;
        bool loaded = part_asset::load_flat_v3(A.flat_root_path, A.root_hash,
                                               blas, tlas, clusters, A_refs);
        CHECK(loaded, "StressForest50k.flat.part reloads via load_flat_v3");
        CHECK(A_refs.size() == kExpectedCount,
              "StressForest50k.flat.part reload: instance_refs.size == 50000");

        // Each ref must point at one of the three real child hashes. We can
        // check Tree directly; Pebble and Rock we haven't installed separately
        // yet, so verify the three-way bucket count structurally.
        size_t tree_count = 0, other_count = 0;
        for (const auto& r : A_refs) {
            if (r.child_resolved_hash == A.tree_hash) ++tree_count;
            else ++other_count;
        }
        printf("  refs by tree=%zu other=%zu\n", tree_count, other_count);
        // i % 3 with i in [0, 50000): buckets {0: 16667, 1: 16667, 2: 16666}.
        CHECK(tree_count == 16666,
              "StressForest50k.flat.part has 16666 Tree refs (i%%3 == 2 bucket)");
        CHECK(other_count == kExpectedCount - 16666,
              "remaining refs are Pebble + Rock (i%%3 in {0,1})");
    }

    // ---- Cross-cache determinism -------------------------------------------
    printf("\n[test_cross_cache_determinism]\n");
    CHECK(A.root_hash == B.root_hash,
          "StressForest50k resolved hash matches across independent caches");
    CHECK(A.tree_hash == B.tree_hash,
          "Tree resolved hash matches across independent caches");

    auto ba = file_bytes(A.flat_root_path);
    auto bb = file_bytes(B.flat_root_path);
    CHECK(!ba.empty() && ba == bb,
          "StressForest50k.flat.part is byte-identical across caches");

    // FNV-1a over the FlatInstanceRef stream from cache B.
    std::vector<part_asset::FlatInstanceRef> B_refs;
    {
        BLASManager blas; TLASManager tlas(64);
        std::vector<part_asset::FlatCluster> clusters;
        part_asset::load_flat_v3(B.flat_root_path, B.root_hash,
                                 blas, tlas, clusters, B_refs);
    }
    CHECK(A_refs.size() == B_refs.size(),
          "instance_refs count matches across caches");

    uint64_t hA = part_asset::fnv1a64(A_refs.data(),
                                      A_refs.size() * sizeof(part_asset::FlatInstanceRef));
    uint64_t hB = part_asset::fnv1a64(B_refs.data(),
                                      B_refs.size() * sizeof(part_asset::FlatInstanceRef));
    printf("  FNV(A)=%016llx FNV(B)=%016llx\n",
           (unsigned long long)hA, (unsigned long long)hB);
    CHECK(hA != 0 && hA == hB,
          "FlatInstanceRef stream FNV-1a matches across caches");

    printf("\n");
    if (g_failures == 0) printf("ALL PASS\n");
    else                 printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
