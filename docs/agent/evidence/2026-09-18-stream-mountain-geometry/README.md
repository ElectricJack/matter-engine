# Streaming Mountains: virtualized procedural rock demo

## Run the editor with UI

From native PowerShell at the repository root:

```powershell
tools/launch-mountain-geometry.ps1
# Add -Build to rebuild the native MSVC editor first.
```

The launcher opens **StreamMountain**, enables RT/GI, selects geometry paging
for `MountainDetailRock`, and positions the camera at the inspection site near
**X=420, Z=1455**. It prints the command-file/log directory. It leaves the editor
open. The mountain's terrain, ordinary rock scatter, shared foliage, clouds,
and lighting remain active. F11 toggles the panes/full viewport.

Close view: `cam 425 25 1465 419 23 1455`. Overview:
`cam 436 30 1473 421 22 1454`. These can be appended to the printed command file.

## What is implemented

- Three independently generated closed rocks: **110,592 source triangles each**,
  **331,776 total**. Fracture grooves, eroded faces, chipped outlines, flakes and
  small pits are triangles. No physics settling or POM supplies this relief.
- The existing sector streamer places each rock under one half-open XYZ owner.
  Geometry identity stays fixed across sector levels.
- Compiler → binary hierarchy pages → root admission → worker page reads →
  raster upload plus BLAS readiness → GPU hierarchy selection → indexed indirect
  drawing. Bounded asynchronous GPU requests drive finer-page loading.
- CPU selection provides matching RT membership. Missing children retain their
  parent until the full replacement is ready. Page allocations are frame-retained.
- Offline attribute projection now uses the existing spatial index rather than
  all-pairs nearest-triangle searches; dense rocks previously exceeded that budget.
- Targeted module admission preserves the terrain/material and shared-foliage
  paths. Selected rocks skip the duplicate legacy flatten preparation.

## Recorded initial full-scene validation

Native MSVC RelWithDebInfo, RTX 4090, 1280×720, native RT/GI. See
[full scene log](mountain-initial-validation.log).

| Measurement | Observed result |
| --- | --- |
| Coarse roots | One per asset; 126, 128, 126 triangles |
| Cold hierarchy compilation, already baked source | 17.07, 14.84, 19.45 seconds |
| Close-view residency | 3 assets, 7,341 tracked pages, zero inflight requests |
| Charged geometry/BLAS reservations | 228,016,792 bytes (217.45 MiB) |
| Stale completions | 0 |
| Scene RT toggle | 0 dispatches disabled; 1 with GI disabled; 2 with GI enabled |
| Close camera return | Same 399,446 total visible scene triangles as before travel |
| Whole scene frame time after return | 62.29 ms; GPU 62.30 ms; culling 0.155 ms |

These timings include the real mountain and are **not an isolated geometry
benchmark or a performance acceptance pass**. The stored scene settings reach
20 km and include substantial foliage/VT work. Logs contain existing terrain
surface-link budget warnings. Source cooking is additional to hierarchy timing.
The first run used two page uploads/frame; the final runtime raises the bounded
batch to eight, with 32 inflight slots. Warm hierarchies reuse the binary cache.

The final launcher was exercised with the UI visible and the eight-upload batch.
All 55 catalog assets prewarmed; the geometry hierarchies were cache hits with no
new compilation. The overview settled at **3 assets, 5,911 tracked pages, zero
inflight requests, zero stale completions, 182.09 MiB charged geometry memory**.
This different camera/viewport selected less detail than the close-up above.
See [editor UI](editor-ui.png), [stdout](final-ui-stdout.log), and
[residency log](final-ui-stderr.log). The editor was left running for review.

## Tests

- [Native hierarchy/residency/filter tests](hierarchy-tests.log): pass.
- [Spatial attribute projection/error tests](projection-tests.log): pass.
- [Native GPU selection, feedback, raster and RT publication](gpu-feedback-tests.log):
  pass, zero Vulkan validation errors.
- [Full native RT regression](rt-regression.log): pass, zero Vulkan validation errors.
- [Dense procedural rock topology](detailed-rock-tests.log): pass.
- [Existing mountain rock/scatter tests](existing-rock-tests.log): pass.
- [Final executable and key source hashes](artifact-hashes.json).

GPU reservation accounting excludes shared renderer arena capacity, textures,
foliage, lighting, and persistent scratch pools. The launcher allows 256 MiB
geometry reservations and 128 MiB worker page payloads, plus the separate
16 MiB root cache. These are not total scene memory limits.

## Captures

- [Detailed close view](settled-close.png)
- [Running editor with panes](editor-ui.png)
- [Actual triangle wireframe](settled-wire.png)
- [Ray tracing disabled](rt-off.png)
- [Ray tracing on, GI disabled](gi-off.png)
- [RT and GI enabled](rt-gi-on.png)
- [Camera away](away.png) and [returned](return.png)

The initial diagnostic captures include temporary session lighting adjustments;
the launcher retains the world's authored lighting. The saved scene props were
not changed. Clouds are visible, and disabling RT removes the cast shadows.

## Limits and remaining work

This is a geometry integration demo, not the finished Nanite-scale system.
The stone material is currently uniform; stable VT material mapping over the
hierarchy and richer rock coloration remain. Coarse fallback shading can look
faceted while detail arrives. Parent reclustering, attribute-aware error, broad
memory-pressure/recovery sweeps, imported-mesh scaling, adjoining displaced
terrain, and total-cost optimization remain in the implementation plan.

Source: `objects/terrain/MountainDetailRock.js`,
`shared-lib/mountain_detail_rocks.js`, `shared-lib/mountain_geometry_site.js`,
and StreamMountain's `objects/WorldSector.js`, all under `projects/world_demo/`.
