---
id: 79555aa2-5a5e-8299-7c7e-bb19ab7481f9
world: PhysicsPlayground
shots: 1
status: triaged
reported: 2026-07-28T06:47:37Z
kind: bug
severity: minor
area: editor/part-workbench
title: Open in Workbench, Reveal and Go do nothing
---

# Open in Workbench, Reveal and Go do nothing

## Report

Clicking open in workbench, Reveal and Go don't appear to do anything.
I would expect Open/Go to load the part in the main viewport. 
Reveal I would expect to select the part in the parent world if it is loaded.

## Repro

Launch from `MatterEditor/` (from a shell that has **not** exported
the MSYS2 UCRT64 PATH — env vars only reach a native exe through its
own prefix, and an MSYS bash swallows them):

```bash
MATTER_WORLD=PhysicsPlayground MATTER_CAM="17.406,14.075,32.946,-0.176,1.411,-0.751" ./build/windows/editor.exe
```

Simulation was in **Pause** for the first shot.

## Evidence

### 1. shot-1.png

![](shot-1.png)

- region: region (594x1076)
- camera: `17.406,14.075,32.946,-0.176,1.411,-0.751`
- sim: Pause @ 1.00x — 10 instances, 1038 tris, 4 batches, 16.66 ms

- `state.json` — per-shot camera/counts plus deep engine state
- `log-tail.txt` — console lines leading up to this report

## Acceptance

_TODO — the check that closes this. Prefer a headless `make -C MatterEngine3/tests run-*` target; fall back to a scripted capture (`MatterEngine3/tools/viewer_shots.sh`) plus what the pixels must show._
