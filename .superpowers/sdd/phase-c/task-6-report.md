# Task 6 Report — Camera-driven refine loop with part eviction + RefineTileDone events

## Summary

Task 6 implements the camera-driven refine loop: after the initial world publish, the worker
thread idles on a 50 ms timed pop and — when no command is pending — upgrades the nearest
coarse Terrain tile to full-res and evicts stale full-res tiles beyond the refine radius.

## What was implemented

### `RefineTileDone` event (events.h)
Appended to `EventType` enum. Fields: `module="Terrain"`, `phase="refine"`, `done=<full_count>`,
`total=<tile_count>`. Emitted after each tile upgrade or eviction.

### `CommandQueue::pop_wait` (async_bake.h / async_bake.cpp)
Timed pop using `cv_.wait_until`. Returns `false` with `out_timed_out=true` on timeout;
`false` (no `out_timed_out`) on shutdown. The worker uses this to interleave idle refine steps
with command processing.

### `RefineController` accessors (refine_controller.h)
Added `tile_at(uint32_t tile_idx)` and `tile_index_of(const TileRecord* tr)` for the execute
step to go from a `TileRecord*` returned by `next()` to its stable integer index.

### `worker_loop` refine mode (matter_engine.cpp)
When `refine_ctrl` is live (built after publish_pipeline step 10), the loop switches from
blocking `pop()` to `pop_wait(50 ms)`:
- **Timeout** → calls `execute_refine_step()`.
- **Command received** → clears refine state on BakeAll/Reload; processes command normally.
- **Shutdown** → clears refine state, returns.

### RefineController build from `bake_plan` (matter_engine.cpp, publish_pipeline step 10)
Builds `GraphNode` list from `ir_.bake_plan` (one entry per resolved_hash, not per module
name), using `part_graph::params_to_json(kv.second.params)` for canonical JSON. This correctly
captures all `N_coarse + N_full` Terrain variants (9+9 in MiniValley, 2601+2601 in Meadow).
Builds `InstanceRef` list from `state.manifest().instances`. Calls `refine_ctrl->build(nodes, refs)`.

### Tail-sort for ref-streamed entries (publish_pipeline, carried-in follow-up a)
`snap_focus` hoisted out of inner block scope; after each ref-streamed tail batch is appended
to `publish_order`, the new entries are sorted ascending by distance from focus.

### Focus y-projection (execute_refine_step, carried-in follow-up b)
Projects `focus[1]=0` before passing to `RefineController::next()` and `evict_beyond()`, since
tile `pos[1]` is always 0 and 3D distance with a nonzero camera y would artificially inflate
tile distances.

### `execute_refine_step` flow
1. `evict_beyond(focus_yz0, refine_radius)` → for each eviction: post GpuJob to
   (a) swap `manifest.instances[idx].part_hash` coarse→full, (b) release full part from
   `GpuCuller` and `PartStore`, (c) apply delta, (d) mark `Coarse`, (e) emit `RefineTileDone`.
2. `next(focus_yz0, &tr)` — highest-priority Coarse tile.
3. `ensure_part_baked(full_hash)` + `ensure_part_flattened(full_hash)` on worker thread.
4. Post GpuJob: `store->get_or_load(full_hash)`, `gpu_culler.ensure_part(full_hash)`, swap
   manifest hash full→coarse, `state.apply(delta)`, `mark(Full)`, emit `RefineTileDone`.
5. On bake failure: log error, `mark(Coarse)`, return.

### `MATTER_REFINE_RADIUS` test seam
`open_world()` reads the env var once; default 160.0f.

### `refine_loop_tests.cpp` (new, gpu flavor)
MiniValley 3×3 sandbox with parametric `Terrain.js` (coarse/full via `res` param) and
`MiniValley.js` root (places 9 coarse tiles). Three test cases:

- **(a) refines_toward_focus** — bake world, pump refine loop, assert 9 `RefineTileDone`
  events, `done_max==9`, `total==9`.
- **(b) eviction** — refine one tile, then move focus far away and reduce radius to 0,
  assert eviction event fires (`done` drops to 0).
- **(c) supersede_cancels_refine** — trigger `reload()` after first RefineTileDone,
  assert new `BakeStarted` received and RefineController re-built.

### `async_bake_tests.cpp`
Extended `ev_type_name()` with `RefineTileDone` case.

### `MatterEngine3/tests/Makefile`
Added `refine_loop_tests.cpp` to `gpu_CPP_SRCS`, `../src/refine_controller.cpp` to
`GPU_RENDER_CPP`, and `refine-loop-tests` / `run-refineloop` targets.

### `valley_layout_tests.cpp`
Added case **(d) phase-2 refinement** inside the `MATTER_VALLEY_FULL_BAKE=1` gate:
warm-bake Meadow, set focus to origin, drive refine loop for up to 30s, assert at least one
`RefineTileDone` event with `module=Terrain`, `phase=refine`, `done>=1`, `total==2601`.

## Gate results

```
run-refineloop:  (a) PASS  (b) PASS  (c) PASS — ALL PASS
run-asyncbake:   ALL PASS
run-releasepart: 37/37 passed — ALL PASS
run-demandbake:  (a–i) PASS — ALL PASS
```

## Key decisions

- **bake_plan over graph_snapshot** — snapshot keyed by module name collapses all Terrain
  variants to 1 entry; bake_plan keyed by resolved_hash captures all N variants.
- **focus y=0 projection** — avoids spurious distance inflation from camera height;
  documented in refine_controller.h comment.
- **50 ms pop_wait** — short enough for a 20fps refine budget, long enough to avoid spin-burn.
- **GpuJob for every swap** — all manifest mutations happen on the GL thread; worker only
  calls demand-bake primitives (CPU-safe).

---

## Fix round (review I1–I4)

### I1 — Swap-confirmed tile state

**Problem:** `mark(Full)` ran unconditionally right after `gpu_jobs.post()`, before the GL job
executed. If `get_or_load` failed inside the job, the tile was permanently stuck in Full state
and never retried.

**Fix:** Introduced `PendingUpgrade` struct and `refine_pending_upgrades_` vector on `Impl`.
Each posted upgrade job receives a `shared_ptr<atomic<int>>` result slot (0=in-flight,
1=success, 2=failure). The GL job writes the result; the worker drains pending results at the
**start** of each `execute_refine_step` call (step 0). Worker calls `mark(Full)` on success and
`mark(Coarse)+log(stderr)` on failure. Worker owns all `mark()` calls — no GL-thread
controller access, no data race.

`ensure_part_flattened`'s return value is now also checked (was silently discarded); flatten
failure marks Coarse and returns early, same as bake failure.

`refine_pending_upgrades_` is cleared wherever `refine_ctrl` is reset (Shutdown, BakeAll,
Reload supersession) to avoid stale result slots referencing a dead controller.

### I2 — Focus-order assertion (discrimination evidence)

**Problem:** Test (a) checked `ev.phase == "refine"` on the first RefineTileDone but did not
verify WHICH tile was refined first. A bug that ignored focus order (e.g., inverting the
comparator in `RefineController::next` to pick the farthest tile) would silently pass.

**Fix — test schema:** The original `MiniValley.js` passed a transform matrix as the third
argument to `placeChild`, which the DSL binding silently ignores (it only reads module + params).
All 9 tiles were placed at origin. Changed to use `pushMatrix/translate/placeChild/popMatrix`
(matching the Meadow.js pattern), so each coarse tile carries its true world-space origin.
World manifest changed from `MiniValley` to `MiniValley expand` so coarse tiles become separate
manifest instances (giving RefineController per-tile positions).

**Fix — event fields:** Added `tile_tx` and `tile_tz` to `Event` (append-only), populated in
`RefineTileDone` events for both upgrade and eviction paths. Added `tile_tx` and `tile_tz` to
`TileRecord` (populated from the map key in `RefineController::build`).

**Fix — assertion:** Test (a) now asserts `ev.tile_tx == 2 && ev.tile_tz == 2` on the first
RefineTileDone. Focus is at (25,0,25); tile (2,2) center is (25,0,25) — distance 0, nearest
tile. The assertion correctly fails when the `RefineController::next` comparator is inverted
(farthest-first) — first tile becomes (0,0) or (0,1) instead of (2,2).

**Discrimination evidence:** With the fix applied and test running, output confirmed:
```
first RefineTileDone: ... tile_tx=2 tile_tz=2 (expect 2,2)  → PASS
```
Without the focus-order assertion (original code), the test would have passed even with
tile (0,0) refined first.

### I3 — Null-hash eviction guard

**Problem:** `evict_beyond` could return tiles with `full_hash==0` (tiles short-circuited with
`mark(Full)` in step 2 when `full_hash==0`). The eviction job would call
`gpu_culler.release_part(0)` and `store->release(0)` — safe no-ops today but fragile against
future refactors.

**Fix:** One-line guard at the start of the eviction loop: if `full_hash_e == 0`, mark Coarse
(silently revert the permanent-done state) and `continue`, skipping the GpuJob. Comment
explains the guard.

### I4 — `run-refineloop` missing `GALLIUM_DRIVER=d3d12`

**Problem:** `MatterEngine3/tests/Makefile:run-refineloop` ran `./refine_loop_tests` without
`GALLIUM_DRIVER=d3d12`. Every other gpu-flavor run target in the file sets this env.

**Fix:** Changed `./refine_loop_tests` to `GALLIUM_DRIVER=d3d12 ./refine_loop_tests`, matching
`run-releasepart`, `run-gpucull`, etc.

### Test results after fix

```
make -C MatterEngine3/tests run-refineloop:
  (a) refines_toward_focus:    PASS
  (b) eviction:                PASS
  (c) supersede_cancels_refine:PASS
  ALL PASS

make -C MatterEngine3/tests run-demandbake:
  (a–i) ALL PASS

make -C MatterEngine3/tests run-asyncbake:
  ALL PASS
```
