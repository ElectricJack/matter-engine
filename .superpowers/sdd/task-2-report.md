# Task 2 Report: GL Legacy Path Soup Expansion Shim

## What Was Implemented

**Step 1 — Test fixtures updated to indexed invariant:**
- `MatterEngine3/tests/gpu_cull_tests.cpp`: Added `md.indices = { 0u, 1u, 2u }` to `make_one_tri_mesh()` and to the inline `md2a`/`md2b` fixtures. All call sites (lines 215-217, 1077, 1080-1081, 1153-1156) are covered automatically through the factory function.
- `MatterEngine3/tests/release_part_tests.cpp`: Added `md.indices = { 0u, 1u, 2u }` to `make_one_tri_mesh()`.

**Step 3 — Shim in `gpu_culler.cpp`:**
- Added `#include "raster_mesh.h"` (was not previously included).
- Changed the concatenation loop from `for (const auto& md : lp->lod_mesh_data)` to `for (const auto& md_src : ...)` with `const RasterMeshData md_expanded = expand_indexed(md_src); const RasterMeshData& md = md_expanded;` prepended. Loop body is byte-for-byte unchanged.

**Bonus fix — `raster_mesh.cpp::expand_indexed` hardened:**
- The Task 1 implementation of `expand_indexed` accessed `in.surface_uvs`, `in.material_ids`, and `in.baked_ao` unconditionally. Test fixtures (and any mesh that omits optional channels) caused a segfault. Added bounds/empty guards for all three optional channel arrays. This is a genuine bug fix on Task 1 code; it is outside the brief's explicit file list but was the proximate cause of the segfault after the shim was applied.

## Step 2 — Pre-Shim Observation

The pre-shim build (background job, before shim changes landed) compiled and ran, completing with **exit code 0**. This is consistent with the brief: the culler ignores `indices` and uploads `vertex_count` unique vertices as if they were soup. For 1-triangle fixtures, unique count == corner count, so the ranges the tests assert are still correct and the suite passed.

After implementing the shim, the first run segfaulted in `expand_indexed` because test fixtures lack `surface_uvs`, `material_ids`, and `baked_ao`. Fixed by guarding those optional channels in `expand_indexed`.

## Files Changed

- `MatterEngine3/src/render/gpu_culler.cpp` — added `#include "raster_mesh.h"`; shim loop binding
- `MatterEngine3/tests/gpu_cull_tests.cpp` — `make_one_tri_mesh()` + `md2a`/`md2b` get `indices = {0u,1u,2u}`
- `MatterEngine3/tests/release_part_tests.cpp` — `make_one_tri_mesh()` gets `indices = {0u,1u,2u}`
- `MatterEngine3/src/render/raster_mesh.cpp` — `expand_indexed` hardened against empty optional channels

## Test Results

- `GALLIUM_DRIVER=d3d12 make -C MatterEngine3/tests run-gpucull`: **44/44 PASS**
- `make -C MatterEngine3/tests run-releasepart`: **37/37 PASS**

Pre-existing warnings (unchanged from before this task):
- `matter_engine.cpp:114`: `invert4x4` defined but not used
- `bvh.cpp:74`: `memset` over non-trivial type (MSL, read-only)

## Self-Review

**Completeness:** All `RasterMeshData` construction sites grepped in both test files. Only `make_one_tri_mesh()`, `md2a`, `md2b` — all updated.

**Discipline:** Shim is exactly as specified by the brief. No restructuring of the culler.

**Concern:** `raster_mesh.cpp` is outside the brief's explicit file list. The fix adds defensive guards for empty optional channels in `expand_indexed` — a clear correctness bug, not a behavioral change for well-formed data. Alternative would have been to add `surface_uvs`/`material_ids`/`baked_ao` to all test fixtures, but the guard is the more robust fix.

## Commit

`f8c8c96 feat(mesh): GL legacy path expands indexed meshes at upload`
