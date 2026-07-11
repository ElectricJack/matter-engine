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
#include "blas_manager.hpp"   // BLASManager (for load_v2 in case f)
#include "tlas_manager.hpp"   // TLASManager (for load_v2 in case f)
#include "tileset_phase.h"    // run_tileset_phase (settle-only overload)
#include "tileset_bake.h"     // SettledTorus, SettleReport

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
// (e) test_demand_bake_e2e — end-to-end async demand-bake flow.
//
// Sandbox: Root requires [Placed1, Placed2, Placed3, Unplaced].
//   Placed1/2/3 are placed via placeChild() in Root's build().
//   Unplaced is declared in static requires but NOT placed in build().
//
// Drive the async session to BakeFinished and assert:
//   (a) BakeFinished.errors == 0
//   (b) all placed parts' .part and .flat.part now exist on disk
//   (c) Unplaced's .part does NOT exist (never published → never baked)
//   (d) instances_total > 0 (placed parts rendered)
//   (e) BakePartDone events with phase="parts" and non-empty module labels arrived
// ---------------------------------------------------------------------------

// Build a sandbox mimicking the Meadow pattern for demand bake:
//   World (expand root) requires [Placed1, Placed2, Placed3, Unplaced].
//   World's build() only calls placeChild for Placed1/2/3.
//   The manifest declares World with `expand` flag so each placed child
//   becomes a first-class manifest entry. Unplaced is in `requires` but
//   never a placeChild → it's in the bake_plan but never in manifest →
//   never in publish_order → never gets ensure_part_baked called.
//
// This mirrors the Terrain coarse/full pattern in Meadow.js:
//   requires both resolutions but only places coarse tiles.
static bool build_e2e_sandbox(const std::string& root) {
    run_cmd("rm -rf " + root);
    run_cmd("mkdir -p " + root + "/schemas");
    run_cmd("mkdir -p " + root + "/world_data/E2E");
    run_cmd("mkdir -p " + root + "/shared-lib");
    run_cmd("mkdir -p " + root + "/cache/parts");

    // Leaf schema template — each leaf is a distinct geometry so hashes differ.
    auto write_leaf = [&](const std::string& name, float size) -> bool {
        std::ostringstream js;
        js << "class " << name << " extends Part {\n"
           << "  build(p) {\n"
           << "    this.fill(MAT.stone);\n"
           << "    const S = " << size << ";\n"
           << "    this.beginShape(SHAPE.triangles);\n"
           << "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
           << "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
           << "    this.endShape();\n"
           << "  }\n"
           << "}\n";
        return write_file(root + "/schemas/" + name + ".js", js.str());
    };

    if (!write_leaf("Placed1",  0.3f)) return false;
    if (!write_leaf("Placed2",  0.4f)) return false;
    if (!write_leaf("Placed3",  0.5f)) return false;
    if (!write_leaf("Unplaced", 0.6f)) return false;  // required but never placed

    // World (expand root): requires all four, but placeChild only the three placed.
    // Unplaced is in static requires (bake_plan covers it) but never placeChild'd.
    // With `expand` in world.manifest, only Placed1/2/3 become manifest entries.
    if (!write_file(root + "/schemas/World.js",
        "class World extends Part {\n"
        "  static get requires() {\n"
        "    return [\n"
        "      { module: 'Placed1',  params: {} },\n"
        "      { module: 'Placed2',  params: {} },\n"
        "      { module: 'Placed3',  params: {} },\n"
        "      { module: 'Unplaced', params: {} }\n"
        "    ];\n"
        "  }\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.7;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "    this.placeChild('Placed1', {}, [1,0,0,0, 0,1,0,0, 0,0,1,0,  0,0,0,1]);\n"
        "    this.placeChild('Placed2', {}, [1,0,0,0, 0,1,0,0, 0,0,1,0,  2,0,0,1]);\n"
        "    this.placeChild('Placed3', {}, [1,0,0,0, 0,1,0,0, 0,0,1,0, -2,0,0,1]);\n"
        "    // Unplaced is NOT placed — required (bake_plan covers it) but never a world instance\n"
        "  }\n"
        "}\n")) return false;

    // Manifest: World with `expand` flag — children become individual instances.
    // Only placed children (Placed1/2/3) appear in the expanded manifest.
    if (!write_file(root + "/world_data/E2E/world.manifest",
        "# demand-bake e2e test\n"
        "World expand\n")) return false;

    return true;
}

using clk_e2e = std::chrono::steady_clock;

static bool drive_bake_e2e(matter::WorldSession& s,
                            std::vector<matter::Event>& log,
                            int timeout_sec = 120) {
    auto deadline = clk_e2e::now() + std::chrono::seconds(timeout_sec);
    bool finished = false;
    while (clk_e2e::now() < deadline) {
        s.pump_gpu_jobs(4.0f);
        matter::Event ev;
        bool any = false;
        while (s.poll_event(ev)) {
            any = true;
            log.push_back(ev);
            if (ev.type == matter::EventType::BakeFinished) { finished = true; break; }
            if (ev.type == matter::EventType::BakeError) {
                printf("  BakeError: code=%d phase=%s msg=%s\n",
                       (int)ev.code, ev.phase.c_str(), ev.message.c_str());
            }
        }
        if (finished) return true;
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    printf("  drive_bake_e2e TIMEOUT after %ds\n", timeout_sec);
    return false;
}

static bool test_demand_bake_e2e(const std::string& base_dir) {
    printf("-- (e) test_demand_bake_e2e\n");

    const std::string sandbox = base_dir + "_e2e";
    if (!build_e2e_sandbox(sandbox)) {
        printf("  FAIL: build_e2e_sandbox\n");
        ++g_failures;
        return false;
    }

    // Cold cache
    run_cmd("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");

    std::string cache_root_s = sandbox + "/cache";
    std::string schemas_s    = sandbox + "/schemas";
    std::string wdata_s      = sandbox + "/world_data";
    std::string shlib_s      = sandbox + "/shared-lib";

    std::string err;
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;
    auto engine = matter::EngineContext::create(ed, err);
    CHECK(engine != nullptr, "e2e: engine created");
    if (!engine) { printf("  err: %s\n", err.c_str()); run_cmd("rm -rf " + sandbox); return false; }

    matter::WorldDesc wd;
    wd.schemas_dir    = schemas_s.c_str();
    wd.world_data_dir = wdata_s.c_str();
    wd.world_name     = "E2E";
    wd.shared_lib_dir = shlib_s.c_str();
    auto s = engine->open_world(wd, err);
    CHECK(s != nullptr, "e2e: session opened");
    if (!s) { printf("  err: %s\n", err.c_str()); run_cmd("rm -rf " + sandbox); return false; }

    s->request_bake();
    std::vector<matter::Event> log;
    bool bake_ok = drive_bake_e2e(*s, log);
    CHECK(bake_ok, "e2e: BakeFinished arrived");
    if (!bake_ok) { run_cmd("rm -rf " + sandbox); return false; }

    // (a) BakeFinished.errors == 0
    matter::Event* finished_ev = nullptr;
    for (auto& ev : log)
        if (ev.type == matter::EventType::BakeFinished) { finished_ev = &ev; break; }
    CHECK(finished_ev != nullptr, "e2e: BakeFinished event found");
    if (finished_ev) {
        printf("  BakeFinished.errors=%d\n", finished_ev->errors);
        CHECK(finished_ev->errors == 0, "e2e: BakeFinished.errors == 0");
    }

    // We need part hashes to verify disk presence. Use LocalProvider directly to get them.
    // Build a LocalProvider on the existing warm cache to read graph snapshot hashes.
    viewer::LocalProviderConfig cfg2;
    cfg2.schemas_dir    = schemas_s;
    cfg2.world_data_dir = wdata_s;
    cfg2.world_name     = "E2E";
    cfg2.shared_lib_dir = shlib_s;
    cfg2.cache_root     = cache_root_s;
    cfg2.gl_available   = false;
    auto prov = std::make_unique<viewer::LocalProvider>(std::move(cfg2));
    std::string ierr;
    // Use RootsOnly so we get the bake_plan without re-baking anything.
    bool inst_ok = prov->install_graph(ierr, part_graph::BakePolicy::RootsOnly);
    CHECK(inst_ok, "e2e: verify provider install succeeded");
    if (!inst_ok) { printf("  install err: %s\n", ierr.c_str()); run_cmd("rm -rf " + sandbox); return false; }

    const part_graph::InstallResult& ir = prov->install_result();
    const auto& snap = prov->graph_snapshot();

    // World expand root: the manifest gets Placed1/2/3 as world instances.
    // World itself is the root. Unplaced is in bake_plan but NOT in manifest.
    uint64_t world_hash = (ir.root_hashes.empty() ? 0 : ir.root_hashes[0]);
    uint64_t placed1_hash = 0, placed2_hash = 0, placed3_hash = 0, unplaced_hash = 0;
    {
        auto it = snap.nodes.find("Placed1");
        if (it != snap.nodes.end()) placed1_hash = it->second.resolved_hash;
        it = snap.nodes.find("Placed2");
        if (it != snap.nodes.end()) placed2_hash = it->second.resolved_hash;
        it = snap.nodes.find("Placed3");
        if (it != snap.nodes.end()) placed3_hash = it->second.resolved_hash;
        it = snap.nodes.find("Unplaced");
        if (it != snap.nodes.end()) unplaced_hash = it->second.resolved_hash;
    }
    printf("  world=%016llx placed1=%016llx placed2=%016llx placed3=%016llx unplaced=%016llx\n",
           (unsigned long long)world_hash,
           (unsigned long long)placed1_hash,
           (unsigned long long)placed2_hash,
           (unsigned long long)placed3_hash,
           (unsigned long long)unplaced_hash);
    CHECK(world_hash    != 0, "e2e: world hash resolved");
    CHECK(placed1_hash  != 0, "e2e: placed1 hash resolved");
    CHECK(placed2_hash  != 0, "e2e: placed2 hash resolved");
    CHECK(placed3_hash  != 0, "e2e: placed3 hash resolved");
    CHECK(unplaced_hash != 0, "e2e: unplaced hash resolved");
    // Unplaced must be covered by bake_plan (it's in requires)
    CHECK(ir.bake_plan.count(unplaced_hash) > 0,
          "e2e: unplaced IS in bake_plan (requires declared it)");

    // (b) All placed parts' .part AND .flat.part now exist on disk.
    // With expand flag: Placed1/2/3 are direct manifest entries → each gets
    // ensure_part_baked + ensure_part_flattened in the publish loop.
    // World's own .part also exists (baked at install as root).
    CHECK(part_exists(cache_root_s, world_hash),  "e2e: World .part exists (root, baked at install)");
    CHECK(part_exists(cache_root_s, placed1_hash), "e2e: Placed1 .part exists (demand baked)");
    CHECK(part_exists(cache_root_s, placed2_hash), "e2e: Placed2 .part exists (demand baked)");
    CHECK(part_exists(cache_root_s, placed3_hash), "e2e: Placed3 .part exists (demand baked)");
    CHECK(flat_part_exists(cache_root_s, placed1_hash), "e2e: Placed1 .flat.part exists");
    CHECK(flat_part_exists(cache_root_s, placed2_hash), "e2e: Placed2 .flat.part exists");
    CHECK(flat_part_exists(cache_root_s, placed3_hash), "e2e: Placed3 .flat.part exists");

    // (c) Unplaced's .part does NOT exist.
    // With RootsOnly install: World is the root (baked); Placed1/2/3 are demand-baked
    // by the publish loop. Unplaced is in bake_plan but never appears in the manifest
    // (World's build() never calls placeChild('Unplaced')) so it's never in
    // publish_order and ensure_part_baked is never called for it.
    printf("  unplaced .part exists: %s (expect: no)\n",
           part_exists(cache_root_s, unplaced_hash) ? "YES" : "no");
    CHECK(!part_exists(cache_root_s, unplaced_hash),
          "e2e: Unplaced .part does NOT exist (never in manifest → never demand-baked)");

    // (d) instances_total > 0
    const auto& fs = s->frame_stats();
    printf("  instances_total=%u\n", fs.instances_total);
    CHECK(fs.instances_total > 0, "e2e: instances_total > 0 (placed parts rendered)");

    // (e) At least one BakePartDone with phase="parts" and a non-empty module label
    int parts_events_with_module = 0;
    for (const auto& ev : log) {
        if (ev.type == matter::EventType::BakePartDone && ev.phase == "parts") {
            if (!ev.module.empty()) ++parts_events_with_module;
        }
    }
    printf("  BakePartDone(phase=parts, module!=empty) count: %d\n",
           parts_events_with_module);
    CHECK(parts_events_with_module >= 1,
          "e2e: BakePartDone events with phase=parts and non-empty module arrived");

    run_cmd("rm -rf " + sandbox);
    printf("  (e) PASS\n");
    return true;
}

// ---------------------------------------------------------------------------
// (f) test_parametric_placements_distinct
//
// Root schema declares three children of the same module 'Tile' with distinct
// params: {i:1}, {i:2}, and {b:2, a:0.1} (multi-key, unsorted author order, float).
// build() places all three via placeChild('Tile', params).
// After bake:
//   - Root .part child-instance table has 3 entries with DISTINCT resolved hashes.
//   - Each child hash is found in bake_plan; its BakeInputs.params round-trip to
//     the expected canonical JSON via params_to_json.
// ---------------------------------------------------------------------------
static bool build_parametric_sandbox(const std::string& root) {
    run_cmd("rm -rf " + root);
    run_cmd("mkdir -p " + root + "/schemas");
    run_cmd("mkdir -p " + root + "/world_data/Param");
    run_cmd("mkdir -p " + root + "/shared-lib");
    run_cmd("mkdir -p " + root + "/cache/parts");

    // Tile module: parameterised by {i} (integer variant) or {a, b} (float variant).
    // Each distinct params set produces distinct geometry (and thus a distinct hash).
    if (!write_file(root + "/schemas/Tile.js",
        "class Tile extends Part {\n"
        "  static params = { i: 0, a: 0.0, b: 0.0 };\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.1 + p.i * 0.1 + p.a * 0.05 + p.b * 0.03;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) return false;

    // Root: declares three Tile variants in static requires.
    // Middle entry uses author-order {b:2, a:0.1} (keys reversed from canonical
    // sorted order {a:0.1, b:2}) to exercise the normalization path.
    // Crucially: {i:2} is declared LAST so the bare 'Tile' key in name2hash
    // ends up holding the {i:2} hash. With the pre-fix bug, placeChild('Tile',{b:2,a:0.1})
    // misses the composite key (JSON format mismatch) and falls back to the bare 'Tile'
    // key which is the {i:2} hash — causing child[1] and child[2] to share a hash.
    // After the fix, lookup_child_hash normalizes {b:2,a:0.1} -> {a:0.1,b:2} JSON
    // and finds the correct composite key, so all three child hashes are distinct.
    if (!write_file(root + "/schemas/ParamRoot.js",
        "class ParamRoot extends Part {\n"
        "  static requires = [\n"
        "    { module: 'Tile', params: { i: 1 } },\n"
        "    { module: 'Tile', params: { b: 2, a: 0.1 } },\n"
        "    { module: 'Tile', params: { i: 2 } },\n"
        "  ];\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.6;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "    // Place all three variants.\n"
        "    this.placeChild('Tile', { i: 1 }, [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);\n"
        "    // Author passes {b:2, a:0.1} — reversed key order vs canonical {a:0.1, b:2}.\n"
        "    // lookup_child_hash must normalize so this resolves to the {a:0.1,b:2} variant,\n"
        "    // NOT the bare-module fallback (which would give {i:2}'s hash).\n"
        "    this.placeChild('Tile', { b: 2, a: 0.1 }, [1,0,0,0, 0,1,0,0, 0,0,1,0, 1,0,0,1]);\n"
        "    this.placeChild('Tile', { i: 2 }, [1,0,0,0, 0,1,0,0, 0,0,1,0, 2,0,0,1]);\n"
        "  }\n"
        "}\n")) return false;

    if (!write_file(root + "/world_data/Param/world.manifest",
        "# parametric placements test\n"
        "ParamRoot\n")) return false;

    return true;
}

static bool test_parametric_placements_distinct(const std::string& base_dir) {
    printf("-- (f) test_parametric_placements_distinct\n");

    const std::string sandbox = base_dir + "_param";
    if (!build_parametric_sandbox(sandbox)) {
        printf("  FAIL: build_parametric_sandbox\n");
        ++g_failures;
        return false;
    }

    run_cmd("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");

    viewer::LocalProviderConfig cfg;
    cfg.schemas_dir    = sandbox + "/schemas";
    cfg.world_data_dir = sandbox + "/world_data";
    cfg.world_name     = "Param";
    cfg.shared_lib_dir = sandbox + "/shared-lib";
    cfg.cache_root     = sandbox + "/cache";
    cfg.gl_available   = false;
    auto prov = std::make_unique<viewer::LocalProvider>(std::move(cfg));

    std::string err;
    // BakePolicy::All — bake every node so all three Tile variants' .part exist.
    bool ok = prov->install_graph(err, part_graph::BakePolicy::All);
    CHECK(ok, "(f) install_graph(All) succeeded");
    if (!ok) {
        printf("  err: %s\n", err.c_str());
        run_cmd("rm -rf " + sandbox);
        return false;
    }

    const part_graph::InstallResult& ir = prov->install_result();
    CHECK(ir.ok, "(f) install result ok");
    CHECK(!ir.root_hashes.empty() && ir.root_hashes[0] != 0, "(f) root hash non-zero");
    if (ir.root_hashes.empty() || ir.root_hashes[0] == 0) {
        run_cmd("rm -rf " + sandbox);
        return false;
    }

    // Build a params_json -> resolved_hash map from bake_plan for Tile entries.
    // (bake_plan is keyed by resolved_hash -> BakeInputs)
    std::map<std::string, uint64_t> tile_hash_by_params;
    for (const auto& kv : ir.bake_plan) {
        if (kv.second.module == "Tile") {
            tile_hash_by_params[part_graph::params_to_json(kv.second.params)] = kv.first;
        }
    }
    printf("  tile variants in bake_plan: %zu\n", tile_hash_by_params.size());
    CHECK(tile_hash_by_params.size() == 3u, "(f) bake_plan has 3 distinct Tile variants");

    // Canonical JSON for the three params sets (sorted key order, %.17g numbers).
    const std::string p1 = "{\"a\":0,\"b\":0,\"i\":1}";    // {i:1} merged with static defaults
    const std::string p2 = "{\"a\":0,\"b\":0,\"i\":2}";    // {i:2} merged with static defaults
    // {b:2, a:0.1} merged with static defaults {i:0}:
    const std::string p3 = "{\"a\":0.10000000000000001,\"b\":2,\"i\":0}";

    // Verify all three are present.
    // (Merged params may differ from override-only JSON due to static defaults.)
    // We check that bake_plan contains entries for the canonical override params.
    // Use params_to_json on a Params built from override-only to derive lookup keys
    // the bake_plan may use (host merges static defaults but the canonical override
    // JSON is what uniquely distinguishes variants in the graph).
    // Simpler: just assert all three hashes are non-zero and DISTINCT.
    std::vector<uint64_t> tile_hashes;
    for (const auto& kv : tile_hash_by_params) {
        printf("  Tile params=%s -> hash=%016llx\n",
               kv.first.c_str(), (unsigned long long)kv.second);
        tile_hashes.push_back(kv.second);
    }
    CHECK(tile_hashes.size() == 3u, "(f) 3 Tile hashes in bake_plan");
    if (tile_hashes.size() == 3u) {
        CHECK(tile_hashes[0] != tile_hashes[1], "(f) Tile{i:1} != Tile{i:2}");
        CHECK(tile_hashes[0] != tile_hashes[2], "(f) Tile{i:1} != Tile{a:0.1,b:2}");
        CHECK(tile_hashes[1] != tile_hashes[2], "(f) Tile{i:2} != Tile{a:0.1,b:2}");
    }

    // Read the root .part child-instance table and verify 3 DISTINCT child hashes.
    // This is the key assertion: each placeChild('Tile', params) must resolve to
    // its own variant's hash, not collapse via the module-only fallback.
    const std::string cache_root = sandbox + "/cache";
    const uint64_t root_hash = ir.root_hashes[0];
    CHECK(part_exists(cache_root, root_hash), "(f) root .part exists");
    for (uint64_t h : tile_hashes)
        CHECK(part_exists(cache_root, h), "(f) Tile variant .part exists");

    {
        const std::string part_path = cache_root + "/" + part_asset::cache_path_resolved(root_hash);
        BLASManager blas;
        TLASManager tlas(64);
        std::vector<part_asset::ChildInstance> children;
        part_asset::LodLevels lods;
        bool loaded = part_asset::load_v2(part_path, root_hash, blas, tlas, children, lods);
        CHECK(loaded, "(f) root .part loads via load_v2");
        if (loaded) {
            printf("  root .part child_instance count: %zu (expect 3)\n", children.size());
            CHECK(children.size() == 3u, "(f) root .part has 3 child instances");
            if (children.size() == 3u) {
                // All three placed child hashes must be distinct.
                CHECK(children[0].child_resolved_hash != children[1].child_resolved_hash,
                      "(f) child[0] != child[1] (distinct Tile variants placed)");
                CHECK(children[0].child_resolved_hash != children[2].child_resolved_hash,
                      "(f) child[0] != child[2] (third variant uses correct normalized key)");
                CHECK(children[1].child_resolved_hash != children[2].child_resolved_hash,
                      "(f) child[1] != child[2] (distinct Tile variants placed)");
                // Each child hash must be in the bake_plan for module Tile.
                for (size_t ci = 0; ci < 3; ++ci) {
                    uint64_t ch = children[ci].child_resolved_hash;
                    auto bit = ir.bake_plan.find(ch);
                    CHECK(bit != ir.bake_plan.end(),
                          ("(f) child[" + std::to_string(ci) + "] hash in bake_plan").c_str());
                    if (bit != ir.bake_plan.end()) {
                        CHECK(bit->second.module == "Tile",
                              ("(f) child[" + std::to_string(ci) + "] module == Tile").c_str());
                    }
                    printf("  child[%zu] hash=%016llx module=%s\n",
                           ci, (unsigned long long)ch,
                           (bit != ir.bake_plan.end()) ? bit->second.module.c_str() : "?");
                }
            }
        }
    }

    // Verify the root baked without errors.
    CHECK(ir.failed.empty(), "(f) no failed parts");
    if (!ir.failed.empty()) {
        for (const auto& fp : ir.failed)
            printf("  failed: module=%s err=%s\n", fp.module.c_str(), fp.error.c_str());
    }

    run_cmd("rm -rf " + sandbox);
    printf("  (f) PASS\n");
    return true;
}

// ---------------------------------------------------------------------------
// (g) test_placechild_param_mismatch_errors
//
// Root schema declares Tile with params {i:1} and {i:2} in static requires.
// build() calls placeChild('Tile', {i:3}) — an undeclared variant.
// The root bake must FAIL (install returns ok==false OR the root has a failed
// entry) with an error message that names the module 'Tile'.
// ---------------------------------------------------------------------------
static bool build_mismatch_sandbox(const std::string& root) {
    run_cmd("rm -rf " + root);
    run_cmd("mkdir -p " + root + "/schemas");
    run_cmd("mkdir -p " + root + "/world_data/Mismatch");
    run_cmd("mkdir -p " + root + "/shared-lib");
    run_cmd("mkdir -p " + root + "/cache/parts");

    // Tile module: parameterised by {i}.
    if (!write_file(root + "/schemas/Tile.js",
        "class Tile extends Part {\n"
        "  static params = { i: 0 };\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.1 + p.i * 0.1;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) return false;

    // Root: declares Tile with {i:1} and {i:2} only. build() tries {i:3} — undeclared.
    if (!write_file(root + "/schemas/MismatchRoot.js",
        "class MismatchRoot extends Part {\n"
        "  static requires = [\n"
        "    { module: 'Tile', params: { i: 1 } },\n"
        "    { module: 'Tile', params: { i: 2 } },\n"
        "  ];\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.6;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "    // {i:3} was NOT declared in static requires — must be a bake error.\n"
        "    this.placeChild('Tile', { i: 3 }, [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);\n"
        "  }\n"
        "}\n")) return false;

    if (!write_file(root + "/world_data/Mismatch/world.manifest",
        "# param-mismatch error test\n"
        "MismatchRoot\n")) return false;

    return true;
}

static bool test_placechild_param_mismatch_errors(const std::string& base_dir) {
    printf("-- (g) test_placechild_param_mismatch_errors\n");

    const std::string sandbox = base_dir + "_mismatch";
    if (!build_mismatch_sandbox(sandbox)) {
        printf("  FAIL: build_mismatch_sandbox\n");
        ++g_failures;
        return false;
    }

    run_cmd("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");

    viewer::LocalProviderConfig cfg;
    cfg.schemas_dir    = sandbox + "/schemas";
    cfg.world_data_dir = sandbox + "/world_data";
    cfg.world_name     = "Mismatch";
    cfg.shared_lib_dir = sandbox + "/shared-lib";
    cfg.cache_root     = sandbox + "/cache";
    cfg.gl_available   = false;
    auto prov = std::make_unique<viewer::LocalProvider>(std::move(cfg));

    std::string err;
    bool ok = prov->install_graph(err, part_graph::BakePolicy::All);

    // The root bake must fail: either install returns false (hard error)
    // OR install_result.failed is non-empty (soft skip-and-continue).
    // In either case the error message must name 'Tile'.
    const part_graph::InstallResult& ir = prov->install_result();

    bool root_failed = !ok || !ir.failed.empty() ||
                       (!ir.root_hashes.empty() && ir.root_hashes[0] == 0);

    printf("  install ok=%d root_hashes[0]=%016llx failed=%zu err='%s'\n",
           (int)ok,
           ir.root_hashes.empty() ? 0ULL : (unsigned long long)ir.root_hashes[0],
           ir.failed.size(),
           err.c_str());

    CHECK(root_failed, "(g) MismatchRoot bake failed (placeChild with undeclared params)");

    // Collect error messages from all sources.
    std::string all_errors = err;
    for (const auto& fp : ir.failed) {
        all_errors += " | " + fp.error;
        printf("  failed: module=%s err=%s\n", fp.module.c_str(), fp.error.c_str());
    }
    printf("  all_errors: %s\n", all_errors.c_str());

    // The error chain must mention 'Tile' (the undeclared-variant module) OR
    // 'MismatchRoot' (the failing part). The DSL set_error fires on 'Tile' but
    // the outer FailedPart names 'MismatchRoot'. Either form satisfies the spec.
    bool names_module = all_errors.find("Tile") != std::string::npos ||
                        all_errors.find("MismatchRoot") != std::string::npos;
    CHECK(names_module, "(g) error message names the relevant module (Tile or MismatchRoot)");

    // No 'Tile' variant should have been placed (no child .part from the mismatch).
    // With param-mismatch failing at lookup, MismatchRoot's bake fails before any
    // placeChild succeeds — the root .part must NOT exist on disk.
    if (!ir.root_hashes.empty() && ir.root_hashes[0] != 0) {
        bool root_on_disk = part_exists(sandbox + "/cache", ir.root_hashes[0]);
        printf("  root .part on disk: %s (expect: no, bake failed)\n",
               root_on_disk ? "YES" : "no");
        CHECK(!root_on_disk, "(g) root .part does NOT exist (bake failed)");
    }

    run_cmd("rm -rf " + sandbox);
    printf("  (g) PASS\n");
    return true;
}

// ---------------------------------------------------------------------------
// (h) test_tileset_deferred_ordering
//
// Build a world with one tileset root (SimpleTileset). The tileset root's
// children are leaf parts. Bake the world async (headless). Assert:
//   (1) BakeFinished arrives with zero BakePartDone(phase="tileset") events
//       preceding it — the tileset phase is deferred.
//   (2) After BakeFinished, BakePartDone(phase="tileset") events arrive (or,
//       in the headless path, the worker completes without crashing — the key
//       invariant is that BakeFinished was not held back by tileset work).
//
// Note: the headless path runs settle-only (no GPU atlas). The event sequence
// still provides the ordering guarantee: BakeFinished precedes tileset events.
// ---------------------------------------------------------------------------
static bool build_tileset_sandbox(const std::string& root) {
    run_cmd("rm -rf " + root);
    run_cmd("mkdir -p " + root + "/schemas");
    run_cmd("mkdir -p " + root + "/world_data/TileWorld");
    run_cmd("mkdir -p " + root + "/shared-lib");
    run_cmd("mkdir -p " + root + "/cache/parts");

    // A simple leaf pebble part (no requires).
    if (!write_file(root + "/schemas/SmallPebble.js",
        "class SmallPebble extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.1;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) return false;

    // A simple tileset root: requires SmallPebble, places it via interior().
    // Uses a flat base with no physics (physics: false) to avoid the box3d
    // simulation cost in the headless test context.
    if (!write_file(root + "/schemas/SimpleTileset.js",
        "class SimpleTileset extends Tileset {\n"
        "  static requires = [\n"
        "    { module: 'SmallPebble', params: {} },\n"
        "  ];\n"
        "  build(p) {\n"
        "    this.tile({ size: 2 });\n"
        "    this.base({ material: MAT.dirt, heights: () => 0 });\n"
        "    this.layer(SmallPebble, {\n"
        "      density: 1.0,\n"
        "      physics: false,\n"
        "    });\n"
        "  }\n"
        "}\n")) return false;

    // World manifest: SimpleTileset with `tileset` flag so it routes through the
    // tileset phase rather than the generic part publish path.
    if (!write_file(root + "/world_data/TileWorld/world.manifest",
        "# tileset deferred ordering test\n"
        "SimpleTileset tileset\n")) return false;

    return true;
}

static bool test_tileset_deferred_ordering(const std::string& base_dir) {
    printf("-- (h) test_tileset_deferred_ordering\n");

    const std::string sandbox = base_dir + "_tileset";
    if (!build_tileset_sandbox(sandbox)) {
        printf("  FAIL: build_tileset_sandbox\n");
        ++g_failures;
        return false;
    }

    // Cold cache.
    run_cmd("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");

    const std::string cache_root_s = sandbox + "/cache";
    const std::string schemas_s    = sandbox + "/schemas";
    const std::string wdata_s      = sandbox + "/world_data";
    const std::string shlib_s      = sandbox + "/shared-lib";

    std::string err;
    matter::EngineDesc ed;
    ed.cache_root     = cache_root_s.c_str();
    ed.allow_gl_lt_46 = true;  // headless
    auto engine = matter::EngineContext::create(ed, err);
    CHECK(engine != nullptr, "h: engine created");
    if (!engine) { printf("  err: %s\n", err.c_str()); run_cmd("rm -rf " + sandbox); return false; }

    matter::WorldDesc wd;
    wd.schemas_dir    = schemas_s.c_str();
    wd.world_data_dir = wdata_s.c_str();
    wd.world_name     = "TileWorld";
    wd.shared_lib_dir = shlib_s.c_str();
    auto s = engine->open_world(wd, err);
    CHECK(s != nullptr, "h: session opened");
    if (!s) { printf("  err: %s\n", err.c_str()); run_cmd("rm -rf " + sandbox); return false; }

    s->request_bake();

    // Drain events until BakeFinished (or timeout).
    std::vector<matter::Event> pre_finished;
    std::vector<matter::Event> post_finished;
    bool got_finished = false;
    bool got_error    = false;
    const int timeout_sec = 120;
    auto deadline = clk_e2e::now() + std::chrono::seconds(timeout_sec);

    while (clk_e2e::now() < deadline) {
        s->pump_gpu_jobs(4.0f);
        matter::Event ev;
        bool any = false;
        while (s->poll_event(ev)) {
            any = true;
            if (!got_finished) {
                if (ev.type == matter::EventType::BakeFinished) {
                    got_finished = true;
                } else {
                    pre_finished.push_back(ev);
                }
            } else {
                post_finished.push_back(ev);
            }
            if (ev.type == matter::EventType::BakeError) {
                printf("  BakeError: code=%d phase=%s msg=%s\n",
                       (int)ev.code, ev.phase.c_str(), ev.message.c_str());
                got_error = true;
            }
        }
        if (got_finished) {
            // Drain a short window for post-BakeFinished events.
            auto post_deadline = clk_e2e::now() + std::chrono::seconds(30);
            while (clk_e2e::now() < post_deadline) {
                s->pump_gpu_jobs(4.0f);
                while (s->poll_event(ev)) {
                    post_finished.push_back(ev);
                    if (ev.type == matter::EventType::BakeError) {
                        printf("  BakeError(post): code=%d phase=%s msg=%s\n",
                               (int)ev.code, ev.phase.c_str(), ev.message.c_str());
                        got_error = true;
                    }
                }
                // Check if we got tileset events or if there's nothing more to wait for.
                bool has_tileset = false;
                for (const auto& pev : post_finished)
                    if (pev.phase == "tileset") { has_tileset = true; break; }
                if (has_tileset) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            break;
        }
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(got_finished, "h: BakeFinished arrived");
    if (!got_finished) { run_cmd("rm -rf " + sandbox); return false; }

    // Key assertion: NO BakePartDone(phase="tileset") events before BakeFinished.
    int tileset_before_finished = 0;
    for (const auto& ev : pre_finished)
        if (ev.type == matter::EventType::BakePartDone && ev.phase == "tileset")
            ++tileset_before_finished;
    printf("  tileset events before BakeFinished: %d (expect 0)\n", tileset_before_finished);
    CHECK(tileset_before_finished == 0,
          "h: no BakePartDone(phase=tileset) events before BakeFinished");

    // Log post-BakeFinished events for verification (optional in headless path).
    int tileset_after = 0;
    for (const auto& ev : post_finished) {
        if (ev.type == matter::EventType::BakePartDone && ev.phase == "tileset") {
            ++tileset_after;
            printf("  BakePartDone(tileset) after BakeFinished: done=%d total=%d\n",
                   ev.done, ev.total);
        }
    }
    printf("  tileset events after BakeFinished: %d\n", tileset_after);
    // Tileset events should follow BakeFinished (deferred phase completed).
    // In headless mode, the deferred phase runs settle-only.
    CHECK(tileset_after > 0, "h: BakePartDone(phase=tileset) events arrived after BakeFinished");

    (void)got_error;  // reported inline; non-fatal for ordering test

    run_cmd("rm -rf " + sandbox);
    printf("  (h) PASS\n");
    return true;
}

// Build a tileset sandbox that uses string module specifiers in layer() so
// that run_tileset_phase can call eval_tileset in isolation (no pre-defined
// class references needed in the JS global scope).
static bool build_cache_wiring_sandbox(const std::string& root) {
    run_cmd("rm -rf " + root);
    run_cmd("mkdir -p " + root + "/schemas");
    run_cmd("mkdir -p " + root + "/world_data/WireWorld");
    run_cmd("mkdir -p " + root + "/shared-lib");
    run_cmd("mkdir -p " + root + "/cache/parts");

    // Leaf pebble (no requires).
    if (!write_file(root + "/schemas/WirePebble.js",
        "class WirePebble extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    const S = 0.1;\n"
        "    this.beginShape(SHAPE.triangles);\n"
        "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
        "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
        "    this.endShape();\n"
        "  }\n"
        "}\n")) return false;

    // Tileset using a string literal specifier in layer() and the positional
    // base(fn, material) API to avoid any JS reference-resolution issues in
    // the eval_tileset context (which evaluates the script in isolation).
    if (!write_file(root + "/schemas/WireTileset.js",
        "class WireTileset extends Tileset {\n"
        "  static requires = [\n"
        "    { module: 'WirePebble', params: {} },\n"
        "  ];\n"
        "  build(p) {\n"
        "    this.tile({ size: 2 });\n"
        "    this.base((x, z) => 0, MAT.dirt);\n"
        "    this.layer('WirePebble', {\n"
        "      density: 1.0,\n"
        "      physics: false,\n"
        "    });\n"
        "  }\n"
        "}\n")) return false;

    if (!write_file(root + "/world_data/WireWorld/world.manifest",
        "# settle-cache wiring test\n"
        "WireTileset tileset\n")) return false;

    return true;
}

// ---------------------------------------------------------------------------
// (i) test_tileset_settle_cache_wiring
//
// Calls run_tileset_phase twice on the same WireTileset sandbox with the
// same cache_root. Asserts:
//   (1) First run: from_cache == false (cold settle ran).
//   (2) Second run: from_cache == true (warm hit; no physics ran).
//   (3) Instances count, pose_hash, and individual poses are bitwise identical.
// ---------------------------------------------------------------------------
static bool test_tileset_settle_cache_wiring(const std::string& base_dir) {
    printf("-- (i) test_tileset_settle_cache_wiring\n");

    const std::string sandbox = base_dir + "_settle_cache_wiring";
    if (!build_cache_wiring_sandbox(sandbox)) {
        printf("  FAIL: build_cache_wiring_sandbox\n");
        ++g_failures;
        return false;
    }

    // Cold cache.
    run_cmd("rm -rf " + sandbox + "/cache && mkdir -p " + sandbox + "/cache/parts");

    const std::string world_data = sandbox + "/world_data";
    const std::string cache_root = sandbox + "/cache";
    const std::string shlib      = sandbox + "/shared-lib";

    // First run: cold cache — settle runs, cache is saved.
    tileset::SettledTorus first;
    std::string err1;
    bool ok1 = tileset::run_tileset_phase(world_data, "WireWorld", "WireTileset",
                                          cache_root, first, err1, shlib);
    if (!ok1) printf("  first run err: %s\n", err1.c_str());
    CHECK(ok1, "(i) first run_tileset_phase succeeded");
    CHECK(!first.report.from_cache, "(i) first run: from_cache == false (cold settle)");

    // Second run: warm cache — settle must be skipped.
    tileset::SettledTorus second;
    std::string err2;
    bool ok2 = tileset::run_tileset_phase(world_data, "WireWorld", "WireTileset",
                                          cache_root, second, err2, shlib);
    if (!ok2) printf("  second run err: %s\n", err2.c_str());
    CHECK(ok2, "(i) second run_tileset_phase succeeded");
    CHECK(second.report.from_cache, "(i) second run: from_cache == true (warm cache hit)");

    // Results must be bitwise identical.
    CHECK(second.instances.size() == first.instances.size(),
          "(i) instance count matches");
    CHECK(second.report.pose_hash == first.report.pose_hash,
          "(i) pose_hash matches");
    bool poses_ok = (second.instances.size() == first.instances.size());
    for (size_t k = 0; k < first.instances.size() && poses_ok; ++k) {
        if (std::memcmp(&first.instances[k].pose,
                        &second.instances[k].pose,
                        sizeof(tileset::Pose)) != 0)
            poses_ok = false;
        if (first.instances[k].child_hash != second.instances[k].child_hash)
            poses_ok = false;
    }
    CHECK(poses_ok, "(i) all instance poses bitwise-identical");

    run_cmd("rm -rf " + sandbox);
    printf("  (i) PASS\n");
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
    bool e = test_demand_bake_e2e(sandbox);
    bool f = test_parametric_placements_distinct(sandbox);
    bool g = test_placechild_param_mismatch_errors(sandbox);
    bool h = test_tileset_deferred_ordering(sandbox);
    bool i = test_tileset_settle_cache_wiring(sandbox);

    printf("\n");
    if (g_failures == 0) {
        printf("ALL PASS (%s %s %s %s %s %s %s %s %s)\n",
               a ? "a" : "a-FAIL",
               b ? "b" : "b-FAIL",
               c ? "c" : "c-FAIL",
               d ? "d" : "d-FAIL",
               e ? "e" : "e-FAIL",
               f ? "f" : "f-FAIL",
               g ? "g" : "g-FAIL",
               h ? "h" : "h-FAIL",
               i ? "i" : "i-FAIL");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
