# Composed VT height — raster and RT POM checkpoint

Partial L4 progress. The new procedural material's composed height now drives
POM in raster and secondary RT. Native correctness checks pass and paired real
editor captures exist. The goal remains active: realistic materials, the full
splat record system, filtering/seams and performance acceptance remain open.

## What changed

- `vt_parallax.glsl` shares the metric, height decoding, residency lookup and
  bounded traversal between raster and RT. Rotation and nonuniform scaling
  preserve world-ray distances. Color, normal and ORM use the final displaced
  coordinate; no legacy Wang detail is applied over the composed material.
- Direct AUX reuses G/B for a 16-bit chart ID and A=2/3 for interior/padding.
  All four bilinear footprint texels must be valid interior of the same chart.
  Chart IDs above 65535 use the existing flat tag-1 fallback. Legacy AUX retains
  its original interpretation and blend arithmetic. No extra resident image,
  descriptor or ray payload is added beyond the previous height checkpoint.
- Each march position resolves resident/coarser height and requires compatible
  input snapshot and height range. Invalid coverage, chart changes and exhausted
  travel retain the original proxy sample. Steps move at most half a desired-mip
  atlas texel, with 128 steps maximum and bounded binary/secant refinement.
  Footprint and distance fade unresolved relief.
- Primary visible-input bit 8 marks composed-height pixels. Lighting/shadow
  consumers recover the proxy from the existing ORM-alpha world-ray distance,
  including when the carrier lacks the legacy POM material flag. Feedback masks
  the extra bit. Secondary RT retains its original visibility position.
- Native coverage readback exposed floating-point classification gaps along
  shared triangle diagonals at 150 texels/metre: 28, 12 and 3 false padding
  texels at mips 0, 1 and 2. A coverage-only tolerance of 1/1024 of the current
  texel fixes them without changing the chosen point, chart or barycentrics.
- Final crossing uses the signed depth bracket to remove upper-bound bias.
  A scale-relative float-roundoff guard handles the deepest envelope endpoint,
  where division followed by multiplication can otherwise miss a valid crossing.

## Native validation

Canonical Windows MSVC RelWithDebInfo, RTX 4090, Vulkan validation enabled.
[Checks and commands](../2026-09-15-direct-source/composed-pom-v4-checks.json)
record eight successful steps: three builds and five suites (`compositor`,
`vt-composed-parallax`, `vt-direct-source`, `surface-parallax`, `vt-feedback`).
There are zero Vulkan validation errors and no source mutations during jobs.
[Source/executable hashes](../2026-09-15-direct-source/composed-pom-v4-source.json)
track 85 native inputs. Builds, GPU tests and editor captures ran sequentially.
[Checkpoint verification](checkpoint-verification.json) verifies current files
against these manifests and all 20 screenshot hashes.

The new POM fixture covers:

- Analytic constant recess, projected raster depth and transported ray distance,
  recovered proxy position, primary RT and secondary hit position.
- Yaw 0/30/90 degrees with scales (1,1,1), (2,.5,1.5), (.5,2,.75).
  Measured world travel is .0164208 / .0300637 / .0099326 m versus expected
  .0164200 / .0300626 / .0099323 m. Maximum secondary position error is 6.4 um.
- A curved height field with correlated color and normal. Measured local recess
  .0481728 m versus .0481700 m analytic at the displaced position.
- Pinned coarse-only coverage, subsequent fine-page feedback, chart-edge
  rejection, broad-cone fade, maximum-travel rejection and zero recess.
- Forty-one deepest-envelope ray angles: maximum depth error .000000238 m.

The compositor reads chart ID 256 and interior/padding classifications across
three mips; all incorrect counts are zero. Existing actual R16 height/range,
adjacent-page gutters, regeneration and legacy reset tests continue to pass.
The previous queue-ordered pixel/metadata publication evidence is retained in
the [height storage checkpoint](../2026-09-15-composed-height/README.md); queue
code was not changed in this POM checkpoint.

## Paired native captures

Same executable, material recipe, camera and explicit settings within each
pair. POM off is captured first, then on after 180 frames each. Output is
1280 x 800, review density 256 texels/metre, exposure -1 EV, POM steps 32,
relief cap .1 m and world-travel cap .3 m. The last view uses `native_rt`.

| Scene/view | POM off | POM on |
|---|---|---|
| Brick close | [image](../2026-09-15-material-look/brick-pom-v1-close-off.png) | [image](../2026-09-15-material-look/brick-pom-v1-close.png) |
| Brick middle | [image](../2026-09-15-material-look/brick-pom-v1-middle-off.png) | [image](../2026-09-15-material-look/brick-pom-v1-middle.png) |
| Brick far | [image](../2026-09-15-material-look/brick-pom-v1-far-off.png) | [image](../2026-09-15-material-look/brick-pom-v1-far.png) |
| Brick grazing | [image](../2026-09-15-material-look/brick-pom-v1-grazing-off.png) | [image](../2026-09-15-material-look/brick-pom-v1-grazing.png) |
| Brick RT | [image](../2026-09-15-material-look/brick-pom-v1-rt-off.png) | [image](../2026-09-15-material-look/brick-pom-v1-rt.png) |
| Terrain close | [image](../2026-09-15-material-look/terrain-pom-v1-close-off.png) | [image](../2026-09-15-material-look/terrain-pom-v1-close.png) |
| Terrain middle | [image](../2026-09-15-material-look/terrain-pom-v1-middle-off.png) | [image](../2026-09-15-material-look/terrain-pom-v1-middle.png) |
| Terrain far | [image](../2026-09-15-material-look/terrain-pom-v1-far-off.png) | [image](../2026-09-15-material-look/terrain-pom-v1-far.png) |
| Terrain grazing | [image](../2026-09-15-material-look/terrain-pom-v1-grazing-off.png) | [image](../2026-09-15-material-look/terrain-pom-v1-grazing.png) |
| Terrain RT | [image](../2026-09-15-material-look/terrain-pom-v1-rt-off.png) | [image](../2026-09-15-material-look/terrain-pom-v1-rt.png) |

[Brick run](../2026-09-15-material-look/brick-pom-v1-result.json) and
[terrain run](../2026-09-15-material-look/terrain-pom-v1-result.json) pass capture
integrity checks with no source/binary changes, validation errors or missing /
uniform screenshots. All sampled VT queues are zero; final occupied slots are
76 and 347 respectively. Terrain requests a few additional pages with displaced
sampling, so these are matched-view comparisons, not frozen-residency buffers.
The source manifests alongside each result contain exact camera/command lists.

Manual review: brick depth is present, but the outlines remain too regular and
paint breakup still needs work. Terrain remains smooth/painted, with thin breaks
visible in the close POM view that need chart-boundary diagnosis. Both retain
the blue lighting cast and raster/RT presentation differences. None of these
images constitutes aesthetic or full seam acceptance.

Other global renderer preferences are not fully frozen. Capture elapsed time
includes startup, frame waits and readback; it is not a generation benchmark.
Loader messages about absent Epic overlay JSON and shader-interface warnings
remain in the logs; there are no Vulkan validation errors.

## Remaining work and cost

Pool accounting remains nine logical bytes per texel plus 16 bytes of metadata
per physical slot, as documented in the height checkpoint. This change adds
POM sampling work; incremental page/shading cost has not been measured.
Do not infer performance improvement from the functional test timings.

Next visual work needs calibrated lighting, irregular masonry/paint structure,
rock-specific fracture/strata and diagnosis of the terrain breaks. Representative
nonlinear height filtering, full two-layer page/mip transitions, failed-replacement
POM views, splat records/indexing/local invalidation, contacts and the original
deferred correctness/performance gates remain required. Proxy silhouettes and
visibility remain the supported contract.
