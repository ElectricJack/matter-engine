# Task 2: Public API Surface — Event Fields, BakeErrorCode, pump_gpu_jobs, enable_live_edit

## Summary
Successfully implemented Task 2: public API surface additions for Phase B async bake. All header modifications, implementation code, test extensions, and full builds completed and verified.

## Implementation Details

### Step 1: Header Edits

#### MatterEngine3/include/matter/events.h
Replaced file body with new enums and struct as specified in brief:
- Added `BakeErrorCode` enum: None, Cancelled, OutOfMemory, ScriptError, GpuError, IoError, Internal
- Extended `Event` struct with Phase B fields (append-only):
  - `std::string phase` — "install" | "compose" | "parts" | "gl" | "cone" | ""
  - `BakeErrorCode code = BakeErrorCode::None` — BakeError classification
  - `int errors = 0` — BakeFinished: failed-part count (skip-and-continue)
- Updated field comments for clarity

#### MatterEngine3/include/matter/world_session.h
- Added to `WorldDesc`: `bool enable_live_edit = false;` with documentation
- Updated `request_bake()` comment to Phase B asynchronous semantics
- Updated `reload()` comment to Phase B asynchronous semantics
- Added new public method:
  ```cpp
  void pump_gpu_jobs(float ms_budget);
  ```
  with GL-thread context documentation

#### MatterEngine3/src/matter_engine.cpp
- Added `#include "async_bake.h"` to src includes
- Added to `WorldSession::Impl`:
  ```cpp
  matter_async::GpuJobQueue gpu_jobs;
  ```
- Implemented pump_gpu_jobs:
  ```cpp
  void WorldSession::pump_gpu_jobs(float ms_budget) {
      impl_->gpu_jobs.pump((double)ms_budget);
  }
  ```

### Step 2: Test Extension

#### MatterEngine3/tests/async_queue_tests.cpp
- Added `#include "matter/events.h"` for public API access
- Added test 9: `test_event_struct_shape()` that:
  - Constructs `matter::Event` 
  - Verifies defaults: `code == BakeErrorCode::None`, `errors == 0`, `phase.empty()`
  - Sets fields and verifies persistence
  - Integrated into main() test sequence

**Note:** Verified `-I../include` already in Makefile INCLUDE_PATHS; no Makefile changes needed.

## Test Execution Results

```
make -C MatterEngine3/tests run-asyncq
=== async_queue_tests ===
[test_pump_runs_posted_jobs_in_order]
ok pump_runs_posted_jobs_in_order
[test_pump_respects_budget_but_always_runs_one]
ok pump_respects_budget_but_always_runs_one
[test_run_blocking_returns_result]
ok run_blocking_returns_result
[test_cancelled_token_skips_job]
ok cancelled_token_skips_job
[test_shutdown_unblocks_waiter]
ok shutdown_unblocks_waiter
[test_bakeall_supersedes_pending_and_cancels_inflight]
ok bakeall_supersedes_pending_and_cancels_inflight
[test_command_shutdown_wakes_pop]
ok command_shutdown_wakes_pop
[test_push_after_shutdown_is_cancelled_and_not_queued]
ok push_after_shutdown_is_cancelled_and_not_queued
[test_event_struct_shape]
ok event_struct_shape_test

ALL PASS
```

Status: **PASS** — All 9 tests pass, including new struct-shape case.

## Build Verification

### MatterEngine3
```
make -C MatterEngine3 -j$(nproc)
```
**Status: PASS** — libmatter_engine3.a built successfully with no errors.

### MatterViewer
```
make -C MatterViewer -j$(nproc)
```
**Status: PASS** — viewer binary built successfully, linked with updated engine library, no compilation errors.

Note: Raylib static library built separately prior to viewer build (dependency setup).

## Files Modified
1. `MatterEngine3/include/matter/events.h`
2. `MatterEngine3/include/matter/world_session.h`
3. `MatterEngine3/src/matter_engine.cpp`
4. `MatterEngine3/tests/async_queue_tests.cpp`

## Verification Checklist
- ✅ Event struct fields added (append-only, no field reordering)
- ✅ BakeErrorCode enum defined with all required values
- ✅ WorldDesc.enable_live_edit field added with correct default
- ✅ pump_gpu_jobs method signature and comment match brief specification
- ✅ Implementation delegates correctly to gpu_jobs.pump()
- ✅ Request_bake() comment updated to Phase B specification
- ✅ Reload() comment updated to Phase B specification
- ✅ Test case added and passes
- ✅ MatterEngine3 compiles successfully
- ✅ MatterViewer compiles successfully (unaffected by changes)
- ✅ All async_queue_tests pass (8 pre-existing + 1 new struct-shape test)

## Fix round 1

**Review findings applied:**

1. `world_session.h`: Moved `pump_gpu_jobs(float ms_budget)` declaration with 3-line comment to immediately AFTER `render()` method (was after `reload()`). Per plan requirement.

2. `world_session.h`: Re-wrapped long single-line comments on `request_bake()` and `reload()` from ~175 chars to ~80-char `//` blocks, preserving exact wording including reload()'s fail-closed note.

3. `async_queue_tests.cpp`: Fixed test 9 output tokens — entry `[test_event_struct_shape]` (already matched), exit changed from `ok event_struct_shape_test` to `ok event_struct_shape` for consistency with all other test pairs.

**Test command:**
```
make -C MatterEngine3 -j$(nproc)
make -C MatterEngine3/tests run-asyncq
```

**Result:**
- Build: PASS — libmatter_engine3.a linked successfully
- Tests: ALL PASS — all 9 cases pass, pristine output with matching [test_X] / ok X tokens

**Commit:** b489497 `fix(phase-b): header comment wrapping + pump_gpu_jobs placement per plan (Task 2 review)`

## Next Steps
Ready for merge pending final review.
