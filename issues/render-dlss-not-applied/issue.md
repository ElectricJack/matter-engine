---
id: f9b7c74c-4a92-1aad-2254-ed0b69277cdb
world: PhysicsPlayground
shots: 3
status: fixed
reported: 2026-07-28T06:46:15Z
kind: bug
severity: major
area: render/dlss
title: DLSS presents the un-upscaled low-resolution image even when enabled
---

# DLSS presents the un-upscaled low-resolution image even when enabled

## Report

DLSS doesn't appear to work in streaming meadow in this build even when "enabled" we just see the low resolution render instead of the nice clean DLSS output.

## Repro

Launch from `MatterEditor/` (from a shell that has **not** exported
the MSYS2 UCRT64 PATH — env vars only reach a native exe through its
own prefix, and an MSYS bash swallows them):

```bash
MATTER_WORLD=PhysicsPlayground MATTER_CAM="20.260,28.154,-59.035,58.730,26.166,-70.036" ./build/windows/editor.exe
```

Simulation was in **Edit** for the first shot — press Play if the defect only appears in motion.

## Evidence

### 1. DLSS not working in this world - grainy

![DLSS not working in this world - grainy](shot-1.png)

- region: viewport (2385x927)
- camera: `20.260,28.154,-59.035,58.730,26.166,-70.036`
- sim: Edit @ 1.00x — 11916 instances, 951041 tris, 239 batches, 84.15 ms

### 2. It's working here

![It's working here](shot-2.png)

- region: region (2354x934)
- camera: `10.670,13.423,-23.623,-0.712,2.983,13.342`
- sim: Edit @ 1.00x — 144 instances, 1164495 tris, 144 batches, 31.73 ms

```bash
MATTER_WORLD=PhysicsPlayground MATTER_CAM="10.670,13.423,-23.623,-0.712,2.983,13.342" ./build/windows/editor.exe
```

### 3. Working here too

![Working here too](shot-3.png)

- region: region (1619x741)
- camera: `17.406,14.075,32.946,-0.176,1.411,-0.751`
- sim: Pause @ 1.00x — 10 instances, 1038 tris, 4 batches, 16.68 ms

```bash
MATTER_WORLD=PhysicsPlayground MATTER_CAM="17.406,14.075,32.946,-0.176,1.411,-0.751" ./build/windows/editor.exe
```

- `state.json` — per-shot camera/counts plus deep engine state
- `log-tail.txt` — console lines leading up to this report

## Root cause

The shot that shows the defect is the **StreamMeadow** one (`state.json` shot 1
records `world: StreamMeadow`; the frontmatter world is where the report was
*filed*, which was PhysicsPlayground). Telemetry shows DLSS was selected AND
active in every shot (`at_file_time.dlss: selected=quality active=quality,
reason=""`, watermark crisp at output resolution in all three) — so this is not
"DLSS off", it is "DLSS evaluating with its history discarded every frame":

1. Every streamed sector **publish** called `vk_temporal.invalidate()`
   (`MatterEngine3/src/matter_engine.cpp`, sector publish job), and every
   **eviction** did the same (`release_sector_entry`). A streaming world
   publishes/evicts nearly every frame while anything moves or settles.
2. Independently, `TemporalState::begin` escalated *any* instance id absent
   from the previous presented frame — i.e. every streamed-in sector's
   instances — to a **global** `frame.reset`
   (`MatterEngine3/src/render/vk_temporal.cpp`).

`frame.reset` feeds `sl::Constants::reset = eTrue`, so DLSS discarded its
accumulation almost every frame in StreamMeadow and its output degenerated to a
spatial-only upscale of the 1590x618 internal render — "we just see the low
resolution render". The same reset restarts GI/denoiser accumulation ("grainy").
Demo and PhysicsPlayground are static worlds: no publishes, no resets, clean
output — exactly what shots 2 and 3 show.

The fix keeps temporal history across streaming events: a newcomer instance
enters with `history_valid=false` (the per-instance mechanism that already
existed) while everything else keeps accumulating; global resets remain for
real discontinuities (world reload, camera cut, renderer reset, extent change).

## Acceptance

**Headless (the gate):**

```bash
make -C MatterEditor build/windows/vulkan_smoke_tests.exe TMP="C:/Users/webde/AppData/Local/Temp" TEMP="C:/Users/webde/AppData/Local/Temp"
cd MatterEditor && ./build/windows/vulkan_smoke_tests.exe
```

`run_vulkan_temporal_tests` (vulkan_smoke_tests.cpp) now asserts the streaming
contract directly:

- "streamed-in instance joins without resetting global history" — a frame that
  adds an unseen id must NOT set `frame.reset`; the survivor keeps
  `history_valid`, the newcomer starts without it. Fails on the pre-fix tree
  (the old code escalated the newcomer to a global reset).
- "instance returning after a presented clear frame starts fresh without a
  global reset" — same contract from the empty-set side.
- The four legitimate global cuts (resize, camera cut, world reload, renderer
  reset) still assert `reset` for exactly one frame each.

The gate is: **no FAIL lines from the temporal section**. The suite as a whole
had not been buildable since the .gtex-bake commit (missing includes and a
missing source in its Makefile target, `-Wextra` breakage in bake_observer.h —
all repaired here) and still carries ~10 pre-existing device-level failures in
subsystems this fix does not touch (material staging, grouped indirect,
culling stats, tileset readback, validation) plus three temporal Halton-delta
expectations that had gone stale against d5f97aa7's Y-down jitter convention
(also repaired here, with the arithmetic re-derived).

**Visual (what the pixels must show), DLSS-active replay:**

Needs a Streamline build (`./build-dlss.sh`, SDK at `/d/SDKs/...`) and the
recorded DLSS mode restored (replay otherwise forces Native):

```bash
cd MatterEditor && MATTER_REPLAY=../issues/render-dlss-not-applied/state.json \
  MATTER_REPLAY_SHOT=1 MATTER_DLSS_MODE=quality \
  MATTER_REPLAY_OUT=/tmp/after-dlss-1.png ./build/windows/editor.exe
```

The log must reach `DLSS selected=Quality active=Quality internal=1590x618
output=2385x927`, and the capture must show temporally accumulated (smooth)
grass/pebble edges rather than the raw internal-resolution aliasing of the
report's shot-1. Measured as mean gradient energy over the viewport crop
(aliasing proxy, watermark strip excluded): pre-fix 3.53, post-fix 2.63 on the
same warm cache — and the post-fix frame resolves *more* streamed detail, so
the drop understates the change. The report's defective shot-1 measures 3.94;
the healthy PhysicsPlayground shot-3, 0.55. Streaming residency is not
reproducible, so compare character, not pixels.
