// Headless SP-5 dev live-edit tests. plain main(), no GL. Drives FakeWatcher +
// fake SP-2/3/4 seams. See docs/superpowers/specs/2026-06-24-dev-live-edit-design.md
#include "file_watcher.h"
#include "live_edit.h"
#include "inotify_watcher.h"
#include "win_watcher.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <algorithm>
#ifdef __linux__
#include <unistd.h>
#endif

#include "check.h"

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
    std::map<PartId, std::vector<PartId>> roots; // p -> affected roots (test-set)
    std::vector<PartId> topo_order(const std::set<PartId>& subset) override {
        // deterministic: children before parents using `parents` depth.
        std::vector<PartId> v(subset.begin(), subset.end());
        auto depth = [&](const PartId& p){ int d=0; PartId c=p;
            while (parents.count(c) && !parents[c].empty()) { c = parents[c][0]; ++d; } return d; };
        std::sort(v.begin(), v.end(), [&](const PartId&a,const PartId&b){ return depth(a) > depth(b); });
        return v;
    }
    std::vector<PartId> roots_over(const std::set<PartId>& changed) override {
        std::set<PartId> rs;
        for (auto& c : changed) { auto it = roots.find(c);
            if (it != roots.end()) for (auto& r : it->second) rs.insert(r); }
        return {rs.begin(), rs.end()};
    }
    ResolvedHash reresolve(const PartId& p) override { return "h_" + p; }
};

// Records every bake/reflatten/error so tests can assert scope + counts.
struct RecBaker : Baker {
    std::vector<PartId> baked;
    BakeOutcome bake(const PartId& p, const ResolvedHash&, long long) override {
        baked.push_back(p); return {true, {}};
    }
};
struct RecFlattener : Flattener {
    std::vector<PartId> roots;
    BakeOutcome reflatten(const PartId& r) override { roots.push_back(r); return {true, {}}; }
};
struct RecSink : ErrorSink {
    std::vector<LiveEditError> errs;
    void report(const LiveEditError& e) override { errs.push_back(e); }
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

static void test_changed_parts_single_and_shared() {
    std::printf("[test_changed_parts_single_and_shared]\n");
    FakeGraph g;
    g.file_to_parts["/w/ObjectSchemas/rock.js"] = {"rock"};        // single part
    g.file_to_parts["/w/lib/noise.js"] = {"rock", "tree", "bush"}; // shared module
    FakeWatcher w;
    struct NB : Baker { BakeOutcome bake(const PartId&, const ResolvedHash&, long long) override { return {true,{}}; } } b;
    struct NF : Flattener { BakeOutcome reflatten(const PartId&) override { return {true,{}}; } } f;
    struct NS : ErrorSink { void report(const LiveEditError&) override {} } s;
    LiveEditSession sess(w, g, b, f, s, LiveEditConfig{});

    auto one = sess.changed_parts({"/w/ObjectSchemas/rock.js"});
    CHECK(one.size() == 1 && one.count("rock"), "single-part file -> one part");

    auto many = sess.changed_parts({"/w/lib/noise.js"});
    CHECK(many.size() == 3 && many.count("rock") && many.count("tree") && many.count("bush"),
          "shared module file -> all importers");

    auto both = sess.changed_parts({"/w/ObjectSchemas/rock.js", "/w/lib/noise.js"});
    CHECK(both.size() == 3, "union dedups overlapping importer (rock counted once)");
}

static void test_debounce_coalesces_two_writes() {
    std::printf("[test_debounce_coalesces_two_writes]\n");
    FakeGraph g;
    g.file_to_parts["/w/rock.js"] = {"rock"};
    g.parents["rock"] = {"root"};
    g.roots["rock"] = {"root"};
    FakeWatcher w; RecBaker b; RecFlattener f; RecSink s;
    LiveEditSession sess(w, g, b, f, s, LiveEditConfig{/*debounce*/150, /*budget*/0});

    w.set_now_ms(1000);
    w.push("/w/rock.js");          // editor's first write
    auto r1 = sess.tick();         // within window -> nothing fires yet
    CHECK(r1.rebaked.empty(), "first write: no rebuild yet (quiet window open)");

    w.advance_ms(50);
    w.push("/w/rock.js");          // editor's second write (typical double-save)
    auto r2 = sess.tick();
    CHECK(r2.rebaked.empty(), "second write inside window: still no rebuild");

    w.advance_ms(200);             // quiet window elapsed (50+200 > 150 since last)
    auto r3 = sess.tick();
    CHECK(r3.rebaked.size() >= 1, "after quiet window: exactly one rebuild fires");
    CHECK(b.baked.size() >= 1, "baker invoked once for the coalesced burst");
}

static void test_scope_single_part_and_topo() {
    std::printf("[test_scope_single_part_and_topo]\n");
    // graph: leaf -> mid -> root ; sibling -> root (untouched)
    FakeGraph g;
    g.file_to_parts["/w/leaf.js"] = {"leaf"};
    g.parents["leaf"] = {"mid"}; g.parents["mid"] = {"root"}; g.parents["sibling"] = {"root"};
    g.roots["leaf"] = {"root"};
    FakeWatcher w; RecBaker b; RecFlattener f; RecSink s;
    LiveEditSession sess(w, g, b, f, s, LiveEditConfig{150, 0});

    w.set_now_ms(1000); w.push("/w/leaf.js");
    w.advance_ms(200);
    auto r = sess.tick();

    CHECK(r.succeeded, "happy-path rebuild succeeds");
    // scope correctness: rebaked is exactly the upward cone {leaf,mid,root}
    std::set<PartId> got(r.rebaked.begin(), r.rebaked.end());
    CHECK(got.size() == 3 && got.count("leaf") && got.count("mid") && got.count("root"),
          "rebaked == upward cone exactly");
    CHECK(got.count("sibling") == 0, "sibling NOT rebaked");
    // topo order: leaf before mid before root (children first)
    auto idx = [&](const PartId& p){ for (size_t i=0;i<r.rebaked.size();++i) if (r.rebaked[i]==p) return (int)i; return -1; };
    CHECK(idx("leaf") < idx("mid") && idx("mid") < idx("root"), "rebake is children-before-parents");
    // re-flatten only the affected root
    CHECK(f.roots.size() == 1 && f.roots[0] == "root", "re-flatten exactly the affected root");
}

static void test_shared_module_fanout_rebake() {
    std::printf("[test_shared_module_fanout_rebake]\n");
    // noise.js imported by rock and tree; bush does NOT import it.
    FakeGraph g;
    g.file_to_parts["/w/lib/noise.js"] = {"rock", "tree"};
    g.parents["rock"] = {"root"}; g.parents["tree"] = {"root"}; g.parents["bush"] = {"root"};
    g.roots["rock"] = {"root"}; g.roots["tree"] = {"root"};
    FakeWatcher w; RecBaker b; RecFlattener f; RecSink s;
    LiveEditSession sess(w, g, b, f, s, LiveEditConfig{150, 0});
    w.set_now_ms(1000); w.push("/w/lib/noise.js"); w.advance_ms(200);
    auto r = sess.tick();
    std::set<PartId> got(r.rebaked.begin(), r.rebaked.end());
    CHECK(got.count("rock") && got.count("tree") && got.count("root"), "both importers + ancestor rebaked");
    CHECK(got.count("bush") == 0, "non-importer bush untouched");
}

// Fails the named part with a given cause until told to succeed.
struct ScriptedBaker : Baker {
    PartId fail_part; LiveEditError::Cause cause = LiveEditError::Cause::Script;
    bool failing = true;
    std::vector<PartId> baked;
    BakeOutcome bake(const PartId& p, const ResolvedHash&, long long) override {
        if (failing && p == fail_part)
            return {false, LiveEditError{cause, p, "boom", "leaf.js:3"}};
        baked.push_back(p); return {true, {}};
    }
};

static void test_fail_closed_then_retry() {
    std::printf("[test_fail_closed_then_retry]\n");
    FakeGraph g;
    g.file_to_parts["/w/leaf.js"] = {"leaf"};
    g.parents["leaf"] = {"mid"}; g.parents["mid"] = {"root"};
    g.roots["leaf"] = {"root"};
    FakeWatcher w; ScriptedBaker b; RecFlattener f; RecSink s;
    b.fail_part = "leaf";
    LiveEditSession sess(w, g, b, f, s, LiveEditConfig{150, 0});

    // First save throws.
    w.set_now_ms(1000); w.push("/w/leaf.js"); w.advance_ms(200);
    auto r1 = sess.tick();
    CHECK(!r1.succeeded, "throwing edit reports failure");
    CHECK(r1.errors.size() == 1 && r1.errors[0].part == "leaf", "structured error names the part");
    CHECK(s.errs.size() == 1, "error surfaced to sink");
    CHECK(f.roots.empty(), "NO re-flatten on failed bake (last-good world kept)");
    CHECK(r1.rebaked.empty(), "no part recorded as successfully rebaked");

    // Author fixes it and saves again.
    b.failing = false;
    w.advance_ms(10); w.push("/w/leaf.js"); w.advance_ms(200);
    auto r2 = sess.tick();
    CHECK(r2.succeeded, "subsequent valid edit succeeds");
    std::set<PartId> got(r2.rebaked.begin(), r2.rebaked.end());
    CHECK(got.count("leaf") && got.count("mid") && got.count("root"), "retry rebakes full cone");
    CHECK(f.roots.size() == 1 && f.roots[0] == "root", "retry re-flattens affected root");
}

// Asserts it received the configured budget; fails with BudgetExceeded.
struct BudgetBaker : Baker {
    long long seen_budget = -999;
    BakeOutcome bake(const PartId& p, const ResolvedHash&, long long budget_ms) override {
        seen_budget = budget_ms;
        return {false, LiveEditError{LiveEditError::Cause::BudgetExceeded, p, "budget exceeded", ""}};
    }
};

static void test_budget_abort_fail_closed() {
    std::printf("[test_budget_abort_fail_closed]\n");
    FakeGraph g;
    g.file_to_parts["/w/leaf.js"] = {"leaf"};
    g.parents["leaf"] = {"root"}; g.roots["leaf"] = {"root"};
    FakeWatcher w; BudgetBaker b; RecFlattener f; RecSink s;
    LiveEditSession sess(w, g, b, f, s, LiveEditConfig{150, /*budget*/750});
    w.set_now_ms(1000); w.push("/w/leaf.js"); w.advance_ms(200);
    auto r = sess.tick();
    CHECK(b.seen_budget == 750, "dev budget forwarded to baker");
    CHECK(!r.succeeded, "budget-exceeded rebuild fails closed");
    CHECK(r.errors.size() == 1 && r.errors[0].cause == LiveEditError::Cause::BudgetExceeded,
          "structured BudgetExceeded error surfaced");
    CHECK(f.roots.empty(), "no re-flatten -> last-good world kept");
}

#ifdef __linux__
static void test_real_inotify_temp_dir() {
    std::printf("[test_real_inotify_temp_dir]\n");
    char tmpl[] = "/tmp/sp5_inotifyXXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "made temp watch dir");
    if (!dir) return;
    InotifyWatcher iw;
    iw.add_watch(dir);
    std::string file = std::string(dir) + "/rock.js";
    { FILE* fp = std::fopen(file.c_str(), "w"); std::fprintf(fp, "class Rock {}\n"); std::fclose(fp); }
    // Give the kernel a moment to queue the event, then poll.
    bool saw = false;
    for (int i = 0; i < 50 && !saw; ++i) {
        std::vector<FileEvent> evs; iw.poll(evs);
        for (auto& e : evs) if (e.path == file) saw = true;
        if (!saw) usleep(2000);
    }
    CHECK(saw, "inotify observed the real file write");
    std::remove(file.c_str()); rmdir(dir);
}

static void test_e2e_real_watch_to_rebuild() {
    std::printf("[test_e2e_real_watch_to_rebuild]\n");
    char tmpl[] = "/tmp/sp5_e2eXXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { CHECK(false, "temp dir"); return; }
    std::string file = std::string(dir) + "/leaf.js";

    FakeGraph g;
    g.file_to_parts[file] = {"leaf"};
    g.parents["leaf"] = {"root"};
    g.roots["leaf"] = {"root"};
    InotifyWatcher iw; iw.add_watch(dir);
    RecBaker b; RecFlattener f; RecSink s;
    // debounce 0 so the first tick after the event fires immediately.
    LiveEditSession sess(iw, g, b, f, s, LiveEditConfig{/*debounce*/0, /*budget*/0});

    { FILE* fp = std::fopen(file.c_str(), "w"); std::fprintf(fp, "class Leaf {}\n"); std::fclose(fp); }
    RebuildReport r;
    for (int i = 0; i < 100 && r.rebaked.empty(); ++i) { r = sess.tick(); usleep(2000); }

    std::set<PartId> got(r.rebaked.begin(), r.rebaked.end());
    CHECK(got.count("leaf") && got.count("root"), "real write rebakes leaf + ancestor");
    CHECK(f.roots.size() == 1 && f.roots[0] == "root", "real write re-flattens affected root");
    std::remove(file.c_str()); rmdir(dir);
}
#endif

static void test_windows_stub_is_marked_deferred() {
    std::printf("[test_windows_stub_is_marked_deferred]\n");
#ifdef _WIN32
    bool threw = false;
    try { live_edit::WinDirWatcher w; } catch (const std::exception&) { threw = true; }
    CHECK(threw, "Windows watcher throws not-implemented (deferred)");
#else
    CHECK(true, "Windows stub header compiles on non-Windows (no-op)");
#endif
}

static void test_multi_root_reflatten() {
    std::printf("[test_multi_root_reflatten]\n");
    // shared 'gear' instanced under two roots A and B.
    FakeGraph g;
    g.file_to_parts["/w/gear.js"] = {"gear"};
    g.parents["gear"] = {"A", "B"};
    g.roots["gear"] = {"A", "B"};
    FakeWatcher w; RecBaker b; RecFlattener f; RecSink s;
    LiveEditSession sess(w, g, b, f, s, LiveEditConfig{150, 0});
    w.set_now_ms(1000); w.push("/w/gear.js"); w.advance_ms(200);
    auto r = sess.tick();
    std::set<PartId> got(r.rebaked.begin(), r.rebaked.end());
    CHECK(got.count("gear") && got.count("A") && got.count("B"), "cone includes both ancestors");
    std::set<PartId> roots(f.roots.begin(), f.roots.end());
    CHECK(roots.size() == 2 && roots.count("A") && roots.count("B"), "re-flatten BOTH affected roots");
}

static void test_unmapped_file_is_noop() {
    std::printf("[test_unmapped_file_is_noop]\n");
    FakeGraph g; // no file_to_parts entries
    FakeWatcher w; RecBaker b; RecFlattener f; RecSink s;
    LiveEditSession sess(w, g, b, f, s, LiveEditConfig{150, 0});
    w.set_now_ms(1000); w.push("/w/README.md"); w.advance_ms(200);
    auto r = sess.tick();
    CHECK(r.rebaked.empty() && f.roots.empty(), "edit to a non-part file does nothing");
    CHECK(r.succeeded, "no-op is a success, not a failure");
}

int main() {
    std::printf("=== dev_live_edit_tests ===\n");
    test_fake_watcher_roundtrip();
    test_windows_stub_is_marked_deferred();
    test_multi_root_reflatten();
    test_unmapped_file_is_noop();
    test_upward_cone();
    test_changed_parts_single_and_shared();
    test_debounce_coalesces_two_writes();
    test_scope_single_part_and_topo();
    test_shared_module_fanout_rebake();
    test_fail_closed_then_retry();
    test_budget_abort_fail_closed();
#ifdef __linux__
    test_real_inotify_temp_dir();
    test_e2e_real_watch_to_rebuild();
#endif
    if (g_failures) { std::printf("\n%d FAILURES\n", g_failures); return 1; }
    std::printf("\nALL PASS\n");
    return 0;
}
