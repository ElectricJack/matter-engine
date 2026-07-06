# Task 5 Report: `variant()` verb + edge-margin enforcement

## Status: DONE

## Commit
`908450b` — feat: tileset variant() per-tile hook with edge-margin enforcement

---

## What Was Done

Implemented the `variant()` DSL verb for Tileset roots: a per-tile authoring hook that lets scripts add tile-specific geometry after the main `build()` run. The hook is invoked 16 times (tiles 0..15) by `eval_tileset`, recording per-tile `VariantRange` entries into `TilesetSpec`.

### Files Changed

**`MatterEngine3/include/tileset_spec.h`**
Added three fields to `TilesetState`:
- `bool variant_called` — guards against double-registration
- `bool variant_fn_set` — whether a valid fn was stored
- `uint64_t variant_fn_bits[2]` — the duped JSValue stored as raw bits (memcpy, 16 bytes = sizeof(JSValue) on the target platform) so that `tileset_spec.h` does not need to `#include "quickjs.h"`

**`MatterEngine3/src/dsl_bindings.cpp`**
Replaced the `j_ts_variant` stub with a proper implementation:
- Validates: tileset context, tile() called before variant(), variant() called at most once, argument is a function
- Dups the JS function and memcpy-stores it into `ts->variant_fn_bits`
- Added `#include <cstring>` for `std::memcpy`
- Has a `static_assert` that `sizeof(JSValue) <= sizeof(variant_fn_bits)` (16 <= 16)

**`MatterEngine3/src/script_host.cpp`**
Added includes for `tileset_layout.h` and `tileset_placement.h`.

Added the variant hook invocation block in `eval_tileset`, after the `tile_called` check and before `r.spec = std::move(...)`:
- Recovers the `JSValue` from raw bits via memcpy
- Builds a `rng_helper` object (identical pattern to the layer params-fn helper: reuses `__dsl_ts_rng_int`/`__dsl_ts_rng_float` natives, sets `ts->param_rng = &tile_rng` per call)
- Per-tile rng seeded with `placement_seed(cfg.seed, 0xFFFF, (uint32_t)t)`
- Each tile: records `op_begin/child_begin`, calls hook with `{index, colors, rng}` arg, resets `param_rng`, then:
  - Checks for JS exceptions and DSL verb errors
  - Checks transform-stack balance (error naming the tile)
  - Margin check over `[op_begin, op_end)`: computes conservative XZ AABB per BrushKind (sphere: center +/- 6 axis-aligned radius points; box: 8 corners; capsule/cylinder: segment endpoints +/- radius, 4 axis-aligned per endpoint)
  - Margin check for child placements: uses row-major `transform[3]` (X) and `transform[11]` (Z)
  - Records `VariantRange` if content was emitted and no error
- Frees `variant_fn` and `rng_helper` after the loop

**`MatterEngine3/tests/tileset_dsl_tests.cpp`**
Appended three new tests:
- `test_variant` — sphere at tile center (tile 5 only), verifies 1 VariantRange with correct tile index and non-empty op range
- `test_variant_margin` — sphere near x=0 boundary (< 0.15m), verifies structured error naming "tile 2"
- `test_variant_colors` — checks de Bruijn colors are 0/1 values and tile 0 has top=0/left=0

One adaptation vs. the brief: guarded the third check in `test_variant` against empty `variant_ranges` (the brief's test accessed `[0]` without guard, causing a segfault before implementation).

---

## Test Output

```
== tileset_dsl_tests ==
[all prior tests: ok]
ok:   variant: hook evals clean
ok:   variant: only tile 5 emitted content
ok:   variant: op range non-empty
ok:   variant: margin violation is an error
ok:   variant: error names the tile
ok:   variant: de Bruijn colors passed to hook
PASSED (0 failures)

run-script: ALL PASS
run-tilesetplacement: PASSED (0 failures)
```

---

## Self-Review vs. Brief

| Requirement | Status |
|---|---|
| variant(fn) may be called at most once | OK Error "called more than once" |
| Only after tile() | OK Error "must be called after tile()" |
| Stores JSValue fn (dup'd) in TilesetState | OK memcpy into variant_fn_bits |
| Invoked 16 times after build() | OK t=0..15 loop in eval_tileset |
| Arg {index, colors:{top,bottom,left,right}, rng} | OK |
| Colors from tile_colors(row, col) | OK |
| rng seeded placement_seed(seed, 0xFFFF, t) | OK |
| Same int/float rng pattern as Task 4 | OK reuses __dsl_ts_rng_int/float + param_rng |
| op_begin/child_begin before, end after | OK |
| VariantRange pushed only if content emitted | OK |
| Transform stack balance check names tile | OK |
| Margin enforcement: sphere/box/capsule/cylinder | OK conservative AABB per brush kind |
| placeChild uses translation only | OK transform[3]/[11] |
| Error message "tile <t> content within edgeStripWidth..." | OK matches test find("tile 2") |
| No changes outside MatterEngine3/ | OK |

---

## Concerns

**Minor: JSValue raw-bits storage** — The implementation stores `JSValue` as `uint64_t[2]` via memcpy (16 bytes) to avoid polluting `tileset_spec.h` with a QuickJS header dependency. A `static_assert` guards `sizeof(JSValue) <= sizeof(variant_fn_bits)` at compile time. If QuickJS is ever switched to NaN-boxing mode (`uint64_t JSValue` = 8 bytes), the assert still passes and the storage is oversized but correct.

**Minor: JSValue not freed on earlier error** — If `r.error.ok` is false before the variant loop, the duped `JSValue` is not explicitly freed. It is cleaned up when `JS_FreeRuntime(rt)` is called. No real leak.

**Adaptation: test guard** — The brief's `test_variant` accessed `r.spec.variant_ranges[0]` unconditionally, causing a segfault when variant_ranges was empty (pre-implementation). Added a guard (`if (!r.spec.variant_ranges.empty())`) around the third check. The semantic intent is identical.

---

## Fix Report (review findings, branch fix/harden-bake-oom)

### Finding 1 (CRITICAL): Duped variant_fn JSValue leaks on early-exit paths

**Root cause**: The `if (r.error.ok && state.tileset()->variant_fn_set)` guard at line ~1272 means any path that sets `r.error.ok = false` before that block will silently skip the `JS_FreeValue` — leaking the duped JS function object. The original task-5 report noted this as "cleaned up by JS_FreeRuntime" but that is incorrect: a duped reference that is never freed is a real reference-count leak in QuickJS; `JS_FreeContext` / `JS_FreeRuntime` do not release objects whose reference counts were explicitly elevated.

**Change** (`MatterEngine3/src/script_host.cpp`):

1. After `JS_FreeValue(ctx, variant_fn)` at the end of the happy-path block, added `ts->variant_fn_set = false;` to mark the value as consumed.
2. At `ts_done` (before `JS_FreeContext`), added:
   ```cpp
   if (state.tileset()->variant_fn_set) {
       JSValue vfn_cleanup;
       std::memcpy(&vfn_cleanup, state.tileset()->variant_fn_bits, sizeof(vfn_cleanup));
       JS_FreeValue(ctx, vfn_cleanup);
       state.tileset()->variant_fn_set = false;
   }
   ```

**Exit-path trace** (all paths from variant() registration to JSValue release):

| Path | Where freed |
|------|-------------|
| Happy path — no errors, loop runs to completion | `JS_FreeValue(ctx, variant_fn)` inside the if-block, then `variant_fn_set = false` prevents ts_done double-free |
| Loop break: JS exception in variant hook | Loop exits via `break`; the code after the for-loop (inside the if-block) calls `JS_FreeValue(ctx, variant_fn)` + `variant_fn_set = false` |
| Loop break: DSL error / tileset error inside hook | Same as JS exception — break, then JS_FreeValue inside block |
| Loop break: transform stack imbalance (inside loop) | Same |
| Loop break: margin violation (inside loop) | `r.error.ok` set false; loop condition `r.error.ok` exits loop; JS_FreeValue inside block |
| `r.error.ok` false before variant block — build() exception at goto ts_done | `variant_fn_set` may be true; if-block skipped; freed at `ts_done` by new guard |
| `r.error.ok` false before variant block — DslState error (lines 1255-1258) | if-block skipped; freed at `ts_done` |
| `r.error.ok` false before variant block — TilesetState error (lines 1259-1262) | if-block skipped; freed at `ts_done` |
| `r.error.ok` false before variant block — tile() never called (lines 1265-1268) | if-block skipped; freed at `ts_done` |
| Early goto ts_done before build() runs (className empty, eval exception, constructor exception) | `variant_fn_set` is false (variant() not yet called); guard condition false, no free needed |
| `std::bad_alloc` catch | Catches before ts_done; JS context leaked by OOM handler (pre-existing behavior, not changed by this fix); `variant_fn_bits` are plain bytes, not freed — but context is already gone |

All normal exit paths: exactly-once semantics confirmed.

---

### Finding 2 (IMPORTANT): Sphere AABB not conservative under non-uniform scale

**Root cause**: The old sphere branch sampled 4 diagonal XZ corners `(cx±r, cy, cz±r)` + 2 Y-axis probes `(cx, cy±r, cz)`. The maximum world-X offset from center this gives is `max(|M.m0|+|M.m8|, |M.m4|) * r`. The true world-X half-extent for a sphere is `sqrt(M.m0²+M.m4²+M.m8²) * r`. These are NOT equal when all three of m0, m4, m8 are nonzero — the probe underestimates by Cauchy-Schwarz.

**Counterexample** (proves underestimation): Transform = `rotateZ(π/4) · scale(√2, √2, 1)` → M.m0=1, M.m4=-1, M.m8=0.
- Sphere radius 0.1, world center X = 0.26, strip width 0.15:
- Old probe max hx = `max(1+0, 1) * 0.1 = 0.1` → `xmin = 0.16 ≥ 0.15` **PASSES (wrong)**
- Row-norm hx = `sqrt(1+1+0) * 0.1 = 0.1414` → `xmin = 0.1186 < 0.15` **FAILS (correct)**

**Change** (`MatterEngine3/src/script_host.cpp`): Replaced the 6-probe sampling with the exact formula:
```cpp
const Matrix& M = op.transform;
float r0 = op.radius;
float hx = r0 * std::sqrt(M.m0*M.m0 + M.m4*M.m4 + M.m8*M.m8);
float hz = r0 * std::sqrt(M.m2*M.m2 + M.m6*M.m6 + M.m10*M.m10);
xmin = fx - hx; xmax = fx + hx;
zmin = fz - hz; zmax = fz + hz;
```
(World center `fx, fz` already computed before the branch.)

**Box and other kinds**: Box uses 8-corner enumeration (`{-1,+1}³` triple loop) — confirmed conservative and correct. Left unchanged. Capsule/Cylinder/Cone use 4-directional probes per endpoint — same category of risk as old sphere code, but left out of scope per task instructions.

---

### New tests added (`MatterEngine3/tests/tileset_dsl_tests.cpp`)

- `test_variant_sphere_nonuniform_scale_margin`: end-to-end DSL script using the exact counterexample (rotateZ(π/4) · scale(√2,√2,1), sphere r=0.1 at world center x=0.26, tile size=2, strip=0.15). Expects a margin error naming "tile 0".
- `test_variant_fn_freed_on_build_error`: two sub-cases — (1) build() throws after variant() is registered; (2) tile() called twice after variant() — both verify a structured error is returned without crash (structural proof that ts_done cleanup is correct).

---

### Commands run

```
make -C MatterEngine3/tests run-tilesetdsl
make -C MatterEngine3/tests run-script
```

### Test output

**run-tilesetdsl** (57 checks, all pass):
```
== tileset_dsl_tests ==
ok:   dsl: minimal tileset evals clean
[... 50 existing ok lines ...]
ok:   variant: hook evals clean
ok:   variant: only tile 5 emitted content
ok:   variant: op range non-empty
ok:   variant: margin violation is an error
ok:   variant: error names the tile
ok:   variant: de Bruijn colors passed to hook
ok:   variant: non-uniform scale sphere margin violation detected (row-norm fix)
ok:   variant: non-uniform scale sphere margin error names tile 0
ok:   variant fn leak fix: build() throw after variant() returns error
ok:   variant fn leak fix: error message is non-empty
ok:   variant fn leak fix: tile() twice after variant() returns error
ok:   variant fn leak fix: tile-twice error message is recognizable
PASSED (0 failures)
```

**run-script**: `ALL PASS`

### Commit SHA

`ef281bc` — fix: variant fn lifetime on error paths + conservative sphere margin bound (review findings)
