# Dev / Creative Live-Edit (SP-5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a file-watch-driven incremental rebuild loop that, on a part-script save, re-resolves the changed part, rebakes only its upward cone in topo order, and re-flattens the affected subtree — reusing SP-2/SP-3/SP-4 with a dev time budget and fail-closed last-good retention.

**Architecture:** A `FileWatcher` interface emits debounced change events (`InotifyWatcher` for Linux, `FakeWatcher` for tests, `ReadDirectoryChangesW` stubbed for Windows). A `LiveEditSession` owns the loop: it maps a changed file to the parts that depend on it, computes the **upward cone** (changed part + all transitive ancestors) against an injected SP-3 `GraphResolver` interface, drives a SCOPED rebake in topo order through an injected `Baker`, then re-flattens each affected root subtree through an injected `Flattener`. Bakes run under SP-2's time budget; any failure keeps the prior `parts/<hash>.part` artifact in place and pushes a structured error to an `ErrorSink`, retrying on the next event. All SP-2/3/4 collaborators are interfaces so the scoping/debounce/last-good logic is unit-testable with fakes; only one task wires the real inotify backend.

**Tech Stack:** C++17, Linux inotify (ReadDirectoryChangesW deferred/stubbed for Windows), depends on SP-2/3/4 interfaces, headless tests under MatterEngine3/tests/.

> **Relocation note (from the master plan):** This sub-plan obeys the `MatterEngine3` relocation contract in `2026-06-24-procedural-part-system-master-plan.md`. All NEW files are under `MatterEngine3/`; the read-only MatterSurfaceLib prototype backend is referenced via `-I../../MatterSurfaceLib/include` and `../../MatterSurfaceLib/src/<dep>`. SP-5 here consumes no prototype sources directly (its seams are SP-2/3/4 interfaces, all new MatterEngine3 files); raylib paths are unchanged (`MatterEngine3/tests` is the same depth as `MatterSurfaceLib/tests`).

---

## File Structure

**New headers (MatterEngine3/include/):**
- `file_watcher.h` — `FileEvent`, `FileEventKind`, abstract `FileWatcher` interface, and `FakeWatcher` (in-header, test-injectable) that lets tests push synthetic events and a clock.
- `inotify_watcher.h` — `InotifyWatcher` declaration (Linux real backend; `#ifdef __linux__`).
- `win_watcher.h` — `WinDirWatcher` declaration, clearly marked **stubbed/deferred** (`#ifdef _WIN32`), throwing `not_implemented` if instantiated.
- `live_edit_interfaces.h` — the SP-3/SP-4/SP-2 seams SP-5 builds on: `PartId`, `ResolvedHash`, `GraphResolver` (re-resolve + ancestors + topo + file→parts reverse map), `Baker` (scoped bake under budget, fail-closed), `Flattener` (re-flatten a root subtree), `ErrorSink` (structured error), `LiveEditError`. Pure-virtual; SP-2/3/4 provide concrete impls at execution time.
- `live_edit.h` — `LiveEditConfig` (debounce window, dev time budget) + `LiveEditSession` (the debounce + upward-cone scoping + rebake + re-flatten + last-good orchestration).

**New sources (MatterEngine3/src/):**
- `inotify_watcher.cpp` — Linux inotify implementation (`inotify_init1`, `inotify_add_watch`, `read()` loop, event→`FileEvent` mapping, debounce coalescing at the source).
- `live_edit.cpp` — `LiveEditSession` implementation (debounce coalescing, `upward_cone()`, topo-ordered scoped rebake, affected-root re-flatten, fail-closed/last-good, retry).

**New tests (MatterEngine3/tests/):**
- `dev_live_edit_tests.cpp` — plain-`main()` CHECK-macro headless suite. Drives `FakeWatcher` + fake `GraphResolver`/`Baker`/`Flattener`/`ErrorSink`. One task additionally drives the **real** `InotifyWatcher` against a temp dir with an actual file touch.

**Modified:**
- `MatterEngine3/tests/Makefile` — append `DEV_TARGET = dev_live_edit_tests` + `SOURCES` + `run-dev` rule (the Makefile already exists from SP-1; do NOT recreate it).
- `build-all.sh` — add `dev_live_edit_tests` to the MatterEngine3 headless suite loop.

---

## Interface contracts (the seams SP-5 depends on)

SP-2/3/4 are not yet implemented. SP-5 codes against these minimal interfaces (declared in `live_edit_interfaces.h`) which the real subsystems implement later. Fakes in the test file implement the same interfaces so all SP-5 logic is testable in isolation.

```cpp
// PartId = stable module identity (script-source hash per SP-3 open question);
// ResolvedHash = the folded transitive hash (SP-1/SP-3) keying parts/<hash>.part.
using PartId       = std::string;
using ResolvedHash = std::string;
```

- `GraphResolver` (SP-3 seam): `parts_for_file(path) -> vector<PartId>` (reverse map: a `.js` part file → the part(s) it defines; a shared-lib module → every importing part), `ancestors(PartId) -> vector<PartId>` (all transitive parents up to roots), `topo_order(set<PartId>) -> vector<PartId>` (children-before-parents over the given subset), `roots_over(set<PartId>) -> vector<PartId>` (the affected root part(s) whose subtree must re-flatten), `reresolve(PartId) -> ResolvedHash` (recompute folded hash from current source).
- `Baker` (SP-2 seam): `bake(PartId, ResolvedHash, time_budget_ms) -> Result` where `Result` is `ok` or a `LiveEditError` (fail-closed; on failure the existing `parts/<hash>.part` is untouched).
- `Flattener` (SP-4 seam): `reflatten(PartId root) -> Result` (re-expands that root's subtree into per-sector instances/BLAS).
- `ErrorSink` (SP-2 structured error): `report(const LiveEditError&)` — console/overlay sink.

---

## Task 1: Test scaffolding + FakeWatcher + CHECK harness

**Files:**
- Create: `MatterEngine3/include/file_watcher.h`
- Create: `MatterEngine3/tests/dev_live_edit_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile` (append; created by SP-1)

- [ ] **Step 1: Write the watcher interface + FakeWatcher (failing-test target needs it to compile)**

Create `MatterEngine3/include/file_watcher.h`:
```cpp
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <deque>

// OS-native file-change abstraction for SP-5 dev live-edit. Real backends:
// InotifyWatcher (Linux), WinDirWatcher (Windows, stubbed). FakeWatcher drives
// tests with synthetic events. See
// docs/superpowers/specs/2026-06-24-dev-live-edit-design.md
namespace live_edit {

enum class FileEventKind { Modified, Created, Deleted };

struct FileEvent {
    std::string path;        // absolute path of the changed script file
    FileEventKind kind = FileEventKind::Modified;
    long long t_ms = 0;      // monotonic event time in milliseconds
};

// A watcher emits FileEvents for files under the watched directories. poll()
// returns events that have arrived since the last call (already coalesced by
// the backend where cheap; LiveEditSession applies the debounce window).
class FileWatcher {
public:
    virtual ~FileWatcher() = default;
    virtual void add_watch(const std::string& dir) = 0;
    // Non-blocking: append newly observed events to `out`. Returns count added.
    virtual int poll(std::vector<FileEvent>& out) = 0;
    // Monotonic clock the session uses for debounce. Real backends use a steady
    // clock; the fake lets tests advance it deterministically.
    virtual long long now_ms() = 0;
};

// Test double: tests push events and control the clock; no OS involvement.
class FakeWatcher : public FileWatcher {
public:
    void add_watch(const std::string& dir) override { watched_.push_back(dir); }
    int poll(std::vector<FileEvent>& out) override {
        int n = 0;
        while (!pending_.empty()) { out.push_back(pending_.front()); pending_.pop_front(); ++n; }
        return n;
    }
    long long now_ms() override { return clock_ms_; }

    // --- test controls ---
    void push(const std::string& path, FileEventKind k = FileEventKind::Modified) {
        pending_.push_back(FileEvent{path, k, clock_ms_});
    }
    void advance_ms(long long d) { clock_ms_ += d; }
    void set_now_ms(long long t) { clock_ms_ = t; }
    const std::vector<std::string>& watched() const { return watched_; }
private:
    std::deque<FileEvent> pending_;
    std::vector<std::string> watched_;
    long long clock_ms_ = 0;
};

} // namespace live_edit
```

- [ ] **Step 2: Write the first failing test (harness + FakeWatcher round-trip)**

Create `MatterEngine3/tests/dev_live_edit_tests.cpp`:
```cpp
// Headless SP-5 dev live-edit tests. plain main(), no GL. Drives FakeWatcher +
// fake SP-2/3/4 seams. See docs/superpowers/specs/2026-06-24-dev-live-edit-design.md
#include "file_watcher.h"
#include <cstdio>
#include <cstdlib>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_failures; } \
    else { std::printf("  ok: %s\n", msg); } } while (0)

using namespace live_edit;

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

int main() {
    std::printf("=== dev_live_edit_tests ===\n");
    test_fake_watcher_roundtrip();
    if (g_failures) { std::printf("\n%d FAILURES\n", g_failures); return 1; }
    std::printf("\nALL PASS\n");
    return 0;
}
```

- [ ] **Step 3: Append the Makefile target**

The `MatterEngine3/tests/Makefile` already exists (created by SP-1). Append this plan's target after the existing `run-*` rules (do NOT recreate the Makefile or edit `MatterSurfaceLib/tests/Makefile`):
```make
# SP-5 dev live-edit: debounce + upward-cone scoping + last-good (headless, GL-free).
# All-new MatterEngine3 sources; inotify_watcher.cpp is Linux-only (guarded by __linux__ internally).
DEV_TARGET = dev_live_edit_tests
DEV_SOURCES = dev_live_edit_tests.cpp ../src/live_edit.cpp ../src/inotify_watcher.cpp

$(DEV_TARGET): $(DEV_SOURCES)
	$(CC) $(DEV_SOURCES) -o $(DEV_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-dev: $(DEV_TARGET)
	./$(DEV_TARGET)
```
Also add `run-dev` to the `.PHONY` line and `$(DEV_TARGET)` to the `clean` rule's `rm -f` list.

> NOTE: `live_edit.cpp` and `inotify_watcher.cpp` do not exist yet, so the target will not build until Task 2/Task 8. To run this first test in isolation before those exist, temporarily build only the test file:
> `cd MatterEngine3/tests && g++ -std=c++17 -I../include dev_live_edit_tests.cpp -o dev_live_edit_tests`

- [ ] **Step 4: Run — expect this isolated compile+run to PASS** (it only exercises the header-only FakeWatcher)

Run: `cd MatterEngine3/tests && g++ -std=c++17 -I../include dev_live_edit_tests.cpp -o dev_live_edit_tests && ./dev_live_edit_tests`
Expected: `ALL PASS`. (This confirms the harness + FakeWatcher; later tasks add `live_edit.cpp`/`inotify_watcher.cpp` and switch to the Makefile target.)

- [ ] **Step 5: Commit**

Run:
```bash
git add MatterEngine3/include/file_watcher.h MatterEngine3/tests/dev_live_edit_tests.cpp MatterEngine3/tests/Makefile
git commit -m "$(cat <<'EOF'
test: scaffold SP-5 dev live-edit suite + FakeWatcher

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Interface seams + LiveEditSession skeleton (no scoping yet)

**Files:**
- Create: `MatterEngine3/include/live_edit_interfaces.h`
- Create: `MatterEngine3/include/live_edit.h`
- Create: `MatterEngine3/src/live_edit.cpp`
- Create: `MatterEngine3/src/inotify_watcher.cpp` (Linux real impl stub for now so the Makefile target links; filled in Task 8)
- Modify: `MatterEngine3/tests/dev_live_edit_tests.cpp`

- [ ] **Step 1: Write the SP-2/3/4 seam interfaces**

Create `MatterEngine3/include/live_edit_interfaces.h`:
```cpp
#pragma once
#include <string>
#include <vector>
#include <set>

// The SP-2/SP-3/SP-4 collaboration seams SP-5 depends on. SP-2/3/4 provide
// concrete implementations at execution time; SP-5 codes against these so its
// scoping/debounce/last-good logic is unit-testable with fakes.
namespace live_edit {

using PartId       = std::string;  // stable module identity (SP-3 source hash)
using ResolvedHash = std::string;  // folded transitive hash (SP-1/SP-3) keying parts/<hash>.part

// Structured error surfaced fail-closed (SP-2). `where` is best-effort source loc.
struct LiveEditError {
    enum class Cause { Script, SessionMisuse, BudgetExceeded, ResolveFailed, FlattenFailed };
    Cause cause = Cause::Script;
    PartId part;          // the part whose bake/flatten failed
    std::string message;  // human-readable
    std::string where;    // best-effort "file:line"
};

struct BakeOutcome { bool ok = false; LiveEditError error; };

// SP-3 seam: resolve + reverse-map + ancestors + topo + affected roots.
class GraphResolver {
public:
    virtual ~GraphResolver() = default;
    // The part(s) defined by, or importing, this script/shared-lib file.
    virtual std::vector<PartId> parts_for_file(const std::string& path) = 0;
    // All transitive parents of `p`, up to the root(s) (excludes p).
    virtual std::vector<PartId> ancestors(const PartId& p) = 0;
    // Children-before-parents order over exactly the given subset.
    virtual std::vector<PartId> topo_order(const std::set<PartId>& subset) = 0;
    // The root part(s) whose subtree must re-flatten given the changed set.
    virtual std::vector<PartId> roots_over(const std::set<PartId>& changed) = 0;
    // Recompute the folded resolved hash of `p` from current source.
    virtual ResolvedHash reresolve(const PartId& p) = 0;
};

// SP-2 seam: scoped bake under a time budget, fail-closed.
class Baker {
public:
    virtual ~Baker() = default;
    // Bake `p` (already resolved to `h`). budget_ms<=0 means unbounded.
    // On failure the existing parts/<h>.part is left untouched (fail-closed).
    virtual BakeOutcome bake(const PartId& p, const ResolvedHash& h, long long budget_ms) = 0;
};

// SP-4 seam: re-flatten one affected root's subtree into per-sector instances/BLAS.
class Flattener {
public:
    virtual ~Flattener() = default;
    virtual BakeOutcome reflatten(const PartId& root) = 0;
};

// SP-2 structured-error sink (console / on-screen overlay).
class ErrorSink {
public:
    virtual ~ErrorSink() = default;
    virtual void report(const LiveEditError& e) = 0;
};

} // namespace live_edit
```

- [ ] **Step 2: Write the session header**

Create `MatterEngine3/include/live_edit.h`:
```cpp
#pragma once
#include "file_watcher.h"
#include "live_edit_interfaces.h"
#include <set>
#include <vector>

namespace live_edit {

struct LiveEditConfig {
    long long debounce_ms = 150;   // coalesce saves within this window into one rebuild
    long long bake_budget_ms = 2000; // dev time budget per bake (SP-2); <=0 = unbounded
};

// Result of one processed rebuild pass (for test instrumentation).
struct RebuildReport {
    std::vector<PartId> rebaked;       // exactly the upward cone, topo order
    std::vector<PartId> reflattened;   // affected roots
    bool succeeded = true;             // false => fail-closed, last-good kept
    std::vector<LiveEditError> errors; // structured errors surfaced this pass
};

// Owns the dev live-edit loop. Pulls debounced events from the watcher, maps
// each changed file to its parts (SP-3 reverse map), computes the upward cone,
// rebakes it in topo order under the dev budget (SP-2), then re-flattens each
// affected root subtree (SP-4). Fail-closed: a failed bake/flatten keeps the
// last-good artifact and surfaces a structured error; retried on next event.
class LiveEditSession {
public:
    LiveEditSession(FileWatcher& w, GraphResolver& g, Baker& b, Flattener& f,
                    ErrorSink& sink, LiveEditConfig cfg)
        : w_(w), g_(g), b_(b), f_(f), sink_(sink), cfg_(cfg) {}

    // Drain ready events, apply debounce, and run at most one rebuild pass for
    // the coalesced change set whose quiet window has elapsed. Returns the
    // report for the pass that ran (succeeded defaults true with empty sets if
    // nothing was ready). Call once per host tick.
    RebuildReport tick();

    // Pure scoping helper (exposed for unit tests): the upward cone of a set of
    // directly-changed parts = the changed parts + all their transitive
    // ancestors, returned as a set.
    std::set<PartId> upward_cone(const std::vector<PartId>& changed) const;

private:
    FileWatcher&  w_;
    GraphResolver& g_;
    Baker&        b_;
    Flattener&    f_;
    ErrorSink&    sink_;
    LiveEditConfig cfg_;

    // Pending debounce state: paths seen and the last event time among them.
    std::set<std::string> pending_paths_;
    long long last_event_ms_ = 0;
    bool have_pending_ = false;

    RebuildReport run_rebuild(const std::set<std::string>& paths);
};

} // namespace live_edit
```

- [ ] **Step 3: Write a minimal source (skeleton: upward_cone + empty tick) — just enough to fail the next test meaningfully**

Create `MatterEngine3/src/live_edit.cpp`:
```cpp
#include "live_edit.h"

namespace live_edit {

std::set<PartId> LiveEditSession::upward_cone(const std::vector<PartId>& changed) const {
    std::set<PartId> cone(changed.begin(), changed.end());
    for (const auto& p : changed) {
        for (const auto& a : g_.ancestors(p)) cone.insert(a);
    }
    return cone;
}

RebuildReport LiveEditSession::run_rebuild(const std::set<std::string>&) {
    return RebuildReport{}; // filled in Task 5
}

RebuildReport LiveEditSession::tick() {
    return RebuildReport{}; // filled in Task 4
}

} // namespace live_edit
```

- [ ] **Step 4: Create a placeholder inotify source so the Makefile target links on Linux**

Create `MatterEngine3/src/inotify_watcher.cpp`:
```cpp
#include "inotify_watcher.h"
// Real Linux inotify backend implemented in Task 8. Header defines the class
// guarded by __linux__; this TU is intentionally empty until then so the test
// Makefile target links without a real backend (tests use FakeWatcher).
```
And create `MatterEngine3/include/inotify_watcher.h`:
```cpp
#pragma once
#include "file_watcher.h"
#ifdef __linux__
namespace live_edit {
// Linux inotify-backed FileWatcher. Implemented in Task 8.
class InotifyWatcher : public FileWatcher {
public:
    InotifyWatcher();
    ~InotifyWatcher() override;
    void add_watch(const std::string& dir) override;
    int poll(std::vector<FileEvent>& out) override;
    long long now_ms() override;
private:
    int fd_ = -1;
};
} // namespace live_edit
#endif // __linux__
```

- [ ] **Step 5: Add a failing test for `upward_cone` using a fake resolver**

In `dev_live_edit_tests.cpp`, add the includes and a fake resolver after the CHECK macro:
```cpp
#include "live_edit.h"
#include <map>

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
```
And call `test_upward_cone();` in `main()`.

- [ ] **Step 6: Run via the Makefile target — expect PASS**

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: `ALL PASS` (both `test_fake_watcher_roundtrip` and `test_upward_cone`).

- [ ] **Step 7: Commit**

Run:
```bash
git add MatterEngine3/include/live_edit_interfaces.h MatterEngine3/include/live_edit.h MatterEngine3/include/inotify_watcher.h MatterEngine3/src/live_edit.cpp MatterEngine3/src/inotify_watcher.cpp MatterEngine3/tests/dev_live_edit_tests.cpp
git commit -m "$(cat <<'EOF'
feat: SP-5 interface seams + upward-cone scoping helper

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: File → parts reverse mapping (single-part vs shared-module)

Covers spec testing bullets **single-part edit scope** (file maps to one part) and the mapping half of **shared-module edit fan-out** (file maps to many importers).

**Files:**
- Modify: `MatterEngine3/tests/dev_live_edit_tests.cpp`
- Modify: `MatterEngine3/src/live_edit.cpp`

- [ ] **Step 1: Add a failing test for the changed-set assembly from a set of paths**

Add to `dev_live_edit_tests.cpp` a small helper exposed on the session. First add a failing test that exercises a new public method `LiveEditSession::changed_parts(const std::set<std::string>&)`:
```cpp
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
```
Call `test_changed_parts_single_and_shared();` in `main()`.

- [ ] **Step 2: Run — expect FAIL (no `changed_parts` member yet → compile error)**

Run: `make -C MatterEngine3/tests dev_live_edit_tests`
Expected: compile error `no member named 'changed_parts'`.

- [ ] **Step 3: Declare + implement `changed_parts`**

In `live_edit.h`, add to the public section:
```cpp
    // Map a set of changed file paths to the directly-changed parts (SP-3
    // reverse map). A shared-lib module fans out to all importers; duplicates
    // across files are de-duplicated.
    std::set<PartId> changed_parts(const std::set<std::string>& paths) const;
```
In `live_edit.cpp`, add:
```cpp
std::set<PartId> LiveEditSession::changed_parts(const std::set<std::string>& paths) const {
    std::set<PartId> out;
    for (const auto& path : paths)
        for (const auto& p : g_.parts_for_file(path)) out.insert(p);
    return out;
}
```

- [ ] **Step 4: Run — expect PASS**

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: `ALL PASS`.

- [ ] **Step 5: Commit**

Run:
```bash
git add MatterEngine3/include/live_edit.h MatterEngine3/src/live_edit.cpp MatterEngine3/tests/dev_live_edit_tests.cpp
git commit -m "$(cat <<'EOF'
feat: SP-5 file-to-parts reverse mapping (single + shared module)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Debounce coalescing

Covers spec testing bullet **Debounce: two writes within the window → one rebuild**.

**Files:**
- Modify: `MatterEngine3/tests/dev_live_edit_tests.cpp`
- Modify: `MatterEngine3/src/live_edit.cpp`

- [ ] **Step 1: Add an instrumented fake baker that records calls (used here + later)**

Add to `dev_live_edit_tests.cpp` (top-level, after `FakeGraph`):
```cpp
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
```

- [ ] **Step 2: Add a failing debounce test**

```cpp
static void test_debounce_coalesces_two_writes() {
    std::printf("[test_debounce_coalesces_two_writes]\n");
    FakeGraph g;
    g.file_to_parts["/w/rock.js"] = {"rock"};
    g.parents["rock"] = {"root"};
    g.roots["rock"] = {"root"}; // see roots_over fake below
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
    // Count distinct rebuild passes by rebaked-cone non-empty: only r3 ran.
}
```
This requires the `FakeGraph` to support `roots_over`/`topo_order`; extend `FakeGraph` now:
```cpp
    std::map<PartId, std::vector<PartId>> roots; // p -> affected roots (test-set)
    // replace the earlier stub implementations:
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
```
(Add `#include <algorithm>` to the test file.)

- [ ] **Step 3: Run — expect FAIL (`tick()` is a no-op skeleton)**

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: FAIL on "after quiet window: exactly one rebuild fires" (tick returns empty).

- [ ] **Step 4: Implement debounce in `tick()`**

Replace `tick()` in `live_edit.cpp`:
```cpp
RebuildReport LiveEditSession::tick() {
    // 1. Drain newly observed events into the pending debounce set.
    std::vector<FileEvent> evs;
    w_.poll(evs);
    for (const auto& e : evs) {
        pending_paths_.insert(e.path);
        last_event_ms_ = (e.t_ms > last_event_ms_) ? e.t_ms : last_event_ms_;
        have_pending_ = true;
    }
    // 2. If nothing pending, nothing to do.
    if (!have_pending_) return RebuildReport{};
    // 3. Only fire once the quiet window has elapsed since the last event.
    if (w_.now_ms() - last_event_ms_ < cfg_.debounce_ms) return RebuildReport{};
    // 4. Quiet window elapsed: run ONE rebuild for the whole coalesced set.
    std::set<std::string> paths;
    paths.swap(pending_paths_);
    have_pending_ = false;
    last_event_ms_ = 0;
    return run_rebuild(paths);
}
```

- [ ] **Step 5: Run — expect PASS**

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: `ALL PASS`.

- [ ] **Step 6: Commit**

Run:
```bash
git add MatterEngine3/src/live_edit.cpp MatterEngine3/tests/dev_live_edit_tests.cpp
git commit -m "$(cat <<'EOF'
feat: SP-5 debounce coalescing of rapid saves into one rebuild

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Scoped rebake in topo order + re-flatten affected roots (happy path)

Covers spec testing bullets **single-part edit scope** (rebake cone + re-flatten affected, others untouched) and **scope correctness** (rebaked == exactly the upward cone), plus **shared-module fan-out** rebake.

**Files:**
- Modify: `MatterEngine3/tests/dev_live_edit_tests.cpp`
- Modify: `MatterEngine3/src/live_edit.cpp`

- [ ] **Step 1: Add failing tests for scope + topo order + re-flatten**

```cpp
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
```
Call both in `main()`.

- [ ] **Step 2: Run — expect FAIL (`run_rebuild` returns empty)**

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: FAIL (rebaked empty, f.roots empty).

- [ ] **Step 3: Implement `run_rebuild` happy path**

Replace `run_rebuild` in `live_edit.cpp`:
```cpp
RebuildReport LiveEditSession::run_rebuild(const std::set<std::string>& paths) {
    RebuildReport rep;
    // 1. Map changed files -> directly-changed parts (SP-3 reverse map).
    std::set<PartId> changed = changed_parts(paths);
    if (changed.empty()) return rep;  // unmapped file (e.g. non-part) -> no-op

    // 2. Upward cone: changed parts + all their transitive ancestors.
    std::vector<PartId> changed_v(changed.begin(), changed.end());
    std::set<PartId> cone = upward_cone(changed_v);

    // 3. Topo order (children-before-parents) over exactly the cone.
    std::vector<PartId> order = g_.topo_order(cone);

    // 4. Re-resolve + bake each in order under the dev budget (SP-2).
    for (const auto& p : order) {
        ResolvedHash h = g_.reresolve(p);
        BakeOutcome o = b_.bake(p, h, cfg_.bake_budget_ms);
        if (!o.ok) {                       // fail-closed handled in Task 6/7
            rep.succeeded = false;
            rep.errors.push_back(o.error);
            sink_.report(o.error);
            return rep;                    // stop; last-good kept downstream
        }
        rep.rebaked.push_back(p);
    }

    // 5. Re-flatten each affected root's subtree (SP-4).
    for (const auto& root : g_.roots_over(changed)) {
        BakeOutcome o = f_.reflatten(root);
        if (!o.ok) { rep.succeeded = false; rep.errors.push_back(o.error); sink_.report(o.error); return rep; }
        rep.reflattened.push_back(root);
    }
    return rep;
}
```

- [ ] **Step 4: Run — expect PASS**

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: `ALL PASS`.

- [ ] **Step 5: Commit**

Run:
```bash
git add MatterEngine3/src/live_edit.cpp MatterEngine3/tests/dev_live_edit_tests.cpp
git commit -m "$(cat <<'EOF'
feat: SP-5 scoped topo rebake of upward cone + affected-root re-flatten

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Fail-closed last-good retention + retry on next save

Covers spec testing bullet **Fail-closed retry: an edit that throws keeps last-good and reports the error; a subsequent valid edit succeeds and updates the world.**

**Files:**
- Modify: `MatterEngine3/tests/dev_live_edit_tests.cpp`
- Modify: `MatterEngine3/src/live_edit.cpp`

The Baker is the fail-closed boundary (SP-2 guarantees the prior `parts/<hash>.part` is untouched on failure). SP-5's responsibility: do NOT re-flatten when a bake fails (so the world keeps the last-good composition), surface the error, and stay ready so the next event retries cleanly.

- [ ] **Step 1: Add a programmable fake baker that can fail once then succeed**

Add to `dev_live_edit_tests.cpp`:
```cpp
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
```

- [ ] **Step 2: Add a failing test for fail-closed then retry**

```cpp
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
```
Call it in `main()`.

- [ ] **Step 3: Run — expect PASS already, or FAIL only if residual debounce state leaks**

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: the Task-5 `run_rebuild` already returns early on `!o.ok` (no re-flatten, error surfaced). This test should PASS. If the retry leg FAILS, the cause is debounce state not reset on the failure path — fix by ensuring `tick()` already cleared `pending_paths_`/`have_pending_` before calling `run_rebuild` (it does in Task 4). Confirm and, if needed, add the explicit reset below.

- [ ] **Step 4: (If the retry leg failed) make the pending-state reset explicit before rebuild**

Confirm in `live_edit.cpp` `tick()` that `pending_paths_` is swapped out and `have_pending_=false` set **before** `run_rebuild` is invoked (so a failed rebuild does not strand stale pending state). This was implemented in Task 4 Step 4; no change needed if the test passes. If it failed, ensure the swap/reset lines precede the `return run_rebuild(paths);` call.

- [ ] **Step 5: Commit**

Run:
```bash
git add MatterEngine3/src/live_edit.cpp MatterEngine3/tests/dev_live_edit_tests.cpp
git commit -m "$(cat <<'EOF'
test: SP-5 fail-closed keeps last-good, retries on next save

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Dev time-budget abort

Covers spec testing bullet **Budget abort: an edit that exceeds the dev time budget aborts with a structured error, last-good kept.**

**Files:**
- Modify: `MatterEngine3/tests/dev_live_edit_tests.cpp`
- Modify: `MatterEngine3/src/live_edit.cpp`

The budget value is owned by `LiveEditConfig.bake_budget_ms` and passed to `Baker::bake`. SP-2 enforces the wall-clock abort and returns `Cause::BudgetExceeded`; SP-5's job is to (a) actually pass the configured budget (set in dev, unbounded `<=0` for install reuse) and (b) treat a budget error like any fail-closed bake error.

- [ ] **Step 1: Add a failing test asserting the budget is forwarded and abort is fail-closed**

```cpp
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
```
Call it in `main()`.

- [ ] **Step 2: Run — expect PASS** (the budget is already forwarded in Task 5's `bake(p, h, cfg_.bake_budget_ms)` and budget errors hit the same fail-closed path)

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: `ALL PASS`. This task is a guard test confirming the budget wiring; if `seen_budget` is wrong, fix the `bake(...)` call site in `run_rebuild` to pass `cfg_.bake_budget_ms`.

- [ ] **Step 3: Commit**

Run:
```bash
git add MatterEngine3/tests/dev_live_edit_tests.cpp MatterEngine3/src/live_edit.cpp
git commit -m "$(cat <<'EOF'
test: SP-5 dev time-budget forwarded + budget abort fails closed

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Real Linux inotify backend + temp-dir touch test

Implements the real watcher and tests it against an actual file touch (the one task using a real OS event, per the hard rules).

**Files:**
- Modify: `MatterEngine3/src/inotify_watcher.cpp`
- Modify: `MatterEngine3/tests/dev_live_edit_tests.cpp`

- [ ] **Step 1: Implement the inotify backend**

Replace `MatterEngine3/src/inotify_watcher.cpp`:
```cpp
#include "inotify_watcher.h"
#ifdef __linux__
#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <climits>
#include <unordered_map>
#include <cstring>

namespace live_edit {

// wd->dir lets us rebuild absolute paths from per-event file names. Stored as a
// member (declared `void* dirs_` in the header to keep <unordered_map> out of
// it); accessed via this helper.
static std::unordered_map<int, std::string>& wd_map(void*& opaque) {
    if (!opaque) opaque = new std::unordered_map<int, std::string>();
    return *static_cast<std::unordered_map<int, std::string>*>(opaque);
}

InotifyWatcher::InotifyWatcher() {
    fd_ = inotify_init1(IN_NONBLOCK);
}
InotifyWatcher::~InotifyWatcher() {
    if (fd_ >= 0) ::close(fd_);
    delete static_cast<std::unordered_map<int, std::string>*>(dirs_);
}

void InotifyWatcher::add_watch(const std::string& dir) {
    if (fd_ < 0) return;
    int wd = inotify_add_watch(fd_, dir.c_str(),
                               IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
    if (wd >= 0) wd_map(dirs_)[wd] = dir;
}

int InotifyWatcher::poll(std::vector<FileEvent>& out) {
    if (fd_ < 0) return 0;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    int added = 0;
    for (;;) {
        ssize_t len = ::read(fd_, buf, sizeof(buf));
        if (len <= 0) break;  // EAGAIN (NONBLOCK) or EOF -> done draining
        for (char* p = buf; p < buf + len; ) {
            auto* ev = reinterpret_cast<struct inotify_event*>(p);
            if (ev->len > 0) {
                auto it = wd_map(dirs_).find(ev->wd);
                std::string dir = (it == wd_map(dirs_).end()) ? std::string() : it->second;
                std::string path = dir.empty() ? std::string(ev->name) : dir + "/" + ev->name;
                FileEventKind k = FileEventKind::Modified;
                if (ev->mask & (IN_CREATE | IN_MOVED_TO))      k = FileEventKind::Created;
                else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) k = FileEventKind::Deleted;
                out.push_back(FileEvent{path, k, now_ms()});
                ++added;
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return added;
}

long long InotifyWatcher::now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

} // namespace live_edit
#endif // __linux__
```
And add the `dirs_` member to `inotify_watcher.h` inside the `__linux__` class: `void* dirs_ = nullptr;`.

- [ ] **Step 2: Add a real-event test (Linux-only, guarded)**

In `dev_live_edit_tests.cpp`, add after the includes:
```cpp
#include "inotify_watcher.h"
#include <cstdio>
#ifdef __linux__
#include <cstdlib>
#include <unistd.h>
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
#endif
```
Guard the call in `main()`:
```cpp
#ifdef __linux__
    test_real_inotify_temp_dir();
#endif
```

- [ ] **Step 3: Run — expect PASS on Linux**

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: `ALL PASS`, including `inotify observed the real file write`.

- [ ] **Step 4: Commit**

Run:
```bash
git add MatterEngine3/include/inotify_watcher.h MatterEngine3/src/inotify_watcher.cpp MatterEngine3/tests/dev_live_edit_tests.cpp
git commit -m "$(cat <<'EOF'
feat: SP-5 Linux inotify watcher backend + real temp-dir touch test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: End-to-end loop with InotifyWatcher driving the session

Wires the real watcher into a `LiveEditSession` and verifies a real file write flows all the way through (debounce → cone → bake → reflatten) with fakes on the SP-2/3/4 side. Confirms the watcher abstraction is interchangeable.

**Files:**
- Modify: `MatterEngine3/tests/dev_live_edit_tests.cpp`

- [ ] **Step 1: Add a failing end-to-end test (Linux-only)**

```cpp
#ifdef __linux__
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
```
Guard the call in `main()` under `#ifdef __linux__`.

- [ ] **Step 2: Run — expect PASS** (all pieces exist; this is an integration check)

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: `ALL PASS`. If the loop times out, the debounce `now_ms()` source on `InotifyWatcher` (monotonic clock) and the event `t_ms` must be on the same clock — they are (both `InotifyWatcher::now_ms`). With `debounce_ms=0` the first tick after the event fires.

- [ ] **Step 3: Commit**

Run:
```bash
git add MatterEngine3/tests/dev_live_edit_tests.cpp
git commit -m "$(cat <<'EOF'
test: SP-5 end-to-end real-inotify write drives scoped rebuild

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Windows ReadDirectoryChangesW stub (deferred, clearly marked)

Per the hard rules: Windows backend is stubbed/deferred; the Linux path is fully implemented above. This task lands the marked stub so the interface is complete and the Windows build has a definite "not implemented" failure rather than a missing symbol.

**Files:**
- Create: `MatterEngine3/include/win_watcher.h`
- Modify: `MatterEngine3/tests/dev_live_edit_tests.cpp`

- [ ] **Step 1: Write the marked Windows stub header**

Create `MatterEngine3/include/win_watcher.h`:
```cpp
#pragma once
#include "file_watcher.h"
#ifdef _WIN32
#include <stdexcept>
namespace live_edit {
// DEFERRED / STUBBED (SP-5): Windows ReadDirectoryChangesW backend is not yet
// implemented. The Linux inotify backend is the fully-implemented v1 path. This
// stub makes the unimplemented state explicit (throws) instead of silently
// no-op'ing. Implement with an overlapped ReadDirectoryChangesW loop mapping
// FILE_NOTIFY_INFORMATION -> FileEvent, mirroring InotifyWatcher.
class WinDirWatcher : public FileWatcher {
public:
    WinDirWatcher() { throw std::runtime_error("WinDirWatcher: ReadDirectoryChangesW backend not implemented (SP-5 deferred)"); }
    void add_watch(const std::string&) override {}
    int poll(std::vector<FileEvent>&) override { return 0; }
    long long now_ms() override { return 0; }
};
} // namespace live_edit
#endif // _WIN32
```

- [ ] **Step 2: Add a guard test that compiles everywhere but only asserts on Windows**

In `dev_live_edit_tests.cpp` add:
```cpp
#include "win_watcher.h"
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
```
Call it in `main()`.

- [ ] **Step 3: Run — expect PASS on Linux**

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: `ALL PASS` (the non-Windows branch is a trivial true).

- [ ] **Step 4: Commit**

Run:
```bash
git add MatterEngine3/include/win_watcher.h MatterEngine3/tests/dev_live_edit_tests.cpp
git commit -m "$(cat <<'EOF'
feat: SP-5 marked Windows ReadDirectoryChangesW stub (deferred)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Multi-root scope + created/deleted event mapping (open-question coverage)

Resolves two spec open questions: a part instanced under multiple roots re-flattens **each** affected root; and created/deleted file events map to add/remove (here: an unmapped/new path that resolves to a part still drives the cone; a path that maps to no part is a no-op). This hardens **scope correctness** for the fan-out-to-multiple-roots case.

**Files:**
- Modify: `MatterEngine3/tests/dev_live_edit_tests.cpp`
- Modify: `MatterEngine3/src/live_edit.cpp` (only if a fix is needed)

- [ ] **Step 1: Add failing tests for multi-root re-flatten + no-op on unmapped file**

```cpp
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
```
Call both in `main()`.

- [ ] **Step 2: Run — expect PASS** (Task 5's `run_rebuild` already iterates all `roots_over` and early-returns on empty `changed`)

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: `ALL PASS`. If multi-root fails, ensure `run_rebuild` loops over the full `g_.roots_over(changed)` vector (it does). If the no-op test fails, ensure the `if (changed.empty()) return rep;` guard with `rep.succeeded` defaulting true is present.

- [ ] **Step 3: Commit**

Run:
```bash
git add MatterEngine3/tests/dev_live_edit_tests.cpp MatterEngine3/src/live_edit.cpp
git commit -m "$(cat <<'EOF'
test: SP-5 multi-root re-flatten + non-part file no-op

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: Wire the suite into build-all.sh + final full-suite run

**Files:**
- Modify: `build-all.sh`

- [ ] **Step 1: Add `dev_live_edit_tests` to the headless suite loop**

In `build-all.sh`, append `dev_live_edit_tests` to the **MatterEngine3** headless suite list (SP-1 added the first MatterEngine3 entry, `part_asset_v2_tests`). Do NOT touch the MatterSurfaceLib prototype's suite loop. Extend the MatterEngine3 loop so it reads:
```sh
    for suite in part_asset_v2_tests dev_live_edit_tests; do
```
(Append `dev_live_edit_tests` to whatever the MatterEngine3 loop lists at this point; later sub-plans append their suites too.)

- [ ] **Step 2: Run the full headless suite to confirm integration**

Run: `./build-all.sh test 2>&1 | tail -30`
Expected: a `--- MatterEngine3 (dev_live_edit_tests) ---` section reporting `ALL PASS`, and no `FAIL` for MatterEngine3.

- [ ] **Step 3: Run the dedicated target one more time for a clean signal**

Run: `make -C MatterEngine3/tests dev_live_edit_tests && ./MatterEngine3/tests/dev_live_edit_tests`
Expected: `ALL PASS` covering every test: fake-watcher round-trip, upward-cone, changed-parts (single + shared), debounce, scope/topo + re-flatten, shared-module fan-out, fail-closed retry, budget abort, real inotify touch, e2e real-watch rebuild, Windows stub, multi-root, non-part no-op.

- [ ] **Step 4: Commit**

Run:
```bash
git add build-all.sh
git commit -m "$(cat <<'EOF'
build: run SP-5 dev_live_edit_tests in build-all headless suite

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Spec testing-coverage checklist

Every bullet in the spec's Testing section is covered:

- **Single-part edit (cone rebake, siblings stay hits, affected re-flatten)** → Task 5 `test_scope_single_part_and_topo`.
- **Shared-module edit fan-out (importers + ancestors rebake, non-importers untouched)** → Task 3 `test_changed_parts_single_and_shared` (mapping) + Task 5 `test_shared_module_fanout_rebake` (rebake).
- **Debounce coalescing (two writes → one rebuild)** → Task 4 `test_debounce_coalesces_two_writes`.
- **Fail-closed retry (throw keeps last-good + error; next valid edit succeeds)** → Task 6 `test_fail_closed_then_retry`.
- **Budget abort (exceed dev budget → structured error, last-good kept)** → Task 7 `test_budget_abort_fail_closed`.
- **Scope correctness (rebaked == exactly the upward cone)** → Task 5 `test_scope_single_part_and_topo` (set equality + sibling exclusion) and Task 2 `test_upward_cone` (unit).

Plus: real OS event (Task 8/9), Windows-deferred marker (Task 10), and open-question hardening (Task 11).
