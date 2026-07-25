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
#include "bake_trace.h"        // Bake Lab §II.6: install-path trace shape
#include "bake_trace_names.h"
#include "matter/bake_observer.h"  // Bake Lab W3: per-rung bake observer seam
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

#ifdef _WIN32
// MinGW-w64 has no realpath(); _fullpath (stdlib.h) is the direct equivalent.
// Callers here all pass 4096-byte buffers (see main's `abs`).
static char* realpath(const char* path, char* resolved) {
    return _fullpath(resolved, path, 4096);
}
#endif

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
// (../../projects/world_demo/schemas, ../shared-lib). The caller passes ABSOLUTE paths
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

// W5 (Part Workbench, static lods) end-to-end: a schema exporting
// `static lods = [ {}, { params:{...} }, { exclude:[...] } ]` installed
// through the REAL FileModuleResolver + HostBaker + PartGraph::install
// (exercising HostBaker::bake_static_lods exactly as matter_engine.cpp's
// install pipeline calls it). Asserts:
//   - parts/<root_hash>.static_lods sidecar has 3 levels
//   - level 0 (no params/exclude) points at the root hash itself
//   - level 1 (params) points at a DISTINCT, on-disk .part
//   - level 2 (exclude) keeps the root hash but sets a non-zero exclude mask,
//     and the root .part's LMSK trailer reflects it (child excluded at level 2
//     only)
// A second install (fully cached) leaves the sidecar and .part untouched.
static void test_static_lods_end_to_end() {
    namespace pg = part_graph;

    const std::string root = "/tmp/me3_static_lods_e2e";
    system(("rm -rf " + root).c_str());
    const std::string schemas = root + "/schemas";
    system(("mkdir -p " + schemas + " " + root + "/parts").c_str());

    // Leaf: a childless part placed by Tree so `exclude` has something to drop.
    write_file(schemas + "/Leaf.js",
        "class Leaf extends Part {\n"
        "  build(p) { this.beginVoxels(0.1); this.fill(1);\n"
        "             this.sphere([0,0,0], 0.1); this.endVoxels(); }\n"
        "}\n");

    // Tree: opts into static lods. LOD1 regenerates with a different `n`
    // (params-driven fresh build); LOD2 excludes Leaf. The seeding rule is
    // exercised indirectly here (both levels bake successfully; the dedicated
    // unit test in script_host_tests.cpp asserts the rng-stream identity
    // directly at the ScriptHost layer).
    write_file(schemas + "/Tree.js",
        "class Tree extends Part {\n"
        "  static params = { seed: 3, n: 2 };\n"
        "  static requires(p) { return [ { module: 'Leaf', params: {} } ]; }\n"
        "  static lods = [\n"
        "    {},\n"
        "    { params: { n: 1 } },\n"
        "    { exclude: ['Leaf'] }\n"
        "  ];\n"
        "  build(p) {\n"
        "    this.beginVoxels(0.1); this.fill(1);\n"
        "    this.box([0,0,0],[0.2,0.2,0.2]); this.endVoxels();\n"
        "    this.placeChild('Leaf');\n"
        "  }\n"
        "}\n");

    char prevcwd[4096]; if (!getcwd(prevcwd, sizeof prevcwd)) prevcwd[0] = '\0';
    CHECK(chdir(root.c_str()) == 0, "static_lods_e2e: chdir into sandbox");

    script_host::ScriptHost host;
    pg::FileModuleResolver resolver(host, "schemas");
    pg::HostBaker baker(host, ".");
    pg::PartGraph graph(resolver, baker);

    pg::InstallResult ir = graph.install({ pg::ChildRequest{"Tree", pg::Params{}} });
    CHECK(ir.ok, "static_lods_e2e: install ok");
    if (!ir.ok) printf("  install error: %s\n", ir.error.c_str());
    CHECK(ir.root_hashes.size() == 1, "static_lods_e2e: one root hash");

    if (ir.ok && ir.root_hashes.size() == 1) {
        const uint64_t root_hash = ir.root_hashes[0];
        const std::string sidecar_path =
            std::string(".") + "/" + part_asset::cache_path_static_lods(root_hash);

        part_asset::StaticLodPlan plan;
        CHECK(part_asset::load_static_lod_plan(sidecar_path, plan),
              "static_lods_e2e: sidecar loads");
        CHECK(plan.level_hashes.size() == 3, "static_lods_e2e: 3 authored levels");
        if (plan.level_hashes.size() == 3) {
            CHECK(plan.level_hashes[0] == root_hash,
                  "static_lods_e2e: level 0 (no params/exclude) == root hash");
            CHECK(plan.level_hashes[1] != root_hash,
                  "static_lods_e2e: level 1 (params) is a distinct rebuild");
            std::ifstream in(part_asset::cache_path_resolved(plan.level_hashes[1]),
                             std::ios::binary);
            CHECK(in.good(), "static_lods_e2e: level 1's .part exists on disk");
            CHECK(plan.level_exclude_masks[1] == 0,
                  "static_lods_e2e: level 1 excludes nothing");
            CHECK(plan.level_hashes[2] == root_hash,
                  "static_lods_e2e: level 2 (exclude only) reuses root hash (decimated)");
            CHECK(plan.level_exclude_masks[2] != 0,
                  "static_lods_e2e: level 2 has a non-zero exclude mask");
        }

        // The root .part's LMSK trailer: the single child (Leaf) must be
        // ABSENT at level 2 and PRESENT at levels 0/1.
        BLASManager blas; TLASManager tlas(64);
        std::vector<part_asset::ChildInstance> kids;
        part_asset::LodLevels lods_out;
        std::vector<part_asset::VolumeEmitter> emitters;
        std::vector<uint32_t> mask;
        const std::string part_path = part_asset::cache_path_resolved(root_hash);
        CHECK(part_asset::load_v2(part_path, root_hash, blas, tlas, kids, lods_out,
                                  emitters, mask),
              "static_lods_e2e: root .part reloads with LMSK-aware load_v2");
        CHECK(kids.size() == 1, "static_lods_e2e: one Leaf placement");
        CHECK(mask.size() == 1, "static_lods_e2e: LMSK trailer present (one child)");
        if (mask.size() == 1) {
            CHECK((mask[0] & (1u << 0)) != 0, "static_lods_e2e: Leaf present at level 0");
            CHECK((mask[0] & (1u << 1)) != 0, "static_lods_e2e: Leaf present at level 1");
            CHECK((mask[0] & (1u << 2)) == 0, "static_lods_e2e: Leaf ABSENT at level 2 (excluded)");
        }

        // Re-install: everything cached, sidecar untouched, nothing re-baked.
        pg::InstallResult ir2 = graph.install({ pg::ChildRequest{"Tree", pg::Params{}} });
        CHECK(ir2.ok, "static_lods_e2e: second install ok");
        CHECK(ir2.baked.empty(), "static_lods_e2e: second install bakes nothing");
    }

    if (prevcwd[0]) (void)chdir(prevcwd);
    system(("rm -rf " + root).c_str());
    printf("  test_static_lods_end_to_end OK\n");
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

// Bake Lab spec §II.6 trace-shape test: a cold-cache PartGraph::install with a
// bake_trace::Collector current must record at least one part-bake span, with
// its phase children (fold + ctx at minimum), IN THAT COLLECTOR — guarding
// against part baking silently moving off the collector's (installing) thread,
// where the thread-local current() would no longer see it.
static void test_install_trace_shape() {
    using namespace part_graph;
    namespace fs = std::filesystem;

    const fs::path root = fs::absolute("part_graph_trace_shape_test");
    std::error_code ec;
    fs::remove_all(root, ec);          // cold cache: no prior parts/
    fs::create_directories(root / "schemas", ec);
    fs::create_directories(root / "parts", ec);
    CHECK(!ec, "trace-shape sandbox created");
    if (ec) return;

    write_file((root / "schemas" / "TraceBox.js").string(),
        "class TraceBox extends Part {"
        " build(p){ this.beginVoxels(0.25); this.fill(MAT.stone);"
        " this.box([0,0,0],[0.5,0.5,0.5]); this.endVoxels(); } }");

    script_host::ScriptHost host;
    FileModuleResolver resolver(host, (root / "schemas").string());
    HostBaker baker(host, root.string());
    PartGraph graph(resolver, baker);

    bake_trace::Collector col;
    bake_trace::set_current(&col);
    InstallResult r = graph.install({ChildRequest{"TraceBox", Params{}}});
    bake_trace::set_current(nullptr);

    CHECK(r.ok && r.baked.size() == 1 && r.hits == 0,
          "trace-shape: cold install actually baked");

    // Walk the snapshot for part-bake spans (install may nest them under
    // other spans; depth-first search keeps the test robust to added layers).
    bake_trace::Span snap = col.snapshot();
    int part_bakes = 0;
    bool have_fold = false, have_ctx = false, all_closed = true;
    std::vector<const bake_trace::Span*> stack{&snap};
    while (!stack.empty()) {
        const bake_trace::Span* s = stack.back();
        stack.pop_back();
        if (s->name && std::strcmp(s->name, bake_trace::kSpanPartBake) == 0) {
            ++part_bakes;
            if (s->end_ms == bake_trace::kOpenEndMs) all_closed = false;
            for (const bake_trace::Span& ph : s->children) {
                if (!ph.name) continue;
                if (std::strcmp(ph.name, bake_trace::kSpanFold) == 0) have_fold = true;
                if (std::strcmp(ph.name, bake_trace::kSpanCtx)  == 0) have_ctx  = true;
                if (ph.end_ms == bake_trace::kOpenEndMs) all_closed = false;
            }
        }
        for (const bake_trace::Span& c : s->children) stack.push_back(&c);
    }
    CHECK(part_bakes >= 1,
          "trace-shape: install recorded a part-bake span in the collector");
    CHECK(have_fold, "trace-shape: part-bake has a fold phase child");
    CHECK(have_ctx, "trace-shape: part-bake has a ctx phase child");
    CHECK(all_closed, "trace-shape: all part-bake/phase spans closed");

    fs::remove_all(root, ec);
}

// Bake Lab W3: BakeObserver::on_mesh_ready must fire exactly once, with a
// plausible (>0) triangle count, when a real part with actual geometry bakes
// through the REAL HostBaker/ScriptHost path (PartGraph::install ->
// HostBaker::bake -> ScriptHost::bake_source). The ladder-rung hook
// (on_rung_ready) is exercised separately in part_flatten_tests.cpp against
// lod_bake::bake_lods() directly -- bake_source itself only produces the
// full-resolution mesh; the LOD ladder is built later (PartStore's load-time
// re-bake), a different subsystem this suite doesn't drive. A NULL observer
// (every other test in this file) must remain completely unaffected -- that
// byte-identity guarantee is what the rest of this suite's untouched
// assertions continue to cover.
struct RecordingMeshObserver : public BakeObserver {
    int calls = 0;
    int last_tris = -1;
    void on_mesh_ready(int tris) override {
        ++calls;
        last_tris = tris;
    }
    // on_rung_ready deliberately left as the no-op default: bake_source never
    // builds a LOD ladder, so this observer must see zero rung calls too.
    int rung_calls = 0;
    void on_rung_ready(int, int, double) override { ++rung_calls; }
};

static void test_bake_observer_mesh_ready() {
    namespace pg = part_graph;
    namespace fs = std::filesystem;

    const fs::path root = fs::absolute("part_graph_bake_observer_test");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "schemas", ec);
    fs::create_directories(root / "parts", ec);
    CHECK(!ec, "bake_observer sandbox created");
    if (ec) return;

    write_file((root / "schemas" / "ObserverBox.js").string(),
        "class ObserverBox extends Part {"
        " build(p){ this.beginVoxels(0.25); this.fill(MAT.stone);"
        " this.box([0,0,0],[0.5,0.5,0.5]); this.endVoxels(); } }");

    script_host::ScriptHost host;
    pg::FileModuleResolver resolver(host, (root / "schemas").string());
    pg::HostBaker baker(host, root.string());
    baker.set_bake_observer(nullptr);  // explicit: default is already null

    // --- Pass 1: NULL observer (the production default) -- must bake fine
    // and (implicitly, via every other cold-cache test in this suite passing
    // unmodified) be byte-identical to the pre-W3 code path.
    pg::PartGraph graph_null(resolver, baker);
    pg::InstallResult r_null = graph_null.install({ pg::ChildRequest{"ObserverBox", pg::Params{}} });
    CHECK(r_null.ok && r_null.baked.size() == 1,
          "bake_observer: null-observer bake succeeds (byte-identity baseline)");

    // --- Pass 2: cold rebake of a DIFFERENT box, this time with an observer
    // wired via HostBaker::set_bake_observer (the W3 facade seam) BEFORE
    // install. A second, distinct schema keeps this a genuine cold bake
    // (ObserverBox is already cached from Pass 1).
    write_file((root / "schemas" / "ObserverBox2.js").string(),
        "class ObserverBox2 extends Part {"
        " build(p){ this.beginVoxels(0.25); this.fill(MAT.stone);"
        " this.box([0,0,0],[0.6,0.6,0.6]); this.endVoxels(); } }");

    RecordingMeshObserver obs;
    baker.set_bake_observer(&obs);
    pg::PartGraph graph_obs(resolver, baker);
    pg::InstallResult r_obs = graph_obs.install({ pg::ChildRequest{"ObserverBox2", pg::Params{}} });
    baker.set_bake_observer(nullptr);  // don't leak the observer into later tests
    CHECK(r_obs.ok && r_obs.baked.size() == 1,
          "bake_observer: observed bake succeeds");
    CHECK(obs.calls == 1, "bake_observer: on_mesh_ready fired exactly once");
    CHECK(obs.last_tris > 0, "bake_observer: on_mesh_ready reported a plausible (>0) tri count");
    CHECK(obs.rung_calls == 0,
          "bake_observer: on_rung_ready did NOT fire from bake_source "
          "(the LOD ladder is a separate subsystem -- see part_flatten_tests.cpp)");

    fs::remove_all(root, ec);
    printf("  test_bake_observer_mesh_ready OK (mesh_ready calls=%d tris=%d)\n",
           obs.calls, obs.last_tris);
}

int main(int argc, char** argv) {
    using namespace part_graph;

    test_stale_material_cache_migrates();
    if (argc == 2 && std::strcmp(argv[1], "--cache-migration-only") == 0)
        return g_failures == 0 ? 0 : 1;

    // Bake Lab §II.6: install-path trace shape with a collector set. Runs
    // early (and selectable in isolation, like --cache-migration-only above)
    // because it is cwd-portable via std::filesystem — the sandboxed tests
    // below assume a POSIX /tmp + shell.
    test_install_trace_shape();
    if (argc == 2 && std::strcmp(argv[1], "--trace-shape-only") == 0)
        return g_failures == 0 ? 0 : 1;

    // W5: static lods end-to-end, selectable in isolation like the two above
    // (also POSIX /tmp-only, runs before the sandboxed tests below).
    test_static_lods_end_to_end();
    if (argc == 2 && std::strcmp(argv[1], "--static-lods-only") == 0)
        return g_failures == 0 ? 0 : 1;

    // Resolve the demo schemas + shared-lib to ABSOLUTE paths NOW, before any test
    // chdir()s into a sandbox, so test_demo_tree_has_leaves can find the real files
    // regardless of cwd. (They are repo-relative to the tests dir we start in.)
    std::string demo_schemas, demo_sharedlib;
    { char abs[4096];
      if (realpath("../../projects/world_demo/schemas", abs)) demo_schemas = abs;
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

    // Bake Lab W3: per-rung bake observer seam -- on_mesh_ready fires exactly
    // once with a plausible tri count on an observed bake; a null observer
    // (every other test above) is unaffected.
    test_bake_observer_mesh_ready();

    if (g_failures == 0) printf("All part_graph integration tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
