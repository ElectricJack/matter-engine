# Part Graph, Build-as-Cache-Miss & Install (SP-3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Orchestrate SP-2 part bakes by resolving a content-addressed dependency graph leaves-first, baking each unique part exactly once on a cache miss, in single-threaded topological order, rooted at a world manifest.

**Project:** MatterEngine3 (consumes MatterSurfaceLib read-only)

**Architecture:** All new code lives in the **new sibling project `MatterEngine3/`**, which consumes the `MatterSurfaceLib/` prototype **read-only** (never modified). `PartGraph` discovers child deps from each module's `static requires` (top-level eval, no `build()`), folds child resolved-hashes into each part's identity via SP-1 `compute_resolved_hash`, memoizes resolve over `(source_hash, canonical_params)`, detects cycles as back-edges on the resolution stack, and walks topo order baking only parts whose `parts/<hash>.part` is absent. All graph logic sits behind a `Baker` seam and a `ModuleResolver` seam so resolve/dedup/topo/cycle/reachability/cache-miss are unit-tested with fakes; a thin final task wires the real SP-2 `ScriptHost` and SP-1 `cache_path_resolved`.

**Tech Stack:** C++17, depends on SP-1 part_asset v2 (`compute_resolved_hash`, `cache_path_resolved` — new `MatterEngine3/part_asset_v2.h`) + SP-2 ScriptHost (`resolve_hash`, `bake_source`, `eval_requires` — new `MatterEngine3/script_host.h`; SP-2 is the hash authority per master C-2; SP-3 obtains hashes through these, never computing them itself). The consumed prototype backend (`part_asset.cpp` v1 for `fnv1a64`, BLAS/TLAS managers, etc.) is referenced read-only from MatterSurfaceLib. Headless tests under `MatterEngine3/tests/` matching the existing assert/style (`CHECK` macro + `failures` counter + return code).

> **Relocation note (from the master plan):** This sub-plan obeys the `MatterEngine3` relocation contract in `2026-06-24-procedural-part-system-master-plan.md`. All NEW files (`part_graph.{h,cpp}`, `part_graph_tests.cpp`) are under `MatterEngine3/`; SP-1's `part_asset_v2.{h,cpp}` and SP-2's `script_host.{h,cpp}` are also NEW MatterEngine3 files (included via `-I../include`, linked as `../src/<new>.cpp`). The consumed prototype backend (`part_asset.cpp`, `blas_manager.cpp`, `bvh.cpp`, `tlas_manager.cpp`, `vertex_ao.cpp`, `occupancy.cpp`, `material_registry.c`) is referenced read-only as `../../MatterSurfaceLib/src/<dep>` with `-I../../MatterSurfaceLib/include`. raylib paths are unchanged (`MatterEngine3/tests` is the same depth as `MatterSurfaceLib/tests`). The tests Makefile already EXISTS (SP-1 created it); this plan APPENDS its target block.

---

## Open-question decisions (binding for this plan)

1. **Canonical params serialization** (`serialize(params)`): a deterministic string —
   keys sorted lexicographically (ASCII), each emitted as `key=value;`. Numbers use
   `%.17g` (round-trippable double), booleans `true`/`false`, strings verbatim. This is
   the **memo-identity** form (used in the memo key and in the GL-free `FakeBaker`'s
   stand-in hash); equal params always serialize equally regardless of authoring key order.
   **Note (master C-2):** the *content* `resolved_hash` of a real script part is **not**
   computed here — SP-2's `resolve_hash` owns it, because it must merge the class's
   `static params` defaults (which SP-3 can't see) before hashing. SP-3 feeds the host the
   override params and memoizes the hash the host returns. The `key=value;` form is only an
   internal install-time identity, never the on-disk cache key for a real bake.
2. **Root-part discovery:** a **manifest file** `WorldData/<world>/world.manifest` —
   a newline-delimited list of root module names (one per line; blank lines / `#`
   comments ignored). Chosen over a naming convention because a world can have multiple
   roots and the manifest is explicit/diff-able. Missing manifest → hard error.
3. **Memoization key = `(source_hash, canonical_params)`** where `source_hash =
   fnv1a64(source_bytes)`. Module **identity is the source hash, not the path**, so a
   moved/renamed script with identical source is one node and dedups correctly.

---

## File Structure

```
MatterEngine3/                 # NEW project; consumes MatterSurfaceLib read-only
  include/
    part_graph.h          # PartGraph, Baker seam, ModuleResolver seam, public structs
  src/
    part_graph.cpp        # resolve (memoized, cycle-detect), topo sort, install driver,
                          # canonical params serialization, manifest parsing
  tests/
    part_graph_tests.cpp  # headless, GL-free; uses FakeBaker + FakeModuleResolver
    Makefile              # APPEND part_graph_tests TARGET/SOURCES/run-graph (file already
                          #   exists from SP-1; do NOT recreate)
```

### Public interface (target shape — built incrementally by the tasks below)

```cpp
// part_graph.h  (final shape)
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace part_graph {

// A param value the canonical serializer understands. Kept tiny on purpose (YAGNI):
// scripts pass numbers/bools/strings; nested objects are out of scope for SP-3 v1.
struct ParamValue {
    enum class Kind { Number, Bool, Str } kind = Kind::Number;
    double      num = 0.0;
    bool        boolean = false;
    std::string str;
    static ParamValue number(double v) { ParamValue p; p.kind = Kind::Number; p.num = v; return p; }
    static ParamValue boolean_(bool v) { ParamValue p; p.kind = Kind::Bool; p.boolean = v; return p; }
    static ParamValue string_(std::string v) { ParamValue p; p.kind = Kind::Str; p.str = std::move(v); return p; }
};
using Params = std::map<std::string, ParamValue>;  // std::map => keys already sorted

// One requested instantiation of a child module by a parent.
struct ChildRequest {
    std::string module;   // child module name (looked up via ModuleResolver)
    Params      params;   // effective params the parent passes to the child
};

// Seam: how SP-3 reads a module's source + its `static requires` WITHOUT calling build().
// Real impl (Task 9) evaluates the module top level via the SP-2 ScriptHost; tests use a fake.
struct ModuleResolver {
    virtual ~ModuleResolver() = default;
    // Returns false if the module name is unknown (=> install hard-errors).
    virtual bool load_source(const std::string& module, std::string& source_out) = 0;
    // `requires` evaluated against `params`. Returns the child instantiations.
    // false => module failed to evaluate (=> hard error).
    virtual bool get_requires(const std::string& module, const Params& params,
                              std::vector<ChildRequest>& children_out) = 0;
};

// Seam: how SP-3 bakes one part. Real impl (Task 12) delegates to SP-2 ScriptHost
// (resolve_hash + bake_source → save_v2); tests use a fake that records bake order
// and can be told to fail.
//
// HASH AUTHORITY (master C-2): SP-3 does NOT compute a part's resolved_hash itself,
// because the hash folds the MERGED params (class `static params` defaults overlaid
// with overrides) and only the host (SP-2) can read those defaults. SP-3 obtains the
// hash through this seam's resolve_hash and memoizes it. The FakeBaker provides a
// deterministic stand-in fold so logic tests stay host-free.
struct Baker {
    virtual ~Baker() = default;
    // Content hash for one part: merge static+override params, fold child_hashes,
    // NO bake. Returns 0 on resolve failure (fail-closed => install hard-errors).
    virtual uint64_t resolve_hash(const std::string& source, const Params& params,
                                  const std::vector<uint64_t>& child_hashes) = 0;
    // True if parts/<resolved_hash>.part already exists (cache hit => skip bake).
    virtual bool cached(uint64_t resolved_hash) = 0;
    // Bake one part. child_hashes are this part's direct children's resolved hashes
    // (already baked, present in cache). Returns false on bake failure (fail-closed).
    // Implementations bake to cache_path_resolved(resolved_hash); resolved_hash is the value
    // resolve_hash returned for the same inputs (host recomputes it identically).
    virtual bool bake(const std::string& source, const Params& params,
                      const std::vector<uint64_t>& child_hashes, uint64_t resolved_hash) = 0;
};

// Canonical params string (sorted keys, %.17g numbers). Public for unit testing.
std::string serialize_params(const Params& params);

// Resolved node in the graph (one per unique (source_hash, canonical_params)).
struct ResolvedNode {
    uint64_t              resolved_hash = 0;
    std::string           module;        // representative module name (for diagnostics)
    std::string           source;        // module source bytes
    Params                params;        // effective params
    std::vector<uint64_t> child_hashes;  // direct children's resolved hashes (sorted by SP-1)
    std::vector<uint64_t> child_keys;    // direct children's memo keys (for topo edges)
};

struct InstallResult {
    bool                     ok = false;
    std::string              error;       // human-readable; names the offending part on failure
    std::vector<uint64_t>    baked;       // resolved hashes baked this run (cache misses)
    int                      hits = 0;    // parts skipped because already cached
};

class PartGraph {
public:
    PartGraph(ModuleResolver& resolver, Baker& baker);

    // Resolve + topo-sort + bake the reachable graph for the given roots.
    InstallResult install(const std::vector<ChildRequest>& roots);

    // Parse WorldData/<world>/world.manifest into root ChildRequests (each root has
    // empty params unless the manifest carries them; v1: roots take their `static params`
    // defaults, supplied as empty Params here). Returns false + error on missing manifest.
    static bool read_manifest(const std::string& world_data_dir, const std::string& world,
                              std::vector<ChildRequest>& roots_out, std::string& error_out);
private:
    ModuleResolver& resolver_;
    Baker&          baker_;
};

} // namespace part_graph
```

---

## Task 1 — Test scaffold + Makefile target (headless, GL-free)

**Files:**
- `MatterEngine3/tests/part_graph_tests.cpp` (new)
- `MatterEngine3/tests/Makefile` (edit — APPEND to the existing SP-1 Makefile)
- `MatterEngine3/include/part_graph.h` (new, minimal stub)
- `MatterEngine3/src/part_graph.cpp` (new, minimal stub)

- [ ] Create `MatterEngine3/include/part_graph.h` with just the namespace + `serialize_params` declaration (filled out further in later tasks):

```cpp
#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace part_graph {

struct ParamValue {
    enum class Kind { Number, Bool, Str } kind = Kind::Number;
    double      num = 0.0;
    bool        boolean = false;
    std::string str;
    static ParamValue number(double v) { ParamValue p; p.kind = Kind::Number; p.num = v; return p; }
    static ParamValue boolean_(bool v) { ParamValue p; p.kind = Kind::Bool; p.boolean = v; return p; }
    static ParamValue string_(std::string v) { ParamValue p; p.kind = Kind::Str; p.str = std::move(v); return p; }
};
using Params = std::map<std::string, ParamValue>;

std::string serialize_params(const Params& params);

} // namespace part_graph
```

- [ ] Create `MatterEngine3/src/part_graph.cpp` with an intentionally WRONG stub so the first test fails:

```cpp
#include "part_graph.h"

namespace part_graph {

std::string serialize_params(const Params&) {
    return "STUB";  // wrong on purpose; Task 2 makes it real
}

} // namespace part_graph
```

- [ ] Create `MatterEngine3/tests/part_graph_tests.cpp` with the harness + one trivial test:

```cpp
#include "part_graph.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

int main() {
    using namespace part_graph;
    // Empty params canonicalize to the empty string.
    CHECK(serialize_params(Params{}) == "", "empty params -> empty string");

    if (failures == 0) printf("All part_graph tests passed\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] APPEND the target to the existing `MatterEngine3/tests/Makefile` (created by SP-1 — do NOT recreate it, and do NOT edit `MatterSurfaceLib/tests/Makefile`). Append `run-graph` to the `.PHONY` line, append `$(GRAPH_TARGET)` to the `clean` rule, and add this block at the end. New sources stay `../src/<new>.cpp`; consumed prototype sources are `../../MatterSurfaceLib/src/<dep>.cpp`. SP-1's `part_asset_v2.cpp` is a NEW MatterEngine3 source (provides `compute_resolved_hash`/`cache_path_resolved`); v1 `part_asset.cpp` is consumed read-only for `fnv1a64` (GL-free; `part_graph.cpp` has no raylib/GL deps, but the test still links the standard `LDLIBS`/`LDFLAGS` for harness uniformity):

```make
# Part graph resolve/dedup/topo/cycle/reachability/cache-miss tests (headless, GL-free).
# NEW MatterEngine3 sources ../src/<new>.cpp + consumed MatterSurfaceLib backend.
GRAPH_TARGET = part_graph_tests
GRAPH_SOURCES = part_graph_tests.cpp ../src/part_graph.cpp ../src/part_asset_v2.cpp \
                ../../MatterSurfaceLib/src/part_asset.cpp \
                ../../MatterSurfaceLib/src/blas_manager.cpp \
                ../../MatterSurfaceLib/src/bvh.cpp \
                ../../MatterSurfaceLib/src/tlas_manager.cpp \
                ../../MatterSurfaceLib/src/vertex_ao.cpp \
                ../../MatterSurfaceLib/src/occupancy.cpp
GRAPH_C   = ../../MatterSurfaceLib/src/material_registry.c
GRAPH_C_OBJ = material_registry.o

$(GRAPH_TARGET): $(GRAPH_SOURCES) $(GRAPH_C)
	gcc -c $(GRAPH_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(GRAPH_SOURCES) $(GRAPH_C_OBJ) -o $(GRAPH_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(GRAPH_C_OBJ)

run-graph: $(GRAPH_TARGET)
	./$(GRAPH_TARGET)
```

> Note: SP-1's `part_asset_v2.cpp` (new) + the consumed v1 `part_asset.cpp` and friends are
> linked so later tasks can call SP-1 `compute_resolved_hash`/`cache_path_resolved` (added by
> SP-1). If SP-1 is not yet implemented when this plan runs, see Task 3's seam note — the
> graph logic depends only on the declared SP-1 signatures, and the fake baker means the
> tests never need a real bake.

- [ ] **Run (expect FAIL):** `make -C MatterEngine3/tests run-graph`
  Expected: `FAIL: empty params -> empty string` then nonzero exit (stub returns `"STUB"`).
- [ ] Make it pass minimally — fix the stub in `MatterEngine3/src/part_graph.cpp`:

```cpp
std::string serialize_params(const Params& params) {
    (void)params;
    return "";  // real implementation arrives in Task 2
}
```

- [ ] **Run (expect PASS):** `make -C MatterEngine3/tests run-graph`
  Expected: `All part_graph tests passed`, exit 0.
- [ ] **Commit:** `git add MatterEngine3/include/part_graph.h MatterEngine3/src/part_graph.cpp MatterEngine3/tests/part_graph_tests.cpp MatterEngine3/tests/Makefile && git commit -m "$(cat <<'EOF'
test(part-graph): scaffold headless part_graph_tests + Makefile target

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Task 2 — Canonical params serialization (sorted keys, %.17g numbers)

**Files:**
- `MatterEngine3/tests/part_graph_tests.cpp` (edit)
- `MatterEngine3/src/part_graph.cpp` (edit)

- [ ] Add failing tests to `main()` in `part_graph_tests.cpp` (before the pass print):

```cpp
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
```

- [ ] **Run (expect FAIL):** `make -C MatterEngine3/tests run-graph`
  Expected: multiple `FAIL:` lines (stub returns `""`), nonzero exit.
- [ ] Implement the real serializer in `MatterEngine3/src/part_graph.cpp`:

```cpp
#include "part_graph.h"
#include <cstdio>

namespace part_graph {

std::string serialize_params(const Params& params) {
    std::string out;
    for (const auto& kv : params) {          // std::map iterates in sorted key order
        out += kv.first;
        out += '=';
        const ParamValue& v = kv.second;
        switch (v.kind) {
            case ParamValue::Kind::Number: {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", v.num);
                out += buf;
                break;
            }
            case ParamValue::Kind::Bool:
                out += (v.boolean ? "true" : "false");
                break;
            case ParamValue::Kind::Str:
                out += v.str;
                break;
        }
        out += ';';
    }
    return out;
}

} // namespace part_graph
```

- [ ] **Run (expect PASS):** `make -C MatterEngine3/tests run-graph`
  Expected: `All part_graph tests passed`, exit 0.
- [ ] **Commit:** `git add MatterEngine3/src/part_graph.cpp MatterEngine3/tests/part_graph_tests.cpp && git commit -m "$(cat <<'EOF'
feat(part-graph): canonical params serialization (sorted keys, %.17g)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Task 3 — Seams (`Baker`, `ModuleResolver`) + fakes for testing

**Files:**
- `MatterEngine3/include/part_graph.h` (edit — add seams + structs + `PartGraph`)
- `MatterEngine3/src/part_graph.cpp` (edit — `PartGraph` ctor, empty `install`)
- `MatterEngine3/tests/part_graph_tests.cpp` (edit — fakes + a wiring test)

This is the explicit early seam task: define `Baker`/`ModuleResolver` so all graph
logic is testable with fakes and **no real JS host / SP-1 file I/O**. The graph core gets
every part's hash through `Baker::resolve_hash` (master C-2: SP-2 is the hash authority) —
it does **not** call `compute_resolved_hash` directly. The GL-free `FakeBaker::resolve_hash`
folds `(source, canonical params, child_hashes)` via SP-1's `compute_resolved_hash` as a
deterministic stand-in, and its `cached`/`bake` replace all disk/host behavior, so the suite
is self-contained even before SP-2 exists. (`memo_key_of` still uses `fnv1a64` for identity.)

- [ ] Replace the contents of `MatterEngine3/include/part_graph.h` with the full
  public interface shown in the **File Structure** section above (the block beginning
  `// part_graph.h  (final shape)`), keeping the `ParamValue`/`Params`/`serialize_params`
  declarations already present.

- [ ] In `MatterEngine3/src/part_graph.cpp` add includes and the `PartGraph` ctor + an empty `install`
  returning a not-yet-implemented result (so the wiring test fails meaningfully):

```cpp
#include "part_graph.h"
#include "part_asset_v2.h"   // SP-1 (MatterEngine3, via -I../include): compute_resolved_hash,
                            //   cache_path_resolved; pulls in v1 part_asset.h for fnv1a64
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace part_graph {

// ... serialize_params unchanged ...

PartGraph::PartGraph(ModuleResolver& resolver, Baker& baker)
    : resolver_(resolver), baker_(baker) {}

InstallResult PartGraph::install(const std::vector<ChildRequest>&) {
    InstallResult r;
    r.ok = false;
    r.error = "not implemented";   // Task 4+ implements resolve/topo/bake
    return r;
}

} // namespace part_graph
```

- [ ] Add reusable fakes near the top of `part_graph_tests.cpp` (after the `CHECK` macro):

```cpp
#include <map>
#include <string>
#include <vector>

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
```

  Add the matching includes at the top of the test file: `#include <functional>`,
  `#include <set>`. The FakeBaker uses `part_asset::compute_resolved_hash` (SP-1) and
  `serialize_params` (Task 1) so its hashes match the test expectations below.

- [ ] Add a wiring test in `main()` (resolve of a single leaf with no children should
  succeed and bake exactly once):

```cpp
    {
        FakeModuleResolver res;
        res.modules["Leaf"] = FakeModule{ "leaf-source", nullptr, false };
        FakeBaker baker;
        PartGraph g(res, baker);
        InstallResult r = g.install({ ChildRequest{"Leaf", Params{}} });
        CHECK(r.ok, "single leaf install succeeds");
        CHECK(baker.bake_order.size() == 1, "single leaf baked exactly once");
    }
```

- [ ] **Run (expect FAIL):** `make -C MatterEngine3/tests run-graph`
  Expected: `FAIL: single leaf install succeeds` / `... baked exactly once` (install
  returns `not implemented`), nonzero exit.
- [ ] Leave the wiring test failing — Task 4 implements `install`. (No PASS step here.)
- [ ] **Commit:** `git add MatterEngine3/include/part_graph.h MatterEngine3/src/part_graph.cpp MatterEngine3/tests/part_graph_tests.cpp && git commit -m "$(cat <<'EOF'
feat(part-graph): add Baker/ModuleResolver seams + test fakes

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Task 4 — Memoized leaves-first resolve + single-leaf bake (cache miss)

**Files:**
- `MatterEngine3/src/part_graph.cpp` (edit — resolve + install core)
- `MatterEngine3/tests/part_graph_tests.cpp` (already has the Task 3 wiring test)

- [ ] Implement resolve + a minimal install in `MatterEngine3/src/part_graph.cpp`. Internal node store
  keyed by memo key `(source_hash, canonical_params)`:

```cpp
namespace {

struct InternalNode {
    uint64_t              memo_key = 0;       // fnv1a64(source) folded with canonical params
    uint64_t              resolved_hash = 0;
    std::string           module;
    std::string           source;
    Params                params;
    std::vector<uint64_t> child_hashes;       // direct children (for SP-1 fold, sorted)
    std::vector<uint64_t> child_keys;         // direct children memo keys (topo edges)
};

// Memo key = fnv1a64(source) combined with fnv1a64(canonical_params). Identity is the
// SOURCE HASH (not the path), per the planning decision, so a renamed identical script
// is one node.
uint64_t memo_key_of(const std::string& source, const std::string& canon_params) {
    uint64_t sh = part_asset::fnv1a64(source.data(), source.size());
    uint64_t ph = part_asset::fnv1a64(canon_params.data(), canon_params.size());
    // fold (order matters: distinct (source,params) => distinct key)
    return part_asset::fnv1a64(&sh, sizeof sh) ^
           (part_asset::fnv1a64(&ph, sizeof ph) * 1099511628211ull);
}

} // namespace
```

  Then the resolve/install body (cycle detection + reachability come in Tasks 7/8; here
  we only handle acyclic graphs and orphans are naturally excluded because we only walk
  from roots):

```cpp
InstallResult PartGraph::install(const std::vector<ChildRequest>& roots) {
    InstallResult result;
    std::unordered_map<uint64_t, InternalNode> memo;   // memo_key -> node
    std::string error;

    // Recursive resolve. Returns memo_key (0 sentinel cannot collide in practice; we
    // also carry a success flag out-of-band via `error`).
    std::function<bool(const ChildRequest&, uint64_t&)> resolve =
        [&](const ChildRequest& req, uint64_t& out_key) -> bool {
            std::string source;
            if (!resolver_.load_source(req.module, source)) {
                error = "missing requires target: " + req.module;
                return false;
            }
            std::string canon = serialize_params(req.params);
            uint64_t key = memo_key_of(source, canon);
            auto it = memo.find(key);
            if (it != memo.end()) { out_key = key; return true; }   // memoized

            std::vector<ChildRequest> kids;
            if (!resolver_.get_requires(req.module, req.params, kids)) {
                error = "module failed to evaluate requires: " + req.module;
                return false;
            }

            std::vector<uint64_t> child_keys;
            std::vector<uint64_t> child_hashes;
            child_keys.reserve(kids.size());
            child_hashes.reserve(kids.size());
            for (const auto& kid : kids) {
                uint64_t ck = 0;
                if (!resolve(kid, ck)) return false;     // leaves-first recursion
                child_keys.push_back(ck);
                child_hashes.push_back(memo.at(ck).resolved_hash);
            }

            InternalNode node;
            node.memo_key      = key;
            node.module        = req.module;
            node.source        = source;
            node.params        = req.params;
            node.child_keys    = child_keys;
            node.child_hashes  = child_hashes;   // SP-1 sorts internally; ok unsorted here
            // Hash authority is SP-2 (master C-2): ask the baker, never compute here.
            // The host merges static+override params before folding, so it sees defaults
            // SP-3 cannot. 0 => resolve failure (fail-closed).
            node.resolved_hash = baker_.resolve_hash(source, req.params, child_hashes);
            if (node.resolved_hash == 0) {
                error = "failed to resolve hash for part: " + req.module;
                return false;
            }
            memo.emplace(key, std::move(node));
            out_key = key;
            return true;
        };

    std::vector<uint64_t> root_keys;
    for (const auto& r : roots) {
        uint64_t k = 0;
        if (!resolve(r, k)) { result.error = error; return result; }
        root_keys.push_back(k);
    }

    // Minimal bake: for Task 4 just bake every resolved node whose hash is a cache miss.
    // (Topo order is enforced in Task 6; here single-leaf graphs already satisfy it.)
    for (const auto& kv : memo) {
        const InternalNode& n = kv.second;
        if (baker_.cached(n.resolved_hash)) { ++result.hits; continue; }
        if (!baker_.bake(n.source, n.params, n.child_hashes, n.resolved_hash)) {
            result.error = "bake failed for part: " + n.module;
            return result;
        }
        result.baked.push_back(n.resolved_hash);
    }
    result.ok = true;
    return result;
}
```

  Add `#include <functional>` and `#include <set>` to `part_graph.cpp`.

- [ ] **Run (expect PASS for the Task 3 wiring test):** `make -C MatterEngine3/tests run-graph`
  Expected: `All part_graph tests passed`, exit 0.
- [ ] **Commit:** `git add MatterEngine3/src/part_graph.cpp && git commit -m "$(cat <<'EOF'
feat(part-graph): memoized leaves-first resolve + cache-miss bake core

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Task 5 — Cache miss → bake / hit → skip (incremental build mechanism)

**Files:**
- `MatterEngine3/tests/part_graph_tests.cpp` (edit)

- [ ] Add tests covering the spec's "first install bakes N; second install bakes 0":

```cpp
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
```

- [ ] **Run (expect PASS):** `make -C MatterEngine3/tests run-graph`
  Expected: `All part_graph tests passed`, exit 0. (Resolve + cache-miss already implemented;
  this locks the behavior with a test.)
- [ ] **Commit:** `git add MatterEngine3/tests/part_graph_tests.cpp && git commit -m "$(cat <<'EOF'
test(part-graph): cache miss bakes; cache hit skips (incremental build)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Task 6 — Topological bake order (children before parents)

**Files:**
- `MatterEngine3/src/part_graph.cpp` (edit — replace map-iteration bake with topo walk)
- `MatterEngine3/tests/part_graph_tests.cpp` (edit)

- [ ] Add a failing topo-order test (the Task-4 map iteration is unordered, so this can
  fail nondeterministically — the test makes the requirement explicit):

```cpp
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
```

- [ ] **Run (expect FAIL or flaky):** `make -C MatterEngine3/tests run-graph`
  Expected: `FAIL: children baked before their parents (topo order)` on at least some
  runs (unordered `std::unordered_map` iteration). Treat any failure as the red state.
- [ ] Replace the bake loop in `install` with a deterministic post-order (DFS) topo walk
  rooted at `root_keys`, so children are always baked before parents and only reachable
  nodes are visited:

```cpp
    // Topological (post-order) bake over the reachable set from roots: a node is baked
    // only after all its children. DFS post-order on a DAG yields children-first order.
    std::set<uint64_t> baked_or_present;     // memo_keys already handled
    std::vector<uint64_t> topo;              // memo_keys in children-first order
    std::function<void(uint64_t)> post = [&](uint64_t key) {
        if (baked_or_present.count(key)) return;
        baked_or_present.insert(key);
        const InternalNode& n = memo.at(key);
        for (uint64_t ck : n.child_keys) post(ck);
        topo.push_back(key);
    };
    for (uint64_t rk : root_keys) post(rk);

    for (uint64_t key : topo) {
        const InternalNode& n = memo.at(key);
        if (baker_.cached(n.resolved_hash)) { ++result.hits; continue; }
        if (!baker_.bake(n.source, n.params, n.child_hashes, n.resolved_hash)) {
            result.error = "bake failed for part: " + n.module;
            return result;
        }
        result.baked.push_back(n.resolved_hash);
    }
    result.ok = true;
    return result;
```

  Delete the Task-4 `for (const auto& kv : memo)` bake loop.

- [ ] **Run (expect PASS):** `make -C MatterEngine3/tests run-graph`
  Expected: `All part_graph tests passed`, exit 0 (topo + the Task 5 cache tests still pass).
- [ ] **Commit:** `git add MatterEngine3/src/part_graph.cpp MatterEngine3/tests/part_graph_tests.cpp && git commit -m "$(cat <<'EOF'
feat(part-graph): deterministic topological (children-first) bake order

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Task 7 — Dedup (same params collapse; different params split)

**Files:**
- `MatterEngine3/tests/part_graph_tests.cpp` (edit)

- [ ] Add dedup tests (resolve memo key = `(source_hash, canonical_params)` already gives
  this; these lock the spec behavior):

```cpp
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
```

- [ ] **Run (expect PASS):** `make -C MatterEngine3/tests run-graph`
  Expected: `All part_graph tests passed`, exit 0.
- [ ] **Commit:** `git add MatterEngine3/tests/part_graph_tests.cpp && git commit -m "$(cat <<'EOF'
test(part-graph): params->hash dedup (same collapses, different splits)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Task 8 — Cycle detection (hard error naming the cycle path)

**Files:**
- `MatterEngine3/src/part_graph.cpp` (edit — track resolution stack, detect back-edge)
- `MatterEngine3/tests/part_graph_tests.cpp` (edit)

- [ ] Add a failing cycle test (a→b→a). Without detection, resolve recurses infinitely
  (stack overflow) — so introduce detection driven by this test:

```cpp
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
```

- [ ] **Run (expect FAIL / hang):** `make -C MatterEngine3/tests run-graph`
  Expected: infinite recursion / crash (no cycle guard yet) — the red state. (If it
  hangs, that confirms the missing guard; proceed to implement.)
- [ ] Add a resolution-stack guard inside the `resolve` lambda in `install`. Maintain a
  `std::vector<std::string> stack` of module names currently being resolved and a
  `std::set<uint64_t> on_stack` of their memo keys keyed by `(module + canon)`; a child
  whose key is already on the stack is a back-edge → cycle. Because resolve is keyed by
  `(source, params)`, push/pop on entry/exit:

```cpp
    std::vector<std::string> stack;            // module names, for the error path
    std::set<uint64_t> on_stack;               // memo_keys currently being resolved

    std::function<bool(const ChildRequest&, uint64_t&)> resolve =
        [&](const ChildRequest& req, uint64_t& out_key) -> bool {
            std::string source;
            if (!resolver_.load_source(req.module, source)) {
                error = "missing requires target: " + req.module;
                return false;
            }
            std::string canon = serialize_params(req.params);
            uint64_t key = memo_key_of(source, canon);

            if (on_stack.count(key)) {                 // back-edge => cycle
                std::string path;
                for (const auto& m : stack) path += m + " -> ";
                path += req.module;
                error = "cycle detected: " + path;
                return false;
            }
            auto it = memo.find(key);
            if (it != memo.end()) { out_key = key; return true; }   // memoized (DAG reuse)

            std::vector<ChildRequest> kids;
            if (!resolver_.get_requires(req.module, req.params, kids)) {
                error = "module failed to evaluate requires: " + req.module;
                return false;
            }

            stack.push_back(req.module);
            on_stack.insert(key);

            std::vector<uint64_t> child_keys, child_hashes;
            for (const auto& kid : kids) {
                uint64_t ck = 0;
                if (!resolve(kid, ck)) { stack.pop_back(); on_stack.erase(key); return false; }
                child_keys.push_back(ck);
                child_hashes.push_back(memo.at(ck).resolved_hash);
            }

            stack.pop_back();
            on_stack.erase(key);

            InternalNode node;
            node.memo_key      = key;
            node.module        = req.module;
            node.source        = source;
            node.params        = req.params;
            node.child_keys    = child_keys;
            node.child_hashes  = child_hashes;
            // Hash authority is SP-2 (master C-2): ask the baker, never compute here.
            // (stack/on_stack already popped above before node construction.)
            node.resolved_hash = baker_.resolve_hash(source, req.params, child_hashes);
            if (node.resolved_hash == 0) {
                error = "failed to resolve hash for part: " + req.module;
                return false;
            }
            memo.emplace(key, std::move(node));
            out_key = key;
            return true;
        };
```

  Replace the Task-4/Task-8 `resolve` lambda body with the above. Crucially, the cycle
  check + `return result` (with `result.ok` left false) happens **before** any bake walk,
  so nothing is baked on a cyclic graph.

- [ ] **Run (expect PASS):** `make -C MatterEngine3/tests run-graph`
  Expected: `All part_graph tests passed`, exit 0.
- [ ] **Commit:** `git add MatterEngine3/src/part_graph.cpp MatterEngine3/tests/part_graph_tests.cpp && git commit -m "$(cat <<'EOF'
feat(part-graph): cycle detection via back-edge on resolution stack

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Task 9 — Reachability (orphans never baked) + transitive invalidation

**Files:**
- `MatterEngine3/tests/part_graph_tests.cpp` (edit)

Reachability already holds (resolve walks only from roots; the topo walk only visits
reachable memo keys). Transitive invalidation already holds (a leaf source change →
different `source_hash` → different `resolved_hash` → folds up). These tests lock both.

- [ ] Add a reachability/orphan test — an unreferenced module in the resolver table is
  never resolved or baked:

```cpp
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
```

  Add `#include "part_asset_v2.h"` to the test file (SP-1, via `-I../include`; for `compute_resolved_hash`).

- [ ] Add a transitive-invalidation test — editing a leaf's source forces leaf + ancestor
  rebake while an unrelated branch stays a hit:

```cpp
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
```

- [ ] **Run (expect PASS):** `make -C MatterEngine3/tests run-graph`
  Expected: `All part_graph tests passed`, exit 0.
- [ ] **Commit:** `git add MatterEngine3/tests/part_graph_tests.cpp && git commit -m "$(cat <<'EOF'
test(part-graph): reachability (no orphan bake) + transitive invalidation

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Task 10 — Failure propagation (child bake fail ⇒ parent unbaked ⇒ install fails named)

**Files:**
- `MatterEngine3/tests/part_graph_tests.cpp` (edit)

The topo walk already returns on the first bake failure with `result.ok=false` and an
error naming the part, and because children are baked first, a failed child means the
parent's bake is never attempted. This test locks that contract.

- [ ] Add a failure-propagation test:

```cpp
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
```

  > Note: the `child_hash`/`root_hash` literal lengths (`9`, `8`) must equal
  > `strlen("src-Child")` and `strlen("src-Root")`. If you change the source strings,
  > update the lengths. Prefer `std::string s="src-Child"; ...s.size()...` to avoid drift.

- [ ] **Run (expect PASS):** `make -C MatterEngine3/tests run-graph`
  Expected: `All part_graph tests passed`, exit 0.
- [ ] **Commit:** `git add MatterEngine3/tests/part_graph_tests.cpp && git commit -m "$(cat <<'EOF'
test(part-graph): child bake failure leaves parent unbaked, install fails named

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Task 11 — World manifest discovery (root-part discovery)

**Files:**
- `MatterEngine3/src/part_graph.cpp` (edit — implement `read_manifest`)
- `MatterEngine3/tests/part_graph_tests.cpp` (edit)

Decision: roots come from `WorldData/<world>/world.manifest` — newline-delimited module
names; blank lines and `#` comments ignored; missing file → hard error. Roots take empty
`Params` (their `static params` defaults are applied by the module's own
`get_requires`/bake; SP-3 v1 manifests carry only names).

- [ ] Add failing tests that write a temp manifest and parse it:

```cpp
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
```

  Add `#include <cstdlib>` (for `system`) to the test file if not present.

- [ ] **Run (expect FAIL):** `make -C MatterEngine3/tests run-graph`
  Expected: link/compile error or `FAIL: manifest parse succeeds` (`read_manifest` not yet
  implemented). The red state.
- [ ] Implement `read_manifest` in `MatterEngine3/src/part_graph.cpp` (add `#include <fstream>`,
  `#include <sstream>`):

```cpp
bool PartGraph::read_manifest(const std::string& world_data_dir, const std::string& world,
                              std::vector<ChildRequest>& roots_out, std::string& error_out) {
    std::string path = world_data_dir + "/" + world + "/world.manifest";
    std::ifstream in(path);
    if (!in) {
        error_out = "world manifest not found: " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        // trim leading/trailing whitespace
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;        // blank
        size_t e = line.find_last_not_of(" \t\r\n");
        std::string name = line.substr(b, e - b + 1);
        if (name.empty() || name[0] == '#') continue; // comment
        roots_out.push_back(ChildRequest{ name, Params{} });
    }
    return true;
}
```

- [ ] **Run (expect PASS):** `make -C MatterEngine3/tests run-graph`
  Expected: `All part_graph tests passed`, exit 0.
- [ ] **Commit:** `git add MatterEngine3/src/part_graph.cpp MatterEngine3/tests/part_graph_tests.cpp && git commit -m "$(cat <<'EOF'
feat(part-graph): world.manifest root discovery (comments/blanks ignored)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Task 12 — Real adapters: wire SP-2 ScriptHost + SP-1 cache_path_resolved (integration seam)

**Files:**
- `MatterEngine3/include/part_graph.h` (edit — declare adapter classes)
- `MatterEngine3/src/part_graph.cpp` (edit — implement adapters)

> **Dependency gate:** This task is the ONLY one requiring SP-2 (`ScriptHost`) and SP-1
> (`cache_path_resolved`/`save_v2`) to be implemented. Tasks 1-11 are fully green with fakes. If
> SP-2 is not yet merged when this plan executes, STOP after Task 11, leave Task 12's code
> behind `#if MATTER_HAVE_SCRIPT_HOST` (default 0), and revisit when SP-2 lands. This keeps
> SP-3's graph logic shipped and tested independently.

The real `ModuleResolver` reads `.js` files from `WorldData/<world>/ObjectSchemas/` and
asks the SP-2 host to top-level-eval the module and read `static requires` (NOT `build()`).
The real `Baker` checks `parts/<hash>.part` existence via SP-1 `cache_path_resolved` and bakes via
`ScriptHost::bake`.

- [ ] Declare adapters in `part_graph.h` (guarded so the GL-free unit tests never pull in
  the host):

```cpp
#if defined(MATTER_HAVE_SCRIPT_HOST)
#include "script_host.h"   // SP-2 (MatterEngine3, via -I../include)

namespace part_graph {

// Reads .js modules from <schemas_dir> and evaluates `static requires` via the host's
// top-level eval (no build()).
class FileModuleResolver : public ModuleResolver {
public:
    FileModuleResolver(script_host::ScriptHost& host, std::string schemas_dir);
    bool load_source(const std::string& module, std::string& source_out) override;
    bool get_requires(const std::string& module, const Params& params,
                      std::vector<ChildRequest>& children_out) override;
private:
    script_host::ScriptHost& host_;
    std::string              schemas_dir_;
};

// Checks parts/<hash>.part existence (SP-1 cache_path_resolved) and delegates hashing/baking to
// SP-2 ScriptHost (resolve_hash + bake_source). SP-2 is the hash authority (master C-2).
class HostBaker : public Baker {
public:
    HostBaker(script_host::ScriptHost& host, std::string parts_dir);
    uint64_t resolve_hash(const std::string& source, const Params& params,
                          const std::vector<uint64_t>& child_hashes) override;
    bool cached(uint64_t resolved_hash) override;
    bool bake(const std::string& source, const Params& params,
              const std::vector<uint64_t>& child_hashes, uint64_t resolved_hash) override;
private:
    script_host::ScriptHost& host_;
    std::string              parts_dir_;
};

} // namespace part_graph
#endif // MATTER_HAVE_SCRIPT_HOST
```

  > The exact `script_host::ScriptHost` member signatures are pinned by master contract C-2:
  > `uint64_t resolve_hash(source, params_json, child_hashes*, count)`,
  > `BakeResult bake_source(source, params_json, opts, child_hashes*, count)`, and
  > `std::vector<RequiredChild> eval_requires(source, params_json)`. The adapters convert
  > SP-3's `Params` to the host's JSON `params_json` and forward `child_hashes` as a pointer
  > pair. **Precondition:** SP-2's `bake_source` writes to `cache_path_resolved(resolved_hash)` and
  > SP-3's `cached`/`install` read from `parts_dir_`; the integration test runs with the
  > working directory set so `parts/` resolves to `parts_dir_` (so both sides agree on the
  > path). The host's `resolve_hash` and `bake_source` recompute the same hash, so the value
  > SP-3 memoized equals where the `.part` lands.

- [ ] Implement the adapters in `src/part_graph.cpp` under the same guard. Add a small
  `params_to_json(const Params&)` helper (the host wants a JSON object, not the `key=value;`
  canonical string used for memo identity). `resolve_hash`/`bake` forward to the host;
  `get_requires` adapts `eval_requires`'s return vector to `ChildRequest`s:

```cpp
#if defined(MATTER_HAVE_SCRIPT_HOST)
#include <cstdio>
#include <fstream>
#include <sstream>

namespace part_graph {

// Params -> JSON object string for the host (numbers via %.17g, strings quoted).
// (Distinct from serialize_params' `key=value;` memo-identity form.) Self-contained
// here because JSON conversion is only needed on the real-host path.
static std::string params_to_json(const Params& params) {
    std::ostringstream os;
    os << '{';
    bool first = true;
    for (const auto& kv : params) {          // Params is an ordered map; host re-sorts
        if (!first) os << ','; first = false;
        os << '"' << kv.first << "\":";
        switch (kv.second.kind) {
            case ParamValue::Kind::Number: {
                char buf[32]; std::snprintf(buf, sizeof buf, "%.17g", kv.second.num);
                os << buf; break;
            }
            case ParamValue::Kind::Bool:
                os << (kv.second.boolean ? "true" : "false"); break;
            case ParamValue::Kind::Str:
                os << '"' << kv.second.str << '"'; break;  // SP-3 v1 params have no quotes/escapes
        }
    }
    os << '}';
    return os.str();
}

// Inverse: a flat JSON object {"k":num|bool|"str", ...} -> Params. SP-3 v1 only sees
// the shapes eval_requires emits (flat numbers/bools/strings), so a tiny hand parser
// suffices; reuse the host's own emitter contract rather than a full JSON lib.
Params params_from_json(const std::string& json);  // defined below

FileModuleResolver::FileModuleResolver(script_host::ScriptHost& host, std::string schemas_dir)
    : host_(host), schemas_dir_(std::move(schemas_dir)) {}

bool FileModuleResolver::load_source(const std::string& module, std::string& out) {
    std::ifstream in(schemas_dir_ + "/" + module + ".js", std::ios::binary);
    if (!in) return false;
    std::ostringstream ss; ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool FileModuleResolver::get_requires(const std::string& module, const Params& params,
                                      std::vector<ChildRequest>& out) {
    std::string source;
    if (!load_source(module, source)) return false;
    // SP-2 eval_requires: eval module top level (no build()), read `static requires`.
    std::vector<script_host::RequiredChild> kids =
        host_.eval_requires(source, params_to_json(params));
    out.clear();
    out.reserve(kids.size());
    for (const auto& k : kids)
        out.push_back(ChildRequest{ k.module_specifier, params_from_json(k.params_json) });
    return true;   // (a thrown `requires` surfaces as a host error -> empty + caller errors)
}

HostBaker::HostBaker(script_host::ScriptHost& host, std::string parts_dir)
    : host_(host), parts_dir_(std::move(parts_dir)) {}

uint64_t HostBaker::resolve_hash(const std::string& source, const Params& params,
                                 const std::vector<uint64_t>& child_hashes) {
    return host_.resolve_hash(source, params_to_json(params),
                              child_hashes.data(), child_hashes.size());
}

bool HostBaker::cached(uint64_t resolved_hash) {
    std::string path = parts_dir_ + "/" + part_asset::cache_path_resolved(resolved_hash);
    std::ifstream in(path, std::ios::binary);
    return in.good();
}

bool HostBaker::bake(const std::string& source, const Params& params,
                     const std::vector<uint64_t>& child_hashes, uint64_t resolved_hash) {
    // SP-2 bake_source recomputes the same hash and writes parts/<hash>.part via save_v2.
    script_host::BakeResult r = host_.bake_source(
        source, params_to_json(params), /*opts*/{},
        child_hashes.data(), child_hashes.size());
    // The hash SP-3 memoized must equal where the .part landed (master C-2 guarantee).
    return r.error.ok && r.resolved_hash == resolved_hash;
}

// Minimal flat-object JSON parser for the shapes eval_requires emits (flat
// number|bool|"string"; SP-3 v1 strings carry no escapes). Unknown shapes -> skip.
Params params_from_json(const std::string& json) {
    Params out;
    size_t i = 0, n = json.size();
    auto skip_ws = [&]{ while (i < n && (json[i]==' '||json[i]=='\t'||json[i]=='\n'||json[i]=='\r')) ++i; };
    auto parse_str = [&](std::string& s) -> bool {
        if (i >= n || json[i] != '"') return false;
        ++i; size_t start = i;
        while (i < n && json[i] != '"') ++i;
        if (i >= n) return false;
        s = json.substr(start, i - start); ++i; return true;
    };
    skip_ws();
    if (i >= n || json[i] != '{') return out; ++i;
    skip_ws();
    if (i < n && json[i] == '}') return out;
    while (i < n) {
        skip_ws();
        std::string key;
        if (!parse_str(key)) break;
        skip_ws();
        if (i >= n || json[i] != ':') break; ++i;
        skip_ws();
        if (i < n && json[i] == '"') {
            std::string v; if (!parse_str(v)) break;
            out[key] = ParamValue::string_(v);
        } else if (json.compare(i, 4, "true") == 0) {
            out[key] = ParamValue::boolean_(true); i += 4;
        } else if (json.compare(i, 5, "false") == 0) {
            out[key] = ParamValue::boolean_(false); i += 5;
        } else {
            size_t start = i;
            while (i < n && json[i] != ',' && json[i] != '}') ++i;
            out[key] = ParamValue::number(std::strtod(json.c_str() + start, nullptr));
        }
        skip_ws();
        if (i < n && json[i] == ',') { ++i; continue; }
        if (i < n && json[i] == '}') break;
    }
    return out;
}

} // namespace part_graph
#endif // MATTER_HAVE_SCRIPT_HOST
```

  > `params_from_json` round-trips with `params_to_json`'s `%.17g` numbers exactly
  > (`double`→`%.17g`→`strtod`→`%.17g` is stable), so a child's memo key and host hash
  > stay consistent across the JSON hop. Needs `#include <cstdlib>` for `std::strtod`.

- [ ] **Build check (no new test, guard off by default):** `make -C MatterEngine3/tests run-graph`
  Expected: `All part_graph tests passed`, exit 0 (adapters compiled out; nothing regressed).
- [ ] When SP-2 is available, add a guarded integration test (`-DMATTER_HAVE_SCRIPT_HOST`)
  that writes two real `.js` schemas + a `world.manifest`, runs `install`, and asserts the
  expected `parts/<hash>.part` files now exist on disk and a second install bakes 0. This
  is the single end-to-end check; all logic-level guarantees are already covered Tasks 5-11.
- [ ] **Commit:** `git add MatterEngine3/include/part_graph.h MatterEngine3/src/part_graph.cpp && git commit -m "$(cat <<'EOF'
feat(part-graph): real FileModuleResolver + HostBaker adapters (SP-2/SP-1 seam)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"`

---

## Spec testing-section coverage map

| Spec test bullet                         | Task |
|------------------------------------------|------|
| Cache miss → bake; hit → skip            | 5    |
| Transitive invalidation                  | 9    |
| Dedup same / different params            | 7    |
| Topo order (children before parent)      | 6    |
| Cycle → hard error, nothing baked        | 8    |
| Reachability / orphan not baked          | 9    |
| Failure propagation (named, parent unbaked) | 10 |
| Manifest root discovery                  | 11   |
| Canonical params serialization           | 2    |
| End-to-end real host (gated on SP-2)     | 12   |
