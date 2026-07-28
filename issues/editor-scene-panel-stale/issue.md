---
id: 56edd5b3-988d-1e9c-5dab-c0eb4e908c6c
world: LightingGarden
shots: 2
status: triaged
reported: 2026-07-28T06:48:52Z
kind: bug
severity: minor
area: editor/scene-panel
title: Scene panel keeps showing the previous world's tree after a new world loads
---

# Scene panel keeps showing the previous world's tree after a new world loads

## Report

Scene window often doesn't update when new worlds are loaded.

## Repro

Launch from `MatterEditor/` (from a shell that has **not** exported
the MSYS2 UCRT64 PATH — env vars only reach a native exe through its
own prefix, and an MSYS bash swallows them):

```bash
MATTER_WORLD=LightingGarden MATTER_CAM="17.406,14.075,32.946,-0.176,1.411,-0.751" ./build/windows/editor.exe
```

Simulation was in **Edit** for the first shot — press Play if the defect only appears in motion.

## Evidence

### 1. shot-1.png

![](shot-1.png)

- region: viewport (2385x927)
- camera: `17.406,14.075,32.946,-0.176,1.411,-0.751`
- sim: Edit @ 1.00x — 8 instances, 11968 tris, 8 batches, 16.79 ms

### 2. shot-3.png

![](shot-3.png)

- region: region (2093x1115)
- camera: `17.406,14.075,32.946,-0.176,1.411,-0.751`
- sim: Edit @ 1.00x — 8 instances, 11968 tris, 8 batches, 16.69 ms

- `state.json` — per-shot camera/counts plus deep engine state
- `log-tail.txt` — console lines leading up to this report

## Acceptance

_TODO — the check that closes this. Prefer a headless `make -C MatterEngine3/tests run-*` target; fall back to a scripted capture (`MatterEngine3/tools/viewer_shots.sh`) plus what the pixels must show._
