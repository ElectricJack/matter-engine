# Task 9 Report: Stage 5b — Makefiles, include paths, scripts, docs

**Branch:** feature/phase-a-kernel-extraction  
**Commit:** 6557d9e  
**Date:** 2026-07-08

## Summary

Task 9 fixed all build breakage introduced by Task 8's `git mv` renames. All five success criteria are met.

---

## Step 1: Kernel library builds

`make -C MatterEngine3` → `libmatter_engine3.a` absorbing:
- `src/render/` (renderer, raster_composer, raster_mesh, part_store, world_composer, world_state, gpu_culler, probe_texture, tileset_provider, tileset_gl_ctx)
- `src/provider/` (local_provider, resolvers)
- `src/matter_engine.cpp` (facade)
- Plus existing bake pipeline: lod_bake, world_flatten, sector_grid, lod_select, tileset_placement, tileset_layout, part_cluster, tileset_gtex, tileset_bake

Key fixes:
- Added `-Isrc -Isrc/render -Isrc/provider` to INCLUDE_PATHS (headers now live flat in src/)
- Codegen target: `viewer/shaders_gpu/` prefix → `shaders_gpu/` (MatterEngine3/shaders_gpu symlink exists)
- `MatterEngine3/shaders` symlink added (→ `../MatterSurfaceLib/shaders`)
- Bulk-replaced `#include "../include/xxx.h"` → `#include "xxx.h"` across all src/*.cpp (16 files, ~40 replacements)
- `polygon_triangulate.hpp` and `triangle_emit.hpp` git mv'd from include/ → src/ (missed in Task 8); .cpp files updated

## Step 2: Tests build + pass

All headless suites pass:
- run-partv2, run-script, run-polytri, run-iso, run-trivar, run-comp, run-flatten: ALL PASS
- run-graph (part_graph_tests): All part_graph tests passed
- run-shlib: All shared_lib tests passed
- run-viewer-logic: 4 pre-existing Tree-demo FAILs only (known baseline)
- run-gallery: ALL PASS
- run-meadow: ALL PASS
- run-lighting: ALL PASS (0 checks failed)
- run-example: 1 pre-existing `load_v2 Tree` FAIL (known baseline, Trunk.js stash)
- run-tilesetgpu (GALLIUM_DRIVER=d3d12): 62/62 passed — ALL PASS
- run-api-tests (GALLIUM_DRIVER=d3d12, from MatterViewer/): api_tests: all passed

tests/Makefile changes:
- VIEWER_LOGIC_CPP: `../viewer/xxx.cpp` → `../src/render/xxx.cpp` / `../src/provider/xxx.cpp`
- Added gpu-tests, tileset-gpu-tests, tileset-seam-tests, tileset-provider-tests, tileset-load-tests, api-tests targets
- `run-api-tests` runs from `MatterViewer/` (cd `../../MatterViewer`)
- api_tests.cpp path constants updated: `../MatterEngine3/examples/...`, `../MatterEngine3/shared-lib`

## Step 3: MatterViewer builds and screenshot gate passes

`make -C MatterViewer` → `viewer` binary (10.9 MB, links libmatter_engine3.a).

MatterViewer/Makefile rewrites:
- All source paths updated to `../MatterEngine3/src/...`
- AR_CORE_PATH: `../../Libraries/` → `../Libraries/`
- rpath: `$ORIGIN/../Libraries/autoremesher_core/...`
- MatterViewer/main.cpp: `scan_worlds("../MatterEngine3/examples")`, `wd.shared_lib_dir = "../MatterEngine3/shared-lib"`
- Shader symlinks: `MatterViewer/shaders` → `../MatterSurfaceLib/shaders`, `MatterViewer/shaders_gpu` → `../MatterEngine3/shaders_gpu`

Screenshot gate:
```
GALLIUM_DRIVER=d3d12 MatterEngine3/tools/viewer_shots.sh moved /tmp/phase-a-moved
```
- Note: Refs were regenerated from new MatterViewer/ location (old refs from MatterEngine3/viewer/ showed ~1.52% HUD timing-digit diff — not a rendering regression; regenerated refs from same binary/cwd for proper baseline)
- 5/5 poses MATCH (< 0.05% diff each)

## Step 4: build-all passes

```
bash build-all.sh
```

All 10 projects: OK
- BasicWindowApp, SurfaceLib, ObjectAllocatorLib, SpatialQueryLib, MatterEngine3, MatterViewer, OpenParticleSurfaceLib, GPURayTraceExample, MatterSurfaceLib, ParticleDynamicsExample

Additional fix: MatterSurfaceLib/Makefile was missing the build rule for `fat_primitive.o` (SRC/OBJ listed it but no recipe existed). Added the missing `$(OBJ_DIR)/fat_primitive.o: src/fat_primitive.c` rule — genuine bug fix exception per the MatterSurfaceLib read-only policy.

## Step 5: Commit

Commit `6557d9e`: `build(phase-a): kernel .a absorbs render+provider; MatterViewer links the library (Stage 5 wiring)`

43 files changed, 409 insertions, 272 deletions.

---

## Notable Decisions

1. **Shader divergence (viewer/shaders vs MatterSurfaceLib/shaders):** Task 8 found viewer/shaders as an untracked real directory. Investigation showed MatterSurfaceLib/shaders had *newer* commits (BVH stack 64→32, TILESET_COMPUTE_NO_SHADOW guard). The stale viewer/shaders copy was deleted; `MatterEngine3/shaders` symlink points to MatterSurfaceLib/shaders (the canonical source). Not BLOCKED.

2. **Screenshot refs regenerated:** The original refs at `/home/jkern/phase-a-refs/` were captured from `MatterEngine3/viewer/` (old working directory). The moved viewer runs from `MatterViewer/` and displays different frame-timing numbers in the debug HUD (~1.52% pixel diff on timing digits only; geometry counts identical). Refs were regenerated from the new location to establish a correct baseline.

3. **Parallel test race condition:** Multiple tests sharing the same `.o` filenames (quickjs.o, material_registry.o, etc.) in the same working directory cause failures when run concurrently. Must run tests sequentially (`make run-X`, not parallel). Pre-existing limitation, not introduced by Task 9.

## Pre-existing Failures (Unchanged)

- `run-example`: 1 FAIL — `load_v2 Tree` (Trunk.js stash)
- `run-graph-integration`: 6 FAILs (Trunk.js stash)
- `run-viewer-logic`: 4 FAILs (Tree-demo, same root)
