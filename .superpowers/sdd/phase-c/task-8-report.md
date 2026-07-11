# Task 8 Report: ExplorerDemo skeleton

## What was built

Five new files in `ExplorerDemo/`:

- **`Makefile`** — kernel-only build: `main.cpp` + `camera_rig.cpp`, links `libmatter_engine3.a` + `libraylib.a` + `libbox3d.a` (box3d required transitively by tileset physics code in the kernel). Include path: `-I../MatterEngine3/include` only. Binary: `explorer`. Shader symlinks: `shaders → ../MatterSurfaceLib/shaders`, `shaders_gpu → ../MatterEngine3/shaders_gpu`.

- **`camera_rig.h` / `camera_rig.cpp`** — hand-rolled free camera with WASD/QE + mouse + composed gamepad input (left stick = move, right stick = look, triggers = up/down). Spawn at (408, 48, 408) facing -Z toward the mountain range. Yaw/pitch maintained explicitly; target rebuilt each frame.

- **`main.cpp`** — window + GL init, EngineContext + WorldSession setup (Meadow world, `cache_root="cache"`, embedded shaders), `request_bake()`, frame loop in order: `rig.update → set_bake_focus → tick → pump_gpu_jobs → poll_event → render → HUD`. Smoke mode: `EXPLORER_SMOKE="secs=<n>[,shot=<path>]"` — `explorer: ready` prints on first frame after BakeStarted, screenshot taken 3s before deadline, self-terminates.

- **`README.md`** — controls reference and usage.

**`build-all.sh`** modified: ExplorerDemo added to `SIMPLE_PROJECTS` array; warm-cache smoke test (15s, no shot, guarded by binary existence, `GALLIUM_DRIVER=d3d12`) added in the GPU test section.

## Deviations from brief

### 1. `libbox3d.a` required at link time
The brief says "link `libmatter_engine3.a` + raylib + `-lGL -lm -lpthread -ldl -lrt -lX11`". In practice, `libmatter_engine3.a` references box3d symbols (`b3CreateBody`, `b3World_Step`, etc.) from its tileset_settle.cpp object. The link fails without `$(BOX3D_DIR)/libbox3d.a`. Added to `LDLIBS`. This is the same pattern as MatterViewer. **No functional deviation.**

### 2. Screenshot timing: 3s before deadline (not 3 frames after BakeStarted)
The brief example takes a screenshot after a short settle. Initial code took it 3 frames after BakeStarted. On cold bake, BakeStarted fires within the first rendered frame before any tiles bake, producing a black frame. Changed to take screenshot 3s before the smoke deadline to maximize terrain visibility. **Rationale: more useful evidence of bake progress.**

### 3. No `.claire` misspelling path
A spurious file was accidentally created at `.claire/worktrees/...` during development — not committed.

## Test / smoke evidence

### Build
```
make -C ExplorerDemo  # exit 0; binary: explorer (5.1MB)
```
Box3d link error found and fixed in first rebuild.

### Cold smoke test (90s, fresh cache)
```
cd ExplorerDemo && GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=90,shot=/tmp/explorer_smoke.png" ./explorer
```
Output: `explorer: bake started`, `explorer: ready`, `explorer: screenshot written`, `explorer: smoke done (90.0s, 5387 frames)`. **exit=0**.

### Cold run bake-timing (200s run to confirm correctness)
```
[bake-timing] install=173859ms compose=8828ms publish=45596ms total=228284ms
```
The Meadow.build() JS install phase takes **173 seconds** on this machine (WSLg / D3D12 / Mesa 25.2). This is the JS interpreter executing 70,701+ `placeChild` calls and `heightAt` noise samples. Geometry cache only covers the mesh geometry — the install phase runs fresh every bake.

### Screenshot verdict

**Cold 90s run screenshot**: black frame with HUD text visible. Expected: the install phase takes 173s, so no terrain tiles bake within the 90s window. The brief says "investigate if black frame" — investigation complete: it is a Meadow.build() JS install phase bottleneck, not a code bug.

**200s cold run screenshot** (`/tmp/explorer_smoke_200s.png`): green meadow terrain tiles visible at center, mountain terrain at edges, scattered coarse tile assembly visible. HUD shows active instances and bake progress. **Valley IS visible.** Screenshot was taken at 197s into the run (3s before deadline).

**Warm 30s screenshot**: also black because the install phase (173s) runs on every bake regardless of cache. The cache only stores geometry; Meadow.build() is re-executed every session.

### BakePartDone events (200s run)
```
bake 1/0 Meadow        ← Meadow root part
bake 2/0 Grass         ← scatter variants
bake 1/2621            ← first of 2,621 terrain tiles
bake 2/2621 Grass
...
bake 28/2621           ← 28 terrain tiles visible at screenshot time
```
All events drain correctly. The demand-bake focus sort is working (camera-nearest tiles bake first).

### Warm rerun (build-all.sh `secs=15`)
The build-all.sh smoke test exits 0 cleanly (binary works, renders, self-terminates). The scene will be black in 15s because install phase takes 173s, but this is intentional: the test only verifies binary liveness, not visual completeness.

## Self-review

- Includes: `matter/*.h`, `raylib.h`, `raymath.h`, `camera_rig.h`, standard C++ only — no engine internals.
- Frame order matches MatterViewer: `set_bake_focus → tick → pump_gpu_jobs → poll_event → render`.
- `set_bake_focus` called every frame before `tick()` with camera position.
- `session.reset()` called before `CloseWindow()`.
- Smoke mode self-terminates cleanly (break from loop, not exit()).
- Gamepad axes guarded by `IsGamepadAvailable(0)`.
- Dead-zone applied to all gamepad axes (0.1 threshold).
- Resolver opts: `active_radius=400.0f, min_projected_size=0.0015f` (Meadow values from MatterViewer).
- No dead code. No extra features beyond the brief.
- `build-all.sh` smoke test guarded by `[ -x "ExplorerDemo/explorer" ]`.
- ExplorerDemo correctly appears in `SIMPLE_PROJECTS` so the build summary includes it.

## Concerns

**Install phase timing (major concern):** The Meadow.build() JS install phase takes ~173s on this machine. This means:
- The brief's expectation of "coarse valley assembling in the 90s cold smoke window" is not achievable with the current Meadow world + hardware.
- The brief's "warm rerun (secs=20) should show much more terrain immediately" is also not achievable because the install phase runs every bake.
- The "No black frame" requirement (Step 5) can only be met with a run ≥ 182s.

This is a Phase C pipeline characteristic. Options to address it (outside Task 8 scope):
1. Cache the install phase output separately from the geometry cache.
2. Use a smaller demo world with a faster install phase.
3. Accept the timing and update the brief's expectations.

The binary is correct. The visual evidence of correct rendering comes from the 200s cold run screenshot. The warm/cold black frames are a world-complexity artifact, not a code bug.

---

## Review Finding Fix: SPAWN_Y corrected to precomputed terrain height + 8

### Finding addressed
Review finding (task-8-review.md, Important): `camera_rig.cpp` `SPAWN_Y = 48.0f` deviates from spec's "heightAt+8" by 40m, producing bird's-eye spawn instead of near-ground immersive start.

### How the terrain height was computed

The world's terrain function lives in `MatterEngine3/shared-lib/terrain_noise.js`. The default seed is `20260709` (from `Meadow.js` `static params = { worldSeed: 20260709 }`). World size is `816.0` (51 tiles x 16 units). World center is `(408, 408)`.

Transcribed the JS `heightField(seed, worldSize).heightAt(x, z)` math into a Node.js script and evaluated:

```
seed (folded to 32-bit): 20260709
cx=408, cz=408  (world center, r=0)
R_MEADOW=130.56  R_FOOT=277.44
ampAt(408,408) = 6  (center is flat meadow; no foothills/mountain amplification)
heightAt(408, 408) = -0.0108694782642485
```

The terrain height at world center is **-0.011 units** (effectively 0.0). The world center sits at the very bottom of the meadow bowl where `r=0` from center, `mt=0`, and the 3-octave FBM plus detail octaves nearly cancel. `ampAt=6` with near-zero FBM value gives approximately zero world-space height.

**Precomputed spawn Y: `0.0 + 8.0 = 8.0f`**

### Code change

`ExplorerDemo/camera_rig.cpp`:
- Comment block updated to document the precomputed terrain height
- `SPAWN_Y` changed from `48.0f` to `8.0f`
- Inline comment: `// precomputed heightAt(408,408)~0 + 8 m above terrain`

### Smoke test

```
cd ExplorerDemo && GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=200,shot=/tmp/explorer_spawn_fix.png" ./explorer
```

Key output (cold parts cache run):
```
explorer: bake started
explorer: ready
explorer: screenshot written to /tmp/explorer_spawn_fix.png
explorer: smoke done (200.0s, 11537 frames)
exit=0
```

Note: A warm-cache run (second run after the first 200s cold run) crashed at GpuCuller slot 50 (SIGSEGV). This is a pre-existing bug in the GpuCuller that occurs when 50+ unique part geometries are loaded; with warm cache, tiles load 10x faster and hit the crash boundary within the 200s window. Cold cache (cleared `cache/parts/`) reproduces the original cold run behavior and exits cleanly. The crash is unrelated to the SPAWN_Y change.

### Screenshot verdict

`/tmp/explorer_spawn_fix.png`: Near-ground immersive meadow view. Camera at Y=8 (8m above terrain surface ~0) with slight downward pitch. Dense green grass geometry fills the frame in all directions -- camera is within the grass canopy. Rendered geometry visible: grass blades, two light-colored rocks. No solid color/black (not inside terrain). HUD visible top-left (FPS:50, inst:7523). Scene content confirms camera is above terrain at near-ground level, inside the meadow environment. PASS: camera is near ground, not bird's-eye aerial.

---

## GpuCuller 300s Regression Fix: Performance::Profiler data race

### Finding

The 300s cold smoke test that was added as the regression gate for the kInitialRegionCap=16 fix crashed with SIGSEGV at ~145 slots — well past the original slot-50 OOM boundary, but before the 300s deadline. The crash presented as a write fault inside `__merge_sort_with_buffer` (UlmmE_ comparator, initial publish sort) on the worker thread. Seven different hypotheses were ruled out by code analysis (NaN comparator, GL/CPU threading, BLASManager destructor GL calls, data races on manifest, SSBO sizing, cmd_template_ OOB, heap overflow in VBO data).

AddressSanitizer (`-fsanitize=address`) pinpointed the root cause as a **heap-use-after-free** in `Performance::Profiler::end_section()`:

- **Freed by T0 (GL thread)**: `pump_gpu_jobs` → publish GL job → `PartStore::get_or_load` → `PartStore::load_flat` → `BLASManager::register_triangles` → `ScopedTimer` dtor → `Profiler::end_section()` → `section_starts_.erase(it)` deletes the map node
- **Read by T8 (worker thread)**: `ensure_part_flattened` → `flatten_part_impl` → `BLASManager::register_triangles` → `ScopedTimer` ctor → `Profiler::begin_section()` → `section_starts_[name]` reads the freed node

Both threads used their own separate `BLASManager` instances but shared the `Performance::Profiler::instance()` singleton, whose `std::unordered_map<string, TimePoint> section_starts_` was accessed from both threads without any synchronization.

The crash manifested non-deterministically (slot counts: 102, 145, 296) because it depended on the GL thread's publish job and the worker thread's `ensure_part_flattened` racing on the profiler map.

### Fix

`MatterSurfaceLib/include/profiler.hpp`: added `mutable std::mutex mutex_` to `Performance::Profiler` and locked it in all mutating and reading public methods: `begin_frame`, `end_frame`, `begin_section`, `end_section`, `reset_stats`, `print_stats`, `get_frame_time_ms`, `get_section_time_ms`. In `begin_section` and `end_frame`, the timestamp is captured BEFORE acquiring the lock to keep timing accurate.

### 300s regression result

```
cd ExplorerDemo && GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=300,shot=/tmp/explorer_regioncap.png" ./explorer
```

```
explorer: bake started
explorer: ready
explorer: screenshot written to /tmp/explorer_regioncap.png
explorer: smoke done (300.0s, 9912 frames)
[bake-timing] install=135056ms compose=93ms publish=164872ms total=300022ms
Window closed successfully
exit=0
```

Part count: **1801 slots** (well past the original slot-50 OOM boundary and the 145-slot crash point).

### Screenshot verdict

`/tmp/explorer_regioncap.png`: Full Meadow Valley scene rendered — dense grass canopy at near-ground level (camera Y=8), scattered rocks, deciduous trees (scatter geometry), mountain range in background. Immersive near-ground view confirming both the SPAWN_Y=8 and the full 300s bake progress. PASS.

### Test suite results (post-fix)

- `make run-partv2`: All part_asset_v2 tests passed
- `make run-demandbake`: ALL PASS (a b c d e f g h i)
- `make run-releasepart`: 37/37 passed — ALL PASS
- `make run-gpucull`: 31/31 passed — ALL PASS

## Fix report: begin_frame timestamp + refineloop gate

### Change made

In `MatterSurfaceLib/include/profiler.hpp`, `begin_frame()` previously captured
its `Clock::now()` timestamp *after* acquiring `mutex_`, making it inconsistent
with `end_frame()` and `begin_section()`, which both capture their timestamps
*before* the lock. The fix introduces a local variable `t` that is populated
with `Clock::now()` before the `std::lock_guard` is constructed, then assigns
`frame_start_ = t` under the lock. This ensures lock-contention time can never
inflate the frame-start measurement.

Commit: `0f70ac4` — `fix(profiler): capture begin_frame timestamp before lock for consistency`

### Tests run

**1. Profiler coverage (run-releasepart)**
Command:
```
GALLIUM_DRIVER=d3d12 make -C MatterEngine3/tests run-releasepart
```
Result: `--- Results: 37/37 passed --- ALL PASS`

**2. Refine-worker path (run-refineloop)** — previously unclosed gate
Command:
```
GALLIUM_DRIVER=d3d12 make -C MatterEngine3/tests run-refineloop
```
Result:
```
  (a) refines_toward_focus:    PASS
  (b) eviction:                PASS
  (c) supersede_cancels_refine:PASS
ALL PASS
```

Both suites compiled cleanly against the updated header (profiler.hpp is
header-only; the tests' dep-tracking caused automatic recompilation).

## ASAN verification at HEAD (0f70ac4)
- Rebuilt libmatter_engine3.a with `EXTRA_CFLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1"`, linked ExplorerDemo/explorer_asan at HEAD; release lib restored afterwards.
- `GALLIUM_DRIVER=d3d12 ASAN_OPTIONS=detect_leaks=0 EXPLORER_SMOKE="secs=600,shot=..." ./explorer_asan`: exit 0, 600.1s, 13313 frames, ZERO ASAN reports (/tmp/explorer_asan2.log). Screenshot shows fully assembled meadow.
- Pre-fix ASAN binary (18:14, before 63ff68f) aborted ~4 min in: heap-use-after-free on Profiler section map — T0 GL thread (pump→load_flat→register_triangles→end_section free) vs T8 bake worker (publish_pipeline→flatten→register_triangles operator[] read). Confirms diagnosis; mutex fix closes it.
