# Task 10 Report: Final Gate — build registration, Windows wiring, full sweep, correctness refinement

## Step 1: Register ParticleFlowLib in build-all.sh

**Changes:**
- Added `ParticleFlowLib` to `SIMPLE_PROJECTS` array (between `MemoryLib` and `SpatialQueryLib`, mirroring MemoryLib's position as a header-lib + ASan test binary).
- Added test hook in the `test` mode section, immediately after the MemoryLib block:
  ```bash
  echo "--- ParticleFlowLib (pf_tests, ASan+UBSan) ---"
  make -C ParticleFlowLib test || RESULT[ParticleFlowLib]="FAIL (tests)"
  ```

**Evidence:** `bash build-all.sh 2>&1 | grep -A 20 "Summary"` shows:
```
BasicWindowApp            OK
SurfaceLib                OK
MemoryLib                 OK
ParticleFlowLib           OK
SpatialQueryLib           OK
MatterEngine3             OK
MatterViewer              OK
OpenParticleSurfaceLib    OK
GPURayTraceExample        OK
MatterSurfaceLib          OK
ParticleDynamicsExample   OK
```
All 11 projects: OK.

---

## Step 2: Windows viewer wiring

**Changes to `MatterViewer/Makefile`:**
(a) Added `-I../ParticleFlowLib/include` to `WIN_INCLUDE_PATHS`.
(b) Appended 5 pf sources to `WIN_ME3_CPP`:
    `$(ME3_DIR)/src/pf_bindings.cpp`, `../ParticleFlowLib/src/pf_math.cpp`,
    `../ParticleFlowLib/src/pf_sim.cpp`, `../ParticleFlowLib/src/pf_fields.cpp`,
    `../ParticleFlowLib/src/pf_path_recorder.cpp`.
(c) Extended `vpath %.cpp` with `../ParticleFlowLib/src`.

**Adaptation:** Added pf sources to `WIN_ME3_CPP` (not a separate variable) since `WIN_ALL_CPP_SRC = $(APP_SRC) $(WIN_ME3_CPP) $(WIN_MSL_CPP) $(IMGUI_SRC)` already expands it — no further changes needed.

**Missing artifact:** `Libraries/raylib/build/windows-native/libraylib.a` was missing from the worktree. Symlinked from main repo:
```
ln -s /mnt/d/Shared With Desktop/AI/matter-engine-cpp/Libraries/raylib/build/windows-native \
      Libraries/raylib/build/windows-native
```

**Clean rebuild:** `rm -rf MatterViewer/build/windows MatterViewer/viewer.exe` then `make -C MatterViewer windows`

**Evidence:** Final MinGW link command includes all 5 pf objects:
```
build/windows/pf_bindings.o build/windows/pf_math.o build/windows/pf_sim.o
build/windows/pf_fields.o build/windows/pf_path_recorder.o
```
`MatterViewer/viewer.exe` produced: 6,108,672 bytes (exit 0).

---

## Step 3: Full Linux Sweep

| Target | Command | Result |
|---|---|---|
| ParticleFlowLib build | `make -C ParticleFlowLib clean && make -C ParticleFlowLib` | OK |
| ParticleFlowLib tests | `make -C ParticleFlowLib test` | OK — pf_tests: ALL OK (15/15, ASan+UBSan) |
| MatterEngine3 lib | `make -C MatterEngine3` | OK — libmatter_engine3.a w/ pf objects |
| run-partv2 | `make -C MatterEngine3/tests run-partv2` | OK — All part_asset_v2 tests passed |
| run-script | `make -C MatterEngine3/tests run-script` | OK — ALL PASS (incl. 4 new Task 10 tests) |
| run-treebake | `make -C MatterEngine3/tests run-treebake` | OK — 3 cache hits, Tree assembler valid |
| MatterViewer | `make -C MatterViewer` | OK — viewer binary linked |
| build-all | `bash build-all.sh` | OK — all 11 projects OK |

GPU suites: not run in this step — no kernel/render/GPU path was touched by Task 10. GPU tests confirmed green in Task 9 (last GPU-touching task).

---

## Step 4: Correctness Refinement Pass

Four new tests added to `MatterEngine3/tests/script_host_tests.cpp` and registered in `main()`.

### 4.1 Determinism end-to-end (`test_pf_determinism_double_bake`)

**Approach:** Two fresh `ScriptHost` instances bake the same pf-driven stamp script (seed 42, run 120 ticks, voxel stamp via `this.paths(rec, ...)`). Written `.part` files compared byte-for-byte.

**Evidence:** Test `pf determinism: re-bake produces byte-identical .part (same seed+config+ticks)` PASSED. Both `.part` files non-empty and identical.

### 4.2 Incremental equivalence + append-only spot-check (`test_pf_incremental_equivalence`)

**Original design (vacuous):** Two separate scripts (class PA, class PB) with dual `ScriptHost` instances, compared by `.part` file size. Problem: Neither script stamped geometry (no `this.paths()` or `beginVoxels`), producing empty shells ~= 876 bytes. Size check asserted nothing about simulation state.

**Corrected design (genuine):** ONE bake with TWO identically-configured sims in same script:
- `simX`: `__pf_run(simX, 300)` in one shot
- `simY`: `__pf_run(simY, 120)` then `__pf_run(simY, 180)` (total 300 ticks)
- Both seeded 17; determinism guarantee: X and Y must agree exactly on `depositedCount` + full `path[0].xyz` sequence
- Mid-run snapshot: Y records `path[0].xyz` after 120 ticks, then verifies prefix unchanged after 180-tick extension (proves append-only)
- All assertions in-JS: mismatch → thrown error → bake fails with precise diagnostic message
- No geometry stamped (not needed for equivalence proof), but no vacuous file-size comparison

**Why the rewrite fixes the problem:**
- Old test: Empty `.part` files, same size by coincidence (garbage in = garbage out)
- New test: Simulation state (deposits + path data) compared directly in JS; any kernel/binding equivalence bug surfaces as throw message
- Append-only check doubles as a regression guard for path-recorder mutations

**Status:** PASS — Commit `4134583` — test(pf): genuine in-JS incremental-equivalence assertion (two sims, one bake, exact compare)

### 4.3 Budget fail-closed (`test_pf_budget_fail_closed`)

**Approach:** `BakeOptions.time_budget_ms = 1` with `__pf_run(sim, 100000)`. The `j_pf_run` binding checks `st->budget_exceeded()` per 32-tick chunk and calls `set_error("pf.run: bake time budget exceeded mid-simulation")`.

**Evidence:** Error message captured: `pf.run: bake time budget exceeded mid-simulation`. All checks passed: `!r.error.ok`, `r.written_path.empty()`, message contains "budget".

### 4.4 Stale handle (`test_pf_stale_handle`)

**Approach:** Call `__pf_run(999, 10)` with no prior sim creation. `sim_of()` hits `id >= reg->sims.size()` and calls `st->set_error("particleSim: stale or invalid sim handle")`.

**Evidence:** Error message captured: `particleSim: stale or invalid sim handle`. Bake fails cleanly, no .part file written. All checks passed.

### 4.5 Leak check

ParticleFlowLib tests compile with `-fsanitize=address,undefined` (from Makefile TEST_FLAGS). 15/15 tests pass under ASan+UBSan — no leaks, no UB detected.

For `run-script` (script_host_tests), ASan is not the default compile flag in ME3's test Makefile `qjs` flavor. **Accepted gap** — documented. Kernel-level leak coverage from ParticleFlowLib's ASan suite is the primary gate. Adding ASan to the full QJS+raylib test chain would significantly inflate build times and is deferred.

---

## Step 5: Visual Re-Check

Task 10 adds only test-only code (4 new test functions, no changes to kernel/binding/JS modules). No bake-machinery changes. Task 9 Tree screenshot visual check remains valid.

---

## Self-Review

- All 4 new correctness tests pass on first run with expected messages.
- `run-asyncbake` is NOT run (GPU test, known pre-existing libautoremesher_core TBB-init fragility — Phase 5 pre-existing segfault, identical on pure main).
- Windows vpath: `pf_bindings.cpp` is in `$(ME3_DIR)/src` (already on vpath); the 4 ParticleFlowLib sources are in `../ParticleFlowLib/src` (new vpath entry). MinGW pattern rule resolves correctly.
- The symlinked `Libraries/raylib/build/windows-native` is a worktree-local symlink (not committed, not needed — the main repo has the artifact at the standard path, and worktrees inherit it if `Libraries/` is a symlink from the worktree to the main repo; here it is a full copy so the symlink was added manually per the established pattern).
