---
id: 424b0f49-194e-1803-8fe5-a2fbe275cef6
world: AnimatedRigGallery
shots: 7
status: unprocessed
reported: 2026-07-28T04:55:01Z
---

# Unprocessed report 424b0f49-194e-1803-8fe5-a2fbe275cef6

_Captured in-editor. Title, kind, severity and area are for the
ingestion pass to fill in from the content below._

## Report

There are multiple issues that I'm seeing wrong with the LODs and rendering.
- First there appear to be distances where two meshes are visible intersecting with different materials,
one of these meshes often looks to have inverted normals
- Second higher LODs with lower resolution do not preserverve smooth normals, or the original material for the object

## Repro

Launch from `MatterEditor/` (from a shell that has **not** exported
the MSYS2 UCRT64 PATH — env vars only reach a native exe through its
own prefix, and an MSYS bash swallows them):

```bash
MATTER_WORLD=AnimatedRigGallery MATTER_CAM="20.000,16.000,34.000,0.529,1.303,2.222" ./build/windows/editor.exe
```

Simulation was in **Edit** for the first shot — press Play if the defect only appears in motion.

## Evidence

### 1. shot-1.png

![](shot-1.png)

- region: region (503x358)
- camera: `20.000,16.000,34.000,0.529,1.303,2.222`
- sim: Edit @ 1.00x — 5 instances, 27 tris, 4 batches, 16.56 ms

### 2. shot-2.png

![](shot-2.png)

- region: region (721x463)
- camera: `10.853,9.651,20.008,-8.718,-4.223,-12.077`
- sim: Edit @ 1.00x — 5 instances, 27 tris, 4 batches, 16.72 ms

```bash
MATTER_WORLD=AnimatedRigGallery MATTER_CAM="10.853,9.651,20.008,-8.718,-4.223,-12.077" ./build/windows/editor.exe
```

### 3. shot-3.png

![](shot-3.png)

- region: region (580x518)
- camera: `3.923,5.048,7.949,-13.874,-8.524,-25.278`
- sim: Edit @ 1.00x — 5 instances, 27 tris, 4 batches, 16.67 ms

```bash
MATTER_WORLD=AnimatedRigGallery MATTER_CAM="3.923,5.048,7.949,-13.874,-8.524,-25.278" ./build/windows/editor.exe
```

### 4. shot-4.png

![](shot-4.png)

- region: region (838x668)
- camera: `2.311,3.819,4.941,-15.485,-9.753,-28.287`
- sim: Edit @ 1.00x — 4 instances, 15 tris, 4 batches, 17.37 ms

```bash
MATTER_WORLD=AnimatedRigGallery MATTER_CAM="2.311,3.819,4.941,-15.485,-9.753,-28.287" ./build/windows/editor.exe
```

### 5. shot-5.png

![](shot-5.png)

- region: region (700x634)
- camera: `2.311,3.819,4.941,-15.485,-9.753,-28.287`
- sim: Edit @ 1.00x — 4 instances, 15 tris, 4 batches, 16.71 ms

```bash
MATTER_WORLD=AnimatedRigGallery MATTER_CAM="2.311,3.819,4.941,-15.485,-9.753,-28.287" ./build/windows/editor.exe
```

### 6. shot-6.png

![](shot-6.png)

- region: region (600x486)
- camera: `3.616,4.817,7.518,-13.721,-8.302,-26.133`
- sim: Edit @ 1.00x — 5 instances, 27 tris, 4 batches, 16.70 ms

```bash
MATTER_WORLD=AnimatedRigGallery MATTER_CAM="3.616,4.817,7.518,-13.721,-8.302,-26.133" ./build/windows/editor.exe
```

### 7. shot-7.png

![](shot-7.png)

- region: region (362x304)
- camera: `12.864,11.532,24.754,-5.069,-1.209,-8.728`
- sim: Edit @ 1.00x — 5 instances, 27 tris, 4 batches, 16.66 ms

```bash
MATTER_WORLD=AnimatedRigGallery MATTER_CAM="12.864,11.532,24.754,-5.069,-1.209,-8.728" ./build/windows/editor.exe
```

- `state.json` — per-shot camera/counts plus deep engine state
- `log-tail.txt` — console lines leading up to this report

## Acceptance

_TODO — the check that closes this. Prefer a headless `make -C MatterEngine3/tests run-*` target; fall back to a scripted capture (`MatterEngine3/tools/viewer_shots.sh`) plus what the pixels must show._
