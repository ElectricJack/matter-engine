# Task 4 Report: Final Gates + Visual Capture (Rock Realism)

**Date:** 2026-07-09
**Branch:** rock-realism (worktree)
**Status:** BLOCKED

---

## Step 1: Full Headless Gate

### run-iso + run-script — PASS

```
$ make -C MatterEngine3/tests run-iso run-script
./iso_primitive_tests
ALL PASS
./script_host_tests
Warning: No draw records to build TLAS from   [×7, pre-existing]
  test_eval_lod_budgets OK
ALL PASS
```

### run-meadow-check — PASS

```
$ make -C MatterEngine3/tests run-meadow-check
./meadow_bake_check
[install] 7s, baked 0 artifact(s), 279 hit(s)
[root] children=44896
[root] unique variants=276

ALL PASS
```

### run-meadow — FAILED

```
$ make -C MatterEngine3/tests run-meadow
./meadow_bake_tests
[modifier] part 7091f86c34f84595 region 0 group 8: retopo unavailable (built without autoremesher), skipped
[modifier] part 7095f46c34fc27a0 region 0 group 8: retopo unavailable (built without autoremesher), skipped
  rock tris=180142 pebble tris=120
FAIL: rock/pebble tri counts stay budget-sane
  grass tris=96
  treebranch leaf instances=90

1 FAILURE(S)
make: *** [Makefile:784: run-meadow] Error 1
```

### Root cause

`MatterEngine3/tests/meadow_bake_tests.cpp` line 141:
```cpp
CHECK(rt < 40000 && pt < 20000, "rock/pebble tri counts stay budget-sane");
```

The `rt < 40000` threshold was written for the old Rock.js which used `beginVoxels(0.15)` with 3–5 blobs and 1–2 cuts. Task 3 rewrote Rock.js to use `beginVoxels(0.10)` (finer voxel grid, ~3.4× more voxels by volume) with 4–7 blobs and 5–9 cuts. The new Rock generates **180142 tris** — ~4.5× over the old threshold.

Task 3's gate was only `run-meadow-check` (passes) and `run-script` (passes). The budget sanity check in `meadow_bake_tests.cpp` was not updated to match the new Rock.js, leaving `run-meadow` broken.

The `retopo unavailable` messages are pre-existing (autoremesher not linked in test builds, expected per all prior task reports).

---

## Step 2: Linux Viewer Screenshots

Not attempted — Step 1 gate failure halts execution per task instructions.

Additional note: `MatterViewer/tools/viewer_shots.sh` does not exist on this branch or worktree. The shots tooling would need to be created or the viewer driven directly via `MATTER_CMD_FIFO`.

---

## Step 3: Windows Clean Rebuild

Not attempted — Step 1 gate failure halts execution per task instructions.

Windows obj layout (for when this gate is unblocked): Windows objects are in `MatterViewer/build/windows/`. The Windows build does direct-source compile (no separate ME3 Windows kernel obj dir). Clean procedure: `rm -rf MatterViewer/build/windows/` then `make -C MatterViewer windows`.

---

## Required Fix to Unblock

In `MatterEngine3/tests/meadow_bake_tests.cpp`, update line 141:

```cpp
// Old (budget for old 0.15-voxel Rock.js):
CHECK(rt < 40000 && pt < 20000, "rock/pebble tri counts stay budget-sane");

// New — measured rock tris ~180k at 0.10 voxel spacing; 250k gives headroom:
CHECK(rt < 250000 && pt < 20000, "rock/pebble tri counts stay budget-sane");
```

After that one-line fix, re-run `make -C MatterEngine3/tests run-iso run-script run-meadow-check run-meadow` to confirm all PASS, then proceed with viewer screenshots and Windows rebuild.

---

## Task 4b: Decimate via retopo target_ratio + drop budget CHECK

**Date:** 2026-07-09
**Executed by:** Claude Sonnet 4.6

### Change 1 — meadow_bake_tests.cpp
Removed `CHECK(rt < 40000 && pt < 20000, "rock/pebble tri counts stay budget-sane")` (line 141).
Kept printf, `CHECK(rt > 200, ...)`, and `CHECK(pt > 50, ...)`.

### Change 2 — Rock.js
Set `target_ratio: 1.0 → 0.35` in the retopo modifier.

### Retopo status — BLOCKED

All test runs produced:
```
[modifier] part ... region 0 group 8: retopo unavailable (built without autoremesher), skipped
  rock tris=180142 pebble tris=120
```

Retopo is compiled out in the headless test build. `target_ratio: 0.35` is the correct value and will fire in viewer/Windows builds where autoremesher is linked, but no tri reduction is observable in this environment.

### Ratios tried

| target_ratio | rock tris | note |
|---|---|---|
| 0.35 | 180142 | retopo skipped — autoremesher unavailable |

Cannot iterate further; decimation is a no-op in the test build.

### Gate results

| Gate | Result |
|---|---|
| run-meadow | PASS (budget CHECK removed) |
| run-script | PASS |
| run-meadow-check | PASS |

### Commit

`a6e031e` — tune(rock): drop stale rock/pebble tri budget CHECK; set retopo target_ratio 0.35
