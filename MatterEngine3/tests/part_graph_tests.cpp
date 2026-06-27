#include "part_graph.h"
#include "part_asset_v2.h"   // SP-1 (via -I../include): compute_resolved_hash
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

using namespace part_graph;

// A scriptless module table: module name -> (source bytes, requires-producer).
// requires-producer maps the parent's params to a list of ChildRequests.
struct FakeModule {
    std::string source;
    std::function<std::vector<ChildRequest>(const Params&)> requires_fn;
    bool eval_fails = false;   // simulate a module that throws at top-level eval
};

struct FakeModuleResolver : ModuleResolver {
    std::map<std::string, FakeModule> modules;
    bool load_source(const std::string& m, std::string& out) override {
        auto it = modules.find(m);
        if (it == modules.end()) return false;
        out = it->second.source;
        return true;
    }
    bool get_requires(const std::string& m, const Params& params,
                      std::vector<ChildRequest>& out) override {
        auto it = modules.find(m);
        if (it == modules.end() || it->second.eval_fails) return false;
        out = it->second.requires_fn ? it->second.requires_fn(params)
                                     : std::vector<ChildRequest>{};
        return true;
    }
};

struct FakeBaker : Baker {
    std::set<uint64_t> on_disk;                 // pre-populated cache state
    std::vector<uint64_t> bake_order;           // hashes in the order baked
    std::map<uint64_t,std::vector<uint64_t>> children_seen; // hash -> child_hashes at bake
    std::set<uint64_t> fail_hashes;             // hashes whose bake should fail
    std::set<uint64_t> resolve_fail_hashes;     // hashes resolve_hash should refuse (=> 0)
    // Deterministic stand-in for SP-2's resolve_hash: the real host merges static
    // defaults first, but a GL-free fake has no JS — fold (source, canonical override
    // params, child_hashes) the same way SP-1's compute_resolved_hash does, so test
    // expectations can mirror it. Identity/invalidation behavior is what we test here.
    uint64_t resolve_hash(const std::string& source, const Params& params,
                          const std::vector<uint64_t>& child_hashes) override {
        std::string canon = serialize_params(params);
        uint64_t h = part_asset::compute_resolved_hash(
            source.data(), source.size(), canon.data(), canon.size(),
            child_hashes.data(), child_hashes.size());
        return resolve_fail_hashes.count(h) ? 0 : h;
    }
    bool cached(uint64_t h) override { return on_disk.count(h) != 0; }
    bool bake(const std::string&, const Params&,
              const std::vector<uint64_t>& child_hashes,
              const std::vector<std::string>& /*child_modules*/, uint64_t h) override {
        if (fail_hashes.count(h)) return false; // fail-closed: nothing "written"
        bake_order.push_back(h);
        children_seen[h] = child_hashes;
        on_disk.insert(h);                      // a successful bake populates the cache
        return true;
    }
};

int main() {
    using namespace part_graph;
    // Empty params canonicalize to the empty string.
    CHECK(serialize_params(Params{}) == "", "empty params -> empty string");

    // Numbers use %.17g; key/value joined with '=' and terminated with ';'.
    {
        Params p;
        p["size"] = ParamValue::number(1.5);
        CHECK(serialize_params(p) == "size=1.5;", "single number param");
    }
    // Keys are emitted in sorted order regardless of insertion order (std::map sorts,
    // but assert the contract explicitly so a future container swap can't break it).
    {
        Params p;
        p["zeta"]  = ParamValue::number(2);
        p["alpha"] = ParamValue::number(1);
        CHECK(serialize_params(p) == "alpha=1;zeta=2;", "keys sorted lexicographically");
    }
    // Bools and strings.
    {
        Params p;
        p["hollow"] = ParamValue::boolean_(true);
        p["name"]   = ParamValue::string_("rock");
        CHECK(serialize_params(p) == "hollow=true;name=rock;", "bool + string params");
    }
    // Equal params (different insertion order) produce identical canonical strings.
    {
        Params a; a["x"] = ParamValue::number(0.1); a["y"] = ParamValue::number(0.2);
        Params b; b["y"] = ParamValue::number(0.2); b["x"] = ParamValue::number(0.1);
        CHECK(serialize_params(a) == serialize_params(b), "order-independent canonical form");
    }

    // Task 3 wiring: single leaf with no children resolves, installs, bakes once.
    {
        FakeModuleResolver res;
        res.modules["Leaf"] = FakeModule{ "leaf-source", nullptr, false };
        FakeBaker baker;
        PartGraph g(res, baker);
        InstallResult r = g.install({ ChildRequest{"Leaf", Params{}} });
        CHECK(r.ok, "single leaf install succeeds");
        CHECK(baker.bake_order.size() == 1, "single leaf baked exactly once");
    }

    // Task 5: cache miss -> bake; cache hit -> skip (incremental build).
    {
        // Parent -> two children A, B. First install bakes all 3; second bakes 0.
        FakeModuleResolver res;
        res.modules["A"]    = FakeModule{ "src-A", nullptr, false };
        res.modules["B"]    = FakeModule{ "src-B", nullptr, false };
        res.modules["Root"] = FakeModule{ "src-Root",
            [](const Params&) {
                return std::vector<ChildRequest>{
                    ChildRequest{"A", Params{}}, ChildRequest{"B", Params{}} };
            }, false };

        FakeBaker baker;
        PartGraph g(res, baker);

        InstallResult r1 = g.install({ ChildRequest{"Root", Params{}} });
        CHECK(r1.ok, "first install ok");
        CHECK(r1.baked.size() == 3, "first install bakes 3 parts");
        CHECK(r1.hits == 0, "first install has 0 cache hits");

        // Second install with the same (now-populated) baker cache: 0 bakes, 3 hits.
        InstallResult r2 = g.install({ ChildRequest{"Root", Params{}} });
        CHECK(r2.ok, "second install ok");
        CHECK(r2.baked.empty(), "second install bakes nothing (all hits)");
        CHECK(r2.hits == 3, "second install reports 3 cache hits");
    }

    // Task 6: topological bake order (children before parents).
    {
        // Chain Root -> Mid -> Leaf. Bake order must be Leaf, Mid, Root.
        FakeModuleResolver res;
        res.modules["Leaf"] = FakeModule{ "src-Leaf", nullptr, false };
        res.modules["Mid"]  = FakeModule{ "src-Mid",
            [](const Params&){ return std::vector<ChildRequest>{ ChildRequest{"Leaf", Params{}} }; }, false };
        res.modules["Root"] = FakeModule{ "src-Root",
            [](const Params&){ return std::vector<ChildRequest>{ ChildRequest{"Mid", Params{}} }; }, false };

        FakeBaker baker;
        PartGraph g(res, baker);
        InstallResult r = g.install({ ChildRequest{"Root", Params{}} });
        CHECK(r.ok && r.baked.size() == 3, "chain install bakes 3");
        // Every parent's children must already be on disk when the parent is baked:
        // assert the recorded child_hashes were all baked earlier in bake_order.
        bool order_ok = true;
        std::set<uint64_t> seen;
        for (uint64_t h : baker.bake_order) {
            for (uint64_t c : baker.children_seen[h])
                if (!seen.count(c)) order_ok = false;  // child baked AFTER parent => fail
            seen.insert(h);
        }
        CHECK(order_ok, "children baked before their parents (topo order)");
    }

    // Task 7: dedup (same params collapse; different params split).
    {
        // Two parents instantiate child "Bolt" with IDENTICAL params -> ONE artifact.
        FakeModuleResolver res;
        res.modules["Bolt"] = FakeModule{ "src-Bolt", nullptr, false };
        auto mk = [](){ Params p; p["len"] = ParamValue::number(2.0); return p; };
        res.modules["P1"] = FakeModule{ "src-P1",
            [mk](const Params&){ return std::vector<ChildRequest>{ ChildRequest{"Bolt", mk()} }; }, false };
        res.modules["P2"] = FakeModule{ "src-P2",
            [mk](const Params&){ return std::vector<ChildRequest>{ ChildRequest{"Bolt", mk()} }; }, false };
        res.modules["Root"] = FakeModule{ "src-Root",
            [](const Params&){ return std::vector<ChildRequest>{
                ChildRequest{"P1", Params{}}, ChildRequest{"P2", Params{}} }; }, false };

        FakeBaker baker;
        PartGraph g(res, baker);
        InstallResult r = g.install({ ChildRequest{"Root", Params{}} });
        CHECK(r.ok, "dedup-same install ok");
        // Root, P1, P2, Bolt -> 4 unique artifacts (Bolt shared once).
        CHECK(r.baked.size() == 4, "identical-params child collapses to one artifact");
    }
    {
        // Same child module, DIFFERENT params -> TWO artifacts.
        FakeModuleResolver res;
        res.modules["Bolt"] = FakeModule{ "src-Bolt", nullptr, false };
        res.modules["Root"] = FakeModule{ "src-Root",
            [](const Params&){
                Params a; a["len"] = ParamValue::number(2.0);
                Params b; b["len"] = ParamValue::number(3.0);
                return std::vector<ChildRequest>{
                    ChildRequest{"Bolt", a}, ChildRequest{"Bolt", b} };
            }, false };
        FakeBaker baker;
        PartGraph g(res, baker);
        InstallResult r = g.install({ ChildRequest{"Root", Params{}} });
        CHECK(r.ok, "dedup-diff install ok");
        // Root + two distinct Bolt artifacts = 3.
        CHECK(r.baked.size() == 3, "differing-params child produces two artifacts");
    }

    // Task 8: cycle detection (hard error naming the cycle path).
    {
        // a -> b -> a cycle: install must hard-error and bake NOTHING.
        FakeModuleResolver res;
        res.modules["a"] = FakeModule{ "src-a",
            [](const Params&){ return std::vector<ChildRequest>{ ChildRequest{"b", Params{}} }; }, false };
        res.modules["b"] = FakeModule{ "src-b",
            [](const Params&){ return std::vector<ChildRequest>{ ChildRequest{"a", Params{}} }; }, false };
        FakeBaker baker;
        PartGraph g(res, baker);
        InstallResult r = g.install({ ChildRequest{"a", Params{}} });
        CHECK(!r.ok, "cycle install fails");
        CHECK(r.error.find("cycle") != std::string::npos, "error message names a cycle");
        CHECK(r.error.find("a") != std::string::npos && r.error.find("b") != std::string::npos,
              "cycle error names the involved modules");
        CHECK(baker.bake_order.empty(), "nothing baked when a cycle is present");
    }

    // Task 9: reachability (orphans never baked).
    {
        FakeModuleResolver res;
        res.modules["Leaf"]   = FakeModule{ "src-Leaf", nullptr, false };
        res.modules["Orphan"] = FakeModule{ "src-Orphan", nullptr, false };  // not in graph
        res.modules["Root"]   = FakeModule{ "src-Root",
            [](const Params&){ return std::vector<ChildRequest>{ ChildRequest{"Leaf", Params{}} }; }, false };
        FakeBaker baker;
        PartGraph g(res, baker);
        InstallResult r = g.install({ ChildRequest{"Root", Params{}} });
        CHECK(r.ok && r.baked.size() == 2, "only Root+Leaf baked");
        // Orphan's hash must never appear in bake_order. Compute it the same way the
        // graph would and assert absence.
        uint64_t orphan_hash = part_asset::compute_resolved_hash(
            "src-Orphan", 10, "", 0, nullptr, 0);
        bool found = false;
        for (uint64_t h : baker.bake_order) if (h == orphan_hash) found = true;
        CHECK(!found, "orphan part is never baked");
    }

    // Task 9: transitive invalidation (edit leaf => leaf + ancestor rebake; sibling stays a hit).
    {
        // Root -> {Edited(Leaf), Stable}. First install bakes 3. Then "edit" Leaf's
        // source; re-resolve: Leaf + Root miss, Stable hits.
        FakeModuleResolver res;
        res.modules["Leaf"]   = FakeModule{ "leaf-v1", nullptr, false };
        res.modules["Stable"] = FakeModule{ "stable-v1", nullptr, false };
        res.modules["Root"]   = FakeModule{ "root-v1",
            [](const Params&){ return std::vector<ChildRequest>{
                ChildRequest{"Leaf", Params{}}, ChildRequest{"Stable", Params{}} }; }, false };
        FakeBaker baker;
        PartGraph g(res, baker);

        InstallResult r1 = g.install({ ChildRequest{"Root", Params{}} });
        CHECK(r1.ok && r1.baked.size() == 3, "initial install bakes 3");

        // Edit the leaf source. Stable + Root-edge unchanged, but Root folds Leaf's hash,
        // so Root's resolved hash changes too -> Root + Leaf miss; Stable still cached.
        res.modules["Leaf"].source = "leaf-v2";
        InstallResult r2 = g.install({ ChildRequest{"Root", Params{}} });
        CHECK(r2.ok, "post-edit install ok");
        CHECK(r2.baked.size() == 2, "edited leaf + ancestor root rebake");
        CHECK(r2.hits == 1, "unrelated Stable branch stays a cache hit");
    }

    // Task 10: failure propagation (child bake fail => parent unbaked => install fails named).
    {
        // Root -> Child. Child's bake fails => Child never recorded, Root never baked,
        // install fails naming Child.
        FakeModuleResolver res;
        res.modules["Child"] = FakeModule{ "src-Child", nullptr, false };
        res.modules["Root"]  = FakeModule{ "src-Root",
            [](const Params&){ return std::vector<ChildRequest>{ ChildRequest{"Child", Params{}} }; }, false };
        FakeBaker baker;
        uint64_t child_hash = part_asset::compute_resolved_hash("src-Child", 9, "", 0, nullptr, 0);
        baker.fail_hashes.insert(child_hash);

        PartGraph g(res, baker);
        InstallResult r = g.install({ ChildRequest{"Root", Params{}} });
        CHECK(!r.ok, "install fails when a child bake fails");
        CHECK(r.error.find("Child") != std::string::npos, "error names the failing part (Child)");
        CHECK(r.baked.empty(), "no part recorded as baked when child fails");
        // Root must NOT have been baked (its child hash is unavailable).
        uint64_t root_hash = part_asset::compute_resolved_hash(
            "src-Root", 8, "", 0, &child_hash, 1);
        bool root_baked = false;
        for (uint64_t h : baker.bake_order) if (h == root_hash) root_baked = true;
        CHECK(!root_baked, "parent is not baked after child bake failure");
    }

    // Task 11: world manifest root discovery.
    {
        // Write a temp world tree and parse the manifest.
        std::string dir = "/tmp/pg_world_test";
        std::string wdir = dir + "/WorldData/w1";
        system(("rm -rf " + dir + " && mkdir -p " + wdir).c_str());
        {
            FILE* f = fopen((wdir + "/world.manifest").c_str(), "w");
            fputs("# roots for w1\nTower\n\nBridge\n", f);
            fclose(f);
        }
        std::vector<ChildRequest> roots;
        std::string err;
        bool ok = PartGraph::read_manifest(dir + "/WorldData", "w1", roots, err);
        CHECK(ok, "manifest parse succeeds");
        CHECK(roots.size() == 2, "manifest yields 2 roots (comments/blank lines ignored)");
        CHECK(roots[0].module == "Tower" && roots[1].module == "Bridge",
              "roots parsed in order");

        // Missing manifest => hard error.
        std::vector<ChildRequest> none;
        std::string err2;
        bool ok2 = PartGraph::read_manifest(dir + "/WorldData", "does_not_exist", none, err2);
        CHECK(!ok2, "missing manifest is a hard error");
        CHECK(!err2.empty(), "missing manifest reports an error message");
        system(("rm -rf " + dir).c_str());
    }

    if (failures == 0) printf("All part_graph tests passed\n");
    return failures == 0 ? 0 : 1;
}
