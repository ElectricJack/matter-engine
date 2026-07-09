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
