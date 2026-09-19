# Procedural masonry shape review and terrain boundary diagnostics

Partial visual progress. The retained brick recipe has more varied structure,
quieter face relief and sharper paint loss. The full material goal remains
active; realism, seamless terrain POM, complete splat records and performance
acceptance remain open. No native renderer or shader source changed here.

## Retained brick changes

- Running-bond pitch stays 255 x 95 mm. Continuous 1.2 mm course wobble,
  per-course offsets and stable per-brick dimension/position/tilt differences
  reduce uniform outlines while preserving the masonry layout.
- A rounded-rectangle SDF supplies corners and chipped edges. The 6–9 mm bevel
  has its own physical width, separate from footprint antialiasing of the color
  boundary. Mortar recedes 4 mm. Clay tone, tilt and broad face relief vary by
  stable identity; fine pitting is reduced from the earlier sharp, busy pattern.
- Paint combines broad coverage with smaller edge breakup, uses a narrower
  footprint-aware transition, and retains 0.18–0.48 mm deposited thickness.
  Loss reveals the underlying brick/mortar. Existing bounded stains and moss
  continue through the same layer/splat evaluator.
- The base height envelope [-8, +6] mm includes all authored grain, face tilt
  and broad relief extrema. Paint and moss extend it through the existing
  conservative deposit bounds.

The original and intermediate recipes are archived in [recipes/](recipes/).
`brick-v3.js` matches the retained scene. `brick-v1.js` exposed excessive
contrast and very smooth faces; v2 widened the bevel, reduced recess, restored
some broad relief and sharpened paint coverage. V3 only enlarges the declared
upper bound to include the authored extrema. These are art iterations, not
changes to the shared material evaluator.

## Matched native comparison

Use **brick-shape-baseline-v2** versus **brick-shape-v3**. Both use the same
MSVC editor executable from the composed-POM v4 build, camera list, output size,
512 texels/metre review density, exposure -0.5 EV and explicit lighting/POM
settings. The original recipe was installed only for its capture and the
current recipe restored after the process exited. No sources changed during
any editor run. Earlier `brick-shape-reference` and `brick-shape-v1` use a
different lighting preset and should not be treated as this matched comparison.

| View | Original recipe | Retained recipe |
|---|---|---|
| Close | [image](../2026-09-15-material-look/brick-shape-baseline-v2-close.png) | [image](../2026-09-15-material-look/brick-shape-v3-close.png) |
| Middle | [image](../2026-09-15-material-look/brick-shape-baseline-v2-middle.png) | [image](../2026-09-15-material-look/brick-shape-v3-middle.png) |
| Far | [image](../2026-09-15-material-look/brick-shape-baseline-v2-far.png) | [image](../2026-09-15-material-look/brick-shape-v3-far.png) |
| Grazing | [image](../2026-09-15-material-look/brick-shape-baseline-v2-grazing.png) | [image](../2026-09-15-material-look/brick-shape-v3-grazing.png) |
| Native RT | [image](../2026-09-15-material-look/brick-shape-baseline-v2-rt.png) | [image](../2026-09-15-material-look/brick-shape-v3-rt.png) |

The manifests also retain raw albedo, normals and wireframe views. The result
is visibly quieter, with more irregular brick/paint outlines. It still reads
as a procedural prototype: fine clay detail, paint flake structure, mortar
variation and lighting need more work. Raster and RT presentation still differ.
The review preset makes comparisons explicit; it does not establish calibrated
photometric lighting or appearance parity.

### Capture settings

The current `capture.py --review-lighting` pins sun multiplier 1.67, visible sky
.77, day ambient .5, twilight ambient .25, irradiance .7 and sunset ratio .25;
sun tint is (1,1,1), sky tint (1,.9,.75). POM is explicitly enabled with 32 steps,
.1 m relief cap and .3 m travel cap. Each view waits 180 frames. Other renderer
preferences remain outside this preset; the complete commands and source hashes
are recorded in the source manifests.

```sh
python3 docs/agent/evidence/2026-09-15-material-look/capture.py brick brick-review \
  --build-manifest docs/agent/evidence/2026-09-15-direct-source/composed-pom-v4-source.json \
  --tpm 512 --exposure -0.5 --review-lighting --debug-views
```

`--debug-views` adds albedo, normals and wireframe. POM settings are now explicit
even without `--compare-pom`. Unknown/refused property commands fail capture
integrity checking. File hashing streams data rather than allocating each
entire executable. An initial prelaunch file-read allocation error was followed
by a successful retry; no editor process had started in that failed attempt.

## Terrain boundary diagnosis

The terrain recipe is unchanged. `terrain-boundary-review` records POM-off/on
pairs at the five normal views and the three debug views, with the same review
settings and density. Its [run result](../2026-09-15-material-look/terrain-boundary-review-result.json)
and [source/timeline](../2026-09-15-material-look/terrain-boundary-review-source.json)
are retained.

- [Normal view without POM](../2026-09-15-material-look/terrain-boundary-review-normals-off.png)
  already shows broad faceting. That problem is not introduced by the new march.
- [Raw albedo without POM](../2026-09-15-material-look/terrain-boundary-review-albedo-off.png)
  versus [with POM](../2026-09-15-material-look/terrain-boundary-review-albedo.png)
  shows additional narrow color changes near edges. For example at pixel
  (1085,420), RGB changes from (31,25,18) to (34,30,24), while neighboring
  (1089,415) stays (32,28,22). The lit view changes at the same location.
- The [wireframe](../2026-09-15-material-look/terrain-boundary-review-wireframe-off.png)
  places long mesh edges through these regions. This narrows the next
  investigation to chart/triangle traversal and boundary fallback; it is not
  proof of the exact shader branch responsible. Current traversal can return
  the proxy sample when its bilinear footprint leaves a chart, and local chart
  metrics do not provide a tested neighbor mapping across curved receivers.

Next work should isolate adjacent non-coplanar charts with a continuous world
material, distinguish chart rejection from ray-budget rejection, and compare
height/color continuity before changing the traversal contract. More rock
fracture/strata detail remains necessary after that boundary issue is understood.

## Verification and limits

[Checkpoint verification](checkpoint-verification.json) checks all six successful
native capture runs, their 56 images, the current 85 native inputs and the
editor executable against the existing build. Historical recipe hashes resolve
to the archived files; the retained recipe resolves to the live scene. All
runs finish with no Vulkan validation errors, missing/uniform images or source /
binary changes. No native builds or GPU jobs overlapped.

The shaded brick views settle at 196 occupied pages for the retained recipe
versus 195 for the original at the same density. Wireframe requests additional
pages, ending at 205/204. All sampled VT queues are zero. These are small-scene
residency observations, not capacity/throughput acceptance. No shared native
code changed, so the prior native POM/compositor/feedback regression evidence
remains applicable; there was no redundant rebuild/test run for recipe edits.

Capture process times include startup, waits and image readback; they do not
measure source generation or steady shading. Existing empty-sector/tracer
warnings, missing Epic overlay JSON and shader-interface warnings remain in
logs. General filtering, page/chart transitions, cross-object blending,
local splat edits and the original performance requirements remain unproven.
