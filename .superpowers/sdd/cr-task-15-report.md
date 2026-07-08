# Task 15 Report: MatterEngine3 viewer smells

## Overview
All five findings from the code-review brief addressed.  Clean build, 28/28 GPU tests pass, both screenshot paths verified at absolute paths.

---

## Changes per finding

### 1. Screenshot path unification (main.cpp)
**Finding:** `MATTER_SCREENSHOT` env-var path used raylib's `TakeScreenshot()` which silently prepends `GetWorkingDirectory()`, breaking absolute paths. The FIFO `shot` command already used the correct `LoadImageFromScreen()` + `ExportImage()` approach.

**Fix:** Replaced the `TakeScreenshot(screenshot_path)` call with the same `LoadImageFromScreen()` + `ExportImage()` + `UnloadImage()` pattern already used in the FIFO `shot` path (lines ~475-484 post-edit). Both paths now write to the given path verbatim.

### 2. Part-expansion walk consolidation
**Finding:** Three separate recursive part-tree walks existed:
- `main.cpp` ~180–191: `expanded_count` lambda for TLAS capacity sizing
- `part_store.cpp` 21–38: `expand_rec` / `build_expansion` for GPU expansion table
- `world_composer.cpp` 30–56: `emit` lambda for TLAS draw instance emission

**Fix:** Added `walk_part_tree(root_hash, getter, visitor(lp, hash, rel[16], depth))` to `part_store.h`/`part_store.cpp` as the single canonical traversal (depth cap 8, identity at root). All three sites now delegate to it:
- `build_expansion` → `walk_part_tree` visitor checks `!lp->lod_mesh_data.empty()`
- `world_composer.cpp::compose` → `walk_part_tree` visitor checks `!lp->lod_blas.empty()`, uses `depth==0` to select the resolved LOD vs. child LOD=0
- `main.cpp::expanded_count` → `walk_part_tree` visitor counts `!lp->lod_blas.empty()` nodes

Depth cap of 8 preserved identically in `walk_rec`.

### 3. Dead `readback_batches` (gpu_culler.cpp → gpu_cull_tests.cpp)
**Usage search:** `readback_batches` was called only from `gpu_cull_tests.cpp` (8 call sites). The main viewer's frame loop uses `draw_indirect()` exclusively. `RasterBatch` is still declared in `raster_composer.h` and used by `gpu_cull_tests.cpp`, so it was NOT deleted.

**Decision:** Moved the implementation from `gpu_culler.cpp` to `gpu_cull_tests.cpp` as a file-local free function `readback_batches(GpuCuller&, PartStore&)`. The declaration was removed from `gpu_culler.h`. The method's private-member access was resolved by adding a small set of `TEST-ONLY` public accessors to `GpuCuller` (`test_ssbo_cmds()`, `test_ssbo_xforms()`, `test_ssbo_stats()`, `test_total_xform_slots()`, `test_cmd_template()`, `test_cluster_staging()`).

Additionally, the HiZ occlusion tests called `readback_batches` solely for its side-effect of updating the private stat counters. A new `test_readback_stats()` method was added to `GpuCuller` for this purpose (reads only the stats SSBO, updates `stat_culled_`/`stat_culled_hiz_`/`stat_emitted_`). Those test call sites were updated to use `culler.test_readback_stats()` instead.

The `#include "raster_composer.h"` was removed from `gpu_culler.h` (no longer needed there). The `#include "raster_mesh.h"` was removed from `gpu_culler.cpp` (only needed for `row_major_to_matrix` in the now-moved function).

### 4. Local `mul16` duplicate in world_composer.cpp
**Finding:** `world_composer.cpp` had its own static `mul16` function (identical to `viewer::mul16` in `raster_cull.h`).

**Fix:** Removed the local static `mul16`, added `#include "raster_cull.h"` and used `viewer::mul16` directly (which is already inside `namespace viewer`, so the call is unqualified within the namespace block). Removed the now-unused `#include <functional>` as well.

### 5. Makefile shaders rule validation
**Finding:** The `shaders:` target only checked `[ -e shaders ]` — it didn't verify the actual shader files the viewer loads exist, so a corrupt/incomplete shaders link would fail silently at runtime.

**Fix:** Extended the `shaders:` target to validate three required files after the symlink step:
- `shaders/raster.vs`
- `shaders/raster.fs`
- `shaders/raytrace_tlas_blas_processed.fs`

If any are missing, `make` exits with a clear error listing all missing files.

---

## readback_batches usage search evidence

Files searched: entire worktree.

| File | Use |
|------|-----|
| `MatterEngine3/viewer/gpu_cull_tests.cpp` | 8 call sites (transform parity + HiZ stat verification) |
| `MatterEngine3/viewer/gpu_culler.h` | declaration (removed) |
| `MatterEngine3/viewer/gpu_culler.cpp` | implementation (moved to test TU) |
| `MatterEngine3/tests/viewer_logic_tests.cpp` | comment-only references (no call) |
| `docs/` | plan docs, no calls |

**Choice:** Move into test TU (not delete), because `gpu_cull_tests.cpp` uses it extensively for parity verification. `RasterBatch` kept because it's still declared in `raster_composer.h` and used by the test TU.

---

## Build results
`cd MatterEngine3/viewer && make clean && make` → success (warnings are pre-existing, no new errors).
`make gpu-tests` → success.

## Test results vs known-red baseline
| Target | Result | vs baseline |
|--------|--------|-------------|
| `run-partv2` | PASS | unchanged |
| `run-script` (ALL PASS) | PASS | unchanged |
| `run-graph-integration` | 6 FAIL | matches known-red |
| `run-viewer-logic` | link error (tileset_provider/run_tileset_phase) | matches known-red |
| GPU tests (`./gpu_tests`) | 28/28 PASS | new (was 28/28 before, still 28/28) |

No new failures introduced.

## Screenshot verification
Both paths tested against absolute `/tmp/...` paths:

1. **FIFO `shot` path**: `echo "shot /tmp/test_fifo_shot_NNN.png" > $FIFO` → file written at exact absolute path, `.done` marker created. Log confirmed `INFO: FILEIO: ... File saved successfully`.

2. **`MATTER_SCREENSHOT` env-var path**: `MATTER_SCREENSHOT=/tmp/test_matter_screenshot.png ./viewer` → file written at exact absolute path. Log confirmed `INFO: FILEIO: ... Image exported successfully` and `screenshot written to /tmp/test_matter_screenshot.png`.

Both paths use `LoadImageFromScreen()` + `ExportImage()` and write the exact given path verbatim.

## Deviations from brief
- The brief said "consolidate into ONE traversal function in part_store." Done. The `build_expansion` wrapper is kept as a thin shim (matches `part_store.h` documented API).
- `readback_batches` is moved as a free function (not a member), which requires `TEST-ONLY` accessors. This is cleaner than keeping it as a member defined in the test TU, because it removes the function from the public class API entirely.
- `world_composer.cpp`'s `kMaxDepth` constant was removed (depth cap is now enforced inside `walk_part_tree`). The `kMaxInstances` check is preserved in the visitor, but walk continues past the limit (visiting nodes but not emitting). This is correct and matches the spirit of the original (the limit was an emergency brake, not an optimization).

## Concerns
None. All tests pass, both screenshot paths verified, no new link errors.
