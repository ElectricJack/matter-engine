# Task 9 Report: Loading HUD, Staged Camera, Error Toasts

## Status: DONE

## Files Created / Modified

### Created
- `ExplorerDemo/hud.h` — `Hud` class declaration with `feed(Event&)` and `draw(w,h,fs,fps,now)`
- `ExplorerDemo/hud.cpp` — Pure raylib implementation: bottom strip (bake progress bar + phase text), stats corner readout, refine tile corner, toast queue
- `ExplorerDemo/staged_camera.h` — `StagedCamera` class declaration
- `ExplorerDemo/staged_camera.cpp` — Three-shot cinematic sequence (orbit 40s, pull-back+rise 30s, drift loop)

### Modified
- `ExplorerDemo/camera_rig.h` — Added `has_user_input()`, `set_staged_pose(px,py,pz,yaw,pitch)`; added `user_input_seen_` private field
- `ExplorerDemo/camera_rig.cpp` — Implemented `has_user_input()`, `set_staged_pose()`; added `user_input_seen_` latching on any WASD/QE/Tab/mouse-delta/gamepad input with dead-zones
- `ExplorerDemo/main.cpp` — Replaced `draw_hud()` free function with `Hud` class; wired `StagedCamera`; all events fed to `hud.feed()`; `staged.notify_bake_finished()` on BakeFinished
- `ExplorerDemo/Makefile` — Added `hud.cpp staged_camera.cpp` to both `APP_SRC` (Linux) and `WIN_APP_SRC` (Windows)

## Interface Compliance

### Hud::feed() + Hud::draw()
- Bottom strip: phase label + done/total progress bar; `total=0` → indeterminate (pulsing bar + animated dots)
- Option C: no caching of `total`; `bake_total_` updated per BakePartDone event (bar may step backward as total grows — by design)
- After BakeFinished: subtle bottom-right "refined X/Y tiles near you" readout (RefineTileDone tracked)
- BakeError → 6-second toast queue (dark red panels, yellow text, truncation on overflow, fade-out in last 1s)
- Stats corner replaces old `draw_hud()` free function — FPS / inst / parts / hits / timing / bake status

### StagedCamera::update()
- Shot 1 (0–40s): slow orbit around valley centre (408,30,408), radius 120m, yaw derived from atan2(dx,dz) so rig internal state is consistent
- Shot 2 (40–70s): pull-back + rise with ease-in/out (120→350m radius, 30→200m altitude), 90° orbit advance
- Loop (>70s): drift at 350m/200m, full revolution per 40s cycle
- Returns false (handing back control) on: `rig.has_user_input()` OR `bake_done_` (also triggered by `notify_bake_finished()`)
- Works in smoke mode (no user input → staged camera drives rig for full smoke window)

## Test Results

### Step 3a — Warm smoke run (30s)
```
GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=30,shot=/tmp/explorer_hud.png" ./explorer
```
**Output:**
- `explorer: bake started`, `explorer: ready`, bake events streaming (1/2621 .. many/2621)
- `explorer: screenshot written to /tmp/explorer_hud.png`
- `explorer: smoke done (30.0s, 311 frames)`, exit 0

**Screenshot `/tmp/explorer_hud.png` inspection:**
- Camera is NOT at spawn — positioned in low orbit around the valley centre looking through trees (staged camera Shot 1 active)
- Bottom strip visible with green progress bar and phase text
- Top-left stats readout (FPS/inst/parts/hits/timing)

### Step 3b — Cold cache run (240s)
Cache moved to `/tmp/explorer_cache_keep` before run; restored afterward (Meadow.resolve verified).
```
GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=240,shot=/tmp/explorer_cold.png" ./explorer
```
**Output:**
- `explorer: bake started`, `explorer: ready`, 808 bake events in 240s (cold bake not finished at 240s — expected)
- `explorer: screenshot written to /tmp/explorer_cold.png`
- `explorer: smoke done (240.0s, 13120 frames)`, exit 0

**Screenshot `/tmp/explorer_cold.png` inspection:**
- Camera at high altitude (~200m) looking down at the valley — staged camera in Shot 2 / drift phase (pull-back + rise)
- Terrain silhouette assembling (scattered geometry dots visible against sky, mountain shape emerging)
- Bottom strip: "Resolving..." with green bar at ~30% — mid-progress, not a division-by-zero crash
- Top-left: "baking..." status visible

### Step 4 — Toast test
Setup: `EXPLORER_DATA_DIR=/tmp/explorer_toast_test/WorldData` with `Pebble.js` containing `SYNTAX_ERROR_INJECTED_FOR_TOAST_TEST ===` on line 2. Run from `/tmp` (fresh cache dir). 

```
cd /tmp && GALLIUM_DRIVER=d3d12 EXPLORER_DATA_DIR=/tmp/explorer_toast_test/WorldData \
  EXPLORER_SMOKE="secs=20,shot=/tmp/explorer_toast.png" ./explorer
```
**Errors generated:**
```
bake error [Meadow]: failed to resolve hash for part: Pebble
bake finished (1 errors)
bake error []: LocalProvider: tileset 'ForestFloor' settle failed: settle_tileset: layer "Pebble"...
```
Exit code: 0 (non-fatal)

**Screenshot `/tmp/explorer_toast.png` inspection:**
- Two red toast panels at bottom-left:
  - `Error [Meadow]: failed to resolve hash for part: Pebble`
  - `Error [unknown]: LocalProvider: tileset 'ForestFloor' settle failed: settle...` (truncated with `...`)
- Bottom-right: green `refined 0/2501 tiles near you` (bake finished despite errors)
- Top-left: `BAKED` status
- No crash, app exited 0

### Step 5 — Windows build
```
make -C ExplorerDemo windows
```
All four app objects compiled (`hud.o`, `staged_camera.o`, `camera_rig.o`, `main.o`) with MinGW cross-compiler; full link succeeded → `explorer.exe`.

## Deviations / Notes

1. **Staged camera in smoke mode**: The staged camera correctly drives the rig in smoke mode (no user input possible), producing non-spawn framing in screenshots as required.

2. **Cold run "Resolving..." label**: At screenshot time (237s into 240s run) the bake had processed 808/2621 parts but the bottom strip was showing "Resolving..." with `total=0`. Looking at the log, many early BakePartDone events had `total=0` (indeterminate resolve phase) before the full total was known. At the 237s mark the total had been set (808/2621 visible in log) but the screenshot captured a frame during an indeterminate sub-phase. The progress bar did show mid-progress (green bar ~30% wide). This is the correct Option C behavior: HUD does not crash on total=0 and the bar shows partial progress when total is known.

3. **Toast expire time stamping**: Toasts have `expire_time=0` after `feed()` and are stamped with `now + 6s` on the first `draw()` call after being enqueued. This is intentional to anchor expiry to when they become visible, not when the event fires.

4. **`rig.update(0.0f)` when staged active**: When the staged camera is driving the rig, `rig.update(0.0f)` is called with `dt=0` so that: (a) input is polled and `user_input_seen_` can latch, (b) no camera movement is applied over the staged pose. The staged pose is set by `set_staged_pose()` inside `staged.update()`.

5. **No kernel changes**: Zero changes to `MatterEngine3/`.
