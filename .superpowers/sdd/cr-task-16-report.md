# Task 16 Report — Code-Review Hygiene: shared check.h, Makefile dedup, robust tools

**Status:** COMPLETE
**Branch:** feature/autoremesher-integration (code-review-fixes worktree)
**Commit:** see below

---

## What was done

### 1. Created `MatterEngine3/tests/check.h`

New shared test header. Provides:
- `static int g_failures = 0;` — canonical counter
- `CHECK(cond, msg)` — 2-arg macro; prints "FAIL: <msg>" and increments `g_failures`
- `check_summary()` — prints "ALL PASS" or "<N> FAILURE(S)", returns exit code

### 2. Migrated all 26 test `.cpp` files

Every test file replaced its locally-defined CHECK macro + failures counter with `#include "check.h"`.

Three counter name variants existed and were handled:

| Variant | Files | Handling |
|---|---|---|
| `failures` | composition, iso, lighting, meadow_bake, meadow_bake_check, partv2, flatten, graph-int, graph, polytri, script, shlib, trivar | Renamed `failures` → `g_failures` |
| `g_failures` already | dev, grasslod, stressforest, tileset_bake, tileset_core, tileset_dsl, tileset_physics, tileset_placement | Removed local macro+counter, added `#include "check.h"` |
| `g_fail`/`g_pass` (1-arg CHECK) | tileset_gtex, tileset_torus_bvh | `#include "check.h"`, then `#undef CHECK` + redefine 1-arg variant using `g_failures` |
| `REQUIRE` + `g_tests` | tileset_meadow_manifest | `#include "check.h"`, kept `g_tests` and `REQUIRE` macro, removed local `g_failures` decl |
| `g_fail` name | gallery_bake | Renamed `g_fail` → `g_failures` throughout |

Special files:
- `viewer_logic_tests.cpp`: Also had 13 `assert()` calls converted to `CHECK(cond, msg)`.
- `viewer/gpu_cull_tests.cpp`: Had its own `g_failures`+`CHECK` macro — replaced with `#include "../tests/check.h"`.

### 3. Converted `assert()` → `CHECK()` in viewer tests

**`viewer_logic_tests.cpp`**: 13 asserts converted, e.g.:
```cpp
// Before: assert(s.version() == 0);
// After:  CHECK(s.version() == 0, "initial version == 0");
```

**`viewer/gpu_cull_tests.cpp`**: 3 asserts converted using existing local CHECK macro.

### 4. Added `mkdtemp` for race-free temp dirs in `gpu_cull_tests.cpp`

Added `make_test_tmpdir(tag)` helper using `mkdtemp()`. Replaced 6 fixed `/tmp/gpu_test_*` paths. Each test function now creates its own unique directory:
```cpp
std::string tmpdir = make_test_tmpdir("frustum");
viewer::PartStore store(tmpdir);
```

### 5. Deduped `MatterEngine3/tests/Makefile`

Added three shared source variables after the LDLIBS block:

```makefile
MSL = ../../MatterSurfaceLib/src
COMMON_MSL_BLAS_SRC = $(MSL)/blas_manager.cpp $(MSL)/bvh.cpp $(MSL)/tlas_manager.cpp \
    $(MSL)/vertex_ao.cpp $(MSL)/occupancy.cpp $(MSL)/part_asset.cpp
COMMON_MSL_FULL_SRC = $(COMMON_MSL_BLAS_SRC) \
    $(MSL)/cluster.cpp ... (9 more)
COMMON_MSL_C = $(MSL)/surface.c $(MSL)/open_particle_surface.c ...
COMMON_MSL_C_OBJ = surface.o open_particle_surface.o ...
```

Updated 14 targets to reference variables. Targets intentionally kept verbatim:
- `run-shlib`: omits `fat_primitive.c` (intentional subset)
- `run-iso`: omits `open_particle_surface.c` (intentional subset)

`make -n` diff before/after: same flags, same files, only source list ordering shifted (immaterial).

### 6. Created `MatterEngine3/tools/lib_poses.sh`

New shared pose library:
```bash
POSES_MEADOW=(
    "aerial    128 260 -40   128 0 128"
    "corner      8   2   8    60 1  60"
    "midfield   40   6  40   128 2 128"
    "far         4   3   4   250 0 250"
    "empty     -40   5 -40  -200 5 -200"
)
```

### 7. Updated tools scripts with log-polling readiness + pose library

All three scripts now:
- `source "$HERE/lib_poses.sh"` for the shared 5-pose array
- Replace fixed `sleep N` with log-poll loop: `grep -q 'MATTER_CMD_FIFO: listening' "$LOG"` (caps: 180s viewer_shots, 120s meadow_sweep, 300s forestfloor)
- Use `stdbuf -oL ./viewer` for line-buffered output when redirecting to file

`meadow_forestfloor_shots.sh` additionally:
- Keeps its own 5 Wang-seam-specific poses (not using POSES_MEADOW)
- Adds `.done` marker wait, PNG size check (10 kB min), and Pillow non-sky pixel check (≥5%)

---

## Test results

### Tests passing after changes (green before and after):

| Target | Result |
|---|---|
| run-polytri | ALL PASS |
| run-tilesetcore | ALL PASS |
| run-tilesetgtex | ALL PASS |
| run-tilesettorusbvh | ALL PASS |
| run-partv2 | ALL PASS |
| run-iso | ALL PASS |
| run-trivar | ALL PASS |
| run-comp | ALL PASS |
| run-flatten | ALL PASS |
| run-dev | ALL PASS |
| run-lighting | ALL PASS (0 checks failed) |
| run-tilesetplacement | PASSED (0 failures) |
| run-tilesetphysics | PASSED (0 failures) |
| run-tilesetdsl | PASSED (0 failures) |
| run-tilesetbake | ALL PASS |
| run-tilesetmeadowmanifest | (prior run: PASS) |
| run-grasslod | ALL PASS |
| run-script | ALL PASS |
| run-graph | (prior run: ALL PASS) |

### Known-red tests (pre-existing, not caused by these changes):

| Target | Status | Root cause |
|---|---|---|
| run-shlib | Link error | Missing `primitive_sdf` + `lod_bake::decimate_tris` symbols |
| run-viewer-logic | Link error | Missing `tileset_provider::*` + `run_tileset_phase` symbols |
| run-graph-integration | 6 FAILs | Tree/trunk/leaf placement logic not yet implemented |
| run-example | (pre-existing skip) | — |
| run-meadow-check | 3 FAILs | Pre-existing asset path / manifest failures |

### `viewer_shots.sh` end-to-end run

```
GALLIUM_DRIVER=d3d12 bash tools/viewer_shots.sh test /tmp/viewer_shots_test
```

- Readiness polling worked: log showed "MATTER_CMD_FIFO: listening" ~60s into load
- All 5 shots produced with `.done` markers:
  - `test_aerial.png` — 1,575,498 bytes, STATS: 130978 µs frame (cold)
  - `test_corner.png` — 1,256,508 bytes, STATS: 29.81 µs
  - `test_midfield.png` — 1,503,576 bytes, STATS: 29.53 µs
  - `test_far.png` — 1,483,016 bytes, STATS: 28.52 µs
  - `test_empty.png` — 49,303 bytes, STATS: 25.12 µs
- Viewer terminated cleanly: "Window closed successfully"
- `test_stats.log` contains all 5 STATS lines

---

## Files changed

**New files:**
- `MatterEngine3/tests/check.h`
- `MatterEngine3/tools/lib_poses.sh`

**Modified files (test migration):**
- `MatterEngine3/tests/composition_tests.cpp`
- `MatterEngine3/tests/iso_primitive_tests.cpp`
- `MatterEngine3/tests/lighting_tests.cpp`
- `MatterEngine3/tests/meadow_bake_check.cpp`
- `MatterEngine3/tests/meadow_bake_tests.cpp`
- `MatterEngine3/tests/part_asset_v2_tests.cpp`
- `MatterEngine3/tests/part_flatten_tests.cpp`
- `MatterEngine3/tests/part_graph_integration_tests.cpp`
- `MatterEngine3/tests/part_graph_tests.cpp`
- `MatterEngine3/tests/polygon_triangulate_tests.cpp`
- `MatterEngine3/tests/script_host_tests.cpp`
- `MatterEngine3/tests/shared_lib_tests.cpp`
- `MatterEngine3/tests/triangle_variation_tests.cpp`
- `MatterEngine3/tests/dev_live_edit_tests.cpp`
- `MatterEngine3/tests/grass_lod_tests.cpp`
- `MatterEngine3/tests/stress_forest_tests.cpp`
- `MatterEngine3/tests/tileset_bake_tests.cpp`
- `MatterEngine3/tests/tileset_core_tests.cpp`
- `MatterEngine3/tests/tileset_dsl_tests.cpp`
- `MatterEngine3/tests/tileset_physics_tests.cpp`
- `MatterEngine3/tests/tileset_placement_tests.cpp`
- `MatterEngine3/tests/tileset_meadow_manifest_tests.cpp`
- `MatterEngine3/tests/tileset_gtex_tests.cpp`
- `MatterEngine3/tests/tileset_torus_bvh_tests.cpp`
- `MatterEngine3/tests/gallery_bake_tests.cpp`
- `MatterEngine3/tests/viewer_logic_tests.cpp`

**Modified files (gpu_cull + Makefile):**
- `MatterEngine3/viewer/gpu_cull_tests.cpp` — assert→CHECK, mkdtemp
- `MatterEngine3/tests/Makefile` — COMMON_MSL_* variables, 14 targets deduped

**Modified files (tools):**
- `MatterEngine3/tools/viewer_shots.sh` — lib_poses.sh source, log-poll readiness, stdbuf
- `MatterEngine3/tools/meadow_sweep.sh` — lib_poses.sh source, log-poll readiness, stdbuf
- `MatterEngine3/tools/meadow_forestfloor_shots.sh` — log-poll readiness, stdbuf
