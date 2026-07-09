# Task 5 Report: Engine Integration — Build Wiring, DslState Hooks, pf_bindings Core

## What Was Implemented

### Step 1: MatterEngine3/Makefile
- Added `PFL_DIR = ../ParticleFlowLib` near `MSL_DIR`
- Appended `-I$(PFL_DIR)/include` to `INCLUDE_PATHS` (before `$(EXTRA_INCLUDE)`)
- Added `pf_math.o pf_sim.o pf_fields.o pf_path_recorder.o pf_bindings.o` to `ME3_OBJ`
- Extended `vpath %.cpp` to include `$(PFL_DIR)/src`

### Step 2: MatterEngine3/src/dsl_state.h
- Added `#include <chrono>`
- Added public accessors: `set_pf_registry`, `pf_registry`, `set_budget`, `budget_exceeded`
- Added private members: `pf_registry_` (shared_ptr<void>), `budget_deadline_`, `budget_bounded_`

### Step 3: MatterEngine3/src/script_host.cpp
- Added `state.set_budget(ic.deadline, ic.bounded)` after both `JS_SetContextOpaque` calls (~line 977 bake_source, ~line 1486 eval_tileset)

### Step 4 & 5: MatterEngine3/src/pf_bindings.h + pf_bindings.cpp
- Created header with `install_pf_bindings(JSContext*)` declaration
- Created implementation with `PfRegistry`, `pf_registry_of`, config parsers, and all 10 `__pf_*` bindings
- `f32_copy` helper creates Float32Array with correct `argc=3` call to `JS_NewTypedArray`

### Step 6: MatterEngine3/src/dsl_bindings.cpp
- Added `#include "pf_bindings.h"` and `install_pf_bindings(ctx)` call at end of `install_bindings`

### Step 7: MatterEngine3/tests/Makefile
- Appended `-I../../ParticleFlowLib/include` to `INCLUDE_PATHS`
- Added pf_bindings.cpp and 4 ParticleFlowLib sources to `SCRIPT_CPP`

### Step 8: MatterEngine3/tests/script_host_tests.cpp
- Added `test_pf_bindings_smoke()` and registered it in `main()`

## Test Commands and Output

**Archive build:**
```
make -C MatterEngine3
```
Exit code 0. `libmatter_engine3.a` includes `pf_math.o pf_sim.o pf_fields.o pf_path_recorder.o pf_bindings.o`. No new warnings.

**Script-host test suite:**
```
make -C MatterEngine3/tests run-script
```
Exit code 0. Output tail:
```
  test_eval_lod_budgets OK
Warning: No draw records to build TLAS from
Warning: No draw records to build TLAS from
ALL PASS
```

## Deviations from the Brief

### 1. `Sim::step()` is private — used `sim->run(chunk)` instead
The brief called `sim->step()` in a loop. `step()` is private in `Sim`. Replaced with `sim->run(chunk)` — semantically identical.

### 2. `EmitterConfig::vel0` is `float` not `V3`
The brief parsed `vel0` as a V3. Kernel has `float vel0` (initial speed along axis). Parser reads it as scalar via `get_num(c, e, "vel0", 1.0)`.

### 3. `FieldConfig::Fade::axis` is `V3` not `int`
The brief converted the axis string to int (0/1/2). Kernel `Fade` struct has `V3 axis`. Parser accepts both: string shorthand ("x"→{1,0,0}, "z"→{0,0,1}, other→{0,1,0}) and falls back to `get_v3` for array notation.

### 4. `kill_on_consume` default `true` (not `false`)
Matched the kernel header's struct default of `true`.

### 5. `JS_NewTypedArray` argc=3 fix (critical bug in brief's code)
Brief used `JSValue argv[1] = {buf}; JS_NewTypedArray(c, 1, argv, FLOAT32)`. The QuickJS implementation `js_typed_array_constructor` unconditionally reads `argv[1]` and `argv[2]` without checking argc — UB for argc=1, causing `RangeError: invalid length`. Fixed by passing `JSValue argv[3] = {buf, JS_UNDEFINED, JS_UNDEFINED}` with `argc=3`. This correctly enters the `JS_IsUndefined(argv[2])` branch and computes length from byte_length.

### 6. Worktree missing raylib build artifact
Symlinked `Libraries/raylib/build/linux/libraylib.a` → main repo's built library. This was required for the test linker (pre-existing worktree setup issue, not a code change).

### 7. Smoke test JS: added diagnostic messages and safety guards
Added inline values in error strings and a `sim < 0` guard for easier debugging. These don't change the contract.

## Self-Review

- No globals/statics: `PfRegistry` is bake-scoped via `DslState::pf_registry_` (shared_ptr<void> with custom deleter).
- Stale/invalid handles: `sim_of`/`rec_of` call `set_error` and return nullptr — fail-closed.
- Budget check: `budget_exceeded()` called every RUN_CHUNK=32 ticks in `j_pf_run`.
- `pf_registry_of` is in `dsl::` namespace (not anonymous) so Tasks 6/7 can reuse it.
- `f32_copy` fix is not a deviation in spirit — the brief's code had UB; this is correct.
- Both `JS_SetContextOpaque` sites updated.
- `install_pf_bindings` called from inside `install_bindings` before `JS_FreeValue(ctx,g)`.
