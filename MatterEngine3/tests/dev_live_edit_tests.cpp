// Headless SP-5 dev live-edit tests. plain main(), no GL. Drives FakeWatcher +
// fake SP-2/3/4 seams. See docs/superpowers/specs/2026-06-24-dev-live-edit-design.md
#include "file_watcher.h"
#include "live_edit.h"
#include <cstdio>
#include <cstdlib>
#include <map>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_failures; } \
    else { std::printf("  ok: %s\n", msg); } } while (0)

using namespace live_edit;

// Fake SP-3 graph: parent/child edges supplied by the test. ancestors() walks
// child->parents edges transitively.
struct FakeGraph : GraphResolver {
    std::map<std::string, std::vector<PartId>> file_to_parts;
    std::map<PartId, std::vector<PartId>> parents;   // p -> direct parents
    std::vector<PartId> parts_for_file(const std::string& path) override {
        auto it = file_to_parts.find(path);
        return it == file_to_parts.end() ? std::vector<PartId>{} : it->second;
    }
    std::vector<PartId> ancestors(const PartId& p) override {
        std::vector<PartId> out; std::set<PartId> seen;
        std::vector<PartId> stk{p};
        while (!stk.empty()) {
            PartId c = stk.back(); stk.pop_back();
            auto it = parents.find(c);
            if (it == parents.end()) continue;
            for (auto& par : it->second)
                if (seen.insert(par).second) { out.push_back(par); stk.push_back(par); }
        }
        return out;
    }
    std::vector<PartId> topo_order(const std::set<PartId>&) override { return {}; }
    std::vector<PartId> roots_over(const std::set<PartId>&) override { return {}; }
    ResolvedHash reresolve(const PartId& p) override { return "h_" + p; }
};

static void test_fake_watcher_roundtrip() {
    std::printf("[test_fake_watcher_roundtrip]\n");
    FakeWatcher w;
    w.add_watch("/tmp/ObjectSchemas");
    w.set_now_ms(1000);
    w.push("/tmp/ObjectSchemas/rock.js", FileEventKind::Modified);
    std::vector<FileEvent> out;
    int n = w.poll(out);
    CHECK(n == 1, "poll returns one pushed event");
    CHECK(out.size() == 1 && out[0].path == "/tmp/ObjectSchemas/rock.js", "event path round-trips");
    CHECK(out[0].t_ms == 1000, "event stamped with fake clock");
    std::vector<FileEvent> out2;
    CHECK(w.poll(out2) == 0, "second poll drains to empty");
}

static void test_upward_cone() {
    std::printf("[test_upward_cone]\n");
    // leaf -> mid -> root ; sibling unrelated.
    FakeGraph g;
    g.parents["leaf"] = {"mid"};
    g.parents["mid"]  = {"root"};
    g.parents["sibling"] = {"root"};
    FakeWatcher w; struct NB : Baker { BakeOutcome bake(const PartId&, const ResolvedHash&, long long) override { return {true,{}}; } } b;
    struct NF : Flattener { BakeOutcome reflatten(const PartId&) override { return {true,{}}; } } f;
    struct NS : ErrorSink { void report(const LiveEditError&) override {} } s;
    LiveEditSession sess(w, g, b, f, s, LiveEditConfig{});
    auto cone = sess.upward_cone({"leaf"});
    CHECK(cone.count("leaf") && cone.count("mid") && cone.count("root"), "cone = leaf+mid+root");
    CHECK(cone.count("sibling") == 0, "sibling NOT in cone");
    CHECK(cone.size() == 3, "cone is exactly 3 parts");
}

int main() {
    std::printf("=== dev_live_edit_tests ===\n");
    test_fake_watcher_roundtrip();
    test_upward_cone();
    if (g_failures) { std::printf("\n%d FAILURES\n", g_failures); return 1; }
    std::printf("\nALL PASS\n");
    return 0;
}
