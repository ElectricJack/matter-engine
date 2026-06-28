// SP-3 Task 12 end-to-end check (gated on SP-2 via -DMATTER_HAVE_SCRIPT_HOST).
//
// Writes real .js part schemas + a world manifest, runs PartGraph::install
// through the REAL FileModuleResolver + HostBaker adapters (which drive the SP-2
// ScriptHost: eval_requires for child discovery, resolve_hash for cache keys,
// bake_source -> save_v2 for the actual .part files), then asserts the expected
// parts/<hash>.part files exist on disk and a second install bakes 0 (all hits).
//
// bake_source writes to the RELATIVE path cache_path_resolved() = "parts/<hash>.part",
// so this test chdir()s into a fresh temp dir and points HostBaker at "parts" so
// both the writer and the cache check agree on the location (plan precondition).

#include "part_graph.h"        // includes script_host.h under the guard
#include "part_asset_v2.h"     // cache_path_resolved
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

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

    // Child-folded hashes, in the same nesting order the graph used: Leaf has no
    // children; TreeBranch folds [Leaf]; Tree folds [TreeBranch].
    uint64_t leaf_hash   = host.resolve_hash(read_file(schemas + "/Leaf.js"), "{}");
    uint64_t lkids[1]    = { leaf_hash };
    uint64_t branch_hash = host.resolve_hash(read_file(schemas + "/TreeBranch.js"), "{}", lkids, 1);
    uint64_t tree_hash   = ir.root_hashes[0];

    // --- Tree: voxel trunk + TreeBranch instances ---
    BLASManager t_blas; TLASManager t_tlas(64);
    std::vector<part_asset::ChildInstance> t_children;
    part_asset::LodLevels t_lods;
    bool t_loaded = part_asset::load_v2(part_asset::cache_path_resolved(tree_hash),
                                        tree_hash, t_blas, t_tlas, t_children, t_lods);
    CHECK(t_loaded, "demo Tree .part reloads");
    CHECK(t_blas.get_unique_blas_count() >= 1, "Tree registered trunk voxel geometry");
    CHECK(!t_children.empty(), "demo Tree placed at least one TreeBranch");
    { size_t tt = 0; for (const auto& e : t_blas.get_entries()) tt += e->triangles.size();
      // Geometry budget guard: the trunk voxel sweep once ballooned to >130k tris
      // (multi-second synchronous LOD bake per part at viewer startup). Keep it sane.
      CHECK(tt < 150000, "Tree trunk triangle count within budget"); }
    printf("  demo Tree placed %zu TreeBranch instance(s)\n", t_children.size());
    for (const auto& c : t_children)
        CHECK(c.child_resolved_hash == branch_hash, "every Tree child is a TreeBranch");

    // --- TreeBranch: mesh twig tubes + Leaf instances ---
    BLASManager b_blas; TLASManager b_tlas(64);
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
    for (const auto& c : b_children)
        CHECK(c.child_resolved_hash == leaf_hash, "every TreeBranch child is a Leaf");

    // --- Leaf: bezier triangle-fan blade (mesh, no children) ---
    BLASManager l_blas; TLASManager l_tlas(64);
    std::vector<part_asset::ChildInstance> l_children;
    part_asset::LodLevels l_lods;
    bool l_loaded = part_asset::load_v2(part_asset::cache_path_resolved(leaf_hash),
                                        leaf_hash, l_blas, l_tlas, l_children, l_lods);
    CHECK(l_loaded, "demo Leaf .part reloads");
    CHECK(l_blas.get_unique_blas_count() >= 1, "Leaf registered blade triangle mesh");
    CHECK(l_children.empty(), "Leaf is a mesh leaf with no children");
}

int main() {
    using namespace part_graph;

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

    if (failures == 0) printf("All part_graph integration tests passed\n");
    return failures == 0 ? 0 : 1;
}
