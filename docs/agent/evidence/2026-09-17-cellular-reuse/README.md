# Reusing cellular surface queries — 2026-09-17

Status: compiler-directed reuse passes native validation, a controlled
page-fill comparison and the final scene capture audit. This continues the
StreamMountain material pass without changing its authored appearance. The
full terrain/building/layer/contact/performance goal remains open.

## Finding and bounded change

Terrain vertex classification already returns the direct source's constant
carrier weight; it does not evaluate the new texture fields at every vertex.
The material does, however, request the same cellular neighborhood three times
per surface evaluation: gap, site attribute and nearest distance.

The evaluator now obtains those features together. CPU reuse is local to one
sample and checks evaluated coordinates and seed. The candidate GPU compiler
proves reuse from immutable source registers and seed. An internal reuse opcode
reads the prior tuple, avoiding live coordinate/seed keys and runtime equality
checks in the shader. A different cellular query replaces the tuple. The tuple
is local to each invocation; neighboring texels, height derivatives and layers
cannot inherit it. The public DSL, source op cap, physical register cap, height
envelope and terrain recipe are unchanged.

## Evidence and rejected experiment

Manifests and native logs use `cellular-reuse-*` in the sibling
`2026-09-16-shared-vt-pixels` directory. The test fixture deliberately changes
position and seed, then returns to the original query; all queries contribute
to its output. This checks stale reuse on CPU and on physical GPU height pages.
Existing large-reference, outer-ring and page-regeneration checks remain.

- `v1`: unoptimized baseline. New CPU/GPU correctness tests pass. Nine
  16-page GPU timestamp samples have median 1.983104 ms, or 0.123944 ms/page.
  Only two warm-up batches precede these initial samples. Fingerprints include
  albedo, normals, ORM, AUX and height at both mips for
  each of the three features.
- `v2`: runtime coordinate/seed-keyed GPU cache. Checks pass and all six page
  fingerprints equal the baseline. Initial timing spikes and a 0.123588 ms/page
  median do not establish a useful speedup. The unchanged-binary repeat has a
  slower region around 3.68 ms/batch. This GPU approach is **not retained**, in
  favor of the simpler compiler proof. These timings did not separate shader
  overhead from clock ramp-up and competing work; they do not diagnose a
  register-pressure regression.
- `v3`: compiler-directed GPU reuse removes the runtime key state. Validation
  passes: surface runtime, actual mountain recipe, GPU composition/regeneration,
  raster/RT direct source, composed POM and the separate GPU geometry-face
  material consumer. The mountain recipe compiles to one neighborhood search
  and two reused feature reads. No gate skips RT. All six physical-page hashes
  match the original baseline. Sources/binaries stay unchanged in every run.
- `v4-reference`: temporary host-packer control disables reuse while preserving
  the same GPU shader binary. This is an experiment, not the retained compiler.
  Its benchmark includes at least one second of continuous warm-up work.
- `v5`: restores the v3 compiler exactly. Production sources match validated
  v3 byte for byte; only the compositor test's warm-up differs. Both the test
  and editor targets build successfully. Native compositor validation passes.

## Controlled warmed result

`nvidia-smi` records the GPU starting at its 210 MHz idle clock, then reaching
2745 MHz in both variants. Two short warm-up fills were insufficient. The new
benchmark sustains work for at least one second, then records nine 16-page
GPU timestamp samples. It does not discard slow measured samples.

`compare_warm.py` alternates the immutable reference and retained executables
twice each. Their original build manifests bind each binary to its source
variant; all current working sources and shader hashes are also checked.
The four runs pass, with identical albedo/normal/ORM/AUX/height fingerprints
for all six feature/mip cases. Shader binaries are identical between controls.

Across 18 measured 16-page batches per variant, the median is
**0.124326 ms/page repeated versus 0.121253 ms/page reused**, an observed
**2.47% reduction**. The optimization removes two of three neighborhood
searches, but those searches account for only a small part of this fixture's
total fill cost. It is not a threefold texture-generation speedup.

- [Alternating measurements and audits](warm-comparison.json)
- [Initial, insufficiently warmed comparison](comparison.json)
- [Shared shader identities](shared-shader-hashes.json)
- [Geometry-face material check](cellular-reuse-v3-face.json)

The temporary disabled-reuse line is removed from production. The final header
matches `vt_surface_tape-compiler-reuse.h` exactly.

## Actual StreamMountain comparison

The [v4 scene capture](../2026-09-17-mountain-cellular/v4/audit.json) exits 0,
retains all ten promised images, reports zero Vulkan validation/command errors,
and preserves source/binary hashes. It restores original props and closes its
own editor. All views settle at 789 variants with an empty page queue.

Against the preceding v3 recipe capture, overview and close albedo/normals
differ by at most one 8-bit level. Grazing foreground (rows 460 onward) also
stays within one level. Larger grazing differences affect 107 albedo pixels
and 496 normal pixels, confined to the distant foliage band at rows 226–392;
the entire grazing image is not claimed identical. Lit images differ with
unfrozen temporal lighting. The [pixel comparison](scene-comparison.json)
retains full-frame metrics and difference bounds.

- [Close POM](../2026-09-17-mountain-cellular/v4/close-lit.png)
- [Close POM disabled](../2026-09-17-mountain-cellular/v4/close-flat.png)
- [Grazing POM](../2026-09-17-mountain-cellular/v4/grazing-lit.png)

Thirty-sample G-buffer medians are 28.911 ms overview, 18.528 ms grazing and
5.989 ms close. Root bake is 55.740 s (41.485 s publish), and streaming reports
2,586 sectors in 170.52 s. Total capture is 289.294 s. These remain non-isolated
observations; the page-fill micro-optimization does not resolve whole-scene
loading. Distant material character, cliff structure, contact blending,
lighting/motion review and the original acceptance gates remain open.

The frozen r2 editor remains independently open on the same GPU. Timing data
is developmental and non-isolated; none of it establishes whole-scene bake or
frame-time acceptance. Original and rejected shader sources are retained here.
