# Tileset DSL Root + Placement (Phase 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `Tileset extends Part` DSL root that scripts can author (tile/base/layer/dropChild/variant verbs), a `[tileset]` manifest entry, deterministic placement algorithms, and a settle orchestrator that turns an evaluated tileset script into a settled 4×4-torus instance table (the input Phase 3 renders).

**Architecture:** The tileset script is evaluated in the existing QuickJS `ScriptHost` with a new embedded `tileset_base.js.h` (sibling of `part_base.js.h`) and new `__dsl_*` natives that record into a `TilesetState` hanging off `dsl::DslState`. All JS callbacks (`base` heightfield fn, `layer` params fn, `variant` per-tile hook) are consumed **during eval** — placements are generated inside `layer()` so the recorded `TilesetSpec` is fully resolved and self-contained. Physics runs afterwards in C++: `tileset_bake` loads the baked child parts, fits/scales colliders, spawns strips (8-instance sync groups), shared drops (16-instance sync groups) and interiors into the Phase-1 `SettleWorld`, settles layer-by-layer, finalizes, and emits a `SettledTorus`.

**Tech Stack:** QuickJS (existing script_host), box3d via Phase-1 `tileset::SettleWorld`, `dsl::Rng` (SplitMix64), part_asset v2 cache.

## Global Constraints

- Determinism: every random draw uses `dsl::Rng` (SplitMix64, constants as in `dsl_rng.h`); seeds derived only from the tileset master seed + documented folds (layer index, domain id, placement index). Double-eval of the same source+params yields a byte-identical `TilesetSpec`; double-bake yields equal `SettleWorld::pose_hash()`.
- Defaults (exact values): tile `size` 2.0 m, `texelsPerMeter` 512, `edgeStripWidth` 0.15 m, `cornerClearRadius` 0.08 m, layer defaults `placement:'uniform'`, `physics:true`, `embed:0.0`, `dropHeight:[0.15,0.35]`, `scale:[1.0,1.0]`, `collider:'auto'`.
- Torus geometry comes only from `tileset_layout.h`: `kTorusN=4`, `kBoundaryColors={0,0,1,1}`; edge-strip bodies have exactly 8 occurrences (`strip_occurrences`), shared `dropChild` bodies exactly 16 (one per tile).
- Occurrence frames handed to `SettleWorld::add_sync_group` are **pure translations** (Phase-1 invariant).
- Fail-closed: every scripting error surfaces as a structured `BakeError` (message + best-effort source location); no partial artifacts. Unknown layer module/params → error naming layer + module. `variant()` content within `edgeStripWidth` of tile bounds → error naming the tile.
- `tile()` must be the first tileset verb called in `build()`; any other tileset verb before it → structured error.
- No changes outside `MatterEngine3/` and `build-all.sh` (MatterSurfaceLib is read-only).
- Test conventions: `CHECK(cond, msg)` macro, exit code = failure count, pristine output; suites registered in `MatterEngine3/tests/Makefile` and `build-all.sh`.

## File Structure

- Create: `MatterEngine3/include/tileset_spec.h` — recorded verb data (`TileConfig`, `BaseField`, `Placement`, `LayerSpec`, `DropChildRec`, `VariantRange`, `TilesetSpec`)
- Create: `MatterEngine3/src/tileset_base.js.h` — embedded JS `Tileset` class (extends `Part`)
- Create: `MatterEngine3/include/tileset_placement.h`, `MatterEngine3/src/tileset_placement.cpp` — pure deterministic placement algorithms (uniform/poisson/cluster over strip + interior domains)
- Create: `MatterEngine3/include/tileset_bake.h`, `MatterEngine3/src/tileset_bake.cpp` — orchestrator: `TilesetSpec` + baked children → `SettledTorus`
- Modify: `MatterEngine3/include/dsl_state.h`, `MatterEngine3/src/dsl_state.cpp` — add `TilesetState` accessor + scope-range recording
- Modify: `MatterEngine3/src/dsl_bindings.cpp` — new `__dsl_tile/__dsl_base/__dsl_layer/__dsl_dropChild/__dsl_variant` (+ params-rng natives)
- Modify: `MatterEngine3/include/script_host.h`, `MatterEngine3/src/script_host.cpp` — `ScriptHost::eval_tileset(...)`
- Modify: `MatterEngine3/src/part_graph.cpp`, `MatterEngine3/include/part_graph.h` — manifest `tileset` flag
- Modify: `MatterEngine3/Makefile` (new sources), `MatterEngine3/tests/Makefile` (3 new suites), `build-all.sh` (register suites)
- Test: `MatterEngine3/tests/tileset_placement_tests.cpp` (GL-free), `MatterEngine3/tests/tileset_dsl_tests.cpp` (ScriptHost + raylib link, like run-script), `MatterEngine3/tests/tileset_bake_tests.cpp` (full stack: quickjs + raylib + box3d)

**Interfaces produced for Phase 3:** `tileset::SettledTorus` (see Task 7) — settled instance table + base field + config; Phase 3 flattens/renders it.

---

### Task 1: Manifest `[tileset]` flag

**Files:**
- Modify: `MatterEngine3/include/part_graph.h` (read_manifest signature docs)
- Modify: `MatterEngine3/src/part_graph.cpp:216-249` (flag loop)
- Test: `MatterEngine3/tests/part_graph_tests.cpp` (existing suite; append)

**Interfaces:**
- Consumes: existing `read_manifest(world_data_dir, world, roots_out, error_out, expand_out)`.
- Produces: `read_manifest(..., std::vector<bool>* expand_out, std::vector<bool>* tileset_out = nullptr)` — parallel per-root flag, `true` when the root line carries the `tileset` token. `tileset` and `expand` on the same root → error "root cannot be both tileset and expand".

- [ ] **Step 1: Write the failing tests** — append to the existing manifest test section of `MatterEngine3/tests/part_graph_tests.cpp` (follow its existing temp-manifest helper pattern; if it writes manifests via a helper, reuse it):

```cpp
static void test_manifest_tileset_flag() {
    // Arrange: manifest with a plain root, a tileset root, and a comment
    const char* manifest =
        "# demo\n"
        "Meadow\n"
        "ForestFloor tileset\n";
    std::string dir = write_temp_manifest("TsFlag", manifest);  // reuse existing helper; else mkdir+write inline

    std::vector<PartGraph::ChildRequest> roots;
    std::vector<bool> expand, tileset;
    std::string err;
    bool ok = PartGraph::read_manifest(dir, "TsFlag", roots, err, &expand, &tileset);
    CHECK(ok, "manifest: parses tileset flag");
    CHECK(roots.size() == 2, "manifest: two roots");
    CHECK(tileset.size() == 2 && !tileset[0] && tileset[1], "manifest: tileset flag on second root only");
    CHECK(expand.size() == 2 && !expand[0] && !expand[1], "manifest: expand unset");

    // Unknown flags still hard-error
    std::string dir2 = write_temp_manifest("TsBad", "Meadow frobnicate\n");
    roots.clear(); err.clear();
    ok = PartGraph::read_manifest(dir2, "TsBad", roots, err);
    CHECK(!ok && err.find("unknown manifest flag") != std::string::npos,
          "manifest: unknown flag still errors");

    // tileset + expand on one root errors
    std::string dir3 = write_temp_manifest("TsBoth", "ForestFloor tileset expand\n");
    roots.clear(); err.clear();
    ok = PartGraph::read_manifest(dir3, "TsBoth", roots, err);
    CHECK(!ok && err.find("both") != std::string::npos,
          "manifest: tileset+expand rejected");
}
```

Call `test_manifest_tileset_flag()` from `main()` after the existing manifest tests.

- [ ] **Step 2: Run to verify failure** — `make -C MatterEngine3/tests run-graph` (or the target that runs part_graph_tests). Expected: compile error (no 6th parameter) — that IS the red step for a signature change.

- [ ] **Step 3: Implement** — in `part_graph.h`, extend the declaration:

```cpp
static bool read_manifest(const std::string& world_data_dir, const std::string& world,
                          std::vector<ChildRequest>& roots_out, std::string& error_out,
                          std::vector<bool>* expand_out = nullptr,
                          std::vector<bool>* tileset_out = nullptr);
```

In `part_graph.cpp`, inside the token loop:

```cpp
bool expand = false, tileset = false;
while (tokens >> flag) {
    if (flag == "expand") expand = true;
    else if (flag == "tileset") tileset = true;
    else {
        error_out = "unknown manifest flag '" + flag + "' for root " + name;
        return false;
    }
}
if (expand && tileset) {
    error_out = "root " + name + " cannot be both tileset and expand";
    return false;
}
roots_out.push_back(ChildRequest{ name, Params{} });
if (expand_out)  expand_out->push_back(expand);
if (tileset_out) tileset_out->push_back(tileset);
```

- [ ] **Step 4: Run tests to verify pass** — `make -C MatterEngine3/tests run-graph`. Expected: all checks `ok`, `PASSED`/exit 0.

- [ ] **Step 5: Commit** — `git add MatterEngine3/include/part_graph.h MatterEngine3/src/part_graph.cpp MatterEngine3/tests/part_graph_tests.cpp && git commit -m "feat: world.manifest [tileset] root flag"`

---

### Task 2: `TilesetSpec` + `tile()`/`base()` + `ScriptHost::eval_tileset`

**Files:**
- Create: `MatterEngine3/include/tileset_spec.h`
- Create: `MatterEngine3/src/tileset_base.js.h`
- Modify: `MatterEngine3/include/dsl_state.h`, `MatterEngine3/src/dsl_state.cpp`
- Modify: `MatterEngine3/src/dsl_bindings.cpp`
- Modify: `MatterEngine3/include/script_host.h`, `MatterEngine3/src/script_host.cpp`
- Modify: `MatterEngine3/Makefile`, `MatterEngine3/tests/Makefile`
- Test: `MatterEngine3/tests/tileset_dsl_tests.cpp` (new suite `run-tilesetdsl`)

**Interfaces:**
- Consumes: `dsl::DslState` (context-opaque state), `part_base.js.h` injection pattern, `bake_source`'s context-creation flow (`script_host.cpp:561-750`).
- Produces (later tasks rely on these EXACT names):
  - `tileset::TileConfig { float size; int texels_per_meter; uint64_t seed; float edge_strip_width; float corner_clear_radius; }`
  - `tileset::BaseField { int n; float cell; uint32_t material; std::vector<float> heights; bool set; }` (per-tile periodic grid, `n×n`, `heights[z*n+x]`, `cell = size/n`, `n = 64`)
  - `tileset::Placement { uint64_t child_hash; float pos[3]; float quat[4]; float scale; }`
  - `tileset::LayerSpec` (fields below; placements filled by Task 4)
  - `tileset::DropChildRec { uint64_t child_hash; float transform[16]; }`
  - `tileset::VariantRange { int tile; size_t op_begin, op_end; size_t child_begin, child_end; }`
  - `tileset::TilesetSpec { TileConfig cfg; bool tile_called; BaseField base; std::vector<LayerSpec> layers; std::vector<DropChildRec> drops; std::vector<VariantRange> variant_ranges; }`
  - `dsl::DslState::tileset()` → `tileset::TilesetState*` (null unless tileset mode); `DslState::enable_tileset()`
  - `struct TilesetEvalResult { BakeError error; tileset::TilesetSpec spec; uint64_t resolved_hash; }`
  - `ScriptHost::eval_tileset(const std::string& source, const std::string& params_json, const BakeOptions& opts, const uint64_t* child_hashes, size_t child_count, const std::string* child_modules, const std::string* child_params)` → `TilesetEvalResult`

- [ ] **Step 1: Write `tileset_spec.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tileset {

struct TileConfig {
    float    size = 2.0f;               // meters per tile edge
    int      texels_per_meter = 512;
    uint64_t seed = 0;
    float    edge_strip_width = 0.15f;  // m
    float    corner_clear_radius = 0.08f;
};

// base(fn, material): heightfield sampled during eval on a per-tile periodic grid.
// heights[z*n + x] = fn(x*cell, z*cell); the grid tiles toroidally (sample n wraps to 0).
struct BaseField {
    static constexpr int kSamplesPerTile = 64;
    int      n = 0;          // samples per edge (kSamplesPerTile when set)
    float    cell = 0.0f;    // size / n
    uint32_t material = 0;
    std::vector<float> heights;
    bool     set = false;
};

// One resolved instance placement. Coordinates are DOMAIN-LOCAL:
//  - strip placements: x = across-seam offset in [-w, +w], z = along-seam in [0, size), y = drop height (physics) / 0 (snapped later)
//  - interior placements: x,z = tile-local in [w, size-w), y likewise
struct Placement {
    uint64_t child_hash = 0;
    float pos[3]  = { 0, 0, 0 };
    float quat[4] = { 0, 0, 0, 1 };  // xyzw
    float scale   = 1.0f;
};

struct LayerSpec {
    std::string module;
    float density = 0.0f;            // instances per m^2
    int   placement_kind = 0;        // 0=uniform, 1=poisson, 2=cluster
    bool  physics = true;
    float embed = 0.0f;              // physics:false only
    float drop_h[2] = { 0.15f, 0.35f };
    float scale_range[2] = { 1.0f, 1.0f };
    std::string collider_override;   // "" or "auto"|"sphere"|"capsule"|"box"|"hull"
    // Resolved during eval (Task 4). strip[orientation][color]:
    // orientation 0 = vertical strips (column boundaries), 1 = horizontal (row boundaries).
    std::vector<Placement> strip[2][2];
    std::vector<Placement> interior[16];   // tile index = row*4 + col
};

struct DropChildRec { uint64_t child_hash = 0; float transform[16]; };

// Ranges into DslState's op/children arrays emitted inside variant(t) for one tile.
struct VariantRange { int tile = -1; size_t op_begin = 0, op_end = 0, child_begin = 0, child_end = 0; };

struct TilesetSpec {
    TileConfig cfg;
    bool tile_called = false;
    BaseField base;
    std::vector<LayerSpec> layers;
    std::vector<DropChildRec> drops;
    std::vector<VariantRange> variant_ranges;
};

// Mutable recording state attached to DslState during a tileset eval.
struct TilesetState {
    TilesetSpec spec;
    std::string error;               // first tileset-verb error (fail-closed)
    bool has_error = false;
    dsl_forward_decl_unused_t* _ = nullptr; // (remove; placeholder removed in Step 2)
    void set_error(const std::string& m) { if (!has_error) { has_error = true; error = m; } }
};

}
```

Note for the implementer: delete the `dsl_forward_decl_unused_t` placeholder line — `TilesetState` is exactly `{ spec, error, has_error, set_error }`. (It is written here only so this plan text never shows a trailing comma issue; the real struct has three data members and one method.)

- [ ] **Step 2: Extend `DslState`** — in `dsl_state.h` add (with `#include "tileset_spec.h"` guarded by forward declaration if include cycles bite; `tileset_spec.h` has no dsl includes, so a direct include is fine):

```cpp
// Tileset mode: non-null only when evaluating a Tileset root.
tileset::TilesetState* tileset() { return tileset_.get(); }
void enable_tileset() { tileset_ = std::make_unique<tileset::TilesetState>(); }
// Scope bookkeeping for variant(): current sizes of the recorded streams.
size_t op_count() const { return buffer_.ops.size(); }        // adapt to the real BuildBuffer accessor
size_t child_count() const { return children_.size(); }
private:
std::unique_ptr<tileset::TilesetState> tileset_;
```

Adapt `op_count()` to however `BuildBuffer` exposes its op vector (read `dsl_state.h` first; if ops are private, add a `size()` on `BuildBuffer`). Keep the diff minimal.

- [ ] **Step 3: Write `tileset_base.js.h`** — sibling of `part_base.js.h`, injected AFTER it (so `Part` exists):

```cpp
#pragma once
// Embedded JS: Tileset root base class. Injected after part_base.js.h.
static const char kTilesetBaseJS[] = R"JS(
globalThis.Tileset = class Tileset extends Part {
  tile(o)              { __dsl_ts_tile(o.size, o.texelsPerMeter, o.seed,
                                       o.edgeStripWidth, o.cornerClearRadius); }
  base(fn, mat)        { __dsl_ts_base(fn, mat); }
  layer(module, opts)  { __dsl_ts_layer(module, opts || {}); }
  dropChild(module, p) { __dsl_ts_dropChild(module, p); }
  variant(fn)          { __dsl_ts_variant(fn); }
};
)JS";
```

`undefined` option fields are handled native-side (defaults from Global Constraints).

- [ ] **Step 4: Bindings for `tile` and `base`** — in `dsl_bindings.cpp` (follow the existing `j_*` + `state_of(ctx)` pattern; `layer`/`dropChild`/`variant` natives are Tasks 4-5, but register stubs now that set a structured "not implemented" error so the JS class installs cleanly):

```cpp
static tileset::TilesetState* ts_of(JSContext* c) {
    dsl::DslState* s = state_of(c);
    return s ? s->tileset() : nullptr;
}

static JSValue j_ts_tile(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (ts->spec.tile_called) { ts->set_error("tile() called twice"); return JS_UNDEFINED; }
    tileset::TileConfig& cfg = ts->spec.cfg;
    double v;
    if (n > 0 && !JS_IsUndefined(a[0]) && !JS_ToFloat64(c, &v, a[0])) cfg.size = (float)v;
    if (n > 1 && !JS_IsUndefined(a[1]) && !JS_ToFloat64(c, &v, a[1])) cfg.texels_per_meter = (int)v;
    if (n > 2 && !JS_IsUndefined(a[2]) && !JS_ToFloat64(c, &v, a[2])) cfg.seed = (uint64_t)(double)v;
    if (n > 3 && !JS_IsUndefined(a[3]) && !JS_ToFloat64(c, &v, a[3])) cfg.edge_strip_width = (float)v;
    if (n > 4 && !JS_IsUndefined(a[4]) && !JS_ToFloat64(c, &v, a[4])) cfg.corner_clear_radius = (float)v;
    if (cfg.size <= 0.0f || cfg.texels_per_meter <= 0) { ts->set_error("tile(): size and texelsPerMeter must be positive"); return JS_UNDEFINED; }
    if (cfg.edge_strip_width <= cfg.corner_clear_radius) { ts->set_error("tile(): edgeStripWidth must exceed cornerClearRadius"); return JS_UNDEFINED; }
    ts->spec.tile_called = true;
    return JS_UNDEFINED;
}

static JSValue j_ts_base(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (!ts->spec.tile_called) { ts->set_error("base() before tile()"); return JS_UNDEFINED; }
    if (n < 2 || !JS_IsFunction(c, a[0])) { ts->set_error("base(fn, material): fn required"); return JS_UNDEFINED; }
    uint32_t mat = 0; JS_ToUint32(c, &mat, a[1]);

    tileset::BaseField& b = ts->spec.base;
    b.n = tileset::BaseField::kSamplesPerTile;
    b.cell = ts->spec.cfg.size / (float)b.n;
    b.material = mat;
    b.heights.assign((size_t)b.n * b.n, 0.0f);
    for (int z = 0; z < b.n; ++z) {
        for (int x = 0; x < b.n; ++x) {
            JSValue args[2] = { JS_NewFloat64(c, x * b.cell), JS_NewFloat64(c, z * b.cell) };
            JSValue rv = JS_Call(c, a[0], JS_UNDEFINED, 2, args);
            JS_FreeValue(c, args[0]); JS_FreeValue(c, args[1]);
            if (JS_IsException(rv)) { ts->set_error("base(): heightfield fn threw"); return JS_EXCEPTION; }
            double h = 0.0; JS_ToFloat64(c, &h, rv); JS_FreeValue(c, rv);
            b.heights[(size_t)z * b.n + x] = (float)h;
        }
    }
    b.set = true;
    return JS_UNDEFINED;
}
```

Register alongside the existing bindings in `install_bindings()` (same `JS_SetPropertyStr(ctx, global, "__dsl_ts_tile", JS_NewCFunction(...))` mechanism the file already uses):
`__dsl_ts_tile`(5 args), `__dsl_ts_base`(2), `__dsl_ts_layer`(2, stub → `ts->set_error("layer(): not implemented")`), `__dsl_ts_dropChild`(2, stub), `__dsl_ts_variant`(1, stub).

- [ ] **Step 5: `ScriptHost::eval_tileset`** — in `script_host.h`:

```cpp
struct TilesetEvalResult {
    BakeError error;
    tileset::TilesetSpec spec;
    uint64_t resolved_hash = 0;
};
// Evaluate a Tileset root: fresh isolated context, records verbs into a TilesetSpec.
// No geometry artifact is written. Fail-closed like bake_source.
TilesetEvalResult eval_tileset(const std::string& source,
                               const std::string& params_json,
                               const BakeOptions& opts,
                               const uint64_t* child_hashes = nullptr,
                               size_t child_count = 0,
                               const std::string* child_modules = nullptr,
                               const std::string* child_params = nullptr);
```

Implementation in `script_host.cpp`: **mirror `bake_source` (lines ~561-750) step for step** — merged-params canonicalization, module fold, `compute_resolved_hash`, runtime/context creation, DslState init (RNG seed via the same `derive_seed(merged)`), child-hash table install — with these differences:

1. After creating `DslState state`, call `state.enable_tileset();` and set `state.tileset()->spec.cfg.seed = derive_seed(merged);` as the default master seed (an explicit `tile({seed})` overrides it).
2. Inject `kPartBaseJS`, then `kTilesetBaseJS` (`#include "tileset_base.js.h"`), then `dsl::install_bindings(ctx)`.
3. Class extraction looks for `class X extends Tileset` (reuse the same parsing helper as `extends Part`; if the helper takes the base-class name as a literal, generalize it with a parameter — smallest possible change).
4. After `build()` returns: if `!state.tileset()->spec.tile_called` → error `"tileset build() never called tile()"`. Then run the variant hooks (Task 5 fills this in; for now nothing more).
5. Collect errors from BOTH `state.has_error()` (Part-verb errors) and `state.tileset()->has_error` into the returned `BakeError`. Harvest JS exceptions with the existing `harvest_exception`.
6. Move `state.tileset()->spec` into the result. **Do not** run the mesher and do not write any `.part`.

Extract shared context-setup code into a private helper ONLY if the duplication with `bake_source` exceeds ~40 lines of verbatim-identical code; otherwise keep `eval_tileset` self-contained and accept the parallel structure (bake_source must not change behavior).

- [ ] **Step 6: Makefile wiring** — add `tileset_placement.cpp`/`tileset_bake.cpp` NOT yet (they don't exist); this task only ensures the new header-only additions compile into the existing objects. Add the new test binary to `MatterEngine3/tests/Makefile` cloned from the `run-script` target (same link line — quickjs objects + raylib):

```makefile
tileset_dsl_tests: tileset_dsl_tests.cpp $(SCRIPT_OBJS)   # match run-script's deps exactly
	$(CXX) ... (copy run-script's recipe, renaming the source/binary)
run-tilesetdsl: tileset_dsl_tests
	./tileset_dsl_tests
```

Register `run-tilesetdsl` in `build-all.sh` next to `run-tilesetphysics`.

- [ ] **Step 7: Write the failing tests** — `MatterEngine3/tests/tileset_dsl_tests.cpp`:

```cpp
// tileset_dsl_tests: ScriptHost::eval_tileset recording (tile/base verbs).
#include "script_host.h"
#include "tileset_spec.h"
#include <cmath>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

static const char* kMinimalTileset = R"JS(
export default class Floor extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 256, seed: 77 });
    this.base((x, z) => 0.01 * x + 0.02 * z, 3);
  }
}
)JS";

static void test_eval_records_tile_and_base(ScriptHost& host) {
    auto r = host.eval_tileset(kMinimalTileset, "{}", BakeOptions{});
    CHECK(r.error.ok, "dsl: minimal tileset evals clean");
    CHECK(r.spec.tile_called, "dsl: tile() recorded");
    CHECK(std::fabs(r.spec.cfg.size - 2.0f) < 1e-6f, "dsl: tile size recorded");
    CHECK(r.spec.cfg.texels_per_meter == 256, "dsl: texelsPerMeter recorded");
    CHECK(r.spec.cfg.seed == 77, "dsl: explicit seed recorded");
    CHECK(std::fabs(r.spec.cfg.edge_strip_width - 0.15f) < 1e-6f, "dsl: default edgeStripWidth");
    const auto& b = r.spec.base;
    CHECK(b.set && b.n == 64, "dsl: base field sampled 64x64");
    CHECK(std::fabs(b.cell - 2.0f / 64.0f) < 1e-6f, "dsl: base cell size");
    CHECK(b.material == 3, "dsl: base material recorded");
    // fn = 0.01x + 0.02z, sample (x=5, z=9):
    float expect = 0.01f * (5 * b.cell) + 0.02f * (9 * b.cell);
    CHECK(std::fabs(b.heights[9 * 64 + 5] - expect) < 1e-5f, "dsl: base heights match fn");
}

static void test_eval_errors(ScriptHost& host) {
    // base() before tile()
    auto r1 = host.eval_tileset(R"JS(
export default class F extends Tileset {
  build() { this.base((x,z)=>0, 1); }
}
)JS", "{}", BakeOptions{});
    CHECK(!r1.error.ok && r1.error.message.find("before tile") != std::string::npos,
          "dsl: base() before tile() is a structured error");

    // build() that never calls tile()
    auto r2 = host.eval_tileset(R"JS(
export default class F extends Tileset {
  build() { this.fill(1); }
}
)JS", "{}", BakeOptions{});
    CHECK(!r2.error.ok && r2.error.message.find("tile()") != std::string::npos,
          "dsl: missing tile() is a structured error");

    // Part verbs still work inside a tileset build (inherited API)
    auto r3 = host.eval_tileset(R"JS(
export default class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 1 });
    this.fill(2);
    this.beginVoxels(0.05);
    this.sphere([1.0, 0.0, 1.0], 0.1);
    this.endVoxels();
  }
}
)JS", "{}", BakeOptions{});
    CHECK(r3.error.ok, "dsl: inherited Part voxel verbs eval clean in tileset");
}

static void test_eval_deterministic(ScriptHost& host) {
    auto a = host.eval_tileset(kMinimalTileset, "{}", BakeOptions{});
    auto b = host.eval_tileset(kMinimalTileset, "{}", BakeOptions{});
    CHECK(a.error.ok && b.error.ok, "dsl: double eval clean");
    CHECK(a.resolved_hash == b.resolved_hash, "dsl: resolved hash stable");
    CHECK(a.spec.base.heights == b.spec.base.heights, "dsl: base samples identical across evals");
}

int main() {
    printf("== tileset_dsl_tests ==\n");
    ScriptHost host;   // construct the way script_host_tests does (copy its setup)
    test_eval_records_tile_and_base(host);
    test_eval_errors(host);
    test_eval_deterministic(host);
    if (g_failures == 0) printf("PASSED (0 failures)\n");
    else                 printf("FAILED (%d failures)\n", g_failures);
    return g_failures;
}
```

Adapt `ScriptHost host;` construction and `BakeOptions{}` to the real constructor/required options — copy exactly how the existing `run-script` suite constructs them (read `MatterEngine3/tests/` script-host test first).

- [ ] **Step 8: Run to verify failure** — `make -C MatterEngine3/tests run-tilesetdsl`. Expected: compile failure (no `eval_tileset`).
- [ ] **Step 9: Implement Steps 1-6, re-run** — expected: all checks `ok`, `PASSED (0 failures)`. Also `make -C MatterEngine3/tests run-script` must still pass untouched (bake_source unchanged).
- [ ] **Step 10: Commit** — `git commit -m "feat: Tileset DSL root — spec recording, tile/base verbs, eval_tileset"`

---

### Task 3: Placement algorithms (`tileset_placement`)

**Files:**
- Create: `MatterEngine3/include/tileset_placement.h`, `MatterEngine3/src/tileset_placement.cpp`
- Modify: `MatterEngine3/Makefile` (add `tileset_placement.cpp` to ME3_CPP), `MatterEngine3/tests/Makefile`, `build-all.sh`
- Test: `MatterEngine3/tests/tileset_placement_tests.cpp` (new GL-free suite `run-tilesetplacement`, modeled on `run-tilesetcore`)

**Interfaces:**
- Consumes: `dsl::Rng` (`dsl_rng.h`).
- Produces (Task 4 relies on these exact signatures):

```cpp
namespace tileset {
struct Point2 { float x, z; };   // domain-local
enum class PlacementKind { Uniform = 0, Poisson = 1, Cluster = 2 };

// Rectangular domain [x0,x1) x [z0,z1) with corner-clear disks.
struct PlacementDomain {
    float x0, x1, z0, z1;
    // Disk centers (domain-local) that placements must clear by `clear_radius`.
    std::vector<Point2> clear_disks;
    float clear_radius = 0.0f;
};

// Deterministic scatter: expected count = density * usable area (rect area; the
// clear disks are handled by rejection). Same seed + domain + kind => same output.
std::vector<Point2> scatter(PlacementKind kind, const PlacementDomain& dom,
                            float density, uint64_t seed);

// Seed folding used by every placement call site (documented, test-guarded):
// fold(master, layer_index, domain_id) with SplitMix64 avalanche per fold.
uint64_t placement_seed(uint64_t master, uint32_t layer_index, uint32_t domain_id);
}
```

Domain ids (used by Task 4, fixed here): vertical strip color 0 → 0, color 1 → 1; horizontal strip color 0 → 2, color 1 → 3; tile interiors → `4 + row*4 + col` (4..19).

- [ ] **Step 1: Write the failing tests** — `MatterEngine3/tests/tileset_placement_tests.cpp`:

```cpp
#include "tileset_placement.h"
#include <cmath>
#include <cstdio>
#include <set>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

using namespace tileset;

static PlacementDomain interior_dom() {
    PlacementDomain d{ 0.15f, 1.85f, 0.15f, 1.85f, {}, 0.0f };
    return d;   // 2.0 m tile, 0.15 strip margin
}

static PlacementDomain strip_dom() {
    // vertical strip: across [-0.15, 0.15), along [0, 2.0), corners cleared at both ends
    PlacementDomain d{ -0.15f, 0.15f, 0.0f, 2.0f,
                       { {0.0f, 0.0f}, {0.0f, 2.0f} }, 0.08f };
    return d;
}

static void test_determinism() {
    for (int kind = 0; kind <= 2; ++kind) {
        auto a = scatter((PlacementKind)kind, interior_dom(), 40.0f, 1234);
        auto b = scatter((PlacementKind)kind, interior_dom(), 40.0f, 1234);
        CHECK(a.size() == b.size(), "placement: same-seed same count");
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i].x == b[i].x && a[i].z == b[i].z;
        CHECK(same, "placement: same-seed bit-identical points");
        auto c = scatter((PlacementKind)kind, interior_dom(), 40.0f, 1235);
        bool differs = c.size() != a.size();
        for (size_t i = 0; !differs && i < a.size() && i < c.size(); ++i)
            differs = a[i].x != c[i].x || a[i].z != c[i].z;
        CHECK(differs, "placement: different seed differs");
    }
}

static void test_bounds_and_clearance() {
    for (int kind = 0; kind <= 2; ++kind) {
        auto pts = scatter((PlacementKind)kind, strip_dom(), 60.0f, 99);
        CHECK(!pts.empty(), "placement: strip domain produces points");
        bool in = true, clear = true;
        for (const auto& p : pts) {
            if (p.x < -0.15f || p.x >= 0.15f || p.z < 0.0f || p.z >= 2.0f) in = false;
            for (const auto& d : strip_dom().clear_disks) {
                float dx = p.x - d.x, dz = p.z - d.z;
                if (std::sqrt(dx*dx + dz*dz) < 0.08f) clear = false;
            }
        }
        CHECK(in,    "placement: all points inside domain rect");
        CHECK(clear, "placement: corner disks respected");
    }
}

static void test_density() {
    // interior area = 1.7*1.7 = 2.89 m^2; density 40 => ~115 expected
    auto pts = scatter(PlacementKind::Uniform, interior_dom(), 40.0f, 7);
    CHECK(pts.size() >= 100 && pts.size() <= 132, "placement: uniform count ~= density*area");
    auto pp = scatter(PlacementKind::Poisson, interior_dom(), 40.0f, 7);
    CHECK(pp.size() >= 70, "placement: poisson reaches >=60% of target at moderate density");
    // poisson min-distance holds: r = 0.7/sqrt(density)
    float r = 0.7f / std::sqrt(40.0f);
    bool ok = true;
    for (size_t i = 0; i < pp.size() && ok; ++i)
        for (size_t j = i + 1; j < pp.size(); ++j) {
            float dx = pp[i].x - pp[j].x, dz = pp[i].z - pp[j].z;
            if (std::sqrt(dx*dx + dz*dz) < r * 0.999f) { ok = false; break; }
        }
    CHECK(ok, "placement: poisson min distance holds");
}

static void test_cluster_shape() {
    auto pts = scatter(PlacementKind::Cluster, interior_dom(), 60.0f, 21);
    CHECK(pts.size() >= 20, "placement: cluster produces points");
    // Clustered: mean nearest-neighbour distance well under uniform expectation.
    // Uniform mean NN ~ 0.5/sqrt(density) = 0.0645; clusters should be < 80% of that.
    float sum = 0.0f;
    for (size_t i = 0; i < pts.size(); ++i) {
        float best = 1e9f;
        for (size_t j = 0; j < pts.size(); ++j) {
            if (i == j) continue;
            float dx = pts[i].x - pts[j].x, dz = pts[i].z - pts[j].z;
            best = std::fmin(best, std::sqrt(dx*dx + dz*dz));
        }
        sum += best;
    }
    float mean_nn = sum / (float)pts.size();
    CHECK(mean_nn < 0.8f * (0.5f / std::sqrt(60.0f)), "placement: cluster mean-NN below uniform");
}

static void test_seed_fold() {
    CHECK(placement_seed(1, 0, 0) != placement_seed(1, 0, 1), "placement: domain id folds");
    CHECK(placement_seed(1, 0, 0) != placement_seed(1, 1, 0), "placement: layer index folds");
    CHECK(placement_seed(1, 0, 0) != placement_seed(2, 0, 0), "placement: master folds");
    CHECK(placement_seed(5, 3, 9) == placement_seed(5, 3, 9), "placement: fold is pure");
}

int main() {
    printf("== tileset_placement_tests ==\n");
    test_determinism();
    test_bounds_and_clearance();
    test_density();
    test_cluster_shape();
    test_seed_fold();
    if (g_failures == 0) printf("PASSED (0 failures)\n");
    else                 printf("FAILED (%d failures)\n", g_failures);
    return g_failures;
}
```

- [ ] **Step 2: Run to verify failure** — `make -C MatterEngine3/tests run-tilesetplacement` (after adding the Makefile target cloned from `run-tilesetcore`, linking `../src/tileset_placement.cpp`). Expected: compile failure (header missing).

- [ ] **Step 3: Implement** — `tileset_placement.cpp`:

```cpp
#include "tileset_placement.h"
#include "dsl_rng.h"
#include <cmath>

namespace tileset {

uint64_t placement_seed(uint64_t master, uint32_t layer_index, uint32_t domain_id) {
    // Two SplitMix64 avalanche folds; constants match dsl_rng.h.
    auto mix = [](uint64_t z) {
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    uint64_t s = mix(master + 0x9E3779B97F4A7C15ull * (uint64_t)(layer_index + 1));
    return mix(s ^ (0xD1B54A32D192ED03ull * (uint64_t)(domain_id + 1)));
}

namespace {

bool clears_disks(const PlacementDomain& d, float x, float z) {
    for (const Point2& c : d.clear_disks) {
        float dx = x - c.x, dz = z - c.z;
        if (dx * dx + dz * dz < d.clear_radius * d.clear_radius) return false;
    }
    return true;
}

float frange(dsl::Rng& r, float a, float b) { return a + (float)r.next_unit() * (b - a); }

std::vector<Point2> scatter_uniform(const PlacementDomain& d, int target, dsl::Rng& rng) {
    std::vector<Point2> out;
    out.reserve((size_t)target);
    int attempts = 0, max_attempts = target * 16 + 64;
    while ((int)out.size() < target && attempts++ < max_attempts) {
        float x = frange(rng, d.x0, d.x1), z = frange(rng, d.z0, d.z1);
        if (!clears_disks(d, x, z)) continue;
        out.push_back({ x, z });
    }
    return out;
}

std::vector<Point2> scatter_poisson(const PlacementDomain& d, int target, float density, dsl::Rng& rng) {
    // Dart throwing with min distance r = 0.7/sqrt(density).
    const float r = 0.7f / std::sqrt(density > 1e-6f ? density : 1e-6f);
    const float r2 = r * r;
    std::vector<Point2> out;
    out.reserve((size_t)target);
    int attempts = 0, max_attempts = target * 30 + 64;
    while ((int)out.size() < target && attempts++ < max_attempts) {
        float x = frange(rng, d.x0, d.x1), z = frange(rng, d.z0, d.z1);
        if (!clears_disks(d, x, z)) continue;
        bool ok = true;
        for (const Point2& p : out) {
            float dx = x - p.x, dz = z - p.z;
            if (dx * dx + dz * dz < r2) { ok = false; break; }
        }
        if (ok) out.push_back({ x, z });
    }
    return out;
}

std::vector<Point2> scatter_cluster(const PlacementDomain& d, int target, dsl::Rng& rng) {
    // Cluster centers (uniform), then gaussian-ish offsets (Box-Muller from rng).
    std::vector<Point2> out;
    out.reserve((size_t)target);
    int n_centers = target / 8 + 1;
    std::vector<Point2> centers;
    for (int i = 0; i < n_centers; ++i)
        centers.push_back({ frange(rng, d.x0, d.x1), frange(rng, d.z0, d.z1) });
    const float sigma = 0.15f;
    int attempts = 0, max_attempts = target * 16 + 64;
    while ((int)out.size() < target && attempts++ < max_attempts) {
        const Point2& c = centers[(size_t)(rng.next_u64() % (uint64_t)centers.size())];
        float u1 = (float)rng.next_unit(), u2 = (float)rng.next_unit();
        if (u1 < 1e-7f) u1 = 1e-7f;
        float mag = sigma * std::sqrt(-2.0f * std::log(u1));
        float x = c.x + mag * std::cos(6.2831853f * u2);
        float z = c.z + mag * std::sin(6.2831853f * u2);
        if (x < d.x0 || x >= d.x1 || z < d.z0 || z >= d.z1) continue;
        if (!clears_disks(d, x, z)) continue;
        out.push_back({ x, z });
    }
    return out;
}

} // namespace

std::vector<Point2> scatter(PlacementKind kind, const PlacementDomain& dom,
                            float density, uint64_t seed) {
    float area = (dom.x1 - dom.x0) * (dom.z1 - dom.z0);
    int target = (int)std::lround(density * (area > 0.0f ? area : 0.0f));
    if (target <= 0) return {};
    dsl::Rng rng(seed);
    switch (kind) {
        case PlacementKind::Poisson: return scatter_poisson(dom, target, density, rng);
        case PlacementKind::Cluster: return scatter_cluster(dom, target, rng);
        default:                     return scatter_uniform(dom, target, rng);
    }
}

} // namespace tileset
```

Header `tileset_placement.h` is exactly the Interfaces block above plus include guards and `#include <cstdint> <vector>`.

- [ ] **Step 4: Run tests to verify pass** — `make -C MatterEngine3/tests run-tilesetplacement`. Expected: `PASSED (0 failures)`. If the poisson ≥60%-of-target or cluster mean-NN bounds flake for the fixed seeds, tune the SEED in the test (not the assertion) — the bounds themselves are the contract.
- [ ] **Step 5: Register in build-all.sh** — add `run-tilesetplacement` to the MatterEngine3 suite list next to `run-tilesetcore`.
- [ ] **Step 6: Commit** — `git commit -m "feat: deterministic tileset placement algorithms (uniform/poisson/cluster)"`

---

### Task 4: `layer()` and `dropChild()` verbs

**Files:**
- Modify: `MatterEngine3/src/dsl_bindings.cpp` (replace the Task-2 stubs)
- Modify: `MatterEngine3/include/dsl_state.h` / `src/dsl_state.cpp` ONLY if the child-hash lookup helper needs exposing (see Step 2)
- Test: `MatterEngine3/tests/tileset_dsl_tests.cpp` (append)

**Interfaces:**
- Consumes: `tileset::scatter`, `tileset::placement_seed`, `tileset::PlacementDomain` (Task 3); `tileset_layout.h` (`kTorusN`); the child-hash composite-key lookup that `DslState::placeChild` uses.
- Produces: fully resolved `LayerSpec::strip[2][2]` + `interior[16]` and `TilesetSpec::drops` during eval. Layer ordering in `TilesetSpec::layers` = script call order (settle order).

**Semantics (implement exactly):**
- `dropChild(module, params)` — like `placeChild` but records `DropChildRec{ child_hash, transform }` (current matrix top) into `spec.drops` instead of the placement children table. Unknown module/params variant → structured error naming the module (same fail-closed path as `placeChild`).
- `layer(module, opts)` — reads opts (defaults from Global Constraints):
  - `density` (required, > 0) — else error `"layer('<module>'): density required"`.
  - `placement`: `'uniform' | 'poisson' | 'cluster'` → `placement_kind` 0/1/2; unknown string → error.
  - `physics` (bool), `embed` (0..1), `dropHeight: [min,max]`, `scale: [min,max]`, `collider` (string), `params` (object or function).
  - Then generates placements immediately, in this fixed order: vertical strip color 0, vertical strip color 1, horizontal strip color 0, horizontal strip color 1, then interiors tile 0..15. Domain ids per Task 3 (0,1,2,3, then 4+tile).
  - Strip domain: across `[-w, +w)`, along `[0, size)`, clear disks at along = 0 and along = size, radius `corner_clear_radius`. Map: vertical strips → `Placement.pos = { across, y, along }`; horizontal strips → `Placement.pos = { along, y, across }`.
  - Interior domain: `[w, size - w)` square (corner disks unnecessary: `corner_clear_radius < edge_strip_width` is enforced by `tile()`).
  - Per placement, using a per-domain `dsl::Rng rng(placement_seed(cfg.seed, layer_index, domain_id))` that is ALSO the source for scatter (pass the same seed to `scatter`, then continue drawing per-point attributes from a SECOND rng seeded `placement_seed(...) ^ 0xA5A5A5A5A5A5A5A5ull` so scatter internals can't shift attribute draws):
    - `scale` = uniform in `scale_range`.
    - physics `true`: `pos.y` = uniform in `dropHeight`; `quat` = uniform random unit quaternion (draw 4 unit floats mapped to N(0,1) via Box-Muller pairs, normalize; guard zero norm by retry).
    - physics `false`: `pos.y = 0` (snapped later by Task 7); `quat` = yaw-only rotation, angle uniform in `[0, 2π)`.
    - `params` resolution: if `params` is a function, build the `r` helper object (below), call `fn(r)`, JSON-stringify the result; if it is an object, stringify once; if absent, empty params. Resolve `module + params` through the SAME composite-key lookup `placeChild` uses; missing variant → error `"layer('<module>'): params variant not declared in static requires"`.
- The `r` helper for params functions: a plain JS object created native-side with methods `int(n)` (integer in `[0,n)`) and `float(a,b)` (uniform in `[a,b)`), both backed by natives that draw from the attribute rng of the CURRENT placement (store an `dsl::Rng*` in `TilesetState` while iterating; natives read it via `ts_of(ctx)`). Draw order: scale, then y/quat draws, then params draws — fixed and documented so re-evals are identical.

- [ ] **Step 1: Write the failing tests** — append to `tileset_dsl_tests.cpp` (declare two child fixture modules via the child-table arguments of `eval_tileset` — same mechanism `bake_source` tests use for `placeChild`):

```cpp
static const char* kLayeredTileset = R"JS(
export default class Floor extends Tileset {
  static requires = [
    { module: 'Pebble', params: { seed: 0 } },
    { module: 'Pebble', params: { seed: 1 } },
    { module: 'Twig' },
  ];
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 42 });
    this.base((x, z) => 0.0, 1);

    this.pushMatrix();
    this.translate(1.0, 0.2, 0.6);
    this.dropChild('Twig');
    this.popMatrix();

    this.layer('Pebble', { density: 30, physics: false, embed: 0.3,
                           params: r => ({ seed: r.int(2) }) });
    this.layer('Twig',   { density: 8, physics: true, dropHeight: [0.1, 0.3],
                           scale: [0.7, 1.3], placement: 'poisson' });
  }
}
)JS";

// Child tables: hashes are arbitrary but distinct; modules/params match `requires`.
static const uint64_t kChildHashes[] = { 0x1111, 0x2222, 0x3333 };
static const std::string kChildModules[] = { "Pebble", "Pebble", "Twig" };
static const std::string kChildParams[]  = { R"({"seed":0})", R"({"seed":1})", "{}" };

static void test_layer_recording(ScriptHost& host) {
    auto r = host.eval_tileset(kLayeredTileset, "{}", BakeOptions{},
                               kChildHashes, 3, kChildModules, kChildParams);
    CHECK(r.error.ok, "layer: layered tileset evals clean");
    CHECK(r.spec.layers.size() == 2, "layer: two layers in call order");
    CHECK(r.spec.drops.size() == 1 && r.spec.drops[0].child_hash == 0x3333,
          "layer: dropChild recorded with Twig hash");
    CHECK(std::fabs(r.spec.drops[0].transform[3] - 1.0f) < 1e-6f ||
          std::fabs(r.spec.drops[0].transform[12] - 1.0f) < 1e-6f,
          "layer: dropChild transform carries translation (row- or col-major slot)");

    const auto& L0 = r.spec.layers[0];
    CHECK(L0.module == "Pebble" && !L0.physics && std::fabs(L0.embed - 0.3f) < 1e-6f,
          "layer: pebble opts recorded");
    // 4 strip lists + 16 interiors all populated
    size_t strip_total = 0, interior_total = 0;
    for (int o = 0; o < 2; ++o) for (int c = 0; c < 2; ++c) strip_total += L0.strip[o][c].size();
    for (int t = 0; t < 16; ++t) interior_total += L0.interior[t].size();
    CHECK(strip_total > 0,    "layer: strip placements generated");
    CHECK(interior_total > 0, "layer: interior placements generated");
    // interior expected ~ density*(size-2w)^2 = 30*1.7^2 = 86.7 per tile
    CHECK(L0.interior[0].size() >= 70 && L0.interior[0].size() <= 100,
          "layer: interior count near density*area");

    // params fn resolved to declared variants only
    bool hashes_ok = true;
    for (const auto& p : L0.interior[0])
        if (p.child_hash != 0x1111 && p.child_hash != 0x2222) hashes_ok = false;
    CHECK(hashes_ok, "layer: params fn resolves to declared variant hashes");
    // both variants appear (density is high enough that P(all-same) ~ 2^-86)
    bool saw0 = false, saw1 = false;
    for (const auto& p : L0.interior[0]) { saw0 |= p.child_hash == 0x1111; saw1 |= p.child_hash == 0x2222; }
    CHECK(saw0 && saw1, "layer: both param variants used");

    // physics:false → yaw-only quats (x,z components zero)
    bool yaw_only = true;
    for (const auto& p : L0.interior[0])
        if (std::fabs(p.quat[0]) > 1e-6f || std::fabs(p.quat[2]) > 1e-6f) yaw_only = false;
    CHECK(yaw_only, "layer: non-physics placements are yaw-only");

    const auto& L1 = r.spec.layers[1];
    CHECK(L1.physics && L1.placement_kind == 1, "layer: twig physics+poisson recorded");
    bool y_in_range = true, scale_in_range = true;
    for (const auto& p : L1.interior[5]) {
        if (p.pos[1] < 0.1f || p.pos[1] > 0.3f) y_in_range = false;
        if (p.scale < 0.7f || p.scale > 1.3f) scale_in_range = false;
    }
    CHECK(y_in_range,     "layer: drop heights within range");
    CHECK(scale_in_range, "layer: scales within range");
    // strip placements stay within the strip domain
    bool strip_in = true;
    for (const auto& p : L1.strip[0][0])
        if (p.pos[0] < -0.15f || p.pos[0] >= 0.15f || p.pos[2] < 0.0f || p.pos[2] >= 2.0f)
            strip_in = false;
    CHECK(strip_in, "layer: vertical strip placements inside strip domain");
}

static void test_layer_determinism(ScriptHost& host) {
    auto a = host.eval_tileset(kLayeredTileset, "{}", BakeOptions{},
                               kChildHashes, 3, kChildModules, kChildParams);
    auto b = host.eval_tileset(kLayeredTileset, "{}", BakeOptions{},
                               kChildHashes, 3, kChildModules, kChildParams);
    CHECK(a.error.ok && b.error.ok, "layer: double eval clean");
    bool same = a.spec.layers.size() == b.spec.layers.size();
    for (size_t l = 0; same && l < a.spec.layers.size(); ++l) {
        const auto& x = a.spec.layers[l]; const auto& y = b.spec.layers[l];
        for (int t = 0; same && t < 16; ++t) {
            same = x.interior[t].size() == y.interior[t].size();
            for (size_t i = 0; same && i < x.interior[t].size(); ++i)
                same = std::memcmp(&x.interior[t][i], &y.interior[t][i], sizeof(tileset::Placement)) == 0;
        }
    }
    CHECK(same, "layer: placements bit-identical across evals");
}

static void test_layer_errors(ScriptHost& host) {
    // density missing
    auto r1 = host.eval_tileset(R"JS(
export default class F extends Tileset {
  static requires = [ { module: 'Twig' } ];
  build() { this.tile({size:2.0, texelsPerMeter:128, seed:1}); this.layer('Twig', {}); }
}
)JS", "{}", BakeOptions{}, &kChildHashes[2], 1, &kChildModules[2], &kChildParams[2]);
    CHECK(!r1.error.ok && r1.error.message.find("density") != std::string::npos,
          "layer: missing density is a structured error");
    // undeclared params variant
    auto r2 = host.eval_tileset(R"JS(
export default class F extends Tileset {
  static requires = [ { module: 'Twig' } ];
  build() { this.tile({size:2.0, texelsPerMeter:128, seed:1});
            this.layer('Twig', { density: 5, params: { seed: 9 } }); }
}
)JS", "{}", BakeOptions{}, &kChildHashes[2], 1, &kChildModules[2], &kChildParams[2]);
    CHECK(!r2.error.ok && r2.error.message.find("variant") != std::string::npos,
          "layer: undeclared params variant is a structured error");
}
```

Call the three new tests from `main()` after the Task-2 tests. Add `#include <cstring>` if missing.

- [ ] **Step 2: Run to verify failure** — `make -C MatterEngine3/tests run-tilesetdsl`. Expected: FAILs from the Task-2 stubs ("layer(): not implemented" structured errors).

- [ ] **Step 3: Implement** — replace the stubs in `dsl_bindings.cpp`:
  - `j_ts_dropChild`: mirror `j_placeChild`'s params-stringify + key building, but write the resolved hash + `state->top()` matrix into `ts->spec.drops`. If `DslState`'s composite-key lookup lives in a private method, expose it as `bool DslState::lookup_child_hash(const std::string& module, const char* params_json, size_t len, uint64_t& out)` (one new public method; `placeChild` refactors to call it — behavior unchanged, run `run-script` to prove it).
  - `j_ts_layer`: parse opts via `JS_GetPropertyStr` (`density`, `placement`, `physics`, `embed`, `dropHeight`, `scale`, `collider`, `params`); build the `LayerSpec`; then generate all 20 domains in the fixed order using `tileset::scatter` + per-domain attribute rng exactly as the Semantics block specifies; for each point draw scale → y/quat → params (fn or object) and emit a `Placement`. Layer index = `spec.layers.size()` at entry. Push the finished `LayerSpec` at the END (so errors mid-generation leave no partial layer).
  - Params-fn `r` object: `JS_NewObject`, attach `int`/`float` natives (`__dsl_ts_rng_int`, `__dsl_ts_rng_float` — register in `install_bindings`) reading `ts->param_rng` (add `dsl::Rng* param_rng = nullptr;` to `TilesetState`); set/clear it around each fn call.
  - Uniform random unit quaternion (physics placements):

```cpp
static void random_unit_quat(dsl::Rng& r, float q[4]) {
    for (;;) {
        float g[4];
        for (int i = 0; i < 4; i += 2) {
            float u1 = (float)r.next_unit(); if (u1 < 1e-7f) u1 = 1e-7f;
            float u2 = (float)r.next_unit();
            float m = std::sqrt(-2.0f * std::log(u1));
            g[i] = m * std::cos(6.2831853f * u2);
            g[i + 1] = m * std::sin(6.2831853f * u2);
        }
        float n = std::sqrt(g[0]*g[0] + g[1]*g[1] + g[2]*g[2] + g[3]*g[3]);
        if (n > 1e-6f) { q[0]=g[0]/n; q[1]=g[1]/n; q[2]=g[2]/n; q[3]=g[3]/n; return; }
    }
}
```

- [ ] **Step 4: Run tests to verify pass** — `make -C MatterEngine3/tests run-tilesetdsl` → `PASSED (0 failures)`; `make -C MatterEngine3/tests run-script` → still green (placeChild refactor is behavior-neutral).
- [ ] **Step 5: Commit** — `git commit -m "feat: tileset layer()/dropChild() verbs with resolved deterministic placements"`

---

### Task 5: `variant()` verb + edge-margin enforcement

**Files:**
- Modify: `MatterEngine3/src/dsl_bindings.cpp` (replace stub), `MatterEngine3/src/script_host.cpp` (invoke hooks post-build), `MatterEngine3/include/dsl_state.h` (only if op-AABB helper needs state access)
- Test: `MatterEngine3/tests/tileset_dsl_tests.cpp` (append)

**Interfaces:**
- Consumes: `DslState::op_count()/child_count()` (Task 2), `tileset_layout.h` `tile_colors(row, col)`.
- Produces: `TilesetSpec::variant_ranges` — one entry per tile that emitted content; `eval_tileset` calls the hook 16 times.

**Semantics:**
- `variant(fn)` may be called at most once; it only REGISTERS the hook (store the `JSValue` fn in `TilesetState` — dup it, free after use).
- After `build()` returns cleanly, `eval_tileset` invokes the hook once per tile `t = 0..15` (row = t/4, col = t%4) with argument `{ index: t, colors: { top, bottom, left, right }, rng: <r-helper> }`. `colors` from `tileset::tile_colors(row, col)`. `rng` = the same `int`/`float` helper as Task 4, seeded `placement_seed(cfg.seed, 0xFFFF, (uint32_t)t)`.
- Around each call, record `op_begin/child_begin = state.op_count()/child_count()`; after, `op_end/child_end`; push a `VariantRange` if anything was emitted. The transform stack must be reset to identity depth-1 before each call (push/pop imbalance inside the hook → structured error naming the tile).
- **Margin enforcement:** every op emitted inside the hook must keep a conservative AABB at least `edge_strip_width` from the tile bounds `[0, size)²` in XZ. Compute per `BuildOp`: transform the brush's local bound corners (sphere: center±r; box: ±halfExtents; capsule/cylinder/cone: segment endpoints ±max radius) by `op.transform`, take XZ min/max. For `placeChild` inside the hook, use the placement translation only. Violation → error `"variant(): tile <t> content within edgeStripWidth of tile bounds"`.

- [ ] **Step 1: Write the failing tests** — append:

```cpp
static void test_variant(ScriptHost& host) {
    auto r = host.eval_tileset(R"JS(
export default class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 3 });
    this.variant(t => {
      if (t.index === 5) {
        this.fill(2);
        this.beginVoxels(0.05);
        this.sphere([1.0, 0.0, 1.0], 0.05);   // center tile, well inside margin
        this.endVoxels();
      }
    });
  }
}
)JS", "{}", BakeOptions{});
    CHECK(r.error.ok, "variant: hook evals clean");
    CHECK(r.spec.variant_ranges.size() == 1 && r.spec.variant_ranges[0].tile == 5,
          "variant: only tile 5 emitted content");
    CHECK(r.spec.variant_ranges[0].op_end > r.spec.variant_ranges[0].op_begin,
          "variant: op range non-empty");
}

static void test_variant_margin(ScriptHost& host) {
    auto r = host.eval_tileset(R"JS(
export default class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 3 });
    this.variant(t => {
      if (t.index === 2) {
        this.beginVoxels(0.05);
        this.sphere([0.05, 0.0, 1.0], 0.04);   // 0.05 from x=0 bound < 0.15 margin
        this.endVoxels();
      }
    });
  }
}
)JS", "{}", BakeOptions{});
    CHECK(!r.error.ok, "variant: margin violation is an error");
    CHECK(r.error.message.find("tile 2") != std::string::npos,
          "variant: error names the tile");
}

static void test_variant_colors(ScriptHost& host) {
    // Hook observes de Bruijn colors: record them via heights hack? No — throw on mismatch.
    auto r = host.eval_tileset(R"JS(
export default class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 3 });
    this.variant(t => {
      const ok = [0,1].includes(t.colors.top) && [0,1].includes(t.colors.left);
      if (!ok) throw new Error('bad colors at tile ' + t.index);
      if (t.index === 0 && (t.colors.top !== 0 || t.colors.left !== 0))
        throw new Error('tile 0 colors wrong');
    });
  }
}
)JS", "{}", BakeOptions{});
    CHECK(r.error.ok, "variant: de Bruijn colors passed to hook");
}
```

Call all three from `main()`.

- [ ] **Step 2: Run to verify failure** — `make -C MatterEngine3/tests run-tilesetdsl`. Expected: FAILs (stub error).
- [ ] **Step 3: Implement** per Semantics; the margin check runs right after each hook call over ops in `[op_begin, op_end)`.
- [ ] **Step 4: Run tests to verify pass** — full `run-tilesetdsl` green, `run-script` still green.
- [ ] **Step 5: Commit** — `git commit -m "feat: tileset variant() per-tile hook with edge-margin enforcement"`

---

### Task 6: Part-mesh → collider bridge

**Files:**
- Create: `MatterEngine3/include/tileset_part_collider.h`, `MatterEngine3/src/tileset_part_collider.cpp`
- Modify: `MatterEngine3/Makefile`, `MatterEngine3/tests/Makefile`
- Test: `MatterEngine3/tests/tileset_bake_tests.cpp` (new suite `run-tilesetbake`; this task adds its first tests)

**Interfaces:**
- Consumes: `part_asset::load_v2(path, hash, blas, tlas, children, lods)`, `BLASManager::get_entries()` (`Tri` triangles — see `example_world.cpp:142-159`), `tileset::fit_collider` (Phase 1).
- Produces:

```cpp
namespace tileset {
// Load a baked part and fit its collision proxy. cache_dir is the parts/ root
// (same directory scheme as HostBaker). Returns false + err on load failure.
bool collider_for_part(const std::string& cache_dir, uint64_t resolved_hash,
                       const char* override_kind,   // nullptr/"auto"/"sphere"/...
                       ColliderFit& out, std::string& err);

// Scale a fit uniformly (half extents, radius, seg_half, hull points, center; volume *= s^3).
ColliderFit scale_fit(const ColliderFit& f, float s);

// Vertical extent of the fitted shape (for surface-snap embed math):
// distance from shape center to its lowest/highest point along +Y at identity orientation.
float fit_half_height(const ColliderFit& f);
}
```

- [ ] **Step 1: Write the failing tests** — `tileset_bake_tests.cpp` (clone the `run-graph-integration` Makefile recipe: it links ScriptHost + HostBaker + raylib; ADD box3d objects like `run-tilesetphysics` does — this suite is the full stack). Bake two fixture parts through the real `HostBaker` into a temp cache (copy the temp-dir + `graph.install` pattern from `part_graph_integration_tests.cpp:51-107`):

```cpp
static const char* kPebbleJs = R"JS(
export default class Pebble extends Part {
  static params = { seed: 0 };
  build(p) {
    this.fill(1);
    this.beginVoxels(0.01);
    this.sphere([0, 0, 0], 0.05);
    this.endVoxels();
  }
}
)JS";

static const char* kTwigJs = R"JS(
export default class Twig extends Part {
  build(p) {
    this.fill(2);
    this.line(-0.1, 0, 0, 0.1, 0, 0, 0.015, 0.015);
  }
}
)JS";

static void test_collider_bridge(const std::string& cache_dir,
                                 uint64_t pebble_hash, uint64_t twig_hash) {
    std::string err;
    tileset::ColliderFit pf;
    CHECK(tileset::collider_for_part(cache_dir, pebble_hash, nullptr, pf, err),
          "bridge: pebble collider fits");
    CHECK(pf.type == tileset::ColliderType::Sphere || pf.type == tileset::ColliderType::Hull,
          "bridge: pebble is sphere-ish");
    CHECK(pf.volume > 1e-7f, "bridge: pebble volume positive");

    tileset::ColliderFit tf;
    CHECK(tileset::collider_for_part(cache_dir, twig_hash, nullptr, tf, err),
          "bridge: twig collider fits");
    CHECK(tf.type == tileset::ColliderType::Capsule, "bridge: twig auto-fits capsule");

    tileset::ColliderFit forced;
    CHECK(tileset::collider_for_part(cache_dir, twig_hash, "box", forced, err),
          "bridge: override accepted");
    CHECK(forced.type == tileset::ColliderType::Box, "bridge: override forces box");

    CHECK(!tileset::collider_for_part(cache_dir, 0xDEAD, nullptr, pf, err) && !err.empty(),
          "bridge: missing part is a structured error");

    // scale_fit
    tileset::ColliderFit s = tileset::scale_fit(tf, 2.0f);
    CHECK(std::fabs(s.radius - tf.radius * 2.0f) < 1e-6f, "bridge: scale_fit scales radius");
    CHECK(std::fabs(s.volume - tf.volume * 8.0f) < 1e-4f * tf.volume, "bridge: scale_fit scales volume cubically");
    CHECK(tileset::fit_half_height(pf) > 0.0f, "bridge: half height positive");
}
```

`main()` sets up the temp cache, writes the two `.js` fixtures, runs `PartGraph::install` (real `FileModuleResolver` + `HostBaker`), captures the two root hashes, then calls `test_collider_bridge`.

- [ ] **Step 2: Run to verify failure** — `make -C MatterEngine3/tests run-tilesetbake`. Expected: compile failure (missing header).
- [ ] **Step 3: Implement** — `collider_for_part`: `part_asset::cache_path_resolved(hash)` under `cache_dir`, `load_v2` into a local `BLASManager`/`TLASManager`, gather every `Tri` vertex into a flat xyz array, call `fit_collider(xyz, count, override_kind)`. `scale_fit`: multiply `center`, `half_extent[3]`, `radius`, `seg_half`, every `hull_points` coordinate by `s`; `volume *= s*s*s`. `fit_half_height`: Sphere → `radius`; Capsule → `seg_half * |axis0·Y| + radius` (conservative: `seg_half + radius` is acceptable — pick the conservative form and note it); Box → `Σ half_extent[i] * |axis[i]·Y|`; Hull → max |y| over hull points relative to center.
- [ ] **Step 4: Run tests to verify pass** — `run-tilesetbake` green. Register the target in `build-all.sh`.
- [ ] **Step 5: Commit** — `git commit -m "feat: baked-part collider bridge (load, fit, scale)"`

---

### Task 7: Settle orchestrator (`tileset_bake`)

**Files:**
- Create: `MatterEngine3/include/tileset_bake.h`, `MatterEngine3/src/tileset_bake.cpp`
- Modify: `MatterEngine3/Makefile`, `MatterEngine3/tests/Makefile`
- Test: `MatterEngine3/tests/tileset_bake_tests.cpp` (append)

**Interfaces:**
- Consumes: `TilesetSpec` (Task 2/4/5), `collider_for_part`/`scale_fit`/`fit_half_height` (Task 6), `SettleWorld`/`BodySpawn`/`SettleParams`/`HeightField` (Phase 1), `tileset_layout.h` (`kTorusN`, `strip_occurrences`, `kBoundaryColors`).
- Produces (Phase 3 consumes this):

```cpp
namespace tileset {
struct SettledInstance {
    uint64_t child_hash = 0;
    float    scale = 1.0f;
    Pose     pose;            // torus-space (world XZ in [0, kTorusN*size))
    int      layer = -1;      // provenance: -1 = shared dropChild, else layer index
};

struct SettleReport {
    bool converged_all = true;
    std::vector<LayerResult> layers;   // per script layer
    uint64_t pose_hash = 0;            // SettleWorld determinism hash
};

struct SettledTorus {
    TileConfig cfg;
    BaseField  base;
    std::vector<SettledInstance> instances;   // spawn order
    std::vector<VariantRange> variant_ranges; // pass-through for Phase 3
    SettleReport report;
};

struct BakeInputs {
    std::string parts_cache_dir;   // where baked children live
};

// Assemble + settle the whole 4x4 torus. Fail-closed: false + err on any
// collider/load failure; non-convergence is a WARNING in report, not an error.
bool settle_tileset(const TilesetSpec& spec, const BakeInputs& in,
                    SettledTorus& out, std::string& err);
}
```

**Assembly semantics (implement exactly):**
1. `T = cfg.size`, torus extent `kTorusN * T`. Build the settle `HeightField` by tiling `spec.base` 4×4 (cell = `base.cell`, `count = base.n * kTorusN + 1` per axis, sample `heights[(z % n)*n + (x % n)]` — the +1 row/col duplicates row 0 for the collider edge). If `base.set == false`, flat 0 heightfield with `cell = T/8`.
2. Colliders: one `collider_for_part` per unique `(child_hash, collider_override)` across all layers + drops (memoize in a map). Per-instance scaled fits via `scale_fit`. Any failure → structured error naming the layer + module.
3. `SettleWorld world(kTorusN * T, height_field, SettleParams{});`
4. **Shared drops first** (before any layer): for each `DropChildRec`, one sync group with 16 occurrence frames (`Pose{ col*T, 0, row*T, 0,0,0,1 }` for every tile) — the drop transform's translation/rotation goes into the SPAWN pose (frame-local), NOT the frames (frames must stay pure translations). Extract translation from the transform matrix and rotation as a quaternion (matrix→quat helper; the matrices produced by the DSL transform stack are TRS — for v1 REQUIRE the rotation part orthonormal; non-uniform scale in dropChild transform → structured error). All drops settle as "layer 0 batch": collect their spawns in one `settle_layer` call.
5. **Per script layer, in order:** build one `settle_layer` batch:
   - Strip placements: for each orientation `o` (0 vertical, 1 horizontal) and color `c`, ONE sync group per PLACEMENT (each strip placement is a canonical body with 8 occurrence frames from `strip_occurrences(c, o == 0)`): frame for vertical occurrence `{boundary b, lane l}` = `Pose{ boundary_x(b) , 0, l*T, ... }` where `boundary_x(b)` = the torus x of that color's boundary index (boundary index i lies at `x = i*T`; verify against `strip_occurrences`' meaning in `tileset_layout.cpp` and the Phase-1 test usage — adapt so occurrence frames land each strip copy on every boundary where its color occurs, lane-offset along the seam). Horizontal mirrors with x/z swapped. Spawn pose = placement's domain-local pos/quat (strip-local: across → x, along → z for vertical; swapped for horizontal), `sync_group` = that group's id, one spawn per occurrence with `instance = k`.
   - Interior placements: tile t (row r, col c) → world pos `{ c*T + px, py, r*T + pz }`, free bodies (`sync_group = -1`).
   - physics `false` placements DO NOT SPAWN: they are surface-snapped analytically — `y = h + fh - embed * 2.0f * fh` where `h = base_height(x, z)` and `fh = fit_half_height(scaled fit)` (embed 0 → resting on surface, 0.5 → half-buried, 1 → fully buried); append directly to `out.instances` (yaw-only quat preserved) in placement order, `layer` = layer index.
   - Densities: `BodySpawn.density` default 400 (leave default); `friction` default.
6. After the last layer: `world.finalize()`. Read back `world.poses()` (spawn order) and zip with the recorded spawn provenance (child_hash, scale, layer, sync-instance flag) to fill `out.instances`. For sync groups, keep ALL occurrence instances (each occurrence is a real torus-space instance Phase 3 renders) — append every spawned body's settled pose with its child hash/scale.
7. Non-physics instances were already appended in step 5; ordering rule (test-guarded): for each layer, physics instances appear in spawn order, then that layer's non-physics instances in placement order. Shared drops appear first of all. Document this in the header comment.
8. `report.pose_hash = world.pose_hash()`; `report.layers` = per-layer `LayerResult`s; `converged_all` = all converged.

- [ ] **Step 1: Write the failing tests** — append to `tileset_bake_tests.cpp`; build a `TilesetSpec` in C++ (no JS needed here — construct the struct directly with the fixture hashes from Task 6's install):

```cpp
static tileset::TilesetSpec make_spec(uint64_t pebble_hash, uint64_t twig_hash) {
    tileset::TilesetSpec s;
    s.tile_called = true;
    s.cfg.size = 2.0f; s.cfg.seed = 99;
    // flat base
    s.base.n = 64; s.base.cell = 2.0f / 64.0f; s.base.material = 1;
    s.base.heights.assign(64 * 64, 0.0f); s.base.set = true;

    // one shared drop (Twig at tile-local (1.0, 0.2, 0.5), identity rotation)
    tileset::DropChildRec d{}; d.child_hash = twig_hash;
    float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(d.transform, I, sizeof I);
    d.transform[3] = 1.0f; d.transform[7] = 0.2f; d.transform[11] = 0.5f; // row-major TX/TY/TZ
    s.drops.push_back(d);

    // layer 0: pebbles, physics:false, embed 0.5
    tileset::LayerSpec l0; l0.module = "Pebble"; l0.density = 10.0f;
    l0.physics = false; l0.embed = 0.5f;
    for (int t = 0; t < 16; ++t)
        for (int i = 0; i < 3; ++i) {
            tileset::Placement p{}; p.child_hash = pebble_hash;
            p.pos[0] = 0.3f + 0.4f * i; p.pos[2] = 0.5f + 0.3f * i; p.scale = 1.0f;
            p.quat[3] = 1.0f;
            l0.interior[t].push_back(p);
        }
    s.layers.push_back(l0);

    // layer 1: twigs, physics:true — strips + interiors
    tileset::LayerSpec l1; l1.module = "Twig"; l1.density = 4.0f; l1.physics = true;
    for (int o = 0; o < 2; ++o)
        for (int c = 0; c < 2; ++c) {
            tileset::Placement p{}; p.child_hash = twig_hash;
            p.pos[0] = (o == 0) ? 0.05f : 1.0f;   // strip-local
            p.pos[1] = 0.2f;
            p.pos[2] = (o == 0) ? 1.0f : 0.05f;
            p.scale = 1.0f; p.quat[3] = 1.0f;
            l1.strip[o][c].push_back(p);
        }
    for (int t = 0; t < 16; ++t) {
        tileset::Placement p{}; p.child_hash = twig_hash;
        p.pos[0] = 1.2f; p.pos[1] = 0.25f; p.pos[2] = 1.4f; p.scale = 1.0f; p.quat[3] = 1.0f;
        l1.interior[t].push_back(p);
    }
    s.layers.push_back(l1);
    return s;
}

static void test_settle_tileset(const std::string& cache_dir,
                                uint64_t pebble_hash, uint64_t twig_hash) {
    tileset::TilesetSpec spec = make_spec(pebble_hash, twig_hash);
    tileset::BakeInputs in{ cache_dir };
    tileset::SettledTorus torus;
    std::string err;
    CHECK(tileset::settle_tileset(spec, in, torus, err), "bake: settle_tileset succeeds");
    CHECK(err.empty(), "bake: no error text on success");

    // Counts: drops 1x16 sync instances; layer1 strips 4 placements x 8 occurrences,
    // interiors 16; layer0 pebbles 16 tiles x 3 (non-physics).
    size_t expect = 16 + (4 * 8 + 16) + 48;
    CHECK(torus.instances.size() == expect, "bake: instance count (16 drops + 48 strips+interiors + 48 pebbles)");

    // All inside torus, none below ground
    float E = 4 * 2.0f;
    bool in_range = true, grounded = true;
    for (const auto& si : torus.instances) {
        if (si.pose.px < 0 || si.pose.px >= E || si.pose.pz < 0 || si.pose.pz >= E) in_range = false;
        if (si.pose.py < -0.05f || si.pose.py > 1.0f) grounded = false;
    }
    CHECK(in_range, "bake: all instances inside torus");
    CHECK(grounded, "bake: all instances near ground");

    // Non-physics embed math: pebble radius 0.05, embed 0.5 => y = fh - embed*2*fh = 0
    // (flat base h=0, fh ~= 0.05): expect y in [-0.01, 0.06] band and equal across tiles.
    float y0 = -1.0f; bool pebble_y_consistent = true;
    for (const auto& si : torus.instances) {
        if (si.layer != 0) continue;
        if (y0 < 0.0f) y0 = si.pose.py;
        else if (std::fabs(si.pose.py - y0) > 1e-6f) pebble_y_consistent = false;
    }
    CHECK(pebble_y_consistent, "bake: snapped pebble height identical across tiles");

    // Sync invariants: the 16 drop instances differ only by tile translation
    const auto* first = &torus.instances[0];
    bool sync_ok = true;
    for (int k = 1; k < 16; ++k) {
        const auto& a = torus.instances[0], & b = torus.instances[k];
        float dx = b.pose.px - a.pose.px, dz = b.pose.pz - a.pose.pz;
        float rx = std::fmod(dx, 2.0f), rz = std::fmod(dz, 2.0f);
        if (std::fabs(rx) > 1e-3f && std::fabs(std::fabs(rx) - 2.0f) > 1e-3f) sync_ok = false;
        if (std::fabs(rz) > 1e-3f && std::fabs(std::fabs(rz) - 2.0f) > 1e-3f) sync_ok = false;
        if (std::fabs(b.pose.py - a.pose.py) > 1e-4f) sync_ok = false;
        if (b.pose.qx != a.pose.qx || b.pose.qw != a.pose.qw) sync_ok = false;
    }
    (void)first;
    CHECK(sync_ok, "bake: 16 drop instances identical modulo tile translation");

    CHECK(torus.report.layers.size() == 2, "bake: per-layer reports present");
    CHECK(torus.report.pose_hash != 0, "bake: pose hash produced");

    // Determinism
    tileset::SettledTorus torus2;
    CHECK(tileset::settle_tileset(spec, in, torus2, err), "bake: second run succeeds");
    CHECK(torus.report.pose_hash == torus2.report.pose_hash, "bake: settle deterministic");

    // Failure path: unknown hash
    spec.layers[0].interior[0][0].child_hash = 0xBAD;
    tileset::SettledTorus t3;
    CHECK(!tileset::settle_tileset(spec, in, t3, err) && err.find("Pebble") != std::string::npos,
          "bake: unknown child hash errors naming the layer module");
}
```

- [ ] **Step 2: Run to verify failure** — `run-tilesetbake`: compile failure (missing `tileset_bake.h`).
- [ ] **Step 3: Implement** per Assembly semantics. Matrix→pose extraction (row-major TRS, rotation must be orthonormal within 1e-3 → else structured error): translation from elements (3,7,11); quaternion via the standard trace method. Keep `tileset_bake.cpp` free of QuickJS includes — it consumes only the spec.
- [ ] **Step 4: Run tests to verify pass** — `run-tilesetbake` green (expect ~2-4 min: 4 sync groups × 8 + 16 drops settle). If wall time exceeds ~5 min, reduce the fixture's `micro_relax_steps` via `SettleParams` — but do NOT loosen assertions.
- [ ] **Step 5: Commit** — `git commit -m "feat: tileset settle orchestrator — spec to settled torus"`

---

### Task 8: World-bake wiring + end-to-end acceptance

**Files:**
- Modify: `MatterEngine3/src/part_graph.cpp` / `include/part_graph.h` — NO (already done in Task 1); this task only ADDS the orchestration entry point:
- Create: `MatterEngine3/include/tileset_phase.h`, `MatterEngine3/src/tileset_phase.cpp`
- Modify: `MatterEngine3/Makefile`, `MatterEngine3/tests/Makefile`
- Test: `MatterEngine3/tests/tileset_bake_tests.cpp` (append)

**Interfaces:**
- Consumes: `PartGraph::read_manifest` (+ tileset flags), `PartGraph::install`, `FileModuleResolver`/`HostBaker` (however `run-graph-integration` constructs them), `ScriptHost::eval_tileset`, `ScriptHost::eval_requires`, `settle_tileset`.
- Produces (Phase 3 renders from this; Phase 3 also adds `.gtex` caching here):

```cpp
namespace tileset {
// Run the tileset phase for one manifest root: resolve+install children,
// eval the tileset script, settle. Fail-closed.
bool run_tileset_phase(const std::string& world_data_dir, const std::string& world,
                       const std::string& root_module,
                       const std::string& parts_cache_dir,
                       SettledTorus& out, std::string& err);
}
```

**Semantics:**
1. Load the root module source via the same resolver the world pipeline uses (mirror `run-graph-integration`'s construction; the module file is `<schemas dir>/<root_module>.js` — follow however part roots locate sources today).
2. `ScriptHost::eval_requires(source, "{}")` → child list; `PartGraph::install` those children (NOT the tileset root itself — it produces no `.part`).
3. Build the parallel arrays (`child_hashes/modules/params`) from the install result + requires list (canonical params JSON exactly as the graph resolves them — reuse the graph's canonicalization, do not re-canonicalize by hand).
4. `eval_tileset(...)` → spec; error → propagate.
5. `settle_tileset(spec, {parts_cache_dir}, out, err)`.

- [ ] **Step 1: Write the failing test** — append; full pipeline from a manifest on disk:

```cpp
static void test_e2e_manifest_to_settled_torus(const std::string& schemas_dir,
                                               const std::string& world_data_dir,
                                               const std::string& cache_dir) {
    // ForestFloor.js fixture written beside the Pebble/Twig fixtures
    static const char* kFloorJs = R"JS(
export default class ForestFloor extends Tileset {
  static requires = [
    { module: 'Pebble', params: { seed: 0 } },
    { module: 'Twig' },
  ];
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 7 });
    this.base((x, z) => 0.0, 1);
    this.layer('Pebble', { density: 8, physics: false, embed: 0.4,
                           params: () => ({ seed: 0 }) });
    this.layer('Twig',   { density: 3, physics: true, dropHeight: [0.1, 0.2] });
  }
}
)JS";
    write_file(schemas_dir + "/ForestFloor.js", kFloorJs);
    write_file(world_data_dir + "/E2E/world.manifest", "ForestFloor tileset\n");

    // Manifest surfaces the flag
    std::vector<PartGraph::ChildRequest> roots; std::vector<bool> expand, ts;
    std::string err;
    CHECK(PartGraph::read_manifest(world_data_dir, "E2E", roots, err, &expand, &ts),
          "e2e: manifest parses");
    CHECK(roots.size() == 1 && ts[0], "e2e: root flagged tileset");

    tileset::SettledTorus torus;
    CHECK(tileset::run_tileset_phase(world_data_dir, "E2E", roots[0].module,
                                     cache_dir, torus, err),
          "e2e: tileset phase runs");
    CHECK(torus.report.converged_all, "e2e: all layers converged");
    CHECK(!torus.instances.empty(), "e2e: settled instances produced");
    CHECK(torus.base.set, "e2e: base field present");

    // Determinism across the whole phase
    tileset::SettledTorus torus2;
    CHECK(tileset::run_tileset_phase(world_data_dir, "E2E", roots[0].module,
                                     cache_dir, torus2, err),
          "e2e: second phase run");
    CHECK(torus.report.pose_hash == torus2.report.pose_hash, "e2e: phase deterministic");

    // Missing child module fails closed
    write_file(schemas_dir + "/BadFloor.js",
        "export default class BadFloor extends Tileset {\n"
        "  static requires = [ { module: 'Nope' } ];\n"
        "  build() { this.tile({size:2.0, texelsPerMeter:64, seed:1});\n"
        "            this.layer('Nope', { density: 1 }); }\n}\n");
    tileset::SettledTorus t3;
    CHECK(!tileset::run_tileset_phase(world_data_dir, "E2E", "BadFloor", cache_dir, t3, err)
          && !err.empty(),
          "e2e: missing child module fails closed");
}
```

Reuse the suite's existing temp-dir/`write_file` helpers from earlier tasks. Call last in `main()`.

- [ ] **Step 2: Run to verify failure** — compile failure (missing `tileset_phase.h`).
- [ ] **Step 3: Implement** per Semantics — `tileset_phase.cpp` is glue, ~100 lines, no new logic.
- [ ] **Step 4: Full suite** — `make -C MatterEngine3/tests run-tilesetbake` green; then `bash build-all.sh test 2>&1 | tail -30` — all four tileset suites (`physics`, `core`, `dsl`, `placement`, `bake`) green; pre-existing MSL link failures are the only allowed FAILs.
- [ ] **Step 5: Commit** — `git commit -m "feat: tileset world-bake phase — manifest to settled torus e2e"`

---

## Verification

After all tasks:

1. `make -C MatterEngine3/tests run-tilesetdsl run-tilesetplacement run-tilesetbake run-tilesetphysics run-tilesetcore` — all `PASSED (0 failures)`, pristine output.
2. `make -C MatterEngine3/tests run-script run-graph run-graph-integration` — unchanged suites still green (placeChild refactor + manifest change are behavior-neutral).
3. `bash build-all.sh test` — no NEW failures vs. the branch baseline (known pre-existing: MatterSurfaceLib `cell_bounds_tests`/`mesh_continuity_tests` link error).
4. Spec coverage check: manifest entry (T1), Tileset root + inherited Part API (T2), placement algorithms (T3), layer/dropChild semantics (T4), variant + margin enforcement (T5), collider autofit per instance (T6), joint settle w/ strips 8× + shared drops 16× + layered order + finalize (T7), phase integration (T8). Phase 3 owns: flatten/TLAS, GPU passes, `.gtex`, caching, PNG dump. Phase 4 owns viewer consumption.




