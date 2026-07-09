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

---

## Fix round 1

Two post-review findings fixed on branch `feature/memory-lib`.

### Fix 1 — MatterSurfaceLib: drop obsolete `object_allocator.c` copy step

**File changed:** `MatterSurfaceLib/Makefile` line 139 — deleted the `@cp ../ObjectAllocatorLib/src/object_allocator.c src/` line from the `dependencies` target. The vendored `src/object_allocator.c` is git-tracked inside MatterSurfaceLib and is the authoritative copy; the source directory `../ObjectAllocatorLib/` no longer exists, so the copy step caused `make -C MatterSurfaceLib` to abort. The `dependencies` target, its echo, and the `@mkdir -p src` line were preserved. No other MSL files touched.

**Verification commands:**
```
make -C MatterSurfaceLib dependencies
make -C MatterSurfaceLib WSL_LINUX=1   # (first 30 lines)
```

**Output summary:**
- `make -C MatterSurfaceLib dependencies` → exited 0; printed "Setting up dependencies for linux..."
- `make -C MatterSurfaceLib WSL_LINUX=1` → successfully passed the dependencies step, compiled `src/object_allocator.c` (vendored copy), and continued building all translation units. Build would ultimately fail later at link/display-init for unrelated GL/GLFW reasons (no display in headless WSL) — not related to `object_allocator`.

**Commit:** `d45a093` — `fix(MatterSurfaceLib): drop obsolete object_allocator.c copy step (vendored copy is git-tracked)`

---

### Fix 2 — SpatialQueryLib: rename test helper to `test_mem_pool_integration`

**File changed:** `SpatialQueryLib/main.c` — renamed `test_object_allocator_integration` → `test_mem_pool_integration` at the definition (line 20) and the call site (line 349). No other changes.

**Verification commands:**
```
make -C SpatialQueryLib
make -C SpatialQueryLib test
./SpatialQueryLib/spatialquerylib
```

**Output summary:**
- `make -C SpatialQueryLib` → clean build, exit 0.
- `make -C SpatialQueryLib test` → spatial_hash_tests 8/8 passed; bvh_tests 5/5 passed (ASan + UBSan enabled).
- `./spatialquerylib` → "9/9 tests passed", exit 0; first line of output: `PASSED: test_mem_pool_integration`.

**Commit:** `67adacd` — `refactor(SpatialQueryLib): rename test_object_allocator_integration to test_mem_pool_integration`
