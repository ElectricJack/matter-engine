# Procedural material layers — native visual checkpoint

Partial L2a/L3/L6 progress. The full material goal remains active; these are
functional prototypes, not accepted final art or performance results.

Subsequent implementation checkpoint: [bounded recipe capacity](../2026-09-15-bounded-layers/README.md)
raises direct sources to 512 operations within the same shader workspace and
instruction arena. The images and 96-op implementation description below remain
the historical `layer-green` capture; they were not recaptured for that change.

Latest visual checkpoint: [brick shape refinement and terrain boundary diagnostics](../2026-09-15-material-shapes/README.md).
The earlier [composed POM checkpoint](../2026-09-15-composed-pom/README.md)
connected height rendering to the [bounded splats and weathering](../2026-09-15-weathering/README.md)
recipes. Historical images and settings below are retained as captured.

## Original layer checkpoint images

| Scene | Close | Middle | Far | Grazing camera | Native RT |
|---|---|---|---|---|---|
| Brick | [image](brick-layer-v2-close.png) | [image](brick-layer-v2-middle.png) | [image](brick-layer-v2-far.png) | [image](brick-layer-v2-grazing.png) | [image](brick-layer-v2-rt.png) |
| Terrain | [image](terrain-layer-v2-close.png) | [image](terrain-layer-v2-middle.png) | [image](terrain-layer-v2-far.png) | [image](terrain-layer-v2-grazing.png) | [image](terrain-layer-v2-rt.png) |

![Brick and paint, native raster](brick-layer-v2-middle.png)

![Rock, soil and moss, native raster](terrain-layer-v2-middle.png)

The brick has 320 x 180 mm running-bond pitch, 12 mm mortar gaps, a nominal
5 mm worn bevel and 7 mm recess, plus 0.8 mm paint thickness. Cell hashing
chooses per-brick variation; broad paint coverage crosses courses. Fine grain
fades between 8 and 30 mm texel footprints. The complete graph is 92 operations.
Its declared composed height range is -10 to +3.8 mm.

Terrain geometry comes from the native density field, with a 7 m radius / 3 m
high dome and broad ground variation. The surface uses world-space rock, soil
and moss signals across streamed cells. Soil replaces the rock using physical
height bias; moss adds 6 mm thickness. One fine signal affects final color,
roughness and height without duplicating the underlying rock relief. The graph
is 88 operations with a -48 to +54 mm final height range. Soil, moss and rock
still need richer structure and more convincing visual calibration.

## What changed

- `s.cellNoise2(seed, x, y)` is a single CPU/GPU scalar instruction using the
  existing integer noise hash. It produces a repeatable random attribute for a
  floored cell coordinate, including negative cells. It has no camera, page,
  physical slot or footprint dependency. It is not a unique feature identifier.
- `s.layer(base, layer, options)` compiles replacement, nonnegative deposition
  and appearance-only operations into the existing program. Height-aware
  coverage follows the design's reference formula; linear RGB, squared
  roughness, metallic, AO and metre height share that weight. Final height
  supplies normals through the existing finite-difference path.
- Both proof recipes use this helper and generate directly into VT. Neither
  requests a periodic source image nor a geometry/physics texture bake.
- The physical page format and 96-operation limit are unchanged. This helper
  does not implement spatial splat records, contributor chunking, local dirty
  bounds, image/stamp layer sources or composed-height POM.

## Verification

Canonical Windows MSVC RelWithDebInfo, native RTX 4090. All ten recorded
build/test steps in [layer-green-checks.json](../2026-09-15-direct-source/layer-green-checks.json)
pass: surface-field CPU, native JS authoring/CPU evaluation, VT compositor,
direct-source raster/RT, existing surface-parallax and the corresponding builds
including the editor. The three GPU suites report zero validation errors.

The [red CPU run](../2026-09-15-direct-source/cell-red-checks.json) demonstrates
that the old evaluator rejects the new cell instruction. Fixed 24-bit goldens
cover signed cell coordinates, full uint32 seeds, within-cell stability,
footprint independence, invalid numeric input and malformed operands. GPU
readbacks cover both mips and eviction/regeneration; maximum encoded-channel
error is 0.004103 against a 0.012 tolerance. Native JS layer fixtures compare
zero/full/partial coverage, physical height bias, squared roughness and all
three height operations against the reference formula. These do not complete
the full L3 GPU layer/seam/normal-frame/overflow test matrix.

The source/binary manifests track 83 native inputs, including all eight native
files changed in this checkpoint. Sources were frozen during each native job.
No competing native GPU jobs ran. Existing unrelated worktree changes remain.
`checkpoint-verification.json` records the final source, binary and image checks.

## Capture settings and interpretation

`capture.py` records the command timeline, source and executable hashes,
cameras, density override, exposure, screenshots and VT statistics. Invoke it
from WSL, for example:

```sh
python3 docs/agent/evidence/2026-09-15-material-look/capture.py brick brick-review \
  --build-manifest docs/agent/evidence/2026-09-15-direct-source/layer-green-source.json \
  --tpm 256 --exposure -1
```

Use `terrain terrain-review` for the second scene. Run captures sequentially.
The script refuses sources/executables that differ from the specified build.
Each view waits 180 presented frames before statistics and capture. It also
rejects nearly uniform screenshots; existence and native exit alone do not
prove the receiver was visible. All current views recorded VT queue zero.
The brick run ends with 76 occupied physical slots; terrain ends with 343.
These figures describe these views, not peak memory or acceptance under pressure.

Both scenes use the explicit **prop chart** override of 256 texels/metre,
1280 x 800 output and exposure -1 EV. The small terrain fixture's charts use
that policy; this does not change the large-terrain density default. Authored
sun/sky values and seeds remain in the scene sources. User/global rendering
preferences beyond the explicit timeline are not fully frozen by this harness.
The reported process times include startup, streaming, view waits and image
readback; they are not isolated material-generation or steady GPU timings.

`brick-layer-v1` exposed a fixture failure: narrowing streaming bands omitted
the analytic wall from the close/far views. Those images were blank despite a
successful process exit. Its result is annotated as a failed visual review.
Restoring the prior streaming policy produces valid `brick-layer-v2` views;
the middle/grazing images remain byte-identical. The fixture still has noisy
empty-cell warnings and needs a cleaner receiver registration path.

Terrain v1 is retained to compare its overly smooth regions with v2's more
irregular boundaries and shared grain. Earlier brightness/paint experiments
and logs remain in the adjacent direct-source evidence directory. The brighter
RT presentation and blue lighting cast remain visible; appearance parity and
lighting calibration are not established by the functional raster/RT tests.

## Next work

Refine the visual recipes and their physical scale; add localized weathering
through bounded splat records and stable placement; expand ordered composition
beyond the scalar-program cap; persist composed height and drive POM from it;
prove page/filter seams, contacts and edits; prepare/cache image and geometry
stamp sources; finish selective physics profiles and the deferred VT timing /
memory acceptance. Keep visual feedback separate from automated correctness.
