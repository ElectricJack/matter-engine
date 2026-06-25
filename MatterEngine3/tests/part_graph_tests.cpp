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
              const std::vector<uint64_t>& child_hashes, uint64_t h) override {
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

    if (failures == 0) printf("All part_graph tests passed\n");
    return failures == 0 ? 0 : 1;
}
