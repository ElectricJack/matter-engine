---
id: 5b254c9b-eb03-33fe-6370-b2f4b1e6b795
world: AnimatedRigGallery
shots: 2
status: triaged
reported: 2026-07-28T06:41:10Z
kind: bug
severity: major
area: render/rt
title: Traced geometry stays in the bind pose while the raster mesh is skinned, so an animated part self-shadows in hard-edged dark patches
---

# Traced geometry stays in the bind pose while the raster mesh is skinned, so an animated part self-shadows in hard-edged dark patches

## Report

Could the wierd dark red color also be an artifact of the animated ray traceed mesh being out of alignment with the raster mesh? Do we animated both? I would like you to figure out if there is a way we can both simplify this and make it more correct.

## Repro

Launch from `MatterEditor/` (from a shell that has **not** exported
the MSYS2 UCRT64 PATH — env vars only reach a native exe through its
own prefix, and an MSYS bash swallows them):

```bash
MATTER_WORLD=AnimatedRigGallery MATTER_CAM="5.310,3.743,2.420,-26.075,-9.679,-18.552" ./build/windows/editor.exe
```

Simulation was in **Pause** for the first shot.

## Evidence

### 1. shot-1.png

![](shot-1.png)

- region: region (720x771)
- camera: `5.310,3.743,2.420,-26.075,-9.679,-18.552`
- sim: Pause @ 1.00x — 4 instances, 15 tris, 4 batches, 16.67 ms

### 2. shot-2.png

![](shot-2.png)

- region: region (759x742)
- camera: `5.310,3.743,2.420,-26.075,-9.679,-18.552`
- sim: Edit @ 1.00x — 4 instances, 15 tris, 4 batches, 16.65 ms

- `state.json` — per-shot camera/counts plus deep engine state
- `log-tail.txt` — console lines leading up to this report

## Acceptance

_TODO — the check that closes this. Prefer a headless `make -C MatterEngine3/tests run-*` target; fall back to a scripted capture (`MatterEngine3/tools/viewer_shots.sh`) plus what the pixels must show._
