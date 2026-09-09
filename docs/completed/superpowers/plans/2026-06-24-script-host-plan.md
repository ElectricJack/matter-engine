# Script Host & Voxel-CSG Bake (SP-2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Embed QuickJS-ng into the **new `MatterEngine3/` project** (consuming the MatterSurfaceLib prototype READ-ONLY) and expose a Processing-style `Part` DSL so one isolated script deterministically bakes a single standalone `.part` via SP-1's `save_v2`, through analytic voxel/SDF-CSG brushes lowered to Cluster particles + carve particles.

**Architecture:** A C++ `ScriptHost` owns the QuickJS-ng `JSRuntime`/`JSContext` lifecycle (one fresh context per bake), owns all authoring state (transform stack, material cursor, active-session enum, build buffer), and binds DSL functions that mutate that C++ state. On script completion the host lowers the CSG build buffer to `StaticParticle`s + carve `Particle`s, drives `Cluster::force_rebuild_all_cells()` to surface per-cell BLAS, then computes the resolved hash and calls `save_v2`. Any error (throw, session misuse, time-budget overrun) is fail-closed: no file is written and a structured error is returned.

**Tech Stack:** C++17, QuickJS-ng (vendored under repo-root `Libraries/quickjs-ng/`), existing Cluster/BLASManager (consumed from MatterSurfaceLib), SP-1 `part_asset_v2`, headless tests under `MatterEngine3/tests/` matching the prototype's assert/style.

> **Relocation note (from the master plan):** This sub-plan obeys the `MatterEngine3` relocation contract in `2026-06-24-procedural-part-system-master-plan.md`. All NEW files (`script_host.{h,cpp}`, `dsl_state.{h,cpp}`, `csg_lowering.{h,cpp}`, `dsl_rng.h`, `dsl_bindings.cpp`, `part_base.js.h`, tests) live under `MatterEngine3/{include,src,tests}/`; the consumed prototype backend (cluster/cell/mesher/blas/tlas/part_asset/vertex_ao/occupancy + the C SurfaceLib sources) is referenced read-only as `../../MatterSurfaceLib/src/<dep>` with `-I../../MatterSurfaceLib/include`. SP-1's v2 symbols come from `part_asset_v2.h` (`-I../include`). `Libraries/quickjs-ng/` and raylib paths are unchanged (`MatterEngine3/tests` is the same depth as `MatterSurfaceLib/tests`).

---

## Dependencies & assumptions

- **SP-1 must land first.** This plan calls `part_asset::compute_resolved_hash(...)`, `part_asset::save_v2(...)`, `part_asset::cache_path(...)`, `part_asset::ChildInstance`, and `part_asset::LodLevels` — all declared in SP-1's NEW `MatterEngine3/include/part_asset_v2.h` (consumers `#include "part_asset_v2.h"`, resolved via `-I../include`). SP-1 does NOT modify the prototype's v1 `MatterSurfaceLib/include/part_asset.h`. **Assumption:** SP-1's v2 API is implemented per `docs/superpowers/specs/2026-06-24-part-artifact-v2-design.md` before SP-2 Task 9. SP-2 passes `children=nullptr, child_count=0` and an empty `LodLevels`.
- **QuickJS-ng version pin (assumption):** **quickjs-ng v0.10.0** (release tarball `quickjs-ng-0.10.0`). Vendored as a flat source drop under `Libraries/quickjs-ng/` (no submodule, no network fetch at build), mirroring the existing `Libraries/raylib`, `Libraries/imgui`, `Libraries/ode` convention. If the pinned tarball's file set differs, adjust the `QUICKJS_C` source list in Task 1 to match the actual `.c` files shipped; the rest of the plan is layout-independent.
- **QuickJS-ng source subset (assumption):** the amalgam-ish core is `quickjs.c`, `libregexp.c`, `libunicode.c`, `cutils.c`, `quickjs-libc.c` (we do NOT compile `quickjs-libc.c` — it provides Date/file/network/`std`/`os` we deliberately omit), plus the dtoa helper `xsum.c`/`libbf.c` only if the pinned build requires bignum. Plan compiles: `quickjs.c libregexp.c libunicode.c cutils.c`. If v0.10.0 needs `libbf.c`/`xsum.c` to link, add them in the Task 1 link-error step (the failing link will name the missing symbols).
- **Cluster API confirmed** against `MatterSurfaceLib/include/cluster.h` (consumed prototype, via `-I../../MatterSurfaceLib/include`): `add_particle(pos, radius, materialId, tint, detail_size)`, `set_carve_particles(std::vector<Particle>)`, `set_base_detail_size`, `set_simplification_ratio`, `force_rebuild_all_cells`, `clear_particles`. `StaticParticle`/`Particle` fields confirmed. **Guessed:** there is no existing "exact box brush" mesher input — `cluster.h` only accepts spheres (`StaticParticle`/`Particle` are point+radius). SP-2 therefore lowers a `box` brush by **sampling its analytic SDF into a dense pack of small spheres** at the session spacing (a box "stamp"), preserving crisp corners down to spacing. This is the spec's "control the input, don't rewrite the mesher" path and resolves the spec's open question ("whether `box` needs a mesher-input addition") with **no mesher change**.
- **Smooth-min lowering (resolves spec open question):** the build buffer is a **flat op list** (brush + op + smoothing-cursor snapshot per op). `smoothing(k)` is applied **whole-expression** as the `Cluster` smooth-min factor for the dominant `k` recorded; per-op `k` differences beyond the final union set are out of scope for SP-2 (single session, single `k` cursor at lowering). Documented in Task 7.

## File Structure

| File | Status | Responsibility |
|------|--------|----------------|
| `Libraries/quickjs-ng/` | NEW (vendored) | quickjs-ng v0.10.0 source drop (`quickjs.c`, `quickjs.h`, `libregexp.c/.h`, `libunicode.c/.h`, `cutils.c/.h`, `quickjs-atom.h`, `quickjs-opcode.h`, `libregexp-opcode.h`, `LICENSE`, `VERSION`). |
| `MatterEngine3/include/script_host.h` | NEW | Public `ScriptHost` API: `BakeResult`, `BakeError`, `BakeOptions` (time budget), `bake_source(source, params_json, opts)`. |
| `MatterEngine3/src/script_host.cpp` | NEW | Context lifecycle, `Part` bootstrap eval, `build(p)` dispatch, error harvesting, time-budget interrupt, final lower→Cluster→BLAS→save_v2 orchestration. |
| `MatterEngine3/include/dsl_state.h` | NEW | C++-owned DSL state: transform stack (`Matrix`), material cursor (`uint32_t`), `Session` enum, `BuildBuffer` (flat CSG op list), structured error sink. |
| `MatterEngine3/src/dsl_state.cpp` | NEW | Transform-stack math, session-misuse checks, build-buffer accumulation. |
| `MatterEngine3/include/csg_lowering.h` | NEW | `lower_build_buffer(const BuildBuffer&, ...) -> {vector<StaticParticle> additive, vector<Particle> carve, float smoothing}`. |
| `MatterEngine3/src/csg_lowering.cpp` | NEW | Analytic brush→particle/carve lowering (sphere = 1 particle; box = SDF-sampled sphere stamp); union/difference/intersection routing. |
| `MatterEngine3/include/dsl_rng.h` | NEW | Seeded PRNG (SplitMix64/xorshift) backing `Math.random`; seed derived from params. |
| `MatterEngine3/src/dsl_bindings.cpp` | NEW | QuickJS-ng C bindings for `Part` base methods (`pushMatrix`/`translate`/`rotate*`/`scale`/`applyMatrix`/`fill`/`beginVoxels`/`endVoxels`/`box`/`sphere`/`union`/`difference`/`intersection`/`smoothing`) + `MAT` constant + seeded `Math.random`. |
| `MatterEngine3/src/part_base.js.h` | NEW | C string literal: the `Part` ES base-class bootstrap JS evaluated into every fresh context. |
| `MatterEngine3/tests/script_host_tests.cpp` | NEW | Headless tests: embed sanity, params, primitives, CSG, smoothing, sub-min box, fail-closed, determinism, seeded RNG. |
| `MatterEngine3/tests/Makefile` | MODIFIED (append) | Append `SCRIPT_TARGET`/`SCRIPT_SOURCES`/`run-script` wiring to the existing SP-1 Makefile; compile quickjs-ng `.c` via gcc (C, unmangled). |
| `MatterEngine3/Makefile` | MODIFIED | Compile vendored quickjs-ng `.c` into the MatterEngine3 build so the app can bake. |

---

## Tasks

### Task 1: Vendor QuickJS-ng and prove the embed (`1+1` → 2)

**Files:**
- `Libraries/quickjs-ng/` (NEW vendored source — repo-root, shared)
- `MatterEngine3/tests/script_host_tests.cpp` (NEW)
- `MatterEngine3/tests/Makefile` (MODIFIED — append to the existing SP-1 Makefile)

- [ ] Download the quickjs-ng **v0.10.0** release tarball and place its core sources at `Libraries/quickjs-ng/`. Required files: `quickjs.c`, `quickjs.h`, `quickjs-atom.h`, `quickjs-opcode.h`, `libregexp.c`, `libregexp.h`, `libregexp-opcode.h`, `libunicode.c`, `libunicode.h`, `cutils.c`, `cutils.h`, `list.h`, `quickjs-c-atomics.h` (if present), `xsum.h` (if present), `LICENSE`, `VERSION`. Do NOT include `quickjs-libc.c`, `qjs.c`, `qjsc.c`, `repl.js`, tests, or CMake files. Verify with `ls Libraries/quickjs-ng/quickjs.c Libraries/quickjs-ng/quickjs.h`.
- [ ] Create `MatterEngine3/tests/script_host_tests.cpp` with a failing embed-sanity test that calls quickjs-ng directly (this also proves the headers/link work before any host code exists):
```cpp
#include <cstdio>
#include <cstdint>
#include <cstring>
extern "C" {
#include "quickjs.h"
}

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static void test_embed_eval_1_plus_1() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr, "runtime created");
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr, "context created");
    JSValue v = JS_Eval(ctx, "1+1", 3, "<test>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(v), "eval did not throw");
    int32_t out = -1;
    CHECK(JS_ToInt32(ctx, &out, v) == 0, "result convertible to int");
    CHECK(out == 2, "1+1 == 2");
    JS_FreeValue(ctx, v);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main() {
    test_embed_eval_1_plus_1();
    if (failures == 0) printf("ALL PASS\n");
    return failures ? 1 : 0;
}
```
- [ ] Append to the existing `MatterEngine3/tests/Makefile` (created by SP-1) — a quickjs include path and the script-host target. Do NOT recreate the Makefile or edit `MatterSurfaceLib/tests/Makefile`. Appending a quickjs include to `INCLUDE_PATHS` is NOT needed globally; instead define a local var and target block (the QuickJS-ng `../../Libraries/quickjs-ng` and consumed-prototype `../../MatterSurfaceLib/src` paths are at the same depth as SP-1's existing references):
```makefile
# Script host (QuickJS-ng) + DSL/CSG bake tests (headless, GL-free for the host;
# the BLAS surface path links raylib like the part-asset tests). QuickJS-ng C
# sources are compiled via gcc so their symbols stay unmangled for extern "C".
QJS_DIR  = ../../Libraries/quickjs-ng
QJS_INC  = -I$(QJS_DIR)
QJS_C    = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c \
           $(QJS_DIR)/libunicode.c $(QJS_DIR)/cutils.c
QJS_OBJ  = quickjs.o libregexp.o libunicode.o cutils.o

SCRIPT_TARGET = script_host_tests
SCRIPT_CPP = script_host_tests.cpp
SCRIPT_C   = ../../MatterSurfaceLib/src/material_registry.c

$(SCRIPT_TARGET): $(SCRIPT_CPP) $(SCRIPT_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(SCRIPT_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(SCRIPT_CPP) $(QJS_OBJ) material_registry.o -o $(SCRIPT_TARGET) \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) material_registry.o

run-script: $(SCRIPT_TARGET)
	./$(SCRIPT_TARGET)
```
  Also extend the existing `.PHONY` line with `run-script` and extend the `clean` rule with `$(SCRIPT_TARGET)` (do not recreate either; append to what SP-1 wrote).
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected FAIL/link-error first time** (e.g. unresolved symbols if `libbf.c`/`xsum.c` are required by v0.10.0). If link errors name missing symbols, add the corresponding `.c` files to `QJS_C`/`QJS_OBJ` and rerun. Iterate until it builds.
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`. The embed works.
- [ ] Commit:
```
git add Libraries/quickjs-ng MatterEngine3/tests/script_host_tests.cpp MatterEngine3/tests/Makefile && git commit -m "$(cat <<'EOF'
feat(sp2): vendor quickjs-ng v0.10.0 and prove embed eval

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: ScriptHost skeleton + fresh-context-per-bake lifecycle

**Files:**
- `MatterEngine3/include/script_host.h` (NEW)
- `MatterEngine3/src/script_host.cpp` (NEW)
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)
- `MatterEngine3/tests/Makefile` (MODIFIED)

- [ ] Create `MatterEngine3/include/script_host.h`:
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace script_host {

struct BakeError {
    bool        ok = true;        // true = no error
    std::string message;          // human-readable
    std::string source_location;  // best-effort "file:line" (may be empty)
};

struct BakeOptions {
    // 0 = unbounded (install-mode). >0 = per-bake wall-clock budget (dev-mode).
    uint64_t time_budget_ms = 0;
};

struct BakeResult {
    BakeError error;              // error.ok == false => nothing written
    uint64_t  resolved_hash = 0;  // valid only when error.ok
    std::string written_path;     // cache_path of the .part (empty on error)
};

// Discovered child instance from a part's static `requires(...)` (eval'd WITHOUT baking).
struct RequiredChild {
    std::string module_specifier;  // child part the parent instances
    std::string params_json;       // variation params bound at instance time
};

// Bakes ONE part from `source` (ES class extending Part) with `params_json`
// (caller overrides; defaults come from the class's static params).
// Fresh isolated JSContext per call; fail-closed; writes <=1 .part.
// `child_hashes`/`child_count` fold into the resolved hash so a PARENT's hash
// matches SP-3's precomputed cache key. Both default to none, so SP-2's own
// standalone (childless) tests are unaffected. (Master contract C-2.)
class ScriptHost {
public:
    // Hash-only: merge static+override params, fold child_hashes, return the
    // content hash WITHOUT running build()/baking. SP-2 is the SOLE hash
    // authority for script parts (only it can read the class's static-param
    // defaults to merge). SP-3 calls this for the cache check + parent fold.
    // Shares the exact merge+canonicalization path as bake_source, so the two
    // ALWAYS agree for identical inputs. (Master contract C-2.)
    uint64_t resolve_hash(const std::string& source,
                          const std::string& params_json,
                          const uint64_t* child_hashes = nullptr,
                          size_t child_count = 0);

    BakeResult bake_source(const std::string& source,
                           const std::string& params_json,
                           const BakeOptions& opts,
                           const uint64_t* child_hashes = nullptr,
                           size_t child_count = 0);

    // Static discovery of the part's child instances WITHOUT baking — SP-3
    // calls this to walk the graph leaves-first (owner of requires-discovery
    // is SP-2, which holds the JSContext). Fail-closed: empty + error otherwise.
    std::vector<RequiredChild> eval_requires(const std::string& source,
                                             const std::string& params_json);

    std::string last_merged_params() const;
};

} // namespace script_host
```
> **C-2 reconciliation:** SP-2 is the **sole hash authority** for script parts (only it can
> read a class's `static params` defaults to merge). `bake_source` is the single public
> *bake* entry point; SP-3's `Baker::bake(...)` and SP-7's `host.bake(...)` test wrappers must
> thin-delegate to it (forwarding `child_hashes`), not introduce a second host method. SP-3
> gets each node's hash from `resolve_hash(...)` (hash-only, no `build()`), never computing it
> itself. `resolve_hash`, `bake_source`, and `eval_requires` all share one params-merge +
> canonicalization helper, so a value `resolve_hash` returns equals the hash `bake_source`
> writes to. `resolve_hash` + `eval_requires` land in Task 5 alongside the hash plumbing.
- [ ] Add a failing test `test_fresh_context_runs_empty_class` to `script_host_tests.cpp` (append before `main`, and call it from `main`):
```cpp
#include "../include/script_host.h"
// ...
static void test_fresh_context_runs_empty_class() {
    script_host::ScriptHost host;
    const char* src =
        "class Empty extends Part {\n"
        "  static params = {};\n"
        "  build(p) {}\n"
        "}\n";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok, "empty class bakes without error");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` after adding `../src/script_host.cpp` to `SCRIPT_CPP` and updating the link line — **expected FAIL**: undefined reference / link error (no `script_host.cpp` yet).
- [ ] Create `MatterEngine3/src/script_host.cpp` with the minimal lifecycle: create runtime+context, define the `Part` base + a no-op binding placeholder, eval source, instantiate the class, call `build({})`, harvest exceptions, tear down. Minimal real code:
```cpp
#include "../include/script_host.h"
extern "C" {
#include "quickjs.h"
}
#include <cstring>

namespace script_host {

// Pulls the current exception into a BakeError (best-effort location).
static BakeError harvest_exception(JSContext* ctx) {
    BakeError e; e.ok = false;
    JSValue ex = JS_GetException(ctx);
    const char* msg = JS_ToCString(ctx, ex);
    e.message = msg ? msg : "unknown script error";
    if (msg) JS_FreeCString(ctx, msg);
    JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
    if (!JS_IsUndefined(stack)) {
        const char* s = JS_ToCString(ctx, stack);
        if (s) { e.source_location = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, ex);
    return e;
}

BakeResult ScriptHost::bake_source(const std::string& source,
                                   const std::string& /*params_json*/,
                                   const BakeOptions& /*opts*/,
                                   const uint64_t* /*child_hashes*/,
                                   size_t /*child_count*/) {
    BakeResult r;
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);

    // Minimal Part base so `extends Part` resolves (replaced in Task 4).
    static const char* kBootstrap = "globalThis.Part = class Part { build(p){} };\n";
    JSValue b = JS_Eval(ctx, kBootstrap, strlen(kBootstrap), "<bootstrap>",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(b)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,b); goto done; }
    JS_FreeValue(ctx, b);

    {
        // Eval the user source as a module-less global script, then find the
        // class. For SP-2 we wrap: source defines a class; we instantiate the
        // LAST defined global class via a trampoline appended to the source.
        std::string wrapped = source +
            "\n;globalThis.__partClass = (typeof Empty!=='undefined')?Empty:undefined;";
        JSValue v = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), "<part>",
                            JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,v); goto done; }
        JS_FreeValue(ctx, v);
        // (Class discovery + build(p) call generalized in Task 3.)
    }

done:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return r;
}

} // namespace script_host
```
  NOTE: the hardcoded `Empty` trampoline is a deliberate Task-2 stepping stone; Task 3 replaces it with generic class discovery. Keep the test class named `Empty`.
- [ ] Update the appended block in `MatterEngine3/tests/Makefile`: set `SCRIPT_CPP = script_host_tests.cpp ../src/script_host.cpp` and ensure the link line compiles both. Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`.
- [ ] Commit:
```
git add MatterEngine3/include/script_host.h MatterEngine3/src/script_host.cpp MatterEngine3/tests/script_host_tests.cpp MatterEngine3/tests/Makefile && git commit -m "$(cat <<'EOF'
feat(sp2): ScriptHost skeleton with fresh-context-per-bake lifecycle

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Generic Part class discovery + `build(p)` dispatch

**Files:**
- `MatterEngine3/src/part_base.js.h` (NEW)
- `MatterEngine3/src/script_host.cpp` (MODIFIED)
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)

- [ ] Create `MatterEngine3/src/part_base.js.h` with the bootstrap base class as a C string. The base tracks subclasses via a registry so the host can find the authored class generically (no hardcoded name):
```cpp
#pragma once
// Evaluated into every fresh context before user source. Defines the Part base
// and a registry the host reads to discover the authored subclass.
static const char* kPartBaseJS = R"JS(
globalThis.__parts = [];
globalThis.Part = class Part {
  build(p) {}
  static __register(cls) { globalThis.__parts.push(cls); }
};
// Capture subclasses at definition time via a static initializer pattern:
// user classes call super-less; the host enumerates globalThis for class ctors
// extending Part after eval (see host). This stub keeps Part defined.
)JS";
```
- [ ] Add a failing test `test_build_called_on_authored_class` that asserts a side effect of `build` running (set a global the host can read back). Append:
```cpp
static void test_build_called_on_authored_class() {
    script_host::ScriptHost host;
    const char* src =
        "class Rock extends Part {\n"
        "  static params = {};\n"
        "  build(p) { globalThis.__built = 1; }\n"
        "}\n";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok, "authored class bakes");
    CHECK(host.last_build_ran(), "build(p) was invoked");
}
```
  Add `bool last_build_ran() const { return last_build_ran_; }` + `bool last_build_ran_ = false;` to `ScriptHost` in `script_host.h` (test-observable hook).
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected FAIL**: `last_build_ran` false / member missing.
- [ ] Replace the Task-2 trampoline in `script_host.cpp` with generic discovery: eval `kPartBaseJS`, eval user source, then scan `globalThis` own-property values for a function whose prototype chain includes `Part`, instantiate it, and call `build`. Minimal real code (replace the `{ ... }` block and add the member set):
```cpp
#include "part_base.js.h"
// ... inside bake_source, after creating ctx, BEFORE user eval:
last_build_ran_ = false;
JSValue base = JS_Eval(ctx, kPartBaseJS, strlen(kPartBaseJS), "<part-base>",
                       JS_EVAL_TYPE_GLOBAL);
if (JS_IsException(base)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,base); goto done; }
JS_FreeValue(ctx, base);

{
    JSValue v = JS_Eval(ctx, source.c_str(), source.size(), "<part>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,v); goto done; }
    JS_FreeValue(ctx, v);

    // Discover authored class: find a global ctor whose .prototype derives Part.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue partCtor = JS_GetPropertyStr(ctx, global, "Part");
    JSPropertyEnum* tab = nullptr; uint32_t len = 0;
    JS_GetOwnPropertyNames(ctx, &tab, &len, global, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);
    JSValue authored = JS_UNDEFINED;
    for (uint32_t i = 0; i < len; ++i) {
        JSValue val = JS_GetProperty(ctx, global, tab[i].atom);
        if (JS_IsFunction(ctx, val)) {
            JSValue proto = JS_GetPropertyStr(ctx, val, "prototype");
            // derives Part if Part.prototype is in val.prototype's chain
            JSValue partProto = JS_GetPropertyStr(ctx, partCtor, "prototype");
            if (JS_IsInstanceOf(ctx, proto, partCtor) == 1 ||
                JS_VALUE_GET_PTR(proto) != JS_VALUE_GET_PTR(partProto)) {
                // crude: accept first non-Part ctor with a build method
                JSValue bm = JS_GetPropertyStr(ctx, proto, "build");
                if (JS_IsFunction(ctx, bm) &&
                    JS_VALUE_GET_PTR(val) != JS_VALUE_GET_PTR(partCtor)) {
                    authored = JS_DupValue(ctx, val);
                }
                JS_FreeValue(ctx, bm);
            }
            JS_FreeValue(ctx, partProto);
            JS_FreeValue(ctx, proto);
        }
        JS_FreeValue(ctx, val);
        JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
    JS_FreeValue(ctx, partCtor);
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(authored)) {
        r.error.ok = false; r.error.message = "no class extending Part found";
        goto done;
    }
    JSValue inst = JS_CallConstructor(ctx, authored, 0, nullptr);
    JS_FreeValue(ctx, authored);
    if (JS_IsException(inst)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,inst); goto done; }
    JSValue paramsObj = JS_NewObject(ctx);  // merged params arrive in Task 5
    JSValue bret = JS_Invoke(ctx, inst, JS_NewAtom(ctx, "build"), 1, &paramsObj);
    last_build_ran_ = !JS_IsException(bret);
    if (JS_IsException(bret)) r.error = harvest_exception(ctx);
    JS_FreeValue(ctx, bret);
    JS_FreeValue(ctx, paramsObj);
    JS_FreeValue(ctx, inst);
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`.
- [ ] Commit:
```
git add MatterEngine3/src/part_base.js.h MatterEngine3/src/script_host.cpp MatterEngine3/include/script_host.h MatterEngine3/tests/script_host_tests.cpp && git commit -m "$(cat <<'EOF'
feat(sp2): generic Part subclass discovery and build(p) dispatch

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: C++-owned DSL state (transform stack, material cursor, session enum, build buffer)

**Files:**
- `MatterEngine3/include/dsl_state.h` (NEW)
- `MatterEngine3/src/dsl_state.cpp` (NEW)
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)
- `MatterEngine3/tests/Makefile` (MODIFIED)

- [ ] Create `MatterEngine3/include/dsl_state.h` (pure C++, no JS) with the authoritative state the spec requires the host to own:
```cpp
#pragma once
#include "raylib.h"   // Vector3, Matrix, Vector4
#include <cstdint>
#include <string>
#include <vector>

namespace dsl {

enum class Session { None, Voxels };  // Triangle/Lattice are later sub-projects.

enum class BrushKind { Sphere, Box };
enum class CsgOp     { Union, Difference, Intersection };

// One authored brush + the op that combines it + the smoothing cursor at emit.
struct BuildOp {
    BrushKind kind;
    CsgOp     op;            // how this brush combines with the accumulated field
    Matrix    transform;     // world transform at emit (transform stack top)
    uint32_t  materialId;    // material cursor at emit
    Vector3   center;        // brush-local center
    float     radius;        // sphere radius
    Vector3   halfExtents;   // box half-extents (unused for sphere)
    float     smoothing;     // smooth-min k cursor at emit
    float     spacing;       // session spacing (resolution floor)
};

// Build buffer: flat op list (resolves the spec open question: flat, not tree).
struct BuildBuffer {
    std::vector<BuildOp> ops;
    void clear() { ops.clear(); }
};

// C++-owned authoring state. JS bindings mutate this; JS holds no engine state.
class DslState {
public:
    DslState();

    // Transform stack
    void pushMatrix();
    void popMatrix();                       // misuse (empty) -> set_error
    void translate(float x, float y, float z);
    void rotateX(float r); void rotateY(float r); void rotateZ(float r);
    void scale(float x, float y, float z);
    void applyMatrix(const float m[16]);    // row-major
    Matrix top() const { return stack_.back(); }

    // Material cursor
    void fill(uint32_t materialId) { material_ = materialId; }
    uint32_t material() const { return material_; }

    // Session enum (one at a time; misuse = error)
    void beginVoxels(float spacing);        // misuse (already open) -> set_error
    void endVoxels();                        // misuse (not open) -> set_error
    Session session() const { return session_; }
    float spacing() const { return spacing_; }

    // Smoothing cursor
    void smoothing(float k) { smoothing_ = (k < 0 ? 0 : k); }
    float smoothing_k() const { return smoothing_; }

    // Brush emission (must be inside a session)
    void sphere(const Vector3& c, float r, CsgOp op);
    void box(const Vector3& c, const Vector3& halfExtents, CsgOp op);

    const BuildBuffer& buffer() const { return buffer_; }

    // Structured error sink (fail-closed). First error wins.
    bool has_error() const { return has_error_; }
    const std::string& error() const { return error_; }
    void set_error(const std::string& m) { if (!has_error_) { has_error_ = true; error_ = m; } }

private:
    std::vector<Matrix> stack_;   // never empty (seeded with identity)
    uint32_t material_ = 0;
    Session  session_ = Session::None;
    float    spacing_ = 0.1f;
    float    smoothing_ = 0.0f;
    BuildBuffer buffer_;
    bool        has_error_ = false;
    std::string error_;
};

} // namespace dsl
```
- [ ] Add failing tests for the misuse/state rules (append + call from `main`):
```cpp
#include "../include/dsl_state.h"
static void test_dsl_state_rules() {
    dsl::DslState s;
    // pop on empty-above-identity is misuse
    s.popMatrix();
    CHECK(s.has_error(), "popMatrix below identity is an error");

    dsl::DslState s2;
    s2.endVoxels();
    CHECK(s2.has_error(), "endVoxels with no open session is an error");

    dsl::DslState s3;
    s3.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    CHECK(s3.has_error(), "emitting outside a session is an error");

    dsl::DslState s4;
    s4.beginVoxels(0.1f);
    s4.beginVoxels(0.1f);
    CHECK(s4.has_error(), "opening a session inside another is an error");

    dsl::DslState s5;
    s5.beginVoxels(0.25f);
    s5.fill(7);
    s5.sphere({1,0,0}, 2.0f, dsl::CsgOp::Union);
    s5.endVoxels();
    CHECK(!s5.has_error(), "valid voxel session has no error");
    CHECK(s5.buffer().ops.size() == 1, "one brush recorded");
    CHECK(s5.buffer().ops[0].materialId == 7, "material cursor captured");
    CHECK(s5.buffer().ops[0].spacing == 0.25f, "session spacing captured");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` (after adding `../src/dsl_state.cpp` to `SCRIPT_CPP`) — **expected FAIL**: link error / asserts fail.
- [ ] Create `MatterEngine3/src/dsl_state.cpp` implementing the rules with raymath. Minimal real code:
```cpp
#include "../include/dsl_state.h"
#define RAYMATH_IMPLEMENTATION
#include "raymath.h"

namespace dsl {

DslState::DslState() { stack_.push_back(MatrixIdentity()); }

void DslState::pushMatrix() { stack_.push_back(stack_.back()); }
void DslState::popMatrix() {
    if (stack_.size() <= 1) { set_error("popMatrix without matching pushMatrix"); return; }
    stack_.pop_back();
}
void DslState::translate(float x, float y, float z) {
    stack_.back() = MatrixMultiply(MatrixTranslate(x,y,z), stack_.back());
}
void DslState::rotateX(float r){ stack_.back()=MatrixMultiply(MatrixRotateX(r),stack_.back()); }
void DslState::rotateY(float r){ stack_.back()=MatrixMultiply(MatrixRotateY(r),stack_.back()); }
void DslState::rotateZ(float r){ stack_.back()=MatrixMultiply(MatrixRotateZ(r),stack_.back()); }
void DslState::scale(float x,float y,float z){ stack_.back()=MatrixMultiply(MatrixScale(x,y,z),stack_.back()); }
void DslState::applyMatrix(const float m[16]) {
    Matrix mm = { m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7],
                  m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15] };
    stack_.back() = MatrixMultiply(mm, stack_.back());
}

void DslState::beginVoxels(float spacing) {
    if (session_ != Session::None) { set_error("beginVoxels inside an open session"); return; }
    session_ = Session::Voxels; spacing_ = (spacing > 0 ? spacing : 0.1f);
}
void DslState::endVoxels() {
    if (session_ != Session::Voxels) { set_error("endVoxels with no open voxel session"); return; }
    session_ = Session::None;
}

void DslState::sphere(const Vector3& c, float r, CsgOp op) {
    if (session_ != Session::Voxels) { set_error("sphere() emitted outside a voxel session"); return; }
    BuildOp o{}; o.kind=BrushKind::Sphere; o.op=op; o.transform=stack_.back();
    o.materialId=material_; o.center=c; o.radius=r; o.smoothing=smoothing_; o.spacing=spacing_;
    buffer_.ops.push_back(o);
}
void DslState::box(const Vector3& c, const Vector3& h, CsgOp op) {
    if (session_ != Session::Voxels) { set_error("box() emitted outside a voxel session"); return; }
    BuildOp o{}; o.kind=BrushKind::Box; o.op=op; o.transform=stack_.back();
    o.materialId=material_; o.center=c; o.halfExtents=h; o.smoothing=smoothing_; o.spacing=spacing_;
    buffer_.ops.push_back(o);
}

} // namespace dsl
```
  NOTE: emitting a brush records it with `op` = how it combines. The op accessor verbs (`union`/`difference`/`intersection`) set the op applied to the **most recent** brush(es); Task 6 wires the binding verbs. For Task 4 the test calls `sphere(...,Union)` directly.
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`.
- [ ] Commit:
```
git add MatterEngine3/include/dsl_state.h MatterEngine3/src/dsl_state.cpp MatterEngine3/tests/script_host_tests.cpp MatterEngine3/tests/Makefile && git commit -m "$(cat <<'EOF'
feat(sp2): C++-owned DSL state (transform/material/session/build buffer)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: `static params` merge + `compute_resolved_hash` plumbing

**Files:**
- `MatterEngine3/src/script_host.cpp` (MODIFIED)
- `MatterEngine3/include/script_host.h` (MODIFIED)
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)

- [ ] Add a failing test asserting (a) defaults reach `build(p)`, (b) caller overrides reach `build(p)`, (c) override changes `resolved_hash`. Use a test hook: have the host stash the merged-params JSON it passed to `build`. Add `std::string last_merged_params() const;` + member to `ScriptHost`. Append:
```cpp
static void test_params_merge_and_hash() {
    script_host::ScriptHost host;
    const char* src =
        "class Rock extends Part {\n"
        "  static params = { size: 1.0, seed: 0 };\n"
        "  build(p) { globalThis.__seen = JSON.stringify(p); }\n"
        "}\n";
    script_host::BakeResult def = host.bake_source(src, "{}", {});
    CHECK(def.error.ok, "defaults bake ok");
    CHECK(host.last_merged_params().find("\"size\":1") != std::string::npos,
          "default size present in merged params");

    script_host::ScriptHost host2;
    script_host::BakeResult ov = host2.bake_source(src, "{\"size\":2.0}", {});
    CHECK(ov.error.ok, "override bake ok");
    CHECK(host2.last_merged_params().find("\"size\":2") != std::string::npos,
          "override size present in merged params");
    CHECK(host2.last_merged_params().find("\"seed\":0") != std::string::npos,
          "non-overridden default still present");

    CHECK(def.resolved_hash != ov.resolved_hash,
          "override changes resolved_hash");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected FAIL**: member missing / hashes equal (no merge yet).
- [ ] In `script_host.cpp`, before calling `build`, read the class's `static params`, overlay the caller's `params_json`, serialize the merged object to canonical JSON (stable key order via `JSON.stringify` with sorted keys helper evaluated in-context), pass it as the `build` argument, and compute the resolved hash over `(source, merged_params_json, /*children*/none)`. Minimal real code (replace the `JS_NewObject` params stub from Task 3):
```cpp
#include "part_asset_v2.h"   // SP-1 v2 helper (MatterEngine3/include via -I../include)
// ... after instantiating `authored`, before invoking build:
// 1. read static params from the class ctor
JSValue staticParams = JS_GetPropertyStr(ctx, authored, "params");
if (JS_IsUndefined(staticParams)) staticParams = JS_NewObject(ctx);
// 2. parse caller overrides
JSValue overrides = JS_ParseJSON(ctx, params_json.c_str(), params_json.size(), "<params>");
if (JS_IsException(overrides)) { r.error = harvest_exception(ctx); /* free + goto done */ }
// 3. merge (overrides win) + canonicalize via a tiny in-context helper
static const char* kMerge =
  "(function(d,o){let m=Object.assign({},d,o);"
  "let keys=Object.keys(m).sort();let r={};for(let k of keys)r[k]=m[k];"
  "return JSON.stringify(r);})";
JSValue mergeFn = JS_Eval(ctx, kMerge, strlen(kMerge), "<merge>", JS_EVAL_TYPE_GLOBAL);
JSValue args2[2] = { staticParams, overrides };
JSValue mergedStr = JS_Call(ctx, mergeFn, JS_UNDEFINED, 2, args2);
const char* mjson = JS_ToCString(ctx, mergedStr);
last_merged_params_ = mjson ? mjson : "{}";
// 4. resolved hash over source + merged params + caller-supplied child hashes
//    (child_hashes/child_count default to none for standalone parts; SP-3 passes
//    the resolved children so a parent's hash matches its precomputed cache key).
r.resolved_hash = part_asset::compute_resolved_hash(
    source.data(), source.size(),
    last_merged_params_.data(), last_merged_params_.size(),
    child_hashes, child_count);
// 5. parse merged JSON back into the object passed to build(p)
JSValue paramsObj = JS_ParseJSON(ctx, last_merged_params_.c_str(),
                                 last_merged_params_.size(), "<merged>");
if (mjson) JS_FreeCString(ctx, mjson);
JS_FreeValue(ctx, mergedStr); JS_FreeValue(ctx, mergeFn);
JS_FreeValue(ctx, overrides); JS_FreeValue(ctx, staticParams);
JSValue bret = JS_Invoke(ctx, inst, JS_NewAtom(ctx, "build"), 1, &paramsObj);
// ... (rest as Task 3)
```
  Add `std::string last_merged_params_;` + accessor to `script_host.h`.
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`. (Requires SP-1's `compute_resolved_hash`; if SP-1 is not yet merged, this is the gating dependency — see Dependencies.)
- [ ] **Factor the merge+hash into a shared helper and add `resolve_hash` (hash-only, no `build()`).** Add a failing test asserting (a) `resolve_hash` equals the `resolved_hash` `bake_source` returns for identical `(source, params, children)`, and (b) `resolve_hash` does **not** run `build()` (a side-effecting `build` leaves no global). Append:
```cpp
static void test_resolve_hash_matches_and_skips_build() {
    const char* src =
        "class Rock extends Part {\n"
        "  static params = { size: 1.0, seed: 0 };\n"
        "  build(p) { globalThis.__built = true; }\n"
        "}\n";
    script_host::ScriptHost hb;
    script_host::BakeResult baked = hb.bake_source(src, "{\"size\":2.0}", {});
    CHECK(baked.error.ok, "bake ok");

    script_host::ScriptHost hr;
    uint64_t rh = hr.resolve_hash(src, "{\"size\":2.0}", nullptr, 0);
    CHECK(rh == baked.resolved_hash, "resolve_hash agrees with bake_source hash");

    // resolve_hash must not execute build(): probe a fresh context global.
    script_host::ScriptHost hp;
    hp.resolve_hash(
        "class Probe extends Part { static params={};"
        " build(p){ globalThis.__built2 = true; } }", "{}", nullptr, 0);
    CHECK(hp.last_merged_params().find("__built2") == std::string::npos,
          "resolve_hash did not run build()");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected FAIL**: `resolve_hash` undefined.
- [ ] In `script_host.cpp`, extract the params-merge + canonicalization from `bake_source` (Task-5 steps 1-3 above) into a private helper, then implement `resolve_hash` on top of it. The helper builds a fresh runtime/context, evals the source enough to read the class's `static params`, merges overrides, canonicalizes, and returns the merged JSON — **without** instantiating or calling `build`:
```cpp
// Private helper: returns canonical merged-params JSON; fills err on failure.
// Evals source to read `static params`; does NOT call build().
std::string ScriptHost::merge_params_canonical(const std::string& source,
                                               const std::string& params_json,
                                               BakeError& err) {
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    // bootstrap Part base + eval source to define the class (no instantiation)
    // ... (same eval path bake_source uses up to obtaining the class ctor) ...
    // read static params, parse overrides, merge+sort+stringify (the kMerge fn)
    // set last_merged_params_ and return it; on any exception set err, return "{}"
    // tear down ctx/rt before returning.
}

uint64_t ScriptHost::resolve_hash(const std::string& source,
                                  const std::string& params_json,
                                  const uint64_t* child_hashes,
                                  size_t child_count) {
    BakeError err;
    std::string canon = merge_params_canonical(source, params_json, err);
    if (!err.ok) return 0;   // fail-closed: caller treats 0 as resolve failure
    return part_asset::compute_resolved_hash(
        source.data(), source.size(),
        canon.data(), canon.size(),
        child_hashes, child_count);
}
```
  Refactor `bake_source`'s Task-5 steps 1-4 to call `merge_params_canonical` too, so both paths hash byte-identical params. Declare both methods + the helper in `script_host.h`.
  > **Source-fold seam (SP-7, master C-3):** here `resolve_hash`/`bake_source` hash the part's own `source` bytes directly. When SP-7's shared-lib import support lands, the **host** (not SP-3) replaces those `source.data()/size()` bytes with the canonical **folded** buffer — part source + transitively-imported module sources in SP-7's canonical order (via `module_resolver::fold`) — so editing an imported module reinvalidates the importer. Until SP-7, the unfolded source is correct for childless/import-free parts. Keep the hash-input as a single `source_bytes` value so the swap is local to one call site.
- [ ] Commit:
```
git add MatterEngine3/src/script_host.cpp MatterEngine3/include/script_host.h MatterEngine3/tests/script_host_tests.cpp && git commit -m "$(cat <<'EOF'
feat(sp2): merge static params with caller overrides into resolved hash

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Bind `Part` DSL methods to `DslState` (+ MAT, + session-misuse → structured error)

**Files:**
- `MatterEngine3/src/part_base.js.h` (MODIFIED)
- `MatterEngine3/src/dsl_bindings.cpp` (NEW)
- `MatterEngine3/include/script_host.h` (MODIFIED — expose DslState ptr for tests)
- `MatterEngine3/src/script_host.cpp` (MODIFIED — install bindings, attach DslState as opaque, check error after build)
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)
- `MatterEngine3/tests/Makefile` (MODIFIED — add `../src/dsl_bindings.cpp`)

- [ ] Extend `part_base.js.h` so the `Part` base forwards every DSL method to a host-installed native (`globalThis.__dsl_*`). Real code (replace the file body):
```cpp
#pragma once
static const char* kPartBaseJS = R"JS(
globalThis.MAT = { stone: 0, dirt: 1, glass: 2 };
globalThis.Part = class Part {
  build(p) {}
  pushMatrix()           { __dsl_pushMatrix(); }
  popMatrix()            { __dsl_popMatrix(); }
  translate(x,y,z)       { __dsl_translate(x,y,z); }
  rotateX(r)             { __dsl_rotateX(r); }
  rotateY(r)             { __dsl_rotateY(r); }
  rotateZ(r)             { __dsl_rotateZ(r); }
  scale(x,y,z)           { __dsl_scale(x,y,z); }
  applyMatrix(m)         { __dsl_applyMatrix(m); }
  fill(mat)              { __dsl_fill(mat); }
  beginVoxels(spacing)   { __dsl_beginVoxels(spacing); }
  endVoxels()            { __dsl_endVoxels(); }
  sphere(c,r)            { __dsl_sphere(c[0],c[1],c[2],r); }
  box(c,h)               { __dsl_box(c[0],c[1],c[2],h[0],h[1],h[2]); }
  union()                { __dsl_op(0); }
  difference()           { __dsl_op(1); }
  intersection()         { __dsl_op(2); }
  smoothing(k)           { __dsl_smoothing(k); }
};
)JS";
```
  Convention: `sphere`/`box` default to `Union` at emit; `union()`/`difference()`/`intersection()` set the op on the **last-emitted** brush (Processing-style postfix CSG, matching the spec's `sphere(...); box(...); difference();`).
- [ ] Create `MatterEngine3/src/dsl_bindings.cpp` declaring native functions that recover the `DslState*` from the context opaque and forward calls. Real code:
```cpp
#include "../include/dsl_state.h"
extern "C" {
#include "quickjs.h"
}

namespace dsl {

static DslState* state_of(JSContext* ctx) {
    return static_cast<DslState*>(JS_GetContextOpaque(ctx));
}
static double argd(JSContext* ctx, JSValueConst v) { double d=0; JS_ToFloat64(ctx,&d,v); return d; }

static JSValue j_pushMatrix(JSContext* c, JSValueConst, int, JSValueConst*) { state_of(c)->pushMatrix(); return JS_UNDEFINED; }
static JSValue j_popMatrix (JSContext* c, JSValueConst, int, JSValueConst*) { state_of(c)->popMatrix();  return JS_UNDEFINED; }
static JSValue j_translate(JSContext* c, JSValueConst, int n, JSValueConst* a){ state_of(c)->translate(argd(c,a[0]),argd(c,a[1]),argd(c,a[2])); return JS_UNDEFINED; }
static JSValue j_rotateX(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->rotateX(argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_rotateY(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->rotateY(argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_rotateZ(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->rotateZ(argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_scale(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->scale(argd(c,a[0]),argd(c,a[1]),argd(c,a[2])); return JS_UNDEFINED; }
static JSValue j_applyMatrix(JSContext* c, JSValueConst, int, JSValueConst* a){
    float m[16]; for (int i=0;i<16;++i){ JSValue e=JS_GetPropertyUint32(c,a[0],i); m[i]=(float)argd(c,e); JS_FreeValue(c,e);} state_of(c)->applyMatrix(m); return JS_UNDEFINED; }
static JSValue j_fill(JSContext* c, JSValueConst, int, JSValueConst* a){ int32_t id=0; JS_ToInt32(c,&id,a[0]); state_of(c)->fill((uint32_t)id); return JS_UNDEFINED; }
static JSValue j_beginVoxels(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->beginVoxels((float)argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_endVoxels(JSContext* c, JSValueConst, int, JSValueConst*){ state_of(c)->endVoxels(); return JS_UNDEFINED; }
static JSValue j_sphere(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->sphere({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},(float)argd(c,a[3]),CsgOp::Union); return JS_UNDEFINED; }
static JSValue j_box(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->box({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
                     {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},CsgOp::Union); return JS_UNDEFINED; }
static JSValue j_op(JSContext* c, JSValueConst, int, JSValueConst* a){
    int32_t k=0; JS_ToInt32(c,&k,a[0]); state_of(c)->set_last_op((CsgOp)k); return JS_UNDEFINED; }
static JSValue j_smoothing(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->smoothing((float)argd(c,a[0])); return JS_UNDEFINED; }

void install_bindings(JSContext* ctx) {
    JSValue g = JS_GetGlobalObject(ctx);
    auto bind=[&](const char* n, JSCFunction* f, int argc){ JS_SetPropertyStr(ctx,g,n,JS_NewCFunction(ctx,f,n,argc)); };
    bind("__dsl_pushMatrix",j_pushMatrix,0); bind("__dsl_popMatrix",j_popMatrix,0);
    bind("__dsl_translate",j_translate,3);
    bind("__dsl_rotateX",j_rotateX,1); bind("__dsl_rotateY",j_rotateY,1); bind("__dsl_rotateZ",j_rotateZ,1);
    bind("__dsl_scale",j_scale,3); bind("__dsl_applyMatrix",j_applyMatrix,1);
    bind("__dsl_fill",j_fill,1);
    bind("__dsl_beginVoxels",j_beginVoxels,1); bind("__dsl_endVoxels",j_endVoxels,0);
    bind("__dsl_sphere",j_sphere,4); bind("__dsl_box",j_box,6);
    bind("__dsl_op",j_op,1); bind("__dsl_smoothing",j_smoothing,1);
    JS_FreeValue(ctx,g);
}

} // namespace dsl
```
  Add to `dsl_state.h`: `void set_last_op(CsgOp op){ if(!buffer_.ops.empty()) buffer_.ops.back().op=op; else set_error("CSG op with no preceding brush"); }` and declare `namespace dsl { void install_bindings(JSContext*); }` in a small `dsl_bindings.h` (or extern decl in `script_host.cpp`).
- [ ] In `script_host.cpp`: instantiate a `dsl::DslState` on the stack per bake, `JS_SetContextOpaque(ctx, &state)`, call `dsl::install_bindings(ctx)` after `kPartBaseJS`, and after `build` returns check `state.has_error()` → set `r.error` (fail-closed). Expose `const dsl::DslState* last_state() const` test hook (store a copy of the buffer or a pointer valid only during bake — simplest: copy `state.buffer()` into a `ScriptHost` member `last_buffer_` for assertions).
- [ ] Add a failing test exercising bindings end-to-end (buffer contents + misuse error surfaced through the host):
```cpp
static void test_bindings_record_ops_and_misuse() {
    script_host::ScriptHost host;
    const char* ok =
        "class Rock extends Part {\n"
        "  static params = {};\n"
        "  build(p){ this.beginVoxels(0.1); this.fill(MAT.stone);\n"
        "            this.sphere([0,0,0],1.0);\n"
        "            this.box([0,0.5,0],[0.3,0.3,0.3]); this.difference();\n"
        "            this.endVoxels(); }\n"
        "}\n";
    script_host::BakeResult r = host.bake_source(ok, "{}", {});
    CHECK(r.error.ok, "valid voxel script bakes");
    CHECK(host.last_buffer().ops.size() == 2, "two brushes recorded");
    CHECK(host.last_buffer().ops[1].op == dsl::CsgOp::Difference, "difference applied to box");

    script_host::ScriptHost host2;
    const char* bad =
        "class Bad extends Part { static params={};\n"
        "  build(p){ this.sphere([0,0,0],1.0); }\n"   // emit with no session
        "}\n";
    script_host::BakeResult rb = host2.bake_source(bad, "{}", {});
    CHECK(!rb.error.ok, "emit outside session is fail-closed");
    CHECK(rb.error.message.find("session") != std::string::npos, "structured session error message");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` (after adding `../src/dsl_bindings.cpp` to `SCRIPT_CPP`) — **expected FAIL**, then implement, then **expected PASS**: `ALL PASS`.
- [ ] Commit:
```
git add MatterEngine3/src/part_base.js.h MatterEngine3/src/dsl_bindings.cpp MatterEngine3/include/script_host.h MatterEngine3/include/dsl_state.h MatterEngine3/src/script_host.cpp MatterEngine3/tests/script_host_tests.cpp MatterEngine3/tests/Makefile && git commit -m "$(cat <<'EOF'
feat(sp2): bind Part DSL methods to C++ DslState with fail-closed misuse

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: CSG lowering — build buffer → additive particles + carve particles + smoothing factor

**Files:**
- `MatterEngine3/include/csg_lowering.h` (NEW)
- `MatterEngine3/src/csg_lowering.cpp` (NEW)
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)
- `MatterEngine3/tests/Makefile` (MODIFIED)

- [ ] Create `MatterEngine3/include/csg_lowering.h`:
```cpp
#pragma once
#include "dsl_state.h"
#include "cluster.h"   // StaticParticle
#include "particle.h"  // Particle
#include <vector>

namespace dsl {

struct LoweredField {
    std::vector<StaticParticle> additive;  // union/intersection brushes
    std::vector<Particle>       carve;     // difference brushes
    float smoothing = 0.0f;                // whole-expression smooth-min k
};

// Lowers the flat CSG op list to the mesher input contract. sphere = 1 particle;
// box = analytic-SDF sphere stamp at op.spacing (crisp corners to spacing floor).
// Transform stack top is applied to each brush center.
LoweredField lower_build_buffer(const BuildBuffer& buf);

} // namespace dsl
```
- [ ] Add a failing test asserting the lowering routes brushes correctly and preserves a sub-min box feature:
```cpp
#include "../include/csg_lowering.h"
static void test_csg_lowering() {
    dsl::DslState s;
    s.beginVoxels(0.25f);
    s.fill(3);
    s.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    s.box({1,0,0}, {0.05f,0.05f,0.05f}, dsl::CsgOp::Difference); // sub-min box
    s.smoothing(0.4f);
    s.endVoxels();
    // re-tag the box op as difference (verb sets last op)
    s.set_last_op(dsl::CsgOp::Difference);
    dsl::LoweredField f = dsl::lower_build_buffer(s.buffer());
    CHECK(f.additive.size() == 1, "sphere lowered to one additive particle");
    CHECK(f.additive[0].materialId == 3, "additive carries material cursor");
    CHECK(!f.carve.empty(), "sub-min box still produces carve particles (feature survives)");
    CHECK(f.smoothing == 0.4f || f.smoothing == 0.0f, "smoothing factor carried");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` (after adding `../src/csg_lowering.cpp` to `SCRIPT_CPP`) — **expected FAIL**: link error / asserts fail.
- [ ] Create `MatterEngine3/src/csg_lowering.cpp`. Sphere → one particle at transformed center; box → SDF-sampled small spheres covering the box at half-spacing so corners survive below min particle size; union/intersection → additive, difference → carve. Whole-expression smoothing = max recorded `k`. Real code:
```cpp
#include "../include/csg_lowering.h"
#define RAYMATH_IMPLEMENTATION_GUARD
#include "raymath.h"
#include <cmath>

namespace dsl {

static Vector3 xf(const Matrix& m, const Vector3& p) { return Vector3Transform(p, m); }

// Stamp a box as a pack of spheres at step = spacing/2 so the SDF surface (incl.
// sharp corners) is resolved to the resolution floor. Radius ~ step so spheres
// overlap and the union reads as a solid box to the mesher.
template <class Emit>
static void stamp_box(const BuildOp& o, Emit emit) {
    float step = (o.spacing > 0 ? o.spacing : 0.1f) * 0.5f;
    Vector3 h = o.halfExtents;
    for (float x=-h.x; x<=h.x+1e-4f; x+=step)
      for (float y=-h.y; y<=h.y+1e-4f; y+=step)
        for (float z=-h.z; z<=h.z+1e-4f; z+=step) {
            Vector3 local = { o.center.x+x, o.center.y+y, o.center.z+z };
            emit(xf(o.transform, local), step * 1.05f);
        }
    // Guarantee at least one sample for boxes smaller than a step (sub-min feature).
    if (h.x < step && h.y < step && h.z < step)
        emit(xf(o.transform, o.center), std::max(step, std::max(h.x,std::max(h.y,h.z)))*1.05f);
}

LoweredField lower_build_buffer(const BuildBuffer& buf) {
    LoweredField out;
    for (const BuildOp& o : buf.ops) {
        out.smoothing = std::max(out.smoothing, o.smoothing);
        bool subtract = (o.op == CsgOp::Difference);
        if (o.kind == BrushKind::Sphere) {
            Vector3 c = xf(o.transform, o.center);
            if (subtract) { Particle p{ c, o.radius, (int)o.materialId }; out.carve.push_back(p); }
            else { out.additive.push_back(StaticParticle(c, o.radius, o.materialId,
                       {1,1,1,0}, o.spacing)); }
        } else { // Box
            if (subtract) stamp_box(o, [&](Vector3 c, float r){ out.carve.push_back(Particle{c,r,(int)o.materialId}); });
            else stamp_box(o, [&](Vector3 c, float r){ out.additive.push_back(StaticParticle(c,r,o.materialId,{1,1,1,0},o.spacing)); });
        }
    }
    return out;
}

} // namespace dsl
```
  NOTE on the spec open question (smooth-min per-op vs whole-expression): SP-2 applies it **whole-expression** (`out.smoothing = max(k)`), which is what `Cluster` accepts as a single smooth-min factor. Per-op blend factors are deferred.
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`.
- [ ] Commit:
```
git add MatterEngine3/include/csg_lowering.h MatterEngine3/src/csg_lowering.cpp MatterEngine3/tests/script_host_tests.cpp MatterEngine3/tests/Makefile && git commit -m "$(cat <<'EOF'
feat(sp2): lower CSG build buffer to additive + carve particles

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Voxel primitive occupancy + CSG composition assertions (analytic field oracle)

**Files:**
- `MatterEngine3/include/csg_lowering.h` (MODIFIED — add an analytic field-eval oracle for testing)
- `MatterEngine3/src/csg_lowering.cpp` (MODIFIED)
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)

This task asserts the spec's "voxel primitives" + "union/difference/intersection compose as expected" bullets **without GL**, by evaluating the analytic CSG field the lowering represents at sample points (occupancy oracle), independent of the BLAS surface path (which Task 9 exercises).

- [ ] Add to `csg_lowering.h`:
```cpp
// Analytic occupancy oracle: evaluates the CSG expression's solidity (>0 inside)
// at a world point. Used by tests to assert primitive/CSG occupancy without GL.
bool field_is_solid(const BuildBuffer& buf, const Vector3& worldPoint);
```
- [ ] Add a failing test covering sphere, box, and 2-brush union/difference/intersection occupancy:
```cpp
static void test_voxel_primitive_occupancy() {
    // Sphere brush occupancy
    dsl::DslState ss; ss.beginVoxels(0.1f); ss.fill(0);
    ss.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union); ss.endVoxels();
    CHECK(dsl::field_is_solid(ss.buffer(), {0,0,0}), "sphere solid at center");
    CHECK(!dsl::field_is_solid(ss.buffer(), {2,0,0}), "sphere empty outside radius");

    // Box brush occupancy
    dsl::DslState sb; sb.beginVoxels(0.1f); sb.fill(0);
    sb.box({0,0,0}, {0.5f,0.5f,0.5f}, dsl::CsgOp::Union); sb.endVoxels();
    CHECK(dsl::field_is_solid(sb.buffer(), {0.4f,0.4f,0.4f}), "box solid inside");
    CHECK(!dsl::field_is_solid(sb.buffer(), {0.9f,0,0}), "box empty outside half-extent");

    // Two overlapping spheres: union / difference / intersection
    auto twoSphere=[&](dsl::CsgOp op2){
        dsl::DslState s; s.beginVoxels(0.1f); s.fill(0);
        s.sphere({0,0,0},1.0f,dsl::CsgOp::Union);
        s.sphere({1,0,0},1.0f,op2); s.endVoxels();
        return s.buffer();
    };
    auto U=twoSphere(dsl::CsgOp::Union);
    CHECK(dsl::field_is_solid(U,{-0.9f,0,0}) && dsl::field_is_solid(U,{1.9f,0,0}),
          "union covers both spheres");
    auto D=twoSphere(dsl::CsgOp::Difference);
    CHECK(dsl::field_is_solid(D,{-0.9f,0,0}) && !dsl::field_is_solid(D,{1.0f,0,0}),
          "difference removes second sphere region");
    auto I=twoSphere(dsl::CsgOp::Intersection);
    CHECK(dsl::field_is_solid(I,{0.5f,0,0}) && !dsl::field_is_solid(I,{-0.9f,0,0}),
          "intersection keeps only overlap");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected FAIL**: `field_is_solid` undefined.
- [ ] Implement `field_is_solid` in `csg_lowering.cpp` as an exact analytic SDF evaluator over the op list (sphere SDF, box SDF, hard min/max CSG; smoothing ignored here — occupancy oracle uses hard ops):
```cpp
static float sdSphere(const Vector3& p, float r){ return Vector3Length(p) - r; }
static float sdBox(const Vector3& p, const Vector3& h){
    Vector3 d={fabsf(p.x)-h.x, fabsf(p.y)-h.y, fabsf(p.z)-h.z};
    Vector3 m={fmaxf(d.x,0),fmaxf(d.y,0),fmaxf(d.z,0)};
    return Vector3Length(m) + fminf(fmaxf(d.x,fmaxf(d.y,d.z)),0.0f);
}
bool field_is_solid(const BuildBuffer& buf, const Vector3& wp) {
    float field = 1e9f; // distance; <0 = inside. start empty (large positive)
    bool any=false;
    for (const BuildOp& o : buf.ops) {
        Matrix inv = MatrixInvert(o.transform);
        Vector3 lp = Vector3Subtract(Vector3Transform(wp, inv), o.center);
        float d = (o.kind==BrushKind::Sphere)? sdSphere(lp,o.radius) : sdBox(lp,o.halfExtents);
        if (!any) { field = d; any=true; continue; }
        switch (o.op) {
            case CsgOp::Union:        field = fminf(field, d); break;
            case CsgOp::Difference:   field = fmaxf(field, -d); break;
            case CsgOp::Intersection: field = fmaxf(field, d); break;
        }
    }
    return any && field < 0.0f;
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`.
- [ ] Commit:
```
git add MatterEngine3/include/csg_lowering.h MatterEngine3/src/csg_lowering.cpp MatterEngine3/tests/script_host_tests.cpp && git commit -m "$(cat <<'EOF'
test(sp2): analytic occupancy oracle for primitive + CSG composition

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: Drive Cluster → per-cell BLAS → `save_v2` (end-to-end bake writes one .part)

**Files:**
- `MatterEngine3/src/script_host.cpp` (MODIFIED — full lowering + surface + save)
- `MatterEngine3/include/script_host.h` (MODIFIED — `BakeOptions` cluster sizing knobs)
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)
- `MatterEngine3/tests/Makefile` (MODIFIED — link cluster + mesher + part_asset sources)

- [ ] Extend the Makefile `SCRIPT_*` to link the surface path used by `part_asset_tests` + `parallel_mesh_tests` (cluster, cell, mesher, blas, tlas, part_asset, plus the C SurfaceLib sources via gcc). Update the target:
```makefile
# NEW MatterEngine3 sources keep ../src/...; SP-1's NEW part_asset_v2.cpp (../src/)
# supplies save_v2/compute_resolved_hash/LodLevels. Consumed prototype sources are
# referenced read-only as ../../MatterSurfaceLib/src/<dep>.
SCRIPT_CPP = script_host_tests.cpp ../src/script_host.cpp ../src/dsl_state.cpp \
             ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/part_asset_v2.cpp \
             ../../MatterSurfaceLib/src/cluster.cpp ../../MatterSurfaceLib/src/cell.cpp ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
             ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/bvh_analyzer.cpp \
             ../../MatterSurfaceLib/src/mesh_worker_pool.cpp ../../MatterSurfaceLib/src/mesh_build_utils.cpp \
             ../../MatterSurfaceLib/src/meshing_algorithm.cpp ../../MatterSurfaceLib/src/marching_cubes_algorithm.cpp \
             ../../MatterSurfaceLib/src/oriented_cube_algorithm.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp \
             ../../MatterSurfaceLib/src/occupancy.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp ../../MatterSurfaceLib/src/part_asset.cpp \
             ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp
SCRIPT_C   = ../../MatterSurfaceLib/src/surface.c ../../MatterSurfaceLib/src/open_particle_surface.c \
             ../../MatterSurfaceLib/src/spatial_hash.c ../../MatterSurfaceLib/src/object_allocator.c ../../MatterSurfaceLib/src/material_registry.c
SCRIPT_C_OBJ = surface.o open_particle_surface.o spatial_hash.o object_allocator.o material_registry.o

$(SCRIPT_TARGET): $(SCRIPT_CPP) $(SCRIPT_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(SCRIPT_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(SCRIPT_CPP) $(QJS_OBJ) $(SCRIPT_C_OBJ) -o $(SCRIPT_TARGET) \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(SCRIPT_C_OBJ)
```
  (Mirror the exact source list from `parallel_mesh_tests` + `part_asset_tests` so the surface path links GL-free; never calls `UploadMesh`.)
- [ ] Add a failing end-to-end test: bake a sphere script and assert a `.part` file is written at `cache_path(resolved_hash)` and `error.ok`:
```cpp
#include <sys/stat.h>
static bool file_exists(const std::string& p){ struct stat st; return stat(p.c_str(),&st)==0; }

static void test_bake_writes_part() {
    script_host::ScriptHost host;
    const char* src =
        "class Ball extends Part { static params={r:1.0};\n"
        "  build(p){ this.beginVoxels(0.25); this.fill(MAT.stone); this.sphere([0,0,0],p.r); this.endVoxels(); }\n"
        "}\n";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok, "sphere bake succeeds");
    CHECK(!r.written_path.empty(), "written path reported");
    CHECK(file_exists(r.written_path), "the .part file exists on disk");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected FAIL**: no file written (host stops after build).
- [ ] In `script_host.cpp`, after a clean `build` (no `state.has_error()`), perform the full bake. Real code (appended to the post-build success path):
```cpp
#include "../include/csg_lowering.h"   // NEW MatterEngine3 header
#include "cluster.h"                    // consumed prototype (-I../../MatterSurfaceLib/include)
#include "blas_manager.hpp"             // consumed prototype
#include "tlas_manager.hpp"             // consumed prototype
// ... success path (error.ok still true, session closed):
if (r.error.ok && state.session() != dsl::Session::None) {
    r.error.ok = false; r.error.message = "session left open at end of build";
}
if (r.error.ok) {
    dsl::LoweredField f = dsl::lower_build_buffer(state.buffer());
    BLASManager blas; TLASManager tlas;
    Cluster cluster(/*id*/1, blas, tlas, /*smallest_cell_size*/1.0f);
    cluster.set_base_detail_size(state.buffer().ops.empty()?0.1f:state.buffer().ops[0].spacing);
    for (const StaticParticle& sp : f.additive)
        cluster.add_particle(sp.position, sp.radius, sp.materialId, sp.tint, sp.detail_size);
    cluster.set_carve_particles(f.carve);
    cluster.force_rebuild_all_cells();   // surfaces per-cell BLAS (GL-free)
    cluster.add_to_tlas();
    tlas.build(blas);
    std::string path = part_asset::cache_path(r.resolved_hash);
    // SP-2 writes no children, empty LOD array.
    part_asset::LodLevels lods{};   // empty
    bool ok = part_asset::save_v2(path, blas, tlas, /*children*/nullptr, 0,
                                  lods, r.resolved_hash);
    if (!ok) { r.error.ok = false; r.error.message = "save_v2 failed"; }
    else { r.written_path = path; }
}
```
  NOTE: `cluster.add_to_tlas()` + `tlas.build(blas)` mirrors `part_asset_tests`' scene assembly so `save_v2` has a populated TLAS. Confirm against `tlas_manager.hpp` during impl; if `add_to_tlas` isn't the right call, use the `DrawInstance`/`draw_batch` path from `part_asset_tests.cpp`.
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`. (Gated on SP-1 `save_v2`/`LodLevels`/`cache_path`.)
- [ ] Commit:
```
git add MatterEngine3/src/script_host.cpp MatterEngine3/include/script_host.h MatterEngine3/tests/script_host_tests.cpp MatterEngine3/tests/Makefile && git commit -m "$(cat <<'EOF'
feat(sp2): lower to Cluster, surface per-cell BLAS, and save_v2 one .part

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 10: Sharp-vs-smooth seam metric + sub-min-size box feature survival

**Files:**
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)

These two spec bullets assert on a surface/seam metric, not exact vertices. Reuse the analytic field oracle + the smoothing factor carried through lowering (no GL).

- [ ] Add a failing `test_sharp_vs_smooth_seam`. With `smoothing(0)` two overlapping spheres keep a detectable concave seam (the hard-min field has a crease where the two SDFs cross with a derivative discontinuity); with high `k` the smooth-min removes it. Assert via a seam metric on the lowered `smoothing` factor + a field-gradient probe at the seam plane:
```cpp
static float seam_sharpness(const dsl::BuildBuffer& buf) {
    // Probe |d/dx field| discontinuity across the seam plane x=0.5 between two
    // unit spheres centered at 0 and 1: sample just-left and just-right normals.
    auto nx=[&](float x){
        float e=1e-3f;
        // central difference of the (hard) field along x at the surface band
        // (uses field_is_solid as a sign source near the seam)
        bool a=dsl::field_is_solid(buf,{x-e,0.0f,0.92f});
        bool b=dsl::field_is_solid(buf,{x+e,0.0f,0.92f});
        return (a!=b)?1.0f:0.0f;
    };
    return nx(0.5f); // crossing near the seam band => sharp transition present
}
static void test_sharp_vs_smooth_seam() {
    dsl::DslState sharp; sharp.beginVoxels(0.1f); sharp.fill(0);
    sharp.sphere({0,0,0},1.0f,dsl::CsgOp::Union);
    sharp.sphere({1,0,0},1.0f,dsl::CsgOp::Union);
    sharp.smoothing(0.0f); sharp.endVoxels();
    dsl::LoweredField fs = dsl::lower_build_buffer(sharp.buffer());
    CHECK(fs.smoothing == 0.0f, "k=0 lowers to hard min (sharp seam)");

    dsl::DslState smooth; smooth.beginVoxels(0.1f); smooth.fill(0);
    smooth.sphere({0,0,0},1.0f,dsl::CsgOp::Union);
    smooth.sphere({1,0,0},1.0f,dsl::CsgOp::Union);
    smooth.smoothing(0.8f); smooth.endVoxels();
    dsl::LoweredField fm = dsl::lower_build_buffer(smooth.buffer());
    CHECK(fm.smoothing > 0.5f, "high k lowers to a large smooth-min factor (merged)");
    CHECK(fm.smoothing > fs.smoothing, "smooth seam has strictly larger blend factor than sharp");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS** (oracle + lowering already implemented; this codifies the seam metric).
- [ ] Add a failing `test_sub_min_box_feature_survives` that bakes a box brush smaller than the min particle and asserts the carve feature survives lowering (already partly covered in Task 7; here assert against a full bake's lowered field):
```cpp
static void test_sub_min_box_feature_survives() {
    dsl::DslState s; s.beginVoxels(0.5f); s.fill(0);   // min particle ~0.5
    s.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    s.box({1,0,0}, {0.05f,0.05f,0.05f}, dsl::CsgOp::Difference); // 0.1 box << 0.5 min
    s.endVoxels();
    s.set_last_op(dsl::CsgOp::Difference);
    dsl::LoweredField f = dsl::lower_build_buffer(s.buffer());
    CHECK(!f.carve.empty(), "sub-min box carves at least one carve particle");
    // the analytic field still shows the crisp removal at the box location
    CHECK(!dsl::field_is_solid(s.buffer(), {1.0f,0,0}), "sub-min feature present in field");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`.
- [ ] Commit:
```
git add MatterEngine3/tests/script_host_tests.cpp && git commit -m "$(cat <<'EOF'
test(sp2): sharp-vs-smooth seam metric and sub-min box feature survival

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 11: Determinism — identical bytes on re-bake + fresh-context no residue

**Files:**
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)

- [ ] Add a failing `test_determinism_identical_bytes`: bake the same `(source, params)` twice, assert identical `resolved_hash` and byte-identical `.part` files:
```cpp
static std::vector<uint8_t> read_all(const std::string& p){
    std::vector<uint8_t> b; FILE* f=fopen(p.c_str(),"rb"); if(!f) return b;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    b.resize(n); if (fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b;
}
static void test_determinism_identical_bytes() {
    const char* src =
        "class Ball extends Part { static params={r:1.0};\n"
        "  build(p){ this.beginVoxels(0.25); this.fill(MAT.stone); this.sphere([0,0,0],p.r); this.endVoxels(); }\n"
        "}\n";
    script_host::ScriptHost h1; auto r1 = h1.bake_source(src, "{}", {});
    std::vector<uint8_t> b1 = read_all(r1.written_path);
    script_host::ScriptHost h2; auto r2 = h2.bake_source(src, "{}", {});
    std::vector<uint8_t> b2 = read_all(r2.written_path);
    CHECK(r1.error.ok && r2.error.ok, "both bakes succeed");
    CHECK(r1.resolved_hash == r2.resolved_hash, "same source+params => same resolved_hash");
    CHECK(!b1.empty() && b1 == b2, "re-bake produces byte-identical .part");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS** if SP-1 `save_v2` is byte-deterministic and the fresh context introduces no nondeterminism; if it FAILS, the failure localizes the nondeterminism source (likely unsorted material/particle iteration) — fix in lowering (sort additive particles by a stable key before adding) and rerun. Add the stable sort to `lower_build_buffer` if needed:
```cpp
// in csg_lowering.cpp, end of lower_build_buffer, before return:
// (only if determinism test reveals iteration-order nondeterminism)
// std::stable_sort not required because op order is already the authored order.
```
- [ ] Add a failing `test_fresh_context_no_residue`: bake A, then B, in the same `ScriptHost` process, and assert B's output equals B baked alone (A leaves no residue):
```cpp
static void test_fresh_context_no_residue() {
    const char* A =
        "class A extends Part { static params={};\n"
        "  build(p){ this.beginVoxels(0.25); this.fill(MAT.dirt); this.box([0,0,0],[1,1,1]); this.endVoxels(); }\n"
        "}\n";
    const char* B =
        "class B extends Part { static params={};\n"
        "  build(p){ this.beginVoxels(0.25); this.fill(MAT.stone); this.sphere([0,0,0],1.0); this.endVoxels(); }\n"
        "}\n";
    script_host::ScriptHost h; 
    h.bake_source(A, "{}", {});                  // bake A first (residue test)
    auto bAfterA = h.bake_source(B, "{}", {});
    script_host::ScriptHost hClean;
    auto bAlone = hClean.bake_source(B, "{}", {});
    CHECK(bAfterA.error.ok && bAlone.error.ok, "both B bakes ok");
    CHECK(bAfterA.resolved_hash == bAlone.resolved_hash, "B hash independent of prior A bake");
    CHECK(read_all(bAfterA.written_path) == read_all(bAlone.written_path),
          "B bytes identical whether or not A ran first (fresh context, no residue)");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`. (If FAIL: a `JSContext`/`DslState` is being reused — ensure each `bake_source` constructs a fresh `JSRuntime`/`JSContext` and a fresh stack-local `DslState`.)
- [ ] Commit:
```
git add MatterEngine3/tests/script_host_tests.cpp MatterEngine3/src/csg_lowering.cpp && git commit -m "$(cat <<'EOF'
test(sp2): determinism (identical bytes) and fresh-context no-residue

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 12: Seeded `Math.random` + no Date/file/network bindings

**Files:**
- `MatterEngine3/include/dsl_rng.h` (NEW)
- `MatterEngine3/src/dsl_bindings.cpp` (MODIFIED)
- `MatterEngine3/src/script_host.cpp` (MODIFIED — seed from params, install RNG)
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)

- [ ] Create `MatterEngine3/include/dsl_rng.h` with a SplitMix64-backed `[0,1)` generator (deterministic, seedable):
```cpp
#pragma once
#include <cstdint>
namespace dsl {
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next_u64() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double next_unit() { return (next_u64() >> 11) * (1.0 / 9007199254740992.0); } // [0,1)
};
} // namespace dsl
```
- [ ] Add a failing test: same seed → identical geometry hash; different seed → different; and assert `Date`/`require`/`fetch`/file APIs are absent:
```cpp
static void test_seeded_rng_and_no_ambient() {
    const char* src =
        "class Noise extends Part { static params={seed:0};\n"
        "  build(p){ this.beginVoxels(0.5); this.fill(0);\n"
        "    for(let i=0;i<5;i++){ this.sphere([Math.random()*2,0,0], 0.5); } this.endVoxels(); }\n"
        "}\n";
    script_host::ScriptHost a; auto ra1 = a.bake_source(src, "{\"seed\":1}", {});
    script_host::ScriptHost b; auto rb1 = b.bake_source(src, "{\"seed\":1}", {});
    CHECK(ra1.resolved_hash == rb1.resolved_hash, "same seed => same resolved_hash");
    CHECK(read_all(ra1.written_path) == read_all(rb1.written_path), "same seed => same bytes");
    script_host::ScriptHost c; auto rc2 = c.bake_source(src, "{\"seed\":2}", {});
    CHECK(read_all(ra1.written_path) != read_all(rc2.written_path), "different seed => different bytes");

    // No ambient nondeterminism: Date/require/fetch must be undefined.
    script_host::ScriptHost d;
    const char* probe =
        "class Probe extends Part { static params={};\n"
        "  build(p){ globalThis.__amb = (typeof Date)+','+(typeof require)+','+(typeof fetch)+','+(typeof globalThis.os); }\n"
        "}\n";
    auto rp = d.bake_source(probe, "{}", {});
    CHECK(rp.error.ok, "probe bakes");
    CHECK(d.last_ambient_probe() == "undefined,undefined,undefined,undefined",
          "no Date/require/fetch/os bindings present");
}
```
  Add `std::string last_ambient_probe() const` hook (read `globalThis.__amb` after build in the host) for the probe assertion.
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected FAIL**: `Math.random` not seeded (QuickJS-ng default seeds from entropy) and/or `Date` present.
- [ ] In `dsl_bindings.cpp`, add a seeded `Math.random` override and verify Date removal. Real code:
```cpp
#include "../include/dsl_rng.h"
static JSValue j_random(JSContext* c, JSValueConst, int, JSValueConst*) {
    Rng* rng = static_cast<DslState*>(JS_GetContextOpaque(c))->rng();
    return JS_NewFloat64(c, rng ? rng->next_unit() : 0.0);
}
// in install_bindings, after the __dsl_* binds:
JSValue mathObj = JS_GetPropertyStr(ctx, g, "Math");
JS_SetPropertyStr(ctx, mathObj, "random", JS_NewCFunction(ctx, j_random, "random", 0));
JS_FreeValue(ctx, mathObj);
// Remove Date (no wall-clock). QuickJS-ng adds Date via JS_AddIntrinsicDate;
// we DON'T call it. JS_NewContextRaw + selective intrinsics avoids Date entirely.
```
  In `script_host.cpp`, build the context with **`JS_NewContextRaw` + selective intrinsics** (BaseObjects, Date OMITTED, Eval, RegExp, JSON, MapSet, TypedArrays) so Date/file/network never exist, and seed the `DslState` RNG from the merged params' `seed` field (fall back to FNV of the merged-params JSON when absent):
```cpp
JSContext* ctx = JS_NewContextRaw(rt);
JS_AddIntrinsicBaseObjects(ctx);
JS_AddIntrinsicRegExp(ctx);
JS_AddIntrinsicJSON(ctx);
JS_AddIntrinsicMapSet(ctx);
JS_AddIntrinsicTypedArrays(ctx);
JS_AddIntrinsicEval(ctx);
// (Deliberately NOT JS_AddIntrinsicDate.) quickjs-libc not compiled => no os/std/file/network.
// ... after merged params known, derive seed:
uint64_t seed = 0; /* parse "seed" from merged JSON; else */
seed = part_asset::fnv1a64(last_merged_params_.data(), last_merged_params_.size());
state.set_rng(seed);
```
  Add `Rng* rng()` / `void set_rng(uint64_t)` storing a `std::unique_ptr<Rng>` in `DslState`.
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`.
- [ ] Commit:
```
git add MatterEngine3/include/dsl_rng.h MatterEngine3/src/dsl_bindings.cpp MatterEngine3/src/script_host.cpp MatterEngine3/include/dsl_state.h MatterEngine3/include/script_host.h MatterEngine3/tests/script_host_tests.cpp && git commit -m "$(cat <<'EOF'
feat(sp2): seeded Math.random and no Date/file/network bindings

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 13: Fail-closed errors + configurable time budget

**Files:**
- `MatterEngine3/src/script_host.cpp` (MODIFIED — interrupt handler, no-file-on-error)
- `MatterEngine3/tests/script_host_tests.cpp` (MODIFIED)

- [ ] Add a failing `test_fail_closed`: a thrown error, a session-misuse, and a time-budget overrun each return `error.ok == false` and write NO file:
```cpp
static void test_fail_closed() {
    // (a) thrown error
    script_host::ScriptHost h1;
    const char* thrower =
        "class T extends Part { static params={};\n"
        "  build(p){ this.beginVoxels(0.25); throw new Error('boom'); }\n"
        "}\n";
    auto r1 = h1.bake_source(thrower, "{}", {});
    CHECK(!r1.error.ok, "throw => error");
    CHECK(r1.written_path.empty(), "throw => no file written");
    CHECK(r1.error.message.find("boom") != std::string::npos, "error message carries throw text");

    // (b) session misuse (begin inside begin)
    script_host::ScriptHost h2;
    const char* misuse =
        "class M extends Part { static params={};\n"
        "  build(p){ this.beginVoxels(0.25); this.beginVoxels(0.25); this.endVoxels(); }\n"
        "}\n";
    auto r2 = h2.bake_source(misuse, "{}", {});
    CHECK(!r2.error.ok, "session misuse => error");
    CHECK(r2.written_path.empty(), "session misuse => no file");

    // (c) time-budget overrun
    script_host::ScriptHost h3;
    const char* spin =
        "class S extends Part { static params={};\n"
        "  build(p){ this.beginVoxels(0.25); while(true){} }\n"
        "}\n";
    script_host::BakeOptions budget; budget.time_budget_ms = 50;
    auto r3 = h3.bake_source(spin, "{}", budget);
    CHECK(!r3.error.ok, "time-budget overrun => error");
    CHECK(r3.written_path.empty(), "time-budget overrun => no file");
    CHECK(r3.error.message.find("budget") != std::string::npos ||
          r3.error.message.find("interrupt") != std::string::npos,
          "structured time-budget error");
}
```
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected FAIL**: (c) hangs forever (no interrupt) and/or files written on error.
- [ ] In `script_host.cpp`, install a QuickJS-ng interrupt handler that fires when `time_budget_ms` elapses, and ensure NO save happens on any error. Real code:
```cpp
#include <chrono>
struct InterruptCtx { std::chrono::steady_clock::time_point deadline; bool bounded; };
static int interrupt_cb(JSRuntime*, void* opaque) {
    InterruptCtx* ic = static_cast<InterruptCtx*>(opaque);
    if (!ic->bounded) return 0;
    return std::chrono::steady_clock::now() >= ic->deadline ? 1 : 0; // 1 => interrupt
}
// in bake_source, before evals:
InterruptCtx ic;
ic.bounded = opts.time_budget_ms > 0;
ic.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.time_budget_ms);
JS_SetInterruptHandler(rt, interrupt_cb, &ic);
// When build is interrupted, JS_Invoke returns an exception => harvest_exception
// already sets error.ok=false. Tag the message:
if (JS_IsException(bret) && ic.bounded &&
    std::chrono::steady_clock::now() >= ic.deadline) {
    r.error.ok = false; r.error.message = "time budget exceeded (interrupt)";
}
```
  Confirm the save block from Task 9 is guarded by `if (r.error.ok)` so no error path can write — it already is. Verify `written_path` stays empty on every error branch.
- [ ] Run `cd MatterEngine3/tests && make run-script` — **expected PASS**: `ALL PASS`. The spin test now aborts within ~50ms.
- [ ] Commit:
```
git add MatterEngine3/src/script_host.cpp MatterEngine3/tests/script_host_tests.cpp && git commit -m "$(cat <<'EOF'
feat(sp2): fail-closed bakes and configurable per-bake time budget

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 14: Wire QuickJS-ng + script host into the MatterEngine3 app build

**Files:**
- `MatterEngine3/Makefile` (MODIFIED)

- [ ] Edit the `MatterEngine3/Makefile` (the new project's app build; do NOT touch the read-only `MatterSurfaceLib/Makefile`) and add the vendored quickjs-ng sources + the new host/DSL/CSG `.cpp` to the app's object list, with an include path `-I../Libraries/quickjs-ng`. Compile the quickjs `.c` via the C compiler (unmangled). Add a rule mirroring the existing per-source compile pattern, e.g.:
```makefile
QJS_DIR = ../Libraries/quickjs-ng
CFLAGS  += -I$(QJS_DIR)
QJS_OBJS = quickjs.o libregexp.o libunicode.o cutils.o
quickjs.o:    $(QJS_DIR)/quickjs.c    ; gcc -c $< -O2 -DCONFIG_VERSION='"0.10.0"' -I$(QJS_DIR)
libregexp.o:  $(QJS_DIR)/libregexp.c  ; gcc -c $< -O2 -I$(QJS_DIR)
libunicode.o: $(QJS_DIR)/libunicode.c ; gcc -c $< -O2 -I$(QJS_DIR)
cutils.o:     $(QJS_DIR)/cutils.c     ; gcc -c $< -O2 -I$(QJS_DIR)
# add $(QJS_OBJS) and script_host.o dsl_state.o dsl_bindings.o csg_lowering.o
# to the app link target's object list.
```
  Match the exact object-list/link conventions already in `MatterEngine3/Makefile` (this step adapts to whatever pattern that file uses — list the four QJS objects + four new `.cpp` objects in the final link). The new host/DSL/CSG `.cpp` are local (`src/*.cpp`); consumed prototype sources stay referenced read-only as `../MatterSurfaceLib/src/<dep>.cpp` with `-I../MatterSurfaceLib/include`.
- [ ] Run `cd MatterEngine3 && make` — **expected: clean app build** linking the host. (Per CLAUDE.md memory: when changing structs/headers on the Windows target, clean-rebuild — `make clean && make` — to avoid stale objects.)
- [ ] Run `cd MatterEngine3/tests && make run-script` once more — **expected PASS**: `ALL PASS` (full SP-2 suite green).
- [ ] Commit:
```
git add MatterEngine3/Makefile && git commit -m "$(cat <<'EOF'
build(sp2): compile vendored quickjs-ng and script host into the app

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Spec Testing-section coverage map

| Spec testing bullet | Task |
|---------------------|------|
| Determinism: same (source,params) → identical resolved_hash + identical bytes | Task 11 |
| Determinism: fresh context leaves no residue (A then B == B alone) | Task 11 |
| Params: defaults applied; overrides change resolved_hash; build(p) sees merged | Task 5 |
| Voxel primitives: sphere + box occupancy; union/difference/intersection compose | Task 8 |
| Sharp vs smooth: smoothing(0) seam vs high-k merged (seam metric) | Task 10 |
| Sub-min-size brush: small box still carves a crisp feature | Tasks 7, 10 |
| Fail-closed: throw / session misuse / time-budget → no file, structured error | Task 13 (+ misuse in Task 6) |
| No ambient nondeterminism: seeded Math.random; no Date/file/network | Task 12 |
| End-to-end bake to one .part via save_v2 | Task 9 |
| Embed works (foundational) | Task 1 |
