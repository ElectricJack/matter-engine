# Brick maze proof — 2026-09-15

Scene: `ClayBrickMaze`. Uses the same eight geometry-baked clay sources and
POM material channels as `ClayBrickWallSurfaceProof`.

## Authored scene

- 18.6 × 18.6 m court with four gates and 20 wall sections.
- Heights: 10, 16 and 24 whole courses (0.930, 1.494 and 2.246 m).
- Ten L walls, two U walls, six straight walls, and two quarter-circle walls.
- Curves use 20 and 14 rigid brick columns, respectively, with controlled
  widening of mortar joints. They are faceted masonry envelopes.
- 608 runtime wall triangles plus a two-triangle ground plane. Detailed bricks
  only supply the material bake. Mesh counts do not grow with wall height.

Authoring files:

- `projects/world_demo/shared-lib/clay_brick_maze_layout.js`: scene placement.
- `projects/world_demo/shared-lib/clay_brick_wall_path.js`: whole-brick corner
  tiling, curve layout, receiver shell and finite-surface declaration.
- `projects/world_demo/objects/ClayBrickWallPath.js`: generic Part entry points.
- `projects/world_demo/scenes/ClayBrickMaze/ClayBrickMaze.js`: materials/camera.

The layout check found no intersecting wall footprints. A 10 cm navigation grid
with walls expanded by a 20 cm body radius connected all four gates and all
remaining floor cells. This is a geometric route check, not a character-controller
or collision-system acceptance test.

## Engine changes needed for the scene

`finiteSurface` v3 accepts explicit planar receivers (`originM`, unit `u/v/n`,
metric `domainM`). V2 remains the six-plane box format. Both use the same
compositor. V3 validates arbitrary rigid placements with oriented-box separation;
positive source overlap remains unsupported. Corners and curves are authored in
JavaScript; native code contains no masonry-specific operations.

Full source banks now retain all six projections of every source. Different
layouts share the bank's GPU pixels and mip directory; their placement bindings
and spatial indexes remain separate. Uploads retain the existing per-frame quotas
and publication/lifetime rules. Source pixels remain bounded to 128 MiB per bank;
spatial candidate limits are unchanged. The binding bound is 24,576 (six faces ×
4,096 placements), fixing the earlier mismatch between those two limits.

## Validation

- Node wall layout and path tests pass (original 40 wall cases plus nine path
  cases / 4,970 source placements, fixed shell counts, dimensions and frames).
- Native recipe tests pass for the actual largest L/U walls and both curves;
  native shell builds produce the expected 20/28/164 triangle counts.
- Native provider tests pass.
- Native GPU compositor tests pass with zero Vulkan validation errors, including
  shared-bank upload/allocation and rendered-channel checks, cancellation,
  bounded upload, and the 4,932-binding large-corner regression.
- Build/test manifests and logs: `../2026-09-15-wall-surface/maze-v1-*` and
  `maze-v3-*`. V2 updates only the scene ground geometry.
- `maze-review-v1` was deliberately stopped after the old binding limit rejected
  the large corner. It is a failed capture, not acceptance evidence.

`maze-review-v4` passed: 18 raster captures (overview, corner, both curves,
walkthrough, close material, albedo, normals and wireframe, each with POM off/on).
Every sampled view has zero queued pages, rejections and evictions; no bake or
Vulkan validation errors. The full overview draws 21 instances / 610 triangles.
Bank preparation reports 48 material-cache hits for each subsequent unique wall.
The conservative VT admission charge is 749.1 MiB across 10 variants; this is
not unique source-pixel ownership or a GPU allocation measurement.

The geometric route check is reproducible with
`node docs/agent/evidence/2026-09-15-brick-maze/validate_layout.mjs`;
`layout-check.json` records the result.

V2/V3 capture runs were stopped to investigate four-FPS presentation pacing.
It persisted with the engine cap disabled and with a visible window; its cause
was not established. V4 uses shorter waits and verifies that page queues settled.
These images and timings are not a performance acceptance result. Tight-curve
outer-face shaded views are dark under the fixed sun; broader lighting/art
approval remains for review.

![Maze overview](maze-review-v4-overview.png)

![Corner with POM](maze-review-v4-corner.png)

![Curved wall with POM](maze-review-v4-curve.png)

## Limits

This adds examples and supports their finite planar composition. Shared brick
images do not yet mean shared periodic *composed wall pages*. Smooth arbitrary
spline receivers, cross-chart POM traversal, chipped outer silhouettes, generalized
weathering/splat placement, RT seam acceptance and the broader VT performance
acceptance remain separate work. The curve bond is aligned by column; the bricks
are not bent or scaled. CPU admission still conservatively charges a referenced
source bank to each catalog; it does not report unique ownership.


## Weathered maze — 2026-09-15

`ClayBrickMaze` now enables stable per-wall JS weathering on all 20 wall
sections, with seven selected graffiti placements. The palette and procedural
masks live in `projects/world_demo/shared-lib/wall_weathering.js`. Layers cover
both source brick and mortar, using the generic post-composition `s.coat`
output. All eight source brick geometries/materials remain reusable. Geometry
remains 608 wall triangles plus the two-triangle ground.

### Evidence

- [Overview](maze-weathering-review-v1-overview.png)
- [Curved wall with graffiti](maze-weathering-review-v1-curve.png)
- [Paint and brick relief close-up](maze-weathering-review-v1-close.png)
- [Weathering around an inside corner](maze-weathering-review-v1-corner.png)
- [Painted straight wall](maze-weathering-review-v1-tag-divider.png)
- [Wireframe](maze-weathering-review-v1-wireframe.png)
- [Capture result](maze-weathering-review-v1-result.json) and
  [source/binary/environment manifest](maze-weathering-review-v1-source.json)

Eleven native captures passed, with no source changes, missing screenshots,
Vulkan validation errors, rejected variants or page evictions. Each captured
view had `queue=0`; the final pool held 1,250 of 25,600 pages. There are now 21
variants including ground, because the 20 walls have distinct weathering.
The scene's conservative CPU admission charge is 1,665.7 MiB of 3,512 MiB;
this counter is not a unique shared-allocation measurement. Source pixel bank
sharing remains independently covered by compositor tests.

### Checks

- JS: `wall_weathering_tests.mjs`, `clay_brick_wall_path_tests.mjs`, and the
  maze route/clearance validator passed.
- Native CPU: `finite_surface_recipe_tests`, `face_material_bake_tests`,
  `surface_field_tests` passed. All 20 wall recipes, including graffiti on
  every wall as a stress case, fit 512 source ops / 96 live GPU registers
  (412–460 parsed operations).
- Native GPU: `vt_compositor_tests` and `solid_face_projection_gpu_tests`
  passed with validation enabled. Coating color error versus an independent
  analytic oracle was 0.005973, and roughness error 0.004993 after compression.
  Height and normal page bytes remained identical to the uncoated fixture.
  The reusable-source material shader also matched its CPU reference.
- Build/check logs are `../2026-09-15-wall-surface/maze-weathering-v1-*`
  (CPU checks) and `maze-weathering-v2-*` (final GPU checks/editor build).
  V1's compositor test build had a test-only string-concatenation compilation
  error; v2 fixes it and is the accepted binary/source manifest.

This is an appearance checkpoint. The capture samples reported about 16.6 ms
per frame on this run, unlike the previous presentation stall; no matched
before/after performance investigation was performed, so the earlier issue
is not claimed fixed. Paint loss is a coverage mask, without raised peeling
edges. General splat placement authoring and the previously recorded POM/RT
seam limitations remain open.


## False vertical joints in weathering — fixed

Feedback on `maze-weathering-review-v1-corner.png` identified dark vertical
bands resembling a second brick grid shifted by half a brick. The bands remain
with POM disabled and appear in raw albedo, without corresponding extra edges
in the normal channel. An ablation setting **only runoff coverage to zero**
removes the bands (`alignment-no-runoff-corner.png`). This isolates their cause
to the JS runoff mask, rather than requiring a brick placement or UV offset.

The old mask used a fractional-coordinate comb at 4.9/6.7 cycles per metre,
modulated by a height-independent hash. Its narrow straight bands ran through
multiple brick courses at approximately masonry spacing. They resembled mortar
joints, especially over opaque paint. The replacement uses broad continuous
noise, 3D breakup along the height, and lower opacity (0.22 maximum versus 0.48).
It retains irregular staining while removing the periodic false joints. Source
geometry, placement, material channels, POM and VT addressing are unchanged.

Matched native evidence at 512 texels/metre:

- [Corner before](alignment-before-corner.png) /
  [corner after](alignment-after-corner.png)
- [Long wall before](alignment-before-long-wall.png) /
  [long wall after](alignment-after-long-wall.png)
- [Curved wall after](alignment-after-curve.png)
- [Original-mask ablation](alignment-no-runoff-corner.png)
- [Before result](alignment-before-result.json) /
  [after result](alignment-after-result.json)
- [Normal/albedo image comparison](alignment-channel-comparison.json)

Both matched runs passed 18 captures each: lit, albedo and normal views of the
corner, long wall and curve, each with POM off/on. All captured views settled
with queue zero, no rejected variants, no evictions and no validation errors.
The six paired normal images remain aligned: mean absolute channel differences
are below 0.00025 out of 255; exactly one pixel across all six pairs differs by
more than three levels (maximum four). Albedo changes where staining changes.
This comparison does not accept previously deferred chart/POM seam traversal.

`wall_weathering_tests.mjs`, `clay_brick_wall_path_tests.mjs` and the native
`finite_surface_recipe_tests` passed. All 20 weathered variants with graffiti
compile to 402–450 source ops, within the unchanged 512-op / 96-live-register
limits. The native executable is unchanged; JS is loaded at runtime. Its frozen
source/binary manifest is
`../2026-09-15-wall-surface/alignment-irregular-runoff-source.json`.
