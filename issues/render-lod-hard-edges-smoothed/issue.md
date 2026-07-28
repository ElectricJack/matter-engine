---
id: ef7053be-b76d-4289-cf3e-869a318a856c
world: LightingGarden
shots: 2
status: triaged
reported: 2026-07-28T06:51:01Z
kind: bug
severity: major
area: render/lod
title: Coarse LOD rungs smooth across hard edges, so boxes shade like blobs when they pop down a rung
---

# Coarse LOD rungs smooth across hard edges, so boxes shade like blobs when they pop down a rung

## Report

Some meshes shouldn't be getting smooth normals at lower LOD, the cubes here look janky when they pop to lower lod

## Repro

Launch from `MatterEditor/` (from a shell that has **not** exported
the MSYS2 UCRT64 PATH — env vars only reach a native exe through its
own prefix, and an MSYS bash swallows them):

```bash
MATTER_WORLD=LightingGarden MATTER_CAM="25.071,13.975,-6.718,-8.176,-6.816,1.493" ./build/windows/editor.exe
```

Simulation was in **Edit** for the first shot — press Play if the defect only appears in motion.

## Evidence

### 1. shot-1.png

![](shot-1.png)

- region: region (1692x923)
- camera: `25.071,13.975,-6.718,-8.176,-6.816,1.493`
- sim: Edit @ 1.00x — 8 instances, 61738 tris, 8 batches, 16.67 ms

### 2. Broken normals on cubes

![Broken normals on cubes](shot-2.png)

- region: region (1148x710)
- camera: `39.540,22.976,-10.234,6.458,2.048,-1.712`
- sim: Edit @ 1.00x — 8 instances, 10286 tris, 8 batches, 16.67 ms

```bash
MATTER_WORLD=LightingGarden MATTER_CAM="39.540,22.976,-10.234,6.458,2.048,-1.712" ./build/windows/editor.exe
```

- `state.json` — per-shot camera/counts plus deep engine state
- `log-tail.txt` — console lines leading up to this report

## Acceptance

_TODO — the check that closes this. Prefer a headless `make -C MatterEngine3/tests run-*` target; fall back to a scripted capture (`MatterEngine3/tools/viewer_shots.sh`) plus what the pixels must show._
