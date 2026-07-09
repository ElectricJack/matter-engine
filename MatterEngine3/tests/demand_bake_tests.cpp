// Phase C Task 13 — demand-driven bake primitives: BakePolicy::RootsOnly,
// retained bake_plan, ensure_part_baked / ensure_part_flattened.
//
// Headless (allow_gl_lt_46=true), uses the async sandbox schema dir.
// Hierarchy: Root -> Child (+ Grandchild), so subtree recursion is exercised.
//
// Tests:
//   (a) test_roots_only_bakes_roots:
//       install(policy=RootsOnly) on a cold cache -> root .part exists on disk;
//       child/grandchild .part do NOT; snapshot has ALL nodes with
//       resolved_hash+params_json; bake_plan covers all nodes.
//
//   (b) test_ensure_part_baked_subtree:
//       ensure_part_baked(child_hash) -> child AND grandchild .part appear;
//       second call is a no-op (cached() -- assert bake count did not increase).
//
//   (c) test_hash_parity:
//       resolved hashes from RootsOnly install == hashes from BakePolicy::All
//       install on a twin cache dir (byte-identical uint64 sets).
//
//   (d) test_ensure_part_flattened:
//       after ensure_part_baked(child), flatten -> .flat.part exists with
//       kFormatVersionFlat.

#include "matter/engine_context.h"
#include "matter/world_session.h"

// Internal headers (accessible under MATTER_HAVE_SCRIPT_HOST build):
#include "provider/local_provider.h"
#include "part_graph.h"
#include "part_asset_v2.h"    // cache_path_resolved, cache_path_flat, peek_format_version, kFormatVersionFlat
#include "script_host.h"      // ScriptHost

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "check.h"

// ---------------------------------------------------------------------------
// Sandbox helpers
// ---------------------------------------------------------------------------

static void run_cmd(const std::string& cmd) { std::system(cmd.c_str()); }

static bool write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    if (!f) return false;
    f << body;
    return true;
}

// Build a sandbox with hierarchy: Root -> Child -> Grandchild.
// Root has two children: Child and a leaf Leaf. Child has one child: Grandchild.
// Schemas are trivial mesh parts (no voxel session, no shared-lib imports).
//
//   Grandchild (leaf)
//   Child  -> requires Grandchild
//   Leaf   (leaf, second child of Root)
//   Root   -> requires Child, Leaf
//
// World manifest places Root once (no flags).
static bool build_sandbox(const std::string& root) {
    run_cmd("rm -rf " + root);
    run_cmd("mkdir -p " + root + "/schemas");
    run_cmd("mkdir -p " + root + "/world_data/Hier");
    run_cmd("mkdir -p " + root + "/shared-lib");
    run_cmd("mkdir -p " + root + "/cache/parts");

    // Grandchild — leaf
    if (!write_file(root + "/schemas/Grandchild.js",
        "class Grandchild extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.3;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) return false;

    // Leaf — leaf (second child of Root)
    if (!write_file(root + "/schemas/Leaf.js",
        "class Leaf extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.4;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) return false;

    // Child — requires Grandchild, placed at origin
    if (!write_file(root + "/schemas/Child.js",
        "class Child extends Part {\n"
        "  static get requires() {\n"
        "    return [{ module: 'Grandchild', params: {} }];\n"
        "  }\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.5;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "    this.placeChild('Grandchild', {}, [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);\n"
        "  }\n"
        "}\n")) return false;

    // Root — requires Child + Leaf
    if (!write_file(root + "/schemas/Root.js",
        "class Root extends Part {\n"
        "  static get requires() {\n"
        "    return [\n"
        "      { module: 'Child', params: {} },\n"
        "      { module: 'Leaf',  params: {} }\n"
        "    ];\n"
        "  }\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.6;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "    this.placeChild('Child', {}, [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);\n"
        "    this.placeChild('Leaf',  {}, [1,0,0,0, 0,1,0,0, 0,0,1,0, 1,0,0,1]);\n"
        "  }\n"
        "}\n")) return false;

    // Manifest: one Root placement
    if (!write_file(root + "/world_data/Hier/world.manifest",
        "# demand-bake tests hierarchy\n"
        "Root\n")) return false;

    return true;
}

// Check whether a .part file exists at <cache_root>/parts/<resolved_hash>.part
static bool part_exists(const std::string& cache_root, uint64_t hash) {
    const std::string path = cache_root + "/" + part_asset::cache_path_resolved(hash);
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

// Check whether a .flat.part file exists at the standard path
static bool flat_part_exists(const std::string& cache_root, uint64_t hash) {
    const std::string path = cache_root + "/" + part_asset::cache_path_flat(hash);
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

// Run install_graph() on the sandbox using LocalProvider directly (headless).
// Returns LocalProvider on success (caller owns it, installs onto the heap).
// Sets up and tears down the provider in a controlled way.
static std::unique_ptr<viewer::LocalProvider> make_provider(
    const std::string& sandbox,
    const std::string& world_name = "Hier") {
    viewer::LocalProviderConfig cfg;
    cfg.schemas_dir    = sandbox + "/schemas";
    cfg.world_data_dir = sandbox + "/world_data";
    cfg.world_name     = world_name;
    cfg.shared_lib_dir = sandbox + "/shared-lib";
    cfg.cache_root     = sandbox + "/cache";
    cfg.gl_available   = false;
    return std::make_unique<viewer::LocalProvider>(std::move(cfg));
}

// ---------------------------------------------------------------------------
// (a) test_roots_only_bakes_roots
// ---------------------------------------------------------------------------
static bool test_roots_only_bakes_roots(const std::string& sandbox) {
    printf("-- (a) test_roots_only_bakes_roots\n");

    // Cold cache
    run_cmd("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");

    auto prov = make_provider(sandbox);
    std::string err;
    bool ok = prov->install_graph(err, part_graph::BakePolicy::RootsOnly);
    CHECK(ok, "install_graph(RootsOnly) succeeded");
    if (!ok) { printf("  err: %s\n", err.c_str()); return false; }

    const part_graph::InstallResult& ir = prov->install_result();
    CHECK(ir.ok, "install result ok");
    CHECK(!ir.root_hashes.empty(), "root_hashes not empty");
    CHECK(ir.root_hashes[0] != 0, "root hash non-zero");

    const std::string cache_root = sandbox + "/cache";

    // Root .part must exist
    uint64_t root_hash = ir.root_hashes[0];
    CHECK(part_exists(cache_root, root_hash), "root .part exists on disk");

    // bake_plan must cover all nodes (root + children + grandchild)
    // At minimum: Root, Child, Leaf, Grandchild = 4 nodes
    CHECK(ir.bake_plan.size() >= 4u, "bake_plan covers all nodes (>= 4)");
    printf("  bake_plan node count: %zu\n", ir.bake_plan.size());

    // bake_plan must contain root_hash
    CHECK(ir.bake_plan.count(root_hash) > 0, "bake_plan contains root hash");

    // Snapshot must have all nodes with resolved_hash and params_json
    const auto& snap = prov->graph_snapshot();
    CHECK(snap.nodes.count("Root") > 0, "snapshot has Root");
    CHECK(snap.nodes.count("Child") > 0, "snapshot has Child");
    CHECK(snap.nodes.count("Leaf") > 0, "snapshot has Leaf");
    CHECK(snap.nodes.count("Grandchild") > 0, "snapshot has Grandchild");
    for (const auto& kv : snap.nodes) {
        CHECK(kv.second.resolved_hash != 0,
              ("snapshot node " + kv.first + " has non-zero resolved_hash").c_str());
    }

    // More direct: check snapshot hashes
    uint64_t child_hash = 0, grandchild_hash = 0, leaf_hash = 0;
    {
        auto it = snap.nodes.find("Child");
        if (it != snap.nodes.end()) child_hash = it->second.resolved_hash;
    }
    {
        auto it = snap.nodes.find("Grandchild");
        if (it != snap.nodes.end()) grandchild_hash = it->second.resolved_hash;
    }
    {
        auto it = snap.nodes.find("Leaf");
        if (it != snap.nodes.end()) leaf_hash = it->second.resolved_hash;
    }

    printf("  root_hash=%016llx child_hash=%016llx grandchild_hash=%016llx leaf_hash=%016llx\n",
           (unsigned long long)root_hash,
           (unsigned long long)child_hash,
           (unsigned long long)grandchild_hash,
           (unsigned long long)leaf_hash);

    CHECK(child_hash != 0, "child hash resolved");
    CHECK(grandchild_hash != 0, "grandchild hash resolved");
    CHECK(leaf_hash != 0, "leaf hash resolved");

    // RootsOnly: child/grandchild/leaf .part must NOT exist
    CHECK(!part_exists(cache_root, child_hash),
          "child .part does NOT exist (RootsOnly)");
    CHECK(!part_exists(cache_root, grandchild_hash),
          "grandchild .part does NOT exist (RootsOnly)");
    CHECK(!part_exists(cache_root, leaf_hash),
          "leaf .part does NOT exist (RootsOnly)");

    printf("  (a) PASS\n");
    return true;
}

// ---------------------------------------------------------------------------
// (b) test_ensure_part_baked_subtree
// ---------------------------------------------------------------------------
static bool test_ensure_part_baked_subtree(const std::string& sandbox) {
    printf("-- (b) test_ensure_part_baked_subtree\n");

    // Fresh cache for this test
    run_cmd("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");

    // Wire an on_part counter so we can assert the second call is a true no-op.
    // ensure_part_baked fires on_part for each part it actually bakes (not for
    // cached() short-circuits), so the counter must increase on the first call
    // and must NOT increase on the second call.
    int on_part_count = 0;
    viewer::LocalProviderConfig cfg;
    cfg.schemas_dir    = sandbox + "/schemas";
    cfg.world_data_dir = sandbox + "/world_data";
    cfg.world_name     = "Hier";
    cfg.shared_lib_dir = sandbox + "/shared-lib";
    cfg.cache_root     = sandbox + "/cache";
    cfg.gl_available   = false;
    cfg.on_part = [&](const char* /*module*/, int /*done*/, int /*total*/) {
        ++on_part_count;
    };
    auto prov = std::make_unique<viewer::LocalProvider>(std::move(cfg));

    std::string err;
    bool ok = prov->install_graph(err, part_graph::BakePolicy::RootsOnly);
    CHECK(ok, "install_graph(RootsOnly) succeeded");
    if (!ok) { printf("  err: %s\n", err.c_str()); return false; }

    // Reset counter to 0 after install (install fires on_part for roots baked).
    const int count_after_install = on_part_count;
    on_part_count = 0;
    printf("  on_part count during install (roots): %d\n", count_after_install);

    const part_graph::InstallResult& ir = prov->install_result();
    const auto& snap = prov->graph_snapshot();
    const std::string cache_root = sandbox + "/cache";

    uint64_t child_hash = 0, grandchild_hash = 0;
    {
        auto it = snap.nodes.find("Child");
        if (it != snap.nodes.end()) child_hash = it->second.resolved_hash;
        it = snap.nodes.find("Grandchild");
        if (it != snap.nodes.end()) grandchild_hash = it->second.resolved_hash;
    }
    CHECK(child_hash != 0, "child hash available");
    CHECK(grandchild_hash != 0, "grandchild hash available");

    // Pre-condition: neither .part exists
    CHECK(!part_exists(cache_root, child_hash),
          "child .part absent before ensure_part_baked");
    CHECK(!part_exists(cache_root, grandchild_hash),
          "grandchild .part absent before ensure_part_baked");

    // ensure_part_baked(child) must bake child AND grandchild (post-order DFS)
    std::string bake_err;
    bool bake_ok = prov->ensure_part_baked(child_hash, bake_err);
    CHECK(bake_ok, "ensure_part_baked(child) succeeded");
    if (!bake_ok) { printf("  err: %s\n", bake_err.c_str()); return false; }

    CHECK(part_exists(cache_root, child_hash),
          "child .part exists after ensure_part_baked");
    CHECK(part_exists(cache_root, grandchild_hash),
          "grandchild .part exists after ensure_part_baked");

    // on_part must have fired at least once (child + grandchild both baked).
    const int count_after_first = on_part_count;
    printf("  on_part count during first ensure_part_baked: %d\n", count_after_first);
    CHECK(count_after_first > 0,
          "on_part fired during first ensure_part_baked (parts were baked)");

    // Second call must be a no-op: cached() short-circuits before any bake,
    // so on_part must NOT fire and the counter must not increase.
    on_part_count = 0;
    std::string bake_err2;
    bool bake_ok2 = prov->ensure_part_baked(child_hash, bake_err2);
    CHECK(bake_ok2, "ensure_part_baked(child) second call succeeded (no-op)");
    printf("  on_part count during second ensure_part_baked (expect 0): %d\n", on_part_count);
    CHECK(on_part_count == 0,
          "on_part did NOT fire on second ensure_part_baked (cached() short-circuit)");

    // Fix 1 direct assertion: unknown top-level hash must fail with an error message.
    std::string stale_err;
    bool stale_ok = prov->ensure_part_baked(0xDEADBEEFDEADBEEFULL, stale_err);
    CHECK(!stale_ok, "ensure_part_baked(stale/garbage hash) returns false");
    CHECK(!stale_err.empty(), "ensure_part_baked(stale/garbage hash) sets err");
    printf("  stale hash error: %s\n", stale_err.c_str());

    printf("  (b) PASS\n");
    return true;
}

// ---------------------------------------------------------------------------
// (c) test_hash_parity
// ---------------------------------------------------------------------------
static bool test_hash_parity(const std::string& sandbox) {
    printf("-- (c) test_hash_parity\n");

    // RootsOnly install on sandbox/cache_a
    run_cmd("rm -rf " + sandbox + "/cache_a && mkdir -p " + sandbox + "/cache_a/parts");
    // BakeAll install on sandbox/cache_b
    run_cmd("rm -rf " + sandbox + "/cache_b && mkdir -p " + sandbox + "/cache_b/parts");

    // Provider A — RootsOnly
    viewer::LocalProviderConfig cfgA;
    cfgA.schemas_dir    = sandbox + "/schemas";
    cfgA.world_data_dir = sandbox + "/world_data";
    cfgA.world_name     = "Hier";
    cfgA.shared_lib_dir = sandbox + "/shared-lib";
    cfgA.cache_root     = sandbox + "/cache_a";
    cfgA.gl_available   = false;
    auto provA = std::make_unique<viewer::LocalProvider>(std::move(cfgA));

    std::string errA;
    bool okA = provA->install_graph(errA, part_graph::BakePolicy::RootsOnly);
    CHECK(okA, "install_graph(RootsOnly) for parity A");
    if (!okA) { printf("  errA: %s\n", errA.c_str()); return false; }

    // Provider B — All (default)
    viewer::LocalProviderConfig cfgB;
    cfgB.schemas_dir    = sandbox + "/schemas";
    cfgB.world_data_dir = sandbox + "/world_data";
    cfgB.world_name     = "Hier";
    cfgB.shared_lib_dir = sandbox + "/shared-lib";
    cfgB.cache_root     = sandbox + "/cache_b";
    cfgB.gl_available   = false;
    auto provB = std::make_unique<viewer::LocalProvider>(std::move(cfgB));

    std::string errB;
    bool okB = provB->install_graph(errB);  // BakePolicy::All is default
    CHECK(okB, "install_graph(All) for parity B");
    if (!okB) { printf("  errB: %s\n", errB.c_str()); return false; }

    // Collect hashes from bake_plan A
    std::set<uint64_t> hashes_a, hashes_b;
    for (const auto& kv : provA->install_result().bake_plan)
        hashes_a.insert(kv.first);
    for (const auto& kv : provB->install_result().bake_plan)
        hashes_b.insert(kv.first);

    printf("  hashes_a count: %zu, hashes_b count: %zu\n",
           hashes_a.size(), hashes_b.size());

    CHECK(hashes_a == hashes_b, "bake_plan hash sets are identical (RootsOnly == All)");

    // Also compare root_hashes
    const auto& irA = provA->install_result();
    const auto& irB = provB->install_result();
    CHECK(irA.root_hashes.size() == irB.root_hashes.size(),
          "root_hashes count matches");
    if (irA.root_hashes.size() == irB.root_hashes.size()) {
        for (size_t i = 0; i < irA.root_hashes.size(); ++i) {
            CHECK(irA.root_hashes[i] == irB.root_hashes[i],
                  "root_hashes[i] matches between policies");
        }
    }

    printf("  (c) PASS\n");
    return true;
}

// ---------------------------------------------------------------------------
// (d) test_ensure_part_flattened
// ---------------------------------------------------------------------------
static bool test_ensure_part_flattened(const std::string& sandbox) {
    printf("-- (d) test_ensure_part_flattened\n");

    // Fresh cache
    run_cmd("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");

    auto prov = make_provider(sandbox);
    std::string err;
    bool ok = prov->install_graph(err, part_graph::BakePolicy::RootsOnly);
    CHECK(ok, "install_graph(RootsOnly) succeeded");
    if (!ok) { printf("  err: %s\n", err.c_str()); return false; }

    const auto& snap = prov->graph_snapshot();
    const std::string cache_root = sandbox + "/cache";

    uint64_t child_hash = 0;
    {
        auto it = snap.nodes.find("Child");
        if (it != snap.nodes.end()) child_hash = it->second.resolved_hash;
    }
    CHECK(child_hash != 0, "child hash resolved");

    // Bake child subtree first
    std::string bake_err;
    bool bake_ok = prov->ensure_part_baked(child_hash, bake_err);
    CHECK(bake_ok, "ensure_part_baked(child) succeeded");
    if (!bake_ok) { printf("  err: %s\n", bake_err.c_str()); return false; }

    // Now flatten
    bool flat_ok = prov->ensure_part_flattened(child_hash);
    CHECK(flat_ok, "ensure_part_flattened(child) succeeded");

    // Verify .flat.part exists and has correct version
    const std::string flat_path = cache_root + "/" + part_asset::cache_path_flat(child_hash);
    CHECK(flat_part_exists(cache_root, child_hash), ".flat.part exists");
    uint32_t fmt_ver = part_asset::peek_format_version(flat_path);
    printf("  flat format version: %u (expected %u)\n",
           fmt_ver, part_asset::kFormatVersionFlat);
    CHECK(fmt_ver == part_asset::kFormatVersionFlat,
          ".flat.part has kFormatVersionFlat version");

    printf("  (d) PASS\n");
    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    const std::string sandbox = "/tmp/demand_bake_tests_sandbox";
    printf("=== demand_bake_tests ===\n");
    printf("sandbox: %s\n\n", sandbox.c_str());

    if (!build_sandbox(sandbox)) {
        printf("FATAL: failed to build sandbox\n");
        return 1;
    }

    bool a = test_roots_only_bakes_roots(sandbox);
    bool b = test_ensure_part_baked_subtree(sandbox);
    bool c = test_hash_parity(sandbox);
    bool d = test_ensure_part_flattened(sandbox);

    printf("\n");
    if (g_failures == 0) {
        printf("ALL PASS (%s %s %s %s)\n",
               a ? "a" : "a-FAIL",
               b ? "b" : "b-FAIL",
               c ? "c" : "c-FAIL",
               d ? "d" : "d-FAIL");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
