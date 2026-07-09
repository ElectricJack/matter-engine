// Task 9: live_edit production seam tests.
// Three-part chain: Root.js requires Mid.js requires Leaf.js.
// Mid.js also imports Shared.js from shared-lib.
// Tests: snapshot correctness, ProdGraphResolver, ProdBaker, ProdFlattener.
// Headless (no GL). Requires -DMATTER_HAVE_SCRIPT_HOST.

#include "part_graph.h"
#include "part_graph_snapshot.h"
#include "live_edit_prod.h"
#include "part_asset_v2.h"
#include "part_flatten.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <limits.h>

#include "check.h"

using namespace part_graph;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static void write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary);
    f << body;
}

// ---------------------------------------------------------------------------
// Test sandbox setup
// ---------------------------------------------------------------------------

// Leaf.js: no children, no shared-lib imports.
static const char* LEAF_JS =
    "class Leaf extends Part {\n"
    "  static params = { scale: 1.0 };\n"
    "  build(p) {\n"
    "    this.beginVoxels(0.1);\n"
    "    this.fill(MAT.stone);\n"
    "    this.sphere([0,0,0], 0.1);\n"
    "    this.endVoxels();\n"
    "  }\n"
    "}\n";

// Leaf.js v2: a param tweak that changes output (used to test reresolve hash change).
static const char* LEAF_JS_V2 =
    "class Leaf extends Part {\n"
    "  static params = { scale: 2.0 };\n"
    "  build(p) {\n"
    "    this.beginVoxels(0.1);\n"
    "    this.fill(MAT.stoneDark);\n"
    "    this.sphere([0,0,0], 0.2);\n"
    "    this.endVoxels();\n"
    "  }\n"
    "}\n";

// Mid.js: requires Leaf, imports Shared from shared-lib.
static const char* MID_JS =
    "import { noise } from 'shared-lib/Shared';\n"
    "class Mid extends Part {\n"
    "  static requires = [{ module: 'Leaf', params: {} }];\n"
    "  build(p) {\n"
    "    this.beginVoxels(0.2);\n"
    "    this.fill(MAT.bark);\n"
    "    this.box([0,0,0],[0.3,0.3,0.3]);\n"
    "    this.endVoxels();\n"
    "    this.pushMatrix();\n"
    "    this.translate(0, 0.5, 0);\n"
    "    this.placeChild('Leaf');\n"
    "    this.popMatrix();\n"
    "  }\n"
    "}\n";

// Root.js: requires Mid.
static const char* ROOT_JS =
    "class Root extends Part {\n"
    "  static requires = [{ module: 'Mid', params: {} }];\n"
    "  build(p) {\n"
    "    this.beginVoxels(0.3);\n"
    "    this.fill(MAT.bark);\n"
    "    this.box([0,0,0],[0.5,0.5,0.5]);\n"
    "    this.endVoxels();\n"
    "    this.pushMatrix();\n"
    "    this.translate(0, 1.0, 0);\n"
    "    this.placeChild('Mid');\n"
    "    this.popMatrix();\n"
    "  }\n"
    "}\n";

// Shared.js (shared-lib): exported utility module.
static const char* SHARED_JS =
    "export const noise = function(x) { return x * 0.5; };\n";

struct Sandbox {
    std::string root;
    std::string schemas;
    std::string shared_lib;
    std::string parts;
};

static Sandbox make_sandbox(const char* name) {
    Sandbox s;
    s.root       = std::string("/tmp/me3_liveprod_") + name;
    s.schemas    = s.root + "/schemas";
    s.shared_lib = s.root + "/shared-lib";
    s.parts      = s.root + "/parts";
    ::system(("rm -rf " + s.root).c_str());
    ::system(("mkdir -p " + s.schemas + " " + s.shared_lib + " " + s.parts).c_str());
    write_file(s.schemas + "/Leaf.js", LEAF_JS);
    write_file(s.schemas + "/Mid.js",  MID_JS);
    write_file(s.schemas + "/Root.js", ROOT_JS);
    write_file(s.shared_lib + "/Shared.js", SHARED_JS);
    return s;
}

// ---------------------------------------------------------------------------
// test_snapshot_structure
// ---------------------------------------------------------------------------
static void test_snapshot_structure() {
    std::printf("[test_snapshot_structure]\n");
    Sandbox s = make_sandbox("snap");

    script_host::ScriptHost host;
    host.set_shared_lib_root(s.shared_lib);
    FileModuleResolver resolver(host, s.schemas);
    // HostBaker takes the CACHE ROOT (parent of parts/), not the parts subdir.
    HostBaker baker(host, s.root);
    PartGraph graph(resolver, baker);

    std::vector<ChildRequest> roots = { ChildRequest{"Root", {}} };
    part_graph_snapshot::Snapshot snap;
    InstallResult ir = graph.install(roots, &snap);
    CHECK(ir.ok, "install ok");
    if (!ir.ok) { std::printf("  install error: %s\n", ir.error.c_str()); return; }

    // 3 nodes: Root, Mid, Leaf
    CHECK(snap.nodes.size() == 3, "snapshot has 3 nodes");
    CHECK(snap.nodes.count("Root"), "Root in snapshot");
    CHECK(snap.nodes.count("Mid"),  "Mid in snapshot");
    CHECK(snap.nodes.count("Leaf"), "Leaf in snapshot");

    // Root is_root flag
    CHECK(snap.nodes.at("Root").is_root, "Root marked is_root");
    CHECK(!snap.nodes.at("Mid").is_root, "Mid not is_root");
    CHECK(!snap.nodes.at("Leaf").is_root, "Leaf not is_root");

    // Children edges: Root->Mid, Mid->Leaf
    CHECK(snap.nodes.at("Root").children.size() == 1 &&
          snap.nodes.at("Root").children[0] == "Mid",
          "Root.children == [Mid]");
    CHECK(snap.nodes.at("Mid").children.size() == 1 &&
          snap.nodes.at("Mid").children[0] == "Leaf",
          "Mid.children == [Leaf]");
    CHECK(snap.nodes.at("Leaf").children.empty(), "Leaf has no children");

    // shared_imports: Mid imports Shared
    CHECK(snap.nodes.at("Mid").shared_imports.size() == 1 &&
          snap.nodes.at("Mid").shared_imports[0] == "Shared",
          "Mid.shared_imports == [Shared]");
    CHECK(snap.nodes.at("Root").shared_imports.empty(), "Root has no shared imports");
    CHECK(snap.nodes.at("Leaf").shared_imports.empty(), "Leaf has no shared imports");

    // resolved_hash non-zero
    CHECK(snap.nodes.at("Root").resolved_hash != 0, "Root hash != 0");
    CHECK(snap.nodes.at("Mid").resolved_hash  != 0, "Mid hash != 0");
    CHECK(snap.nodes.at("Leaf").resolved_hash != 0, "Leaf hash != 0");

    // source_path set for each node
    CHECK(!snap.nodes.at("Root").source_path.empty(), "Root source_path set");
    CHECK(!snap.nodes.at("Mid").source_path.empty(),  "Mid source_path set");
    CHECK(!snap.nodes.at("Leaf").source_path.empty(), "Leaf source_path set");

    // by_file: abs path to module list
    std::string mid_path  = s.schemas + "/Mid.js";
    std::string leaf_path = s.schemas + "/Leaf.js";
    std::string root_path = s.schemas + "/Root.js";
    CHECK(snap.by_file.count(mid_path)  && snap.by_file.at(mid_path)  == std::vector<std::string>{"Mid"},
          "by_file[Mid.js] == [Mid]");
    CHECK(snap.by_file.count(leaf_path) && snap.by_file.at(leaf_path) == std::vector<std::string>{"Leaf"},
          "by_file[Leaf.js] == [Leaf]");
    CHECK(snap.by_file.count(root_path) && snap.by_file.at(root_path) == std::vector<std::string>{"Root"},
          "by_file[Root.js] == [Root]");

    // by_import: Shared -> [Mid]
    std::string shared_path = s.shared_lib + "/Shared.js";
    CHECK(snap.by_import.count("Shared") &&
          snap.by_import.at("Shared") == std::vector<std::string>{"Mid"},
          "by_import[Shared] == [Mid]");

    std::printf("  snapshot_structure: ok\n");
}

// ---------------------------------------------------------------------------
// test_resolver_parts_for_file
// ---------------------------------------------------------------------------
static void test_resolver_parts_for_file() {
    std::printf("[test_resolver_parts_for_file]\n");
    Sandbox s = make_sandbox("pffile");

    script_host::ScriptHost host;
    host.set_shared_lib_root(s.shared_lib);
    FileModuleResolver resolver(host, s.schemas);
    HostBaker baker(host, s.root);
    PartGraph graph(resolver, baker);

    part_graph_snapshot::Snapshot snap;
    InstallResult ir = graph.install({ChildRequest{"Root", {}}}, &snap);
    CHECK(ir.ok, "install ok");
    if (!ir.ok) return;

    live_edit_prod::ProdGraphResolver gr(snap, host, s.schemas, s.shared_lib);

    // parts_for_file for a normal schema file
    std::string mid_path = s.schemas + "/Mid.js";
    auto pf = gr.parts_for_file(mid_path);
    CHECK(pf.size() == 1 && pf[0] == "Mid",
          "parts_for_file(Mid.js) == [Mid]");

    // parts_for_file for shared-lib file -> importers
    std::string shared_path = s.shared_lib + "/Shared.js";
    auto pfs = gr.parts_for_file(shared_path);
    CHECK(pfs.size() == 1 && pfs[0] == "Mid",
          "parts_for_file(Shared.js) == [Mid] (importer)");

    std::printf("  resolver_parts_for_file: ok\n");
}

// ---------------------------------------------------------------------------
// test_resolver_ancestors
// ---------------------------------------------------------------------------
static void test_resolver_ancestors() {
    std::printf("[test_resolver_ancestors]\n");
    Sandbox s = make_sandbox("anc");

    script_host::ScriptHost host;
    host.set_shared_lib_root(s.shared_lib);
    FileModuleResolver resolver(host, s.schemas);
    HostBaker baker(host, s.root);
    PartGraph graph(resolver, baker);

    part_graph_snapshot::Snapshot snap;
    InstallResult ir = graph.install({ChildRequest{"Root", {}}}, &snap);
    CHECK(ir.ok, "install ok");
    if (!ir.ok) return;

    live_edit_prod::ProdGraphResolver gr(snap, host, s.schemas, s.shared_lib);

    // ancestors(Leaf) == {Mid, Root} (in some order)
    auto anc = gr.ancestors("Leaf");
    std::set<std::string> anc_set(anc.begin(), anc.end());
    CHECK(anc_set.count("Mid") && anc_set.count("Root"),
          "ancestors(Leaf) contains Mid and Root");
    CHECK(anc_set.size() == 2, "ancestors(Leaf) exactly 2 nodes");
    CHECK(!anc_set.count("Leaf"), "Leaf not in its own ancestors");

    // ancestors(Root) == {} (no parents)
    auto anc_root = gr.ancestors("Root");
    CHECK(anc_root.empty(), "ancestors(Root) empty");

    std::printf("  resolver_ancestors: ok\n");
}

// ---------------------------------------------------------------------------
// test_resolver_topo_order
// ---------------------------------------------------------------------------
static void test_resolver_topo_order() {
    std::printf("[test_resolver_topo_order]\n");
    Sandbox s = make_sandbox("topo");

    script_host::ScriptHost host;
    host.set_shared_lib_root(s.shared_lib);
    FileModuleResolver resolver(host, s.schemas);
    HostBaker baker(host, s.root);
    PartGraph graph(resolver, baker);

    part_graph_snapshot::Snapshot snap;
    InstallResult ir = graph.install({ChildRequest{"Root", {}}}, &snap);
    CHECK(ir.ok, "install ok");
    if (!ir.ok) return;

    live_edit_prod::ProdGraphResolver gr(snap, host, s.schemas, s.shared_lib);

    // topo_order({Root, Mid, Leaf}) -> children before parents
    std::set<std::string> subset = {"Root", "Mid", "Leaf"};
    auto order = gr.topo_order(subset);
    CHECK(order.size() == 3, "topo_order returns all 3 nodes");

    auto idx = [&](const std::string& m) -> int {
        for (int i = 0; i < (int)order.size(); ++i)
            if (order[i] == m) return i;
        return -1;
    };

    // Leaf before Mid before Root (children-first)
    CHECK(idx("Leaf") < idx("Mid"),  "Leaf before Mid in topo order");
    CHECK(idx("Mid")  < idx("Root"), "Mid before Root in topo order");

    std::printf("  resolver_topo_order: ok\n");
}

// ---------------------------------------------------------------------------
// test_resolver_roots_over
// ---------------------------------------------------------------------------
static void test_resolver_roots_over() {
    std::printf("[test_resolver_roots_over]\n");
    Sandbox s = make_sandbox("roots");

    script_host::ScriptHost host;
    host.set_shared_lib_root(s.shared_lib);
    FileModuleResolver resolver(host, s.schemas);
    HostBaker baker(host, s.root);
    PartGraph graph(resolver, baker);

    part_graph_snapshot::Snapshot snap;
    InstallResult ir = graph.install({ChildRequest{"Root", {}}}, &snap);
    CHECK(ir.ok, "install ok");
    if (!ir.ok) return;

    live_edit_prod::ProdGraphResolver gr(snap, host, s.schemas, s.shared_lib);

    // roots_over({Leaf}) should walk up to Root
    auto roots = gr.roots_over({"Leaf"});
    std::set<std::string> rset(roots.begin(), roots.end());
    CHECK(rset.count("Root"), "roots_over({Leaf}) contains Root");
    CHECK(!rset.count("Mid"), "roots_over({Leaf}) does not contain Mid");
    CHECK(!rset.count("Leaf"), "roots_over({Leaf}) does not contain Leaf itself");

    std::printf("  resolver_roots_over: ok\n");
}

// ---------------------------------------------------------------------------
// test_reresolve_and_bake
// ---------------------------------------------------------------------------
static void test_reresolve_and_bake() {
    std::printf("[test_reresolve_and_bake]\n");
    Sandbox s = make_sandbox("rebake");

    script_host::ScriptHost host;
    host.set_shared_lib_root(s.shared_lib);
    FileModuleResolver resolver(host, s.schemas);
    HostBaker baker(host, s.root);
    PartGraph graph(resolver, baker);

    part_graph_snapshot::Snapshot snap;
    InstallResult ir = graph.install({ChildRequest{"Root", {}}}, &snap);
    CHECK(ir.ok, "install ok");
    if (!ir.ok) { std::printf("  install error: %s\n", ir.error.c_str()); return; }

    uint64_t leaf_hash_install = snap.nodes.at("Leaf").resolved_hash;
    uint64_t mid_hash_install  = snap.nodes.at("Mid").resolved_hash;
    uint64_t root_hash_install = snap.nodes.at("Root").resolved_hash;

    // --- Edit Leaf.js on disk ---
    write_file(s.schemas + "/Leaf.js", LEAF_JS_V2);

    live_edit_prod::ProdGraphResolver gr(snap, host, s.schemas, s.shared_lib);
    live_edit_prod::ProdBaker         pb(snap, host, s.root);

    // reresolve(Leaf) -> new hash
    live_edit::ResolvedHash new_leaf_h = gr.reresolve("Leaf");
    CHECK(!new_leaf_h.empty(), "reresolve(Leaf) returned a hash");
    uint64_t new_leaf_hash = std::strtoull(new_leaf_h.c_str(), nullptr, 10);
    CHECK(new_leaf_hash != leaf_hash_install, "reresolve(Leaf) returns NEW hash after edit");

    // ProdBaker.bake(Leaf, new_hash, 0) -> parts/<new_hash>.part exists
    auto outcome = pb.bake("Leaf", new_leaf_h, 0);
    CHECK(outcome.ok, "ProdBaker.bake(Leaf) succeeded");
    if (!outcome.ok)
        std::printf("  bake error: %s\n", outcome.error.message.c_str());

    std::string part_path = s.root + "/" + part_asset::cache_path_resolved(new_leaf_hash);
    CHECK(file_exists(part_path), "parts/<new_leaf_hash>.part exists after bake");

    // Cascade: reresolve Mid (its child Leaf has a new hash)
    live_edit::ResolvedHash new_mid_h = gr.reresolve("Mid");
    CHECK(!new_mid_h.empty(), "reresolve(Mid) returned a hash");
    uint64_t new_mid_hash = std::strtoull(new_mid_h.c_str(), nullptr, 10);
    CHECK(new_mid_hash != mid_hash_install, "reresolve(Mid) returns NEW hash (child changed)");

    auto mid_out = pb.bake("Mid", new_mid_h, 0);
    CHECK(mid_out.ok, "ProdBaker.bake(Mid) succeeded");

    // Cascade: reresolve Root
    live_edit::ResolvedHash new_root_h = gr.reresolve("Root");
    CHECK(!new_root_h.empty(), "reresolve(Root) returned a hash");
    uint64_t new_root_hash = std::strtoull(new_root_h.c_str(), nullptr, 10);
    CHECK(new_root_hash != root_hash_install, "reresolve(Root) returns NEW hash (child chain changed)");

    auto root_out = pb.bake("Root", new_root_h, 0);
    CHECK(root_out.ok, "ProdBaker.bake(Root) succeeded");

    std::printf("  reresolve_and_bake: ok\n");
}

// ---------------------------------------------------------------------------
// test_flattener
// ---------------------------------------------------------------------------
static void test_flattener() {
    std::printf("[test_flattener]\n");
    Sandbox s = make_sandbox("flat");

    script_host::ScriptHost host;
    host.set_shared_lib_root(s.shared_lib);
    FileModuleResolver resolver(host, s.schemas);
    HostBaker baker(host, s.root);
    PartGraph graph(resolver, baker);

    part_graph_snapshot::Snapshot snap;
    InstallResult ir = graph.install({ChildRequest{"Root", {}}}, &snap);
    CHECK(ir.ok, "install ok");
    if (!ir.ok) { std::printf("  install error: %s\n", ir.error.c_str()); return; }

    // Edit Leaf + cascade reresolve + bake all parts in topo order first
    write_file(s.schemas + "/Leaf.js", LEAF_JS_V2);

    live_edit_prod::ProdGraphResolver gr(snap, host, s.schemas, s.shared_lib);
    live_edit_prod::ProdBaker         pb(snap, host, s.root);

    auto leaf_h = gr.reresolve("Leaf");
    CHECK(!leaf_h.empty(), "reresolve(Leaf)");
    pb.bake("Leaf", leaf_h, 0);

    auto mid_h = gr.reresolve("Mid");
    CHECK(!mid_h.empty(), "reresolve(Mid)");
    pb.bake("Mid", mid_h, 0);

    auto root_h = gr.reresolve("Root");
    CHECK(!root_h.empty(), "reresolve(Root)");
    auto bake_ok = pb.bake("Root", root_h, 0);
    CHECK(bake_ok.ok, "bake Root ok");

    // ProdFlattener.reflatten(Root) -> writes .flat.part
    live_edit_prod::ProdFlattener pf(snap, host, s.root);
    auto flat_out = pf.reflatten("Root");
    CHECK(flat_out.ok, "ProdFlattener.reflatten(Root) succeeded");
    if (!flat_out.ok)
        std::printf("  flatten error: %s\n", flat_out.error.message.c_str());

    // Check that .flat.part exists for the new root hash
    uint64_t new_root_hash = std::strtoull(root_h.c_str(), nullptr, 10);
    std::string flat_path = s.root + "/" + part_asset::cache_path_flat(new_root_hash);
    CHECK(file_exists(flat_path), "parts/<root_hash>.flat.part exists after reflatten");

    std::printf("  flattener: ok\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::printf("=== live_edit_prod_tests ===\n");
    test_snapshot_structure();
    test_resolver_parts_for_file();
    test_resolver_ancestors();
    test_resolver_topo_order();
    test_resolver_roots_over();
    test_reresolve_and_bake();
    test_flattener();

    if (g_failures) { std::printf("\n%d FAILURES\n", g_failures); return 1; }
    std::printf("\nALL PASS\n");
    return 0;
}
