# Finite brick source materials and filtering — 2026-09-15

**Status: source preparation and sampling pass; wall VT integration is open.**
All six orientations of eight real clay solids now have GPU-baked appearance
at their reconstructed 3D hits. Their finite sampler filters coherent channels
without repeating or stretching a wall image. This does not yet change the
`ClayBrickWall` inspection representation into a low-poly textured receiver.

Subsequent work resolves the normal-oriented microheight and connects these
sources to the C++ VT compositor. See the [finite VT integration checkpoint](../2026-09-15-stamp-vt/README.md)
for current status, real brick-page readbacks and the compression correction.
The measurements below describe the earlier source preparation checkpoint.

## Visual evidence

These are plots of actual native GPU output channels, **not editor screenshots**.
The color plots convert linear RGB to sRGB for display. Gray outside a face
indicates missing source coverage. Geometry depth is relative to the nominal
face; the material's much smaller normal-oriented height is shown separately.

- [Six-face channel comparison](gpu-face-channels.png)
- [Eight front-face material variants](gpu-clay-variants.png)
- [GPU filtering at 1, 4, 16, 64 and 256 mm footprints](gpu-finite-filter.png)

The broad faces retain actual dents, scratches, pores and edge chips. Source
material supplies quieter firing-color variation, roughness and fine grain.
The end faces still have much less geometric damage; tops/bottoms are largely
flat away from the edge chips. These remain art observations, not realism
acceptance. A lit wall with mortar, peeling paint and moss is still required.

## Implementation

`face_material_bake.*` validates a complete projected face, reconstructs each
covered pixel's source-space hit and normal, and packs the existing shared
surface program. `gpu_face_material_vk.*` evaluates that same program using
`vt_surface_tape.glsl`; it does not implement another noise/material language.
Local source recipes are reusable. World/field-dependent weathering is rejected
at this stage and belongs to receiver/layer composition.

The material result contains linear RGB, ORM, the geometric normal perturbed
by source microheight, normal-oriented microheight in metres, and coverage.
Appearance modifiers receive the actual footprint on both CPU and GPU. Material
identity includes geometry identity, program identity, footprint and evaluator
version, while excluding scheduling generation. Appearance edits preserve the
geometry dependency. Geometry-cache behavior remains independently tested.

GPU material work uses bounded batches, retains device resources through
submission completion, checks cancellation/currentness between batches, and
publishes a complete result only. The cancellation test stops after one real
GPU submission and compares every retained output texel. A submission failure
poisons the service rather than reusing potentially in-flight resources.

`finite_surface_stamp.*` prepares an immutable mip chain for a validated
geometry/material pair. `finite_surface_stamp.glsl` supplies the matching
GPU sampler. Important rules:

- One physical U/V domain for all levels; no periodic address mode or edge
  color clamp. Finite filter support can overlap outside that domain.
- Coverage-premultiplied color, ORM, normals and both heights. Divide by
  coverage only after spatial/level filtering, avoiding dark edge fringes.
- Average squared roughness; take the square root after sampling. Normalize
  filtered normals at the end. Linear color remains linear during filtering.
- Area-weighted reduction retains all rows/columns of odd-sized levels.
- Select levels from the physical footprint and integrate its box over the
  selected mip pixels, with a minimum width of one source pixel. This becomes
  bilinear at magnification. Level selection bounds each lookup to at most
  3×3 texels, plus the adjacent level during transitions.
- Beyond the last mip, continue dividing covered area by footprint area.
  A finite brick/splat must become less opaque when smaller than one receiver
  pixel; merely clamping to a 1×1 mip does not provide that behavior.
- Validate finite output, coverage, normal length, identity, dimensions and
  channel ranges before publication. Cancellation/failure keeps the previous
  immutable source. Per-source float mip allocation is capped at 128 MiB.

**The two heights intentionally retain different directions.** Geometry depth
is along the projection axis. Material microheight is along the source surface
normal. Adding these scalars without resolving the receiver projection would
misrepresent sloped dents. The production receiver must resolve this before
publishing its single composed-height channel; the sampler does not claim to
have solved that conversion.

## Native validation

Canonical MSVC `RelWithDebInfo`, native Vulkan on the existing RTX 4090 device.
Accepted commands, exits, source hashes, binary hashes and validation scans:

- `source-material-v1`: CPU material/reference, projected-face cache and
  shared surface-field tests pass. Its first GPU attempt failed because the
  test world omitted required field values; it is retained as failed evidence.
- `source-material-v2/v3`: corrected GPU material fixture passes all 48
  faces. V3 adds analytic coordinate/footprint comparisons and cancellation /
  supersession after a completed GPU batch.
- [finite-stamp-v3-checks.json](finite-stamp-v3-checks.json): finite-source CPU
  tests, native GPU sampler test and editor build pass. No source changes
  occurred during the runs and no Vulkan validation errors were reported.
- [finite-stamp-v5-checks.json](finite-stamp-v5-checks.json): final GPU gate,
  including separate geometry-depth and microheight error limits, passes.
- [finite-stamp-v5-summary.json](finite-stamp-v5-summary.json): aggregate
  measurements, final source revalidation and hashes for every raw artifact.

There are **646,896 GPU sampling queries over 48 faces**, including every mip,
fractional level transitions, transparent boundaries, subpixel footprints and
invalid/extreme coordinates. Maximum dimensionless channel error is
`1.98129565e-5`. Maximum geometry-depth error is `1.15483999e-7 m`
(0.1155 micrometres), below the declared 0.25-micrometre gate; microheight
error is `1.51339918e-9 m`, below its 0.01-micrometre gate.

The material oracle covers all six orientations of seed 0, plus an analytic
source that exercises actual source coordinates and footprint-dependent
appearance. Maximum material channel error is `4.76837158e-7`, microheight
error `4.51109372e-10 m`, and normal component error `5.77419996e-6`.
Coverage agrees exactly. Changing GPU batch size produces bit-identical
material outputs on this device. The retained complete output also compares
exactly after cancellation and supersession.

CPU semantic cases cover odd-dimension area conservation, no color fringes,
RMS roughness, finite addressing, area attenuation for subpixel sources,
invalid data, partial output, cancellation, supersession, dependency identity
and distinct height axes. Existing cold/warm geometry-cache checks still run
for every face and require zero warm projector callbacks.

Failed attempts remain available:

- `finite-stamp-v2` compiled but Windows could not reopen the test executable
  during linking (`LNK1104`). A process query found no editor/test/link process;
  the next link succeeded without source changes. No specific lock owner was
  established.
- `finite-stamp-v4` added a deliberately tight 0.1-micrometre height diagnostic.
  Two sharply chipped faces exceeded it, at most 0.1155 micrometres. V5 declares
  a 0.25-micrometre float-filter comparison bound (1/4000 of the 1 mm source
  pitch), retaining independent bounds for dimensionless channels and material
  microheight. No production algorithm, projection tolerance or existing VT
  acceptance requirement was loosened by that diagnostic adjustment.

Material `host_ms` and submission waits in the log are host service diagnostics.
The material baker deliberately reports no GPU timestamp measurement. Neither
the test process wall time nor these waits establish runtime performance.

## Memory and remaining integration

The sampler's current float interchange record is **64 bytes per texel**.
All 48 mip chains total 1,485,456 texels / 95,069,184 bytes (90.66 MiB).
The largest individual chain is 2,796,224 bytes (2.67 MiB). The test processes
one face at a time; the aggregate is not a measured simultaneous allocation.
These figures exclude the geometry/material input patches and GPU scratch.
This is not the intended final source storage density. Block-compressed source
payloads, a checked prepared-artifact directory, warm artifact hits and bounded
upload/publication still need implementation and measurement.

Next required work, preserving the full design:

1. Bind prepared finite sources through immutable VT source/layer snapshots.
   Production page composition must use the same sampler as this probe and
   preserve the previous complete snapshot while replacements are pending.
2. Feed the existing physical wall layout to receiver geometry, finite source
   selection, mortar and weathering. Source variants must match the layout's
   actual dimensions. This fixture uses the default 245×84×112 mm brick;
   the modular header wall uses 117.5 mm depth and must prepare that profile,
   rather than stretching these sources.
3. Resolve projected geometric depth and normal-oriented material detail into
   the receiver height/normal contract. Verify raster/RT/POM channel registration.
4. Add shared layer/splat binding, including candidate queries over the full
   footprint, paint loss revealing the actual substrate, and moss/deposit
   thickness. Keep source prototypes shared across placements.
5. Capture the resulting low-poly walls and terrain in the native editor;
   continue visual iteration, then complete all deferred correctness and
   performance acceptance. The composed POM chart-seam issue remains open.

## Reproduction

From the repository root in WSL:

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo finite_surface_stamp_tests
./tools/build-windows-from-wsl.sh RelWithDebInfo solid_face_projection_gpu_tests
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
```

Run the two test executables from the repository root using native PowerShell.
Set `MATTER_VK_VALIDATION=1`; setting `MATTER_CLAY_FACE_DUMP` to an existing
native output directory also writes the raw channel records and metadata.
The check JSON files retain the exact commands used. No editor is left running.

[raw-face-channels.zip](raw-face-channels.zip) preserves all 204 raw/metadata
files. `plot_channels.py <extracted-directory>` regenerates the three figures.
`summarize.py <raw-directory>` verifies the accepted source identity, summarizes
the logs and rebuilds the archive/manifest. It intentionally refuses a changed
source tree rather than claiming the old run validates new implementation.
