# Task 10 Report: Escape Menu + New-Seed Flow

## What Was Built

### New files
- `ExplorerDemo/menu.h` — `Menu` class: ESC/gamepad Start toggles; entries Resume/New seed/Quit; raylib-drawn overlay; pauses camera input, world keeps rendering behind
- `ExplorerDemo/menu.cpp` — Implementation: navigation (real + synthetic keys), New-seed action (`session.regenerate(random_device 64-bit)`), Quit action, pure raylib drawing

### Modified files
- `ExplorerDemo/staged_camera.h` — Added `reset()` method: clears `elapsed_`, `user_taken_`, `bake_done_`; called on New-seed to re-arm the cinematic sequence
- `ExplorerDemo/main.cpp` — Wired `Menu` and synthetic key injection:
  - `SetExitKey(KEY_NULL)` added at init (disables raylib's default ESC→close so ESC opens our menu instead)
  - `keys=<csv>` parsing added to `SmokeOpts` (`parse_keys_csv()` helper, `SyntheticKey` struct)
  - Per-frame: fire timed synthetic keys, toggle/open menu on ESC/gamepad-Start, call `menu.update()`, call `menu.draw()` inside BeginDrawing/EndDrawing
  - On New-seed: reset `bake_done`/`bake_started` local flags when staged camera re-arms
- `ExplorerDemo/Makefile` — Added `menu.cpp` to both `APP_SRC` (Linux) and `WIN_APP_SRC` (Windows)

## Commands Run and Results

### Build — Linux
```
make -C ExplorerDemo explorer
```
Result: clean, zero warnings, zero errors.

### Build — Windows cross-compile gate
```
make -C ExplorerDemo windows
```
Result: `explorer.exe` produced, zero errors. (Both Linux and Windows builds verified after SetExitKey fix.)

### Seed 1 reference run (45s warm)
```
GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=45,shot=/tmp/explorer_seed1.png" ./explorer
```
- `explorer: bake started` ✓
- `explorer: screenshot written to /tmp/explorer_seed1.png` ✓
- `explorer: smoke done (45.0s)` ✓

### New-seed verification run (700s)
```
GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=700,shot=/tmp/explorer_seed2.png,keys=esc@60,down@61,enter@62" ./explorer
```
Key observations from log:
- `explorer: bake started` (first bake) ✓
- `explorer: synthetic key 256 at 60.0s` (ESC→menu opens) ✓
- `explorer: synthetic key 264 at 61.0s` (DOWN→move to New seed) ✓
- `explorer: synthetic key 257 at 62.0s` (ENTER→select New seed) ✓
- `explorer: new seed <uint64>` ✓
- `explorer: bake started` (second bake, from regenerate) ✓
- `bake finished (0 errors)` ✓
- `explorer: screenshot written to /tmp/explorer_seed2.png` ✓
- `explorer: smoke done (700.1s, 29322 frames)` ✓

Second bake timing: `install=178s compose=19s publish=213s total=430s` (cold bake, different seed → resolve cache miss expected per spec).

### Screenshot comparison
- `explorer_seed1.png`: Ground-level dense-tree close-up (staged camera shot 1, low orbit), dark foliage in foreground
- `explorer_seed2.png`: High-altitude view of a hilltop with trees and green grass; different vegetation density and terrain layout — confirms different world seed

## Timing Note / Deviation from Brief

The brief stated "60s figure is stale" and specified `secs=420` with instruction to "bump if 420s proves too short." The second bake (cold, new seed) took ~430s wall-clock (install=178s background + compose=19s background + publish=213s wall-clock on GPU rate-limited at 4ms/frame). Starting at t=62 (when menu New-seed is selected), the second bake finishes at t≈492. With `secs=420`, the smoke timer fired before the second BakeFinished. Used **secs=700** to provide margin.

## Self-Review Findings

1. **SetExitKey(KEY_NULL)**: Added after discovering that real ESC press would both toggle the menu AND trigger `WindowShouldClose()` via raylib's default ESC→close. Without this, pressing ESC to open the menu while in normal play would also close the window. Fixed proactively.

2. **`bake_done`/`bake_started` reset**: After New-seed, local flags reset via `!staged.user_has_control()` check inside the `if (menu.is_open())` block. This executes once after `menu.update()` closes the menu (the block was entered when menu was open; the check runs after `menu.update()` closes it). Correct.

3. **Staged camera re-arm**: `staged.reset()` called in `menu.update()` before `close()`. The `user_has_control()` = false after reset, allowing the cinematic sequence to restart. In smoke mode, no real input sets `user_input_seen_` in the rig, so the re-arm works correctly.

4. **Synthetic ESC open vs. close**: When menu is closed and synthetic ESC fires, the code takes the `else if (synth_key == KEY_ESCAPE)` branch and calls `menu.open()`. When menu is open and synthetic ESC fires, `menu.update()` is called with `key_esc=true`, which calls `close()` (Resume semantics). Both paths are correct.

5. **Camera paused during menu**: When `menu.is_open()`, `rig.update()` is not called (no camera movement). `staged.update()` receives `dt=0` to suppress elapsed accumulation.

6. **`parse_keys_csv` edge cases**: The parser correctly stops at a token containing `=` (next k=v option). The `keys=` value is comma-separated with `name@seconds` format. Supported: `esc`→KEY_ESCAPE(256), `up`→KEY_UP(265), `down`→KEY_DOWN(264), `enter`→KEY_ENTER(257).

## Deviations from Brief

- **secs=700 used instead of secs=420**: The brief said to bump if 420s is too short. The cold new-seed bake takes ~430s wall-clock starting from t=62, so 700s is needed. The verification command in the brief's Step 2 should be updated to reflect secs=700.
- **Second BakeFinished IS emitted**: The `bake finished (0 errors)` log line confirms BakeFinished event was polled. The event is polled correctly after the second bake completes at t≈492.
- **SetExitKey(KEY_NULL)**: Not mentioned in brief, added as necessary correctness fix so ESC works as menu toggle without closing the window.

---

## Fix: C1/I1

**Review findings addressed:** C1 (real ESC double-toggle) and I1 (synthetic-key-only coverage).

### Root cause confirmed

The original code called `IsKeyPressed(KEY_ESCAPE)` in two places per frame:
1. `main.cpp` ~line 312: `if (IsKeyPressed(KEY_ESCAPE) && synth_key != KEY_ESCAPE) menu.toggle()`
2. `menu.cpp` ~line 56: `bool key_esc = IsKeyPressed(KEY_ESCAPE);` inside `update()`

raylib's `IsKeyPressed` is stateless (checks `prev==0 && cur==1` each call) so it returns `true` on both reads in the same frame. When the menu was closed and ESC was pressed: `toggle()` opened the menu, then `menu.update()` immediately read ESC as true again and called `close()`. The menu never appeared for real keyboard input.

### Fix applied

**Single decision point:** introduced `FrameInput` struct in `menu.h` with four bool flags (`esc`, `up`, `down`, `enter`). All input is collected into this struct once per frame in `main.cpp`, in order:
1. Real keyboard (`IsKeyPressed` — called once each)
2. Gamepad buttons (`IsGamepadButtonPressed` — called once each)
3. Synthetic keys (smoke-mode: OR into flags)

`menu.update()` signature changed from `(session, staged, int synthetic_key, quit_requested)` to `(session, staged, const FrameInput&, quit_requested)`. The method body contains zero `IsKeyPressed` / `IsGamepadButtonPressed` calls.

**Toggle logic:** in `main.cpp`, the ESC open/close decision is:
```cpp
if (finput.esc && !menu.is_open()) {
    menu.open();
    finput.esc = false;  // consumed by open; not also passed to close in update()
}
if (menu.is_open()) {
    menu.update(*session, staged, finput, quit_from_menu);
    ...
}
```
When menu is closed and ESC fires: `menu.open()` is called, `finput.esc` is cleared, `menu.update()` sees `esc=false` → no immediate close. When menu is open and ESC fires: `menu.open()` branch not taken (menu already open), `menu.update()` sees `esc=true` → calls `close()`. The double-toggle is impossible by construction.

**Removed:** the old special-case synthetic guard (`synth_key != KEY_ESCAPE`) and the `else if (synth_key == KEY_ESCAPE) { menu.open(); }` branch. Both are superseded by the unified `FrameInput` path.

**Added:** `menu.open()` and `menu.close()` now print `"menu: open"` / `"menu: closed"` to stdout, making state transitions observable in smoke logs.

**Files changed:**
- `ExplorerDemo/menu.h`: added `FrameInput` struct; changed `update()` signature; added `open()`/`close()` log prints; removed `toggle()`; added `#include <cstdio>`.
- `ExplorerDemo/menu.cpp`: `update()` now takes `const FrameInput&`; removed all `IsKeyPressed`/`IsGamepadButtonPressed` calls; consumes only `input.esc/up/down/enter`.
- `ExplorerDemo/main.cpp`: single decision point block replaces old dual-read toggle + synthetic-special-case; `FrameInput` built from real keys + gamepad + synthetic; passed to `menu.update()`.

### Commands run and output evidence

**Build — Linux:**
```
make -C ExplorerDemo
```
Result: clean, zero warnings, zero errors.

**Build — Windows cross-compile:**
```
make -C ExplorerDemo windows
```
Result: `explorer.exe` (8,074,729 bytes, timestamp 22:56 Jul 9), zero errors.

**Menu open/close smoke — GALLIUM_DRIVER=d3d12:**
```
GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=45,shot=/tmp/explorer_menufix.png,keys=esc@10,down@12,esc@20" ./explorer > /tmp/explorer_menufix.log 2>&1
```

Log excerpt (key lines from `/tmp/explorer_menufix.log`):
```
GPU cull path: enabled (GL 4.6 ok)
explorer: bake started
explorer: ready
explorer: synthetic key 256 at 10.0s
menu: open
explorer: synthetic key 264 at 12.0s
explorer: synthetic key 256 at 20.0s
menu: closed
explorer: screenshot written to /tmp/explorer_menufix.png
explorer: smoke done (45.0s, 2693 frames)
```

Assertions verified:
- `menu: open` appears after `synthetic key 256 at 10.0s` ✓ (first ESC opens)
- `menu: closed` appears after `synthetic key 256 at 20.0s` ✓ (second ESC closes — the C1 toggle path)
- No spurious `menu: closed` immediately after `menu: open` ✓ (double-toggle eliminated)
- `smoke done (45.0s)` ✓ (self-terminates)
- Screenshot captured at 1280×720, HUD visible at 60 FPS ✓

The synthetic-key path and real-keyboard path are now the same code path from `FrameInput` onward, so this smoke coverage is valid evidence for real-keyboard correctness.
