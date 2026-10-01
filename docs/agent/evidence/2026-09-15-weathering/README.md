# Bounded splat authoring and weathering — native checkpoint

Partial L2a/L3/L5/L6 progress. The material goal remains active. These images
show functional material composition and visual iteration; they do not establish
final realism, full splat infrastructure or performance acceptance.

## Current images

| Scene | Close | Middle | Far | Grazing | Native RT |
|---|---|---|---|---|---|
| Brick v3 | [image](../2026-09-15-material-look/brick-weathering-v3-close.png) | [image](../2026-09-15-material-look/brick-weathering-v3-middle.png) | [image](../2026-09-15-material-look/brick-weathering-v3-far.png) | [image](../2026-09-15-material-look/brick-weathering-v3-grazing.png) | [image](../2026-09-15-material-look/brick-weathering-v3-rt.png) |
| Terrain v2 | [image](../2026-09-15-material-look/terrain-weathering-v2-close.png) | [image](../2026-09-15-material-look/terrain-weathering-v2-middle.png) | [image](../2026-09-15-material-look/terrain-weathering-v2-far.png) | [image](../2026-09-15-material-look/terrain-weathering-v2-grazing.png) | [image](../2026-09-15-material-look/terrain-weathering-v2-rt.png) |

![Brick, paint and foundation weathering](../2026-09-15-material-look/brick-weathering-v3-middle.png)

![Terrain, strata and bounded moss](../2026-09-15-material-look/terrain-weathering-v2-middle.png)

[Previous brick](../2026-09-15-material-look/brick-layer-v2-middle.png) and
[previous terrain](../2026-09-15-material-look/terrain-layer-v2-middle.png) use
the same capture cameras, 256 texels/metre prop-chart override and -1 EV exposure.
The scene lights and geometry are unchanged. User/global renderer preferences
beyond the capture timeline are still not fully frozen.

## Shared authoring implementation

- `s.coverageShape(placement)` creates local/world box or ellipsoid coverage,
  with optional orthonormal axes. `s.splat(base, layer, placement, options)`
  applies optional extra coverage through the existing `s.layer` evaluator.
- Box signed distance is Euclidean. Ellipsoid distance uses a conservative
  radial approximation; long axes have wider feathering. Placement validation
  rejects invalid shape/anchor, nonfinite vectors, nonpositive dimensions and
  feather widths, and non-orthonormal axes.
- Edge width is `max(feather, footprint * footprintScale)`. The scale defaults
  to one and must be supplied for non-unit local-to-anchor transforms. This
  softens boundaries but is not complete area-preserving subpixel filtering.
  Tiny splats still need a better coarse-mip coverage solution.
- Appearance, replacement and deposition share RGB, squared roughness,
  metallic, AO and physical height semantics. The height result supplies normals
  through the existing finite-difference evaluator. No per-splat draw is added.
- These authored placements compile into a whole-surface scalar program. There
  are no independently keyed records, procedural candidate IDs, spatial culling,
  local move/delete invalidation, receiver/contact filters or sparse instance
  overrides yet. They consume the same 512-op / 96-live-register budget.

## Visual changes and review

Brick (290 operations): 255 x 95 mm running-bond pitch, 10 mm mortar, 6 mm
recess, per-brick kiln tone/face height/wear, 3–6 mm corner radii, and quieter
pitting. Paint adds 0.25–0.75 mm, with raised edges sharing its coverage. Two
bounded stains preserve height. A foundation moss patch favors the joints and
adds 1–3 mm thickness. Mortar cavity occlusion remains underneath thin paint
and stains. These are authored material approximations, not measured brick scans.

The first moss pass produced overly dark, blocky blobs. V2 warps its coverage,
reduces thickness/saturation and favors mortar; v3 preserves substrate occlusion
under coatings. The brick scale and variation are clearer, but perfect course
alignment, the blue lighting cast and the raster/RT brightness difference still
need attention. POM is not yet driven by the new composed height.

Terrain (229 operations): continuous tilted/warped strata, subdued rock/soil
variation, quieter grain, and the same moss material used both by a broad field
and a bounded world-space damp pocket. V2 breaks up the initial mask's obvious
noise-grid shapes. The resulting terrain remains too smooth and painted to call
convincing rock. The 2 m geometry fixture is coarse; rock-specific fracture/form
operators and lighting calibration are next visual work, not more indiscriminate
fine noise. A localized pattern being visible is not proof of realism.

## Verification

Canonical Windows MSVC RelWithDebInfo, RTX 4090. The final native steps in
[weathering-occlusion-checks.json](../2026-09-15-direct-source/weathering-occlusion-checks.json)
pass: authoring-test build, complete `eval_world_tests`, and editor build.

The new independent CPU oracle checks 720 combinations of shape, local/world
anchor, operation, signed sample position and footprint. Fixtures use rotated
axes and a translated world context. Color, AO, squared roughness, metallic and
height match; seven malformed placements fail with author-facing diagnostics.
Fresh authoring contexts produce identical programs. Both real scene recipes
parse and pack within GPU limits. These semantic checks are CPU checks; images
exercise the GPU path, but are not numeric GPU coverage/seam goldens.

Earlier failures in `weathering` were in the new scene-test fixture: it omitted
the material registration normally done by the world loader. Registering the
same module-level names fixed the tests; production loading was unchanged.
The previous bounded-program GPU/legacy correctness checkpoint remains recorded
[here](../2026-09-15-bounded-layers/README.md); no GPU evaluator, packing, page format
or residency implementation changed in this weathering step.

Capture manifests/results/timelines/logs accompany each image prefix in the
material-look directory. Both final captures exit successfully, record no
source/executable changes during execution, and detect no Vulkan validation errors.
Every view records VT queue zero; brick ends at 76 occupied slots, terrain at 343.
The capture's `passed` flag validates execution/artifact integrity, not art quality.
These small fixtures do not establish pressure behavior or production throughput.

The final native manifest tracks 83 inputs. Brick v3 matches it. Terrain v2 uses
the identical current editor binary and unchanged runtime/scene sources; only
`eval_world_tests.cpp` changed afterward to add AO/metallic oracle checks.
`checkpoint-verification.json` enumerates that historical difference and verifies
all ten image hashes. Sources stayed frozen during every native job/capture.
No native/GPU jobs overlapped; all hidden editors exited.

## Remaining work

Complete source/stamp preparation and selective physics caching; implement stable
splat records, spatial admission and local dependency invalidation; preserve shared
base pages with instance overrides; add composed-height POM and layered GPU/seam/
filtering tests; improve rock structure and lighting; then complete the original
VT latency, throughput and memory targets. Full author visual acceptance is open.
