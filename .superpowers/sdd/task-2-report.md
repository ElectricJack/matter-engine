# Task 2 Report: Migrate consumers and the build system to MemoryLib mem_pool API

## What Was Done

Retargeted every consumer of the old `ObjectAllocatorLib` / `oa_*` API to the renamed
`MemoryLib` / `mem_pool_*` API introduced in Task 1. All changes are mechanical renames
with one exception (see Findings below).

### Files changed (20 total)

| File | Change |
|------|--------|
| `SpatialQueryLib/Makefile` | `OA_LIB` → `MEMORY_LIB`; paths to `../MemoryLib`; `object_allocator` → `mem_pool`; removed `mem_pool.c` from `BVH_TEST_SRCS` |
| `SpatialQueryLib/src/bvh.c` | Deleted dead `#include "object_allocator.h"` (zero call sites confirmed) |
| `SpatialQueryLib/main.c` | Header, type, and all `oa_*` call sites renamed to `mem_pool_*` |
| `SpatialQueryLib/src/spatial_hash.c` | Header renamed |
| `OpenParticleSurfaceLib/Makefile` | `OA_LIB` → `MEMORY_LIB`; paths; build rule target |
| `OpenParticleSurfaceLib/src/open_particle_surface.c` | Header renamed |
| `OpenParticleSurfaceLib/install_dependencies.sh` | All `ObjectAllocatorLib` / `object_allocator` refs renamed |
| `OpenParticleSurfaceLib/build.sh` | Same |
| `SurfaceLib/Makefile` | `OA_LIB` → `MEMORY_LIB`; paths; build rule target |
| `GPURayTraceExample/Makefile` | Same + stale comment fixed |
| `GPURayTraceExample/README.md` | `(copied from ObjectAllocatorLib)` → `(compiled from MemoryLib)` |
| `ParticleDynamicsExample/Makefile` | `OA_LIB` → `MEMORY_LIB`; paths; build rule target |
| `ParticleDynamicsExample/build.sh` | All refs renamed |
| `ParticleDynamicsExample/README.md` | Dependency name + removed stale `object_allocator.c` from file tree |
| `ParticleDynamicsExample/tests/Makefile` | Stale `../../SpatialQueryLib/src/object_allocator.c` → `../../MemoryLib/src/mem_pool.c` |
| `build-all.sh` | Line 35: project name; lines 142-145: simplified binary-name loop |
| `.gitignore` | Four edits: main binary, comment, two test binaries, linux dir |
| `README.md` | Five edits: section header, dep refs, table row, test count |
| `CLAUDE.md` | Library name in architecture section |
| `ROADMAP.md` | Two occurrence renames |

## Findings

**bvh.c dead include** — `SpatialQueryLib/src/bvh.c` had `#include "object_allocator.h"` at
line 2 with zero `oa_*` call sites in the file. The include was removed entirely. `mem_pool.c`
was also removed from `BVH_TEST_SRCS` since bvh tests never called pool functions.

**Stale path in ParticleDynamicsExample/tests/Makefile** — Line 10 referenced
`../../SpatialQueryLib/src/object_allocator.c`, a path that never existed (SpatialQueryLib
never owned `object_allocator.c`). Fixed to `../../MemoryLib/src/mem_pool.c`.

**MatterEngine3/MatterViewer MSL references are not in scope** — `MatterEngine3/Makefile`,
`MatterEngine3/tests/Makefile`, and `MatterViewer/Makefile` all reference
`$(MSL_DIR)/src/object_allocator.c`. These point to MatterSurfaceLib's vendored internal
copy (never ObjectAllocatorLib). Confirmed pre-existing on `main` via
`git show main:MatterEngine3/Makefile | grep object_allocator`. Not touched.

## Gate Results

Full `bash build-all.sh test` run completed (build died mid-grasslod OOM; remaining suites
run individually). All failures are pre-existing on `main`:

| Test Suite | Result |
|---|---|
| MemoryLib | PASS (6/6) |
| SpatialQueryLib | PASS (spatial hash + BVH suites) |
| MatterSurfaceLib (all suites) | PASS |
| MatterEngine3 run-partv2 through run-dev | PASS |
| MatterEngine3 run-example | FAIL (load_v2 Tree) — pre-existing |
| MatterEngine3 run-graph-integration | FAIL (6 FAILs, Tree.js disabled) — pre-existing |
| MatterEngine3 run-viewer-logic | FAIL (4 FAILs) — pre-existing |
| MatterEngine3 run-grasslod | PASS |
| MatterEngine3 run-stressforest | PASS (12/12) |
| MatterEngine3 run-tilesetdsl | PASS |
| MatterEngine3 run-tilesetbake | PASS |
| MatterEngine3 run-tilesettorusbvh | PASS (12/12) |
| MatterEngine3 run-tilesetmeadowmanifest | PASS (4/4) |
| MatterEngine3 run-tilesetphysics/core/placement/gtex/shader-source | FAIL (pre-existing include-path build failures) |

All pre-existing failures confirmed identical on unmodified `main` via `git stash` checks.

## Straggler Grep

```
grep -rn "ObjectAllocatorLib|oa_create|oa_alloc|object_allocator" \
  --include="*.c" --include="*.h" --include="*.cpp" --include="*.hpp" \
  --include="Makefile" --include="*.sh" --include="*.md" . \
  | grep -v "^./MatterSurfaceLib/" | grep -v "^./docs/" \
  | grep -v "^./Libraries/" | grep -v "^./Examples/" \
  | grep -v "^./.superpowers/sdd/"
```

Residuals (all acceptable):
- `MatterEngine3/Makefile`, `MatterEngine3/tests/Makefile`, `MatterViewer/Makefile` —
  `$(MSL_DIR)/src/object_allocator.c` (MatterSurfaceLib vendored copy, pre-existing)
- `MemoryLib/OBJECT_ALLOCATOR.md` — legacy docs inside MemoryLib, not a consumer
- `SpatialQueryLib/main.c:20` — function name `test_object_allocator_integration()`
  (test helper name, not an API symbol — intentionally kept per brief scope)

## Commit

`23ad9f1` — `refactor: retarget all ObjectAllocatorLib consumers to MemoryLib mem_pool API`
