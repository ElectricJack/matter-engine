// SP-3 Task 12 end-to-end check (gated on SP-2 via -DMATTER_HAVE_SCRIPT_HOST).
//
// Writes real .js part schemas + a world manifest, runs PartGraph::install
// through the REAL FileModuleResolver + HostBaker adapters (which drive the SP-2
// ScriptHost: eval_requires for child discovery, resolve_hash for cache keys,
// bake_source -> save_v2 for the actual .part files), then asserts the expected
// parts/<hash>.part files exist on disk and a second install bakes 0 (all hits).
//
// bake_source writes to the RELATIVE path cache_path_resolved() = "parts/<hash>.part",
// so most tests chdir()s into a fresh temp dir and point HostBaker at "parts" so
// both the writer and the cache check agree on the location (plan precondition);
// the test_foreign_cwd_install variant intentionally avoids chdir to prove absolute-path independence.

#include "part_graph.h"        // includes script_host.h under the guard
#include "part_asset_v2.h"     // cache_path_resolved
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"

static bool file_exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

static void write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary);
    f << body;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// SP-3 Task 7: prove the graph-driven placement path. A parent that declares a
// child via `static requires` AND places it via placeChild() must, after a real
// PartGraph::install (FileModuleResolver -> HostBaker -> ScriptHost), produce a
// parent .part carrying exactly one ChildInstance row pointing at the leaf, at the
// placed transform (y=1). The resolved hash is recomputed here by folding the leaf
// hash exactly as the graph did, to locate the parent's .part for reload.
//
// NOTE: the host's classic bake path (HostBaker sets no shared-lib root) evaluates
// the part as a GLOBAL script, so a bare `class X extends Part` declaration is
// required — `export default class` would throw under JS_EVAL_TYPE_GLOBAL. This
// mirrors script_host_tests.cpp's place-child round-trip.
static void test_install_with_placement() {
    namespace pg = part_graph;

    const std::string root = "/tmp/me3_graph_place";
    system(("rm -rf " + root).c_str());
    const std::string schemas = root + "/schemas";
    system(("mkdir -p " + schemas + " " + root + "/parts").c_str());

    write_file(schemas + "/LeafX.js",
        "class LeafX extends Part {"
        "  build(p){ this.beginVoxels(0.1); this.fill(MAT.leaf);"
        "            this.sphere([0,0,0],0.1); this.endVoxels(); } }");
    write_file(schemas + "/TreeX.js",
        "class TreeX extends Part {"
        "  static requires = [{ module: 'LeafX' }];"
        "  build(p){"
        "    this.beginVoxels(0.2); this.fill(MAT.bark);"
        "    this.box([0,0,0],[0.3,0.3,0.3]); this.endVoxels();"
        "    this.pushMatrix(); this.translate(0,1,0); this.placeChild('LeafX'); this.popMatrix();"
        "  } }");

    // chdir so bake_source's relative "parts/<hash>.part" lands under <root>/parts;
    // HostBaker(".") joins "." + "/" + "parts/<hash>.part" for its cache check.
    char prevcwd[4096]; if (!getcwd(prevcwd, sizeof prevcwd)) prevcwd[0] = '\0';
    CHECK(chdir(root.c_str()) == 0, "chdir into placement sandbox");

    script_host::ScriptHost host;
    pg::FileModuleResolver resolver(host, "schemas");
    pg::HostBaker baker(host, ".");
    pg::PartGraph graph(resolver, baker);

    pg::InstallResult ir = graph.install({ pg::ChildRequest{ "TreeX", pg::Params{} } });
    CHECK(ir.ok, "install of TreeX with a required+placed LeafX succeeds");
    if (!ir.ok) printf("  install error: %s\n", ir.error.c_str());

    // Recompute the resolved hashes the way the graph did: leaf with no children,
    // tree with the leaf hash folded in (children affect the resolved hash).
    uint64_t leaf_hash = host.resolve_hash(read_file("schemas/LeafX.js"), "{}");
    uint64_t kids[1] = { leaf_hash };
    uint64_t tree_hash = host.resolve_hash(read_file("schemas/TreeX.js"), "{}", kids, 1);

    BLASManager blas; TLASManager tlas(64);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    bool loaded = part_asset::load_v2(part_asset::cache_path_resolved(tree_hash),
                                      tree_hash, blas, tlas, children, lods);
    CHECK(loaded, "TreeX .part reloads");
    CHECK(children.size() == 1, "TreeX recorded one LeafX instance");
    if (children.size() == 1) {
        CHECK(children[0].child_resolved_hash == leaf_hash, "instance points at LeafX");
        // row-major translation lives in transform[3],[7],[11]; y is [7].
        CHECK(children[0].transform[7] == 1.0f, "instance placed at y=1");
    }

    if (prevcwd[0]) (void)chdir(prevcwd);
    system(("rm -rf " + root).c_str());
}

// SP-3 Tasks 8/9: bake the REAL demo Tree (an L-system that places instanced Leaf
// children) through the graph and assert its .part carries Leaf instances. Unlike
// the synthetic tests above, this installs the on-disk demo schemas dir, so it must
// also point the host at the real shared-lib (Tree.js `import`s lsystem). The host's
// resolve_hash/bake_source fold the imported module source into the hash, so the
// Tree hash recomputed here MUST use the SAME host (shared-lib root set) that
// installed it, and MUST fold the Leaf child hash (kids,1) exactly as the graph did.
//
// Paths: the demo schemas/shared-lib are repo-relative to the tests dir
// (../examples/world_demo/schemas, ../shared-lib). The caller passes ABSOLUTE paths
// resolved from the original cwd before any chdir, so resolution survives a chdir.
// The real demo tree is a faithful port of MatterEngine2's three-mode system:
//   Tree  -> voxel sphere-sweep trunk + instanced TreeBranch twigs
//   TreeBranch -> mesh line-tube twig + instanced Leaf blades
//   Leaf  -> bezier triangle-fan blade (mesh)
// This installs the on-disk schemas through the real graph and walks the whole
// chain Tree -> TreeBranch -> Leaf, asserting each level reloads with the right
// child structure and that the mesh levels actually registered geometry.
static void test_demo_tree_has_leaves(const std::string& schemas,
                                      const std::string& sharedlib) {
    namespace pg = part_graph;

    script_host::ScriptHost host;
    host.set_shared_lib_root(sharedlib);
    pg::FileModuleResolver resolver(host, schemas);
    pg::HostBaker baker(host, ".");
    pg::PartGraph graph(resolver, baker);

    std::vector<pg::ChildRequest> roots = { pg::ChildRequest{ "Tree", pg::Params{} } };
    pg::InstallResult ir = graph.install(roots);
    CHECK(ir.ok, "demo Tree installs");
    if (!ir.ok) printf("  install error: %s\n", ir.error.c_str());
    CHECK(ir.root_hashes.size() == 1, "install returned the Tree root hash");
    if (ir.root_hashes.empty()) return;

    // TreeBranch requires four Leaf variants ({shade:0..3}), each a distinct
    // content hash; placeChild('Leaf',{shade:N}) selects among them. Recompute the
    // four expected variant hashes the same way the graph bakes them (Leaf has no
    // children, params fold into its own hash). The branch hash is NOT recomputed
    // here (it folds all four variant hashes in graph order); instead it is read
    // back from the Tree's own child placements below.
    const std::string leaf_src = read_file(schemas + "/Leaf.js");
    uint64_t leaf_shade[4];
    for (int s = 0; s < 4; ++s) {
        char pj[24]; std::snprintf(pj, sizeof pj, "{\"shade\":%d}", s);
        leaf_shade[s] = host.resolve_hash(leaf_src, pj);
    }
    auto is_leaf_variant = [&](uint64_t h) {
        for (int s = 0; s < 4; ++s) if (leaf_shade[s] == h) return true;
        return false;
    };
    uint64_t tree_hash   = ir.root_hashes[0];
    // Trunk is a childless voxel leaf, so its hash is a plain source+params hash.
    uint64_t trunk_hash  = host.resolve_hash(read_file(schemas + "/Trunk.js"), "{}");

    // --- Tree: geometry-less assembler placing one Trunk + N TreeBranch ---
    BLASManager t_blas; TLASManager t_tlas(256);
    std::vector<part_asset::ChildInstance> t_children;
    part_asset::LodLevels t_lods;
    bool t_loaded = part_asset::load_v2(part_asset::cache_path_resolved(tree_hash),
                                        tree_hash, t_blas, t_tlas, t_children, t_lods);
    CHECK(t_loaded, "demo Tree .part reloads");
    CHECK(!t_children.empty(), "demo Tree placed children");
    // Tree requires [Trunk, TreeBranch]; sort placements by which part they name.
    // The branch hash folds all four Leaf variants, so it can't be recomputed here
    // -- read it back from the (non-trunk) placements instead.
    size_t trunk_placements = 0;
    uint64_t branch_hash = 0;
    for (const auto& c : t_children) {
        if (c.child_resolved_hash == trunk_hash) ++trunk_placements;
        else branch_hash = c.child_resolved_hash;   // all branches share one hash
    }
    CHECK(trunk_placements >= 1, "Tree placed the Trunk");
    CHECK(branch_hash != 0, "Tree placed at least one TreeBranch");
    printf("  demo Tree placed %zu child instance(s) (%zu trunk)\n",
           t_children.size(), trunk_placements);

    // --- Trunk: voxel geometry, no children ---
    BLASManager k_blas; TLASManager k_tlas(64);
    std::vector<part_asset::ChildInstance> k_children;
    part_asset::LodLevels k_lods;
    bool k_loaded = part_asset::load_v2(part_asset::cache_path_resolved(trunk_hash),
                                        trunk_hash, k_blas, k_tlas, k_children, k_lods);
    CHECK(k_loaded, "demo Trunk .part reloads");
    CHECK(k_blas.get_unique_blas_count() >= 1, "Trunk registered voxel geometry");
    { size_t tt = 0; for (const auto& e : k_blas.get_entries()) tt += e->triangles.size();
      // Geometry budget guard: the trunk voxel sweep once ballooned to >130k tris
      // (multi-second synchronous LOD bake per part at viewer startup). Keep it sane.
      CHECK(tt < 400000, "Trunk triangle count within budget"); }

    // --- TreeBranch: mesh twig tubes + Leaf instances ---
    BLASManager b_blas; TLASManager b_tlas(256);
    std::vector<part_asset::ChildInstance> b_children;
    part_asset::LodLevels b_lods;
    bool b_loaded = part_asset::load_v2(part_asset::cache_path_resolved(branch_hash),
                                        branch_hash, b_blas, b_tlas, b_children, b_lods);
    CHECK(b_loaded, "demo TreeBranch .part reloads");
    CHECK(b_blas.get_unique_blas_count() >= 1, "TreeBranch registered twig tube mesh");
    CHECK(!b_children.empty(), "demo TreeBranch placed at least one Leaf");
    { size_t bt = 0; for (const auto& e : b_blas.get_entries()) bt += e->triangles.size();
      // A single twig once baked to 513k tris (~20s LOD bake) via line()'s stacked
      // UV-sphere tubes. Guard the budget so it can't silently regress again.
      CHECK(bt < 150000, "TreeBranch twig triangle count within budget"); }
    printf("  demo TreeBranch placed %zu Leaf instance(s)\n", b_children.size());
    // Every placed Leaf must resolve to one of the four real {shade:N} variant
    // hashes -- this is the regression guard for parametric-child resolution.
    bool all_variants = true;
    for (const auto& c : b_children)
        if (!is_leaf_variant(c.child_resolved_hash)) all_variants = false;
    CHECK(all_variants, "every placed Leaf is one of the four real shade variants");

    // --- Leaf: bezier triangle-fan blade (mesh, no children) ---
    BLASManager l_blas; TLASManager l_tlas(64);
    std::vector<part_asset::ChildInstance> l_children;
    part_asset::LodLevels l_lods;
    bool l_loaded = part_asset::load_v2(part_asset::cache_path_resolved(leaf_shade[0]),
                                        leaf_shade[0], l_blas, l_tlas, l_children, l_lods);
    CHECK(l_loaded, "demo Leaf .part reloads");
    CHECK(l_blas.get_unique_blas_count() >= 1, "Leaf registered blade triangle mesh");
    CHECK(l_children.empty(), "Leaf is a mesh leaf with no children");
}

// Task 3 (Phase B): foreign-cwd guard.
// Proves that PartGraph::install with an ABSOLUTE parts_dir writes artifacts
// under that absolute path regardless of the process cwd.  Before the fix,
// bake_source's relative "parts/<hash>.part" was written to cwd (which was
// abs_cache_root only because LocalProvider chdir'd there).
// Test procedure:
//   1. Prepare a fresh absolute sandbox.
//   2. chdir("/") — a foreign cwd with no "parts/" subdir.
//   3. Run install with HostBaker(abs_cache_root).
//   4. Assert artifacts exist under abs_cache_root/parts/, NOT under /parts/.
//   5. Restore original cwd so subsequent tests run from the correct dir.
static void test_foreign_cwd_install() {
    namespace pg = part_graph;

    // Save original cwd so we can restore it at the end of this test.
    char orig_cwd[4096];
    if (!getcwd(orig_cwd, sizeof orig_cwd)) orig_cwd[0] = '\0';

    const std::string root = "/tmp/me3_foreign_cwd";
    system(("rm -rf " + root).c_str());
    const std::string schemas = root + "/schemas";
    system(("mkdir -p " + schemas + " " + root + "/parts").c_str());

    write_file(schemas + "/ForeignBox.js",
        "class ForeignBox extends Part {\n"
        "  static params = {};\n"
        "  build(p) { this.fill(1);\n"
        "    this.beginShape(SHAPE.strip);\n"
        "    this.vertex(0,0,0); this.vertex(1,0,0); this.vertex(0,1,0);\n"
        "    this.endShape(); }\n"
        "}\n");

    // chdir to "/" — a completely foreign directory that has no parts/ subdir.
    CHECK(chdir("/") == 0, "foreign_cwd: chdir(\"/\") succeeds");

    script_host::ScriptHost host;
    // HostBaker receives the ABSOLUTE parts parent dir; after the fix, bake_source
    // must write to abs_cache_root/parts/<hash>.part rather than ./parts/<hash>.part.
    pg::FileModuleResolver resolver(host, schemas);
    pg::HostBaker baker(host, root);
    pg::PartGraph graph(resolver, baker);

    pg::InstallResult ir = graph.install({ pg::ChildRequest{"ForeignBox", pg::Params{}} });
    CHECK(ir.ok, "foreign_cwd: install with absolute parts_dir succeeds");
    if (!ir.ok) printf("  foreign_cwd install error: %s\n", ir.error.c_str());

    if (ir.ok && ir.root_hashes.size() == 1) {
        // Artifact must exist under the absolute sandbox, not under /parts/.
        const std::string abs_part = root + "/" + part_asset::cache_path_resolved(ir.root_hashes[0]);
        CHECK(file_exists(abs_part), "foreign_cwd: .part exists under absolute cache_root");
    }

    // Restore cwd so subsequent tests (lod_sidecar etc.) work correctly.
    if (orig_cwd[0]) (void)chdir(orig_cwd);
    system(("rm -rf " + root).c_str());
    printf("  test_foreign_cwd_install done\n");
}

// SP-3 Task 13: budget-variant baking + .lods sidecar.
// An opted-in childless schema (static lodBudgets = [1.0, 0.5]) in a fresh
// temp sandbox is installed through the REAL FileModuleResolver + HostBaker.
// After install:
//   - parts/<root_hash>.lods exists with anchor_size, 2 budget/hash lines
//   - budgets[0]==1.0 maps to the root hash itself (no re-bake)
//   - budgets[1]==0.5 maps to a distinct variant .part on disk
// Re-install with everything cached: sidecar untouched, ir.baked empty.
// A schema WITHOUT lodBudgets gets no sidecar.
static void test_lod_variant_sidecar() {
    namespace pg = part_graph;

    const std::string root = "/tmp/me3_lod_sidecar";
    system(("rm -rf " + root).c_str());
    const std::string schemas = root + "/schemas";
    system(("mkdir -p " + schemas + " " + root + "/parts").c_str());

    // BudgetGrass: opted in, childless. build() emits n=ceil(lodBudget*4) strips.
    write_file(schemas + "/BudgetGrass.js",
        "class BudgetGrass extends Part {\n"
        "  static params = { seed: 0, lodBudget: 1.0 };\n"
        "  static lodBudgets = [1.0, 0.5];\n"
        "  static lodAnchorSize = 0.5;\n"
        "  build(p) {\n"
        "    const n = Math.max(1, Math.ceil(p.lodBudget * 4));\n"
        "    this.fill(1);\n"
        "    for (let i = 0; i < n; ++i) {\n"
        "      this.beginShape(SHAPE.strip);\n"
        "      this.vertex(i, 0, 0); this.vertex(i + 1, 0, 0); this.vertex(i, 1, 0);\n"
        "      this.endShape();\n"
        "    }\n"
        "  }\n"
        "}\n");

    // PlainBox: no lodBudgets — should get NO sidecar.
    write_file(schemas + "/PlainBox.js",
        "class PlainBox extends Part { static params = {};\n"
        "  build(p) { this.fill(1); this.beginShape(SHAPE.strip);\n"
        "  this.vertex(0,0,0); this.vertex(1,0,0); this.vertex(0,1,0);\n"
        "  this.endShape(); } }\n");

    char prevcwd[4096]; if (!getcwd(prevcwd, sizeof prevcwd)) prevcwd[0] = '\0';
    CHECK(chdir(root.c_str()) == 0, "lod_sidecar: chdir into sandbox");

    script_host::ScriptHost host;
    pg::FileModuleResolver resolver(host, "schemas");
    pg::HostBaker baker(host, ".");
    pg::PartGraph graph(resolver, baker);

    // --- BudgetGrass: first install ---
    pg::InstallResult ir = graph.install({ pg::ChildRequest{"BudgetGrass", pg::Params{}} });
    CHECK(ir.ok, "lod_sidecar: BudgetGrass install ok");
    if (!ir.ok) printf("  install error: %s\n", ir.error.c_str());
    CHECK(ir.root_hashes.size() == 1, "lod_sidecar: one root hash");

    if (ir.ok && ir.root_hashes.size() == 1) {
        uint64_t root_hash = ir.root_hashes[0];
        std::string sidecar_path = std::string(".") + "/" + part_asset::cache_path_lods(root_hash);

        part_asset::LodVariants v;
        CHECK(part_asset::load_lod_sidecar(sidecar_path, v), "lod_sidecar: sidecar loads");
        CHECK(v.anchor_size == 0.5, "lod_sidecar: anchor_size == 0.5");
        CHECK(v.budgets.size() == 2, "lod_sidecar: 2 budget entries");
        CHECK(v.hashes.size() == 2, "lod_sidecar: 2 hash entries");
        if (v.budgets.size() == 2 && v.hashes.size() == 2) {
            CHECK(v.hashes[0] == root_hash, "lod_sidecar: budget 1.0 == main bake hash");
            CHECK(v.hashes[1] != root_hash, "lod_sidecar: budget 0.5 is a distinct variant");
            // The variant .part must exist on disk.
            std::ifstream in(part_asset::cache_path_resolved(v.hashes[1]), std::ios::binary);
            CHECK(in.good(), "lod_sidecar: variant .part exists on disk");
        }

        // Re-install: everything cached, sidecar untouched (no re-bake).
        pg::InstallResult ir2 = graph.install({ pg::ChildRequest{"BudgetGrass", pg::Params{}} });
        CHECK(ir2.ok, "lod_sidecar: second install ok");
        CHECK(ir2.baked.empty(), "lod_sidecar: second install bakes nothing");
    }

    // --- PlainBox: no lodBudgets => no sidecar ---
    pg::InstallResult ir3 = graph.install({ pg::ChildRequest{"PlainBox", pg::Params{}} });
    CHECK(ir3.ok, "lod_sidecar: PlainBox install ok");
    if (ir3.ok && ir3.root_hashes.size() == 1) {
        std::string nos = std::string(".") + "/" + part_asset::cache_path_lods(ir3.root_hashes[0]);
        std::ifstream nosin(nos);
        CHECK(!nosin.good(), "lod_sidecar: PlainBox has no sidecar");
    }

    if (prevcwd[0]) (void)chdir(prevcwd);
    system(("rm -rf " + root).c_str());
    printf("  test_lod_variant_sidecar OK\n");
}

static void test_stale_material_cache_migrates() {
    using namespace part_graph;
    namespace fs = std::filesystem;

    const fs::path root = fs::absolute("part_graph_cache_migration_test");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "schemas", ec);
    fs::create_directories(root / "parts", ec);
    CHECK(!ec, "cache migration sandbox created");
    if (ec) return;

    write_file((root / "schemas" / "CacheBox.js").string(),
        "class CacheBox extends Part {"
        " build(p){ this.beginVoxels(0.25); this.fill(MAT.stone);"
        " this.box([0,0,0],[0.5,0.5,0.5]); this.endVoxels(); } }");

    script_host::ScriptHost host;
    FileModuleResolver resolver(host, (root / "schemas").string());
    HostBaker baker(host, root.string());
    PartGraph graph(resolver, baker);

    InstallResult first = graph.install({ChildRequest{"CacheBox", Params{}}});
    CHECK(first.ok && first.baked.size() == 1,
          "cache migration fixture bakes once");
    if (!first.ok || first.baked.size() != 1) {
        fs::remove_all(root, ec);
        return;
    }

    const uint64_t hash = first.baked[0];
    const std::string path =
        (root / part_asset::cache_path_resolved(hash)).string();
    std::string stale = read_file(path);
    CHECK(stale.size() >= 44, "stale-schema fixture has common body");
    if (stale.size() >= 44) {
        const uint32_t prior_schema = MaterialRegistrySchemaVersion() - 1u;
        std::memcpy(stale.data() + 40, &prior_schema, sizeof(prior_schema));
        const uint64_t body_hash =
            part_asset::fnv1a64(stale.data() + 40, stale.size() - 40);
        std::memcpy(stale.data() + 32, &body_hash, sizeof(body_hash));
        write_file(path, stale);

        CHECK(!baker.cached(hash),
              "HostBaker rejects valid-header prior-schema .part");
        InstallResult migrated =
            graph.install({ChildRequest{"CacheBox", Params{}}});
        CHECK(migrated.ok && migrated.baked.size() == 1 &&
                  migrated.baked[0] == hash,
              "stale-schema .part automatically rebakes");

        BLASManager blas; TLASManager tlas(16);
        std::vector<part_asset::ChildInstance> children;
        part_asset::LodLevels lods;
        CHECK(part_asset::load_v2(path, hash, blas, tlas, children, lods),
              "rebaked .part loads successfully");

        InstallResult warm =
            graph.install({ChildRequest{"CacheBox", Params{}}});
        CHECK(warm.ok && warm.baked.empty() && warm.hits == 1,
              "second pass after stale-schema migration is warm");
    }
    fs::remove_all(root, ec);
}

int main(int argc, char** argv) {
    using namespace part_graph;

    test_stale_material_cache_migrates();
    if (argc == 2 && std::strcmp(argv[1], "--cache-migration-only") == 0)
        return g_failures == 0 ? 0 : 1;

    // Resolve the demo schemas + shared-lib to ABSOLUTE paths NOW, before any test
    // chdir()s into a sandbox, so test_demo_tree_has_leaves can find the real files
    // regardless of cwd. (They are repo-relative to the tests dir we start in.)
    std::string demo_schemas, demo_sharedlib;
    { char abs[4096];
      if (realpath("../examples/world_demo/schemas", abs)) demo_schemas = abs;
      if (realpath("../shared-lib", abs)) demo_sharedlib = abs; }

    // Fresh sandbox so parts/<hash>.part and the schemas live in a known place.
    const std::string root = "/tmp/me3_graph_integration";
    system(("rm -rf " + root).c_str());
    const std::string schemas = root + "/schemas";
    const std::string parts   = root + "/parts";   // == <root>/parts; we chdir to <root>
    system(("mkdir -p " + schemas + " " + parts).c_str());

    // A two-part graph: Wall (leaf, no children) and Tower (parent that requires
    // two Wall instances with identical params -> dedup to ONE Wall artifact).
    write_file(schemas + "/Wall.js",
        "class Wall extends Part {\n"
        "  static params = { h: 1.0 };\n"
        "  build(p) { this.beginVoxels(0.25); this.fill(MAT.stone);\n"
        "             this.box([0,0,0],[0.5,p.h,0.5]); this.endVoxels(); }\n"
        "}\n");
    write_file(schemas + "/Tower.js",
        "class Tower extends Part {\n"
        "  static params = {};\n"
        "  static requires(p) {\n"
        "    return [ { module: 'Wall', params: { h: 1.0 } },\n"
        "             { module: 'Wall', params: { h: 1.0 } } ];\n"
        "  }\n"
        "  build(p) { this.beginVoxels(0.25); this.fill(MAT.stone);\n"
        "             this.box([0,0,0],[0.5,2.0,0.5]); this.endVoxels(); }\n"
        "}\n");

    // chdir so bake_source's relative "parts/<hash>.part" lands in <root>/parts.
    char prevcwd[4096]; if (!getcwd(prevcwd, sizeof prevcwd)) prevcwd[0] = '\0';
    CHECK(chdir(root.c_str()) == 0, "chdir into sandbox");

    script_host::ScriptHost host;
    FileModuleResolver resolver(host, "schemas");
    // cache_path_resolved() already yields "parts/<hash>.part" (relative to cwd),
    // and bake_source writes there; HostBaker::cached joins parts_dir_ + "/" +
    // cache_path_resolved(), so parts_dir_ is the PARENT of parts/ (here, ".").
    HostBaker baker(host, ".");
    PartGraph graph(resolver, baker);

    // First install: Tower + (deduped) Wall => 2 artifacts baked, 0 hits.
    InstallResult r1 = graph.install({ ChildRequest{"Tower", Params{}} });
    CHECK(r1.ok, "first install succeeds end-to-end");
    if (!r1.ok) printf("  install error: %s\n", r1.error.c_str());
    CHECK(r1.baked.size() == 2, "first install bakes exactly 2 artifacts (Tower + 1 deduped Wall)");
    CHECK(r1.hits == 0, "first install has no cache hits");

    // The .part files for each baked hash must now be on disk.
    for (uint64_t h : r1.baked) {
        std::string path = part_asset::cache_path_resolved(h); // "parts/<hash>.part"
        CHECK(file_exists(path), "baked .part exists on disk");
    }

    // Second install: everything is cached now => 0 bakes, all hits.
    InstallResult r2 = graph.install({ ChildRequest{"Tower", Params{}} });
    CHECK(r2.ok, "second install succeeds");
    CHECK(r2.baked.empty(), "second install bakes nothing (incremental cache hit)");
    CHECK(r2.hits == (int)r1.baked.size(), "second install reports a hit for every prior artifact");

    if (prevcwd[0]) (void)chdir(prevcwd);
    system(("rm -rf " + root).c_str());

    // SP-3 Task 7: graph-driven placement (requires + placeChild) round-trip.
    test_install_with_placement();

    // SP-3 Tasks 8/9: the real demo Tree bakes Leaf instances through the graph.
    CHECK(!demo_schemas.empty() && !demo_sharedlib.empty(),
          "resolved demo schemas + shared-lib absolute paths");
    if (!demo_schemas.empty() && !demo_sharedlib.empty())
        test_demo_tree_has_leaves(demo_schemas, demo_sharedlib);

    // Task 3 (Phase B): foreign-cwd guard — install with absolute parts_dir works
    // regardless of process cwd.
    test_foreign_cwd_install();

    // SP-3 Task 13: budget-variant baking + .lods sidecar.
    test_lod_variant_sidecar();

    if (g_failures == 0) printf("All part_graph integration tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
