# StreamMountain continuous terrain material

Status: direct material conversion, authored terrain density and first visual
passes implemented and checked. Final appearance and performance acceptance
remain open; the close aggregate and its distance transition need further work.

Follow-up: the [terrain POM pass](../2026-09-17-mountain-pom/README.md) broadens
the embedded stone features, adds centimetre-scale relief and preserves the
aggregate's average coverage across footprints. Its native checks pass; see
that checkpoint for capture validity and remaining visual work.

## Changes

- `projects/world_demo/shared-lib/mountain_surface.js` authors a direct GPU
  material in physical world coordinates: broad mineral drift, soft soil/ground
  cover, exposed stone, talus color, snow and concavity-driven dampness. Height
  and its derived normals use continuous metre-space noise. Embedded aggregate
  uses one signal for color, roughness and shallow relief. Grain and medium
  relief fade with VT footprint. This first recipe has restrained common relief;
  it does not yet reproduce distinct angular scree stones or forest litter.
- `StreamMountain.js` uses this source without the five terrain detail atlases
  or the extra ForestFloor tileset root. Five palette handles remain declared
  to preserve forest material registry ordering. The three bark detail sources
  remain. Terrain geometry, habitat/forest placement, saved lighting, streaming
  ranges and user object visibility settings are unchanged.
- Direct-source v1 has one validated carrier at weight 1. CPU vertex
  classification now returns that constant without executing the entire color,
  height, noise and field-query tape.
- StreamMountain exposed a sparse-voxel foliage pipeline attachment mismatch:
  foliage declared R16G16B16A16_UINT while the paired VT feedback target is
  R32G32B32A32_UINT. A shared format header now supplies both contracts.
- Near-ground review exposed the existing 16 texels/m terrain chart limit
  (6.25 cm/texel). `streaming.terrainTexelsPerMeter` now authors the requested
  finest terrain density, with strict finite-number validation in [1,2048] and
  a legacy default of 16. It reaches both memory and artifact staging. Runtime
  chart preparation keeps nested tile-size/rung scaling and the 8192 atlas cap;
  packing can lower the requested density. Changing it requires world reload.
  StreamMountain requests 64 texels/m (1.5625 cm/texel before packing/mips).

## Generation findings

Only ScreeDetail actually settled its three stone layers in the old set.
ForestFloor and the other alpine sources already used analytic placement.
The improvement comes from removing the scene's terrain source geometry/atlas
preparation altogether, not merely reducing solver iterations. Legacy sources
and optional Wang generation remain available for other scenes.

The warm baseline recorded these source preparation/upload jobs:

| Source | Milliseconds |
| --- | ---: |
| ForestFloor | 3309.7 |
| AlpineRockDetail | 1902.7 |
| ScreeDetail | 2082.1 |
| AlpineSnowDetail | 1583.6 |
| AlpineMeadowDetail | 2087.1 |
| Total | 10965.2 |

Candidate v1/v2 requested only the three bark jobs; none of these five terrain
jobs ran. Five 4096-square source payloads of about 85.33 MiB each are no longer
required by this scene. This is about 426.7 MiB of source payload, **not** a
measured reduction in GPU allocation. Reserved VT pool capacity is unchanged.

World installation still prewarms 20 forest/other child variants. The baseline
`bake.reset` job took 46.67 seconds; v2 took 36.49 seconds. These are individual
warm runs, not a controlled benchmark or cold-generation speedup. V1 also ran
alongside a CPU test and is unsuitable for timing comparison. Material/source
preparation, forest baking, page refinement and full-scene rendering are separate
costs; the latter remains substantial with the full forest resident.
V4's world installation took 47.91 seconds, demonstrating why the shorter v2
run cannot establish a whole-scene load speedup. The five terrain source jobs
are absent in every candidate, independently of this forest/install variability.

## Evidence and validation

`baseline-sources.json` and `StreamMountain-before.js` retain the starting inputs.
`baseline/` contains the actual old-material overview and log. That run exited 1
with ten Vulkan validation errors from the foliage format mismatch; it is visual
reference, not a passing correctness gate.

`after-v1/` preserves the initial continuous-material trial: less repeated relief,
but overly distinct green/brown patches. `after-v2/` contains the muted, softer
palette at overview, close and grazing angles. Both editor sessions exited 0
without Vulkan validation errors. V2 close views exposed insufficient ground
resolution, prompting the explicit density work above.

`after-v3/` contains the 64 texels/m capture and a separately labeled daylight
diagnostic. Runtime lighting overrides were restored; the scene props hash did
not change. The material still looked too smooth. `after-v4/` adds the embedded
aggregate and a longer stationary close view. That session exited 0 with zero
Vulkan validation errors and unchanged source hashes. After the extended hold,
the VT queue reached zero: 788 registered variants, zero rejections, zero
evictions, 10.03/64 MiB indirection, and 1065/25600 occupied pool slots in the
close view (1243 slots after the grazing view). Reserved capacity is unchanged.
These are live use counters, not an allocated-VRAM saving.

The aggregate remains somewhat coarse, and an obvious band between detailed
and smooth ground survives the empty-queue capture. This is not explained by
pending VT work. Resolve recipe filtering/mip transitions before calling the
near-ground result visually accepted. The scene's dark warm lighting also makes
material assessment difficult; the daylight diagnostic was overly bright and
is not a proposed lighting preset.

Native build/source/log manifests live in `../2026-09-16-shared-vt-pixels/`.
`mountain-material-v2` built four MSVC targets and passed five focused modes:
`surface`, `mountain`, `sparse-voxel`, `vt-feedback-pair`, `vt-direct-source`.
GPU checks reported zero validation errors; raster/RT direct-material checks ran
without RT skips. The real mountain source compiled to 153 GPU operations, and
native samples covered finite RGB/ORM, bounded nonflat height, distance filtering
and translated receiver equivalence. Its field hash stayed `078c235c00e0fac2`
and habitat hash stayed `7baba303c9baa129`.

`mountain-material-v3` adds the native staging check and passes six modes
(`chart` plus the five above), across 1003 frozen source files and five MSVC
targets. See `native-audit-v3.json`. V4 changes only the JS recipe and its native
height assertions: the focused mountain check passes with 181 GPU operations
and sampled heights [-0.021478,-0.005139] m, within authored [-0.026,0]. Editor
and GPU-test binaries are byte-identical to v3; the live v4 captures exercise
the actual revised JS recipe. V5 additionally applies authored density to small
streamed sectors whose radius selects the ordinary geometry ladder; its focused
staging fixture covers a 16-m sector as well as the 64-m/nested cases. This
does not alter the StreamMountain branch used in the v4 captures.
Both v5 MSVC targets and its native chart/staging check pass, with unchanged
sources/binaries during validation; see `native-audit-v5.json`. The current
development editor is the v5 build. No editor process was left running.

The attempted full world-definition suite (`mountain-material-v1-test-world.log`)
reported 22 scene-only object references from unrelated villa/Kreuzenstein shared
libraries. It was terminated after 215.7 seconds before the mountain test ran.
The focused `--mountain-material` mode avoids those unrelated assets; the whole
world-definition suite is **not** reported passing.

## Remaining work

1. Smooth the near/middle detail transition with the real VT mip/filter path;
   the queue-empty capture shows that additional waiting does not fix it.
2. Develop convincing distinct rock, soil, moss and litter at close range,
   retaining quieter distance detail. Current common relief is a starting point.
3. Add analytic geometry/bounded material stamps where actual stones or litter
   are needed; use physics only for selected piles. Add cross-object contact
   blending, projection constraints and stable splats under the main plan.
4. Obtain visual review under repeatable lighting and camera paths, including
   raster/RT, page transitions and motion. Existing scene lighting is very dark
   and warm; a neutral-light material review is still needed.
5. Measure matched populated-world frame time, cold/warm source preparation,
   edit latency and actual allocated/live memory. Current screenshots have
   different streaming completion levels and do not establish an FPS speedup.

The broader wall/layer goal remains active. The frozen r2 exporter handoff is
unchanged; these experiments are in the development editor build.
