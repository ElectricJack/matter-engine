# Work hand-off: dense Streaming Mountains terrain and BLAS cache

## Current objective

Continue Streaming Mountains with 0.25 m finest terrain sampling, dense
procedural rocks, trees, volumetric clouds/fog, cloud shadows, RT and GI. POM
is disabled. The current priorities are fixing visible terrain seams and sector
bake failures without reducing the approved visual fidelity.

Repository: `/mnt/d/shared with desktop/ai/matter-engine-cpp`

Canonical editor build:

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
```

## Recent authoring changes

- `projects/world_demo/scenes/streaming/StreamMountain/StreamMountain.js`
  - `terrainOnly` is now `false`.
  - Forest materials are module-scope `FOREST_MATERIALS`; this fixed the
    loader error caused by declaring them inside `forestMaterials()`.
  - Added 9 m, 4 m, 1.5 m, and 0.75 m ground relief.
- `.../StreamMountain/objects/WorldSector.js`
  - Finest terrain rung is 3, corresponding to 0.25 m voxels.
  - Dense geometry rocks are emitted in terrain-only mode too.
- `projects/world_demo/shared-lib/mountain_geometry_site.js`
  - 1,283 deterministic rock placements.
  - 19 reusable resolution-128 rock variants.
  - Sizes use exact quarter-metre increments to survive native float conversion.

Tests passing:

```bash
node projects/world_demo/tests/mountain_detail_rocks_tests.mjs
node projects/world_demo/tests/mountain_geometry_site_tests.mjs
node projects/world_demo/tests/mountain_forest_tests.mjs
node projects/world_demo/tests/alpine_ecology_tests.mjs
```

The geometry stress site contains approximately 252 million placed source
triangles, but only about 3.74 million unique source triangles. It stresses
instancing and residency rather than unique geometry capacity.

## Sector bake failure fixes

Evidence from the first 0.25 m run is in:

- `errors.txt`
- `README.md`
- `hierarchy-tests.log`

Two failure classes were found:

1. `placeChild: no declared child ...` for decimal rock sizes. Fixed by using
   float32-exact quarter-metre sizes.
2. `group exceeded compiler staging triangle limit`. In
   `MatterEngine3/src/geometry/geometry_compiler.cpp`, hierarchy merging now
   checks the combined triangle count before choosing a neighbor. If no legal
   neighbor fits, it emits an independent root instead of rejecting valid
   geometry.

`MatterEngine3/tests/geometry_hierarchy_tests.cpp` includes a tight-limit
   regression checking complete triangle coverage, group bounds, and no new
   outer cracks. The test passed with `ALL PASS`.

## BLAS and RT terrain work

Files changed include:

- `MatterEngine3/src/render/vk_blas_cache.h`
- `MatterEngine3/src/render/vk_scene_renderer.cpp/.h`
- `MatterEngine3/src/render/part_store.cpp`
- `MatterEngine3/src/render/geometry_world_runtime.cpp`
- `MatterEngine3/src/matter_engine.cpp`
- `MatterEngine3/src/render/vk_context.cpp`

Completed work:

- BLAS cache lookups start before warmup GPU budgets.
- Cached restores have a separate budget from cold BLAS builds.
- Completed cache payloads remain queued during writer backpressure.
- Capture limit increased from 32 to 256 per frame slot.
- RT terrain pages are no longer restricted to raster-only mode.
- RT page proxies carry their source sector hash and resolve the source
  sector’s VT chart/rung slot.
- Frame-retained Vulkan resources are cleared before device destruction; this
  fixed an exit-time crash observed during a dense RT run.

GPU tests passed with zero validation errors. Representative result:

```text
BLAS cache: restored=97 miss=49 captured=49 rejected=0
validation errors: 0
ALL PASS
```

The VT/RT source-atlas proxy test also passed.

Integrated StreamMountain RT/GI runs demonstrated persistent reuse. A second
launch reached approximately 7,741 BLAS restores, with zero compatibility
rejections. Geometry refinement was still active after 60 seconds, so this is
not yet proof of sub-second loading.

## Current editor session

Review launcher:

`/mnt/c/tmp/matter-blas-mountain/dense-terrain-repair/launch.py`

It enables:

```text
render_path native_rt
render.gi.enabled = true
render.pom.enabled = false
render.volumetrics.enabled = true
render.cloud_shadows.enabled = true
MATTER_SEAM_TRACE=1
```

The last known editor PID was 44712, but verify it before rebuilding. The last
session had successfully passed forest material registration and was preparing
tree assets. The current review screenshot while preparing was:

`/mnt/c/tmp/matter-blas-mountain/dense-terrain-repair/preparing.png`

## Seam investigation

Relevant files:

- `MatterEngine3/src/seam_weld.cpp`
- `MatterEngine3/src/seam_boundary.h`
- `MatterEngine3/src/matter_engine.cpp`

The seam welder supports adjacent rungs differing by one. Rung 3 is 0.25 m.
It rejects larger differences. With `MATTER_SEAM_TRACE=1`, inspect `[seam]`
lines in:

`C:/tmp/matter-blas-mountain/dense-terrain-repair/editor.log`

Important counters include `missing`, `fine_inc`, `coarse_null`, `gap`, `err`,
`viol`, `norec`, and `regfail`.

Do not assume every visible seam is a welder defect. First separate:

- Missing sectors caused by failed bakes.
- Temporary coarse/fine handoff gaps.
- Cross-rung weld failures.
- VT/chart alignment seams.
- Normal/material discontinuities.

## Recommended next actions

1. Verify/close the current editor before rebuilding.
2. Inspect the repaired log for remaining `sector bake failed`, `paging failed`,
   and `[seam]` messages.
3. Capture a settled screenshot after tree and terrain preparation completes.
4. Confirm `geometry_prepare_end ... ready=1` for formerly failing sectors.
5. If seams remain with zero bake failures, use seam counters to identify the
   failing rung/face path.
6. Run a clean editor shutdown and verify no crash dump is produced.
7. Preserve the 0.25 m detail level. Do not replace QEM simplification until
   the seam source is known; the existing simplifier is boundary-locked and
   attribute-aware.

## Evidence

- `docs/agent/evidence/2026-09-19-dense-terrain-failures/`
- `docs/agent/evidence/2026-09-19-blas-restore/`

