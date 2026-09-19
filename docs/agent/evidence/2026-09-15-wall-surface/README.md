# Part-owned finite surfaces: provider and wall integration

Status: the straight three-box material path now renders prepared brick detail.
Periodic-module, corner/curve, full POM seam and performance acceptance remain open.

## Whole-wall checkpoint

- `ClayBrickWallSurface` now emits one box per wall and no child geometry.
  Native tests verify 12 triangles for each of the three proof sizes.
- Generic `finiteSurface` version 2 declares reusable source recipes and rigid
  placements. Brick recipes, colors, physical sizing and bonds remain in JS.
- The GPU compositor queries a bounded spatial candidate grid and filters
  source coverage/color/normal/roughness/height over recessed mortar. This first
  implementation is for disjoint structural footprints, not general ordered
  overlapping splats or depth unions.
- Provider-owned weak source caching shares completed source preparation.
  The second and third walls each reused 40 of 48 prepared faces; unused faces
  can expire and be prepared again. GPU payloads remain duplicated across
  distinct wall catalogs; source compression and payload-bank separation are open.
- [composite-v2 checks](composite-v2-checks.json): native recipe/receiver,
  provider lifecycle/composition, GPU compositor clay-fixture regressions and
  editor build pass with unchanged source hashes and zero validation errors.
- [composite-walls-v1 capture](composite-walls-v1-result.json): all 14 POM on/off
  screenshots pass automation with no bake/Vulkan errors. Visual inspection
  confirms source detail in [raster](composite-walls-v1-close.png),
  [normals](composite-walls-v1-normals.png) and [RT](composite-walls-v1-rt.png).
  [Wireframe](composite-walls-v1-wireframe.png) shows three simple boxes.
  RT still has artifacts at some outer edges; box silhouettes/cross-chart POM
  and exact source-reference matching are not accepted yet.
- The capture reports 3 VT variants, 139.2 MiB charged mesh/input memory,
  30–113 resident pages in shaded views, empty fill queues, no evictions or
  rejections. Wireframe reaches 185 resident pages. This is a visual checkpoint,
  not a performance improvement claim. Cold scene bake/publish was 13.8 seconds.

## Earlier per-brick checkpoint (superseded)

The earlier low-poly captures below were visually flat and did not meet the
user's requested box representation. Keep them as failed diagnostic evidence.

## Implemented

- `part_surface::prepare` prepares all six finite faces, caches projected
  geometry by geometry identity, bakes appearance, and publishes one complete
  immutable catalog. Failed/cancelled preparation retains the previous output.
- LocalProvider reads `static finiteSurface(p)` on cold bakes and geometry-cache
  hits. Queued projection/material jobs own their inputs, including callbacks
  discarded by the caller before the GPU queue finishes.
- The renderer retains the catalog with its part and binds categorical face
  selectors before eager/deferred VT registration and memory admission.
- `ClayBrickWallSurfaceProof` uses the authoritative whole-brick layout and
  twelve-triangle `ClayBrickSurface` receivers. The earlier `ClayBrickWallProof`
  still uses detailed source geometry. The new wall uses ordinary instanced
  children: the separate `sharedSurfaces` foliage path bypasses these VT inputs.

## Projection correction

The exact modular 117.5 mm-deep recipes exposed nonconvergence on the top face
of seeds 1 and 7. The previous normal-offset inverse switched to a wider filter
as coverage reached zero, producing a discontinuous displacement field.
Preparation now continues the ordinary filter's boundary value outside coverage;
the final output still uses ordinary finite coverage. Iteration limits and
convergence tolerance were not relaxed. The prepared representation version is 2.

## Native validation

- [modular-v3 checks](modular-v3-checks.json): stamp CPU tests, provider lifecycle
  tests, GPU projection/material preparation, and MSVC editor build pass.
- The GPU test covers the previous 48 faces plus the 48 faces of the exact
  production modular recipes; all pass with no Vulkan validation errors.
- Provider tests cover cold preparation, geometry-cache reuse, appearance edits,
  partial failure, an abandoned queued callback, and receiver plane matching.
- [walls-v3 capture](walls-v3-result.json): all seven screenshots were produced,
  with unchanged sources/binary, no bake/binding errors, no VT rejections or
  evictions, and an empty fill queue at each observation. **Visual review fails:**
  [close](walls-v3-close.png), [albedo](walls-v3-albedo.png), and
  [normals](walls-v3-normals.png) remain flat. Investigate final page composition
  and draw sampling before calling this integration complete.
- All eight source variants report six geometry-cache hits in that scene.
  Geometry reuse does not eliminate appearance preparation on scene startup.

`walls-v1` failed during projection. `walls-v2` used the wrong shared-surface
rendering path and had no active VT. Their failed records are retained.
The attempted `demand_bake_tests` CMake target does not exist; it is not a passing
regression test. The [provider-regressions-v1 checks](provider-regressions-v1-checks.json)
pass authored-world provider cache, resolve-cache and conifer-LOD provider tests.
Compositor validation of the boundary correction is tracked with the new
whole-wall integration checks below.

## Remaining limits

Prepared source pixels are still float interchange data: about 10.87 MB per
six-face variant, approximately 83 MiB for eight variants. Final source
compression/cache, chipped silhouette visibility, general overlapping splats,
cross-brick weathering, distant merged-LOD appearance, POM chart traversal and
performance acceptance remain open.

The user requested an interactive editor preview during this investigation.
That session has ended; the user explicitly authorized closing it and resuming
native rebuilds/validation. Whole-wall composition is now being implemented.
The new [specification](../../../superpowers/specs/2026-09-15-whole-wall-vt-composition.md)
also requires repeating modules, 90-degree junctions and curved-wall proofs.
Those requirements are not yet visually validated.

## Composite admission scope

The first version-2 receiver supports disjoint source bounds with rigid,
axis-aligned placement on the six box planes. It rejects overlap, non-axis
rotations, reflection, scale, out-of-bounds sources, duplicate IDs and oversized
source preparation. Candidate grids and reference storage have explicit caps.
These restrictions prevent accepting a declaration whose source faces would
silently disappear or whose overlap would be averaged incorrectly. Curved and
L-shaped receivers need explicit surface mapping/projection support before
those authored layouts can use this API. The user-requested path proofs remain
open; a rotated set of per-brick runtime meshes is not an acceptable substitute.


## Spatial composition regression

[composite-v3](composite-v3-checks.json) passes the native declaration/receiver
and provider tests after adding strict placement and aggregate-source admission.
The GPU test initially needed its new oracle's link dependency corrected; the
final fixture uses the exact overlap area of constant rectangular sources as
an independent oracle. [composite-v4](composite-v4-checks.json) passes GPU
compositor tests, including two colored/depth sources sampled through the
spatial index at mips 0, 3 and 6. Maximum height error was 0.564 micrometres;
maximum compressed linear color error was 0.039107. Vulkan validation errors: 0.
The failed `composite-grid-v1` attempt was a transient executable link lock;
`composite-v3`'s GPU target was an unresolved test-oracle symbol. Neither is
counted as passing validation.


## Final six-face capture

[composite-walls-v2](composite-walls-v2-result.json) uses the final
`composite-v4` editor and adds back, left-end, top and bottom views. All 22
POM on/off screenshots completed with unchanged sources/binary, zero bake or
validation errors, zero VT rejections/evictions and empty fill queues at each
observation. The renderer census reports 3 parts, 3 instances and 36 triangles.
Warm scene bake/publish took 3.34 seconds; the first wall reused all 48 geometry
projections, and subsequent walls reused 40 prepared material faces each.
The shaded circuit reached 211 resident pages (251 after wireframe).

Front/group/grazing raster screenshots are byte-identical to the first
composite capture. Back and left-end shaded views show the expected bond but
are dark under the fixed front-facing sun; they are not sufficient to accept
fine-detail matching against the high-poly reference. Top/bottom/cap coverage,
POM behavior and box receiver geometry are visible. RT boundary artifacts,
source-memory duplication, periodic reuse, corner/curve rendering and complete
visual/performance acceptance remain open.
