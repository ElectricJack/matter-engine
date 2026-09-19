# Direct procedural source v1 — native implementation evidence

Partial L2a implementation. The full layered-material goal remains active.

The [later material/layer checkpoint](../2026-09-15-material-look/README.md)
contains current recipes, additional native verification and new terrain/brick
views. The manifests below describe the earlier source-v1 checkpoint.

## What works

`surfaces(s).source(material, recipe)` records a versioned complete material in
the existing scalar program. RGB, roughness, metallic, AO and metre-valued
height are evaluated directly by the GPU VT compositor. Height drives the
normal through physical centred differences. `s.footprint` supplies the local
texel edge length. No periodic source image or physics bake is needed for this
path. Source pages carry exact AUX alpha byte 1; legacy pages retain 255.
Raster and RT consumers suppress legacy Wang overlays and unrelated POM on
those pages. Existing legacy materials keep their established path.

See the [design's implementation boundary](../../../superpowers/specs/2026-09-14-layered-surface-texturing-design.md)
for syntax, limits and planned extensions.

## Native verification

Canonical Windows MSVC RelWithDebInfo builds; NVIDIA RTX 4090, Vulkan validation
requested for GPU suites. Commands, exits, elapsed whole-process times and
source hashes are retained in `*-checks.json`, `*-source.json` and raw logs.
No source edits occurred during native execution.

- `source-red`: the previous implementation rejects the new source/footprint
  contract, with two expected surface-field failures.
- `source-green`: catches a duplicate CMake registration; removed the newly
  added duplicate and retained the existing eval-world target.
- `source-green2`: surface-field suite passes. GPU source values and normals
  pass, but the edit fixture omitted the compositor invalidation notification.
  Added the same invalidation used by other source-edit fixtures.
- `source-green3`: compositor passes, including two mip footprints, exact source
  identity, analytical height normal, eviction/regeneration byte equality and
  source edits. Maximum channel and normal-component error is 0.003922 in the
  analytic fixture; declared format tolerances are 0.012 and 0.025 respectively.
- `eval_world_tests` initially crashed in its existing StreamMountain fixture:
  the fixture did not register the five forest materials now declared by the
  shipped world. Added those registrations and an early diagnostic return on
  world-load failure instead of dereferencing an empty material vector.
- `source-integration`: eval-world suite, direct-source raster/RT test,
  `vt-surfaces`, `vt-input-snapshot`, `surface-parallax`, `vt-rt` and `vt-feedback`
  all pass. All six GPU modes report zero validation errors. Editor rebuild
  succeeds. Direct-source raster and RT normal-component errors are 0.003922
  with both ground-detail and finished-surface carrier flags.

The native logs include existing Vulkan loader warnings about missing Epic
Overlay manifests and unused shader interfaces. They are not validation errors.
Whole-process durations include startup and shader/pipeline creation; they are
not material generation or steady-state performance measurements.

`final-verification.json` compares current sources and binaries to their run
manifests. The external before-snapshots and owned patch are under
`D:/tmp/matter-vt/20260915-direct-source/`; unrelated worktree changes are preserved.

## First brick capture

![First native procedural brick preview](brick-first-preview.png)

World: `ProceduralBrickProof`. This is an early native raster preview with a
4.8 x 2.5 metre wall, 320 x 180 mm running-bond pitch, 12 mm mortar gaps,
8 mm bevel profile, 12 mm recess and up to 0.8 mm paint thickness. Its graph
has 89 deduplicated operations. Per-brick variation and paint coverage affect
coherent source channels; the paint exposes the computed substrate.

`brick-receiver-source.json`, `brick-receiver-result.json` and
`brick-receiver.log` identify the successful run and image. The camera, lighting,
seeds and complete recipe are in the scene. This preview is **too blurry and
dark** and is not aesthetic acceptance. The retained early VT statistics were
sampled during startup, not after refinement; this capture does not prove
settled residency or final texture sharpness.

The first attempt inherited the default WorldSector scatter dependencies and
was cancelled before capture. The second used empty cells and showed that the
static wall root was not registered on this streaming route; its manually
captured frame was blank. Both logs/results are retained. The current fixture
emits its sole wall in the nearest origin sector. Empty surrounding cells emit
existing no-geometry/tracer warnings, so this scene still needs fixture cleanup.
The successful capture took 49.88 seconds including editor startup; this is not
an isolated source-generation timing.

## Settled brick density comparison

![Native procedural brick preview at 256 texels per metre](brick-density256.png)

Matched native captures use the same recipe, camera, lighting and executable.
Both wait for bake completion, idle and another 180 frames before recording
statistics and the screenshot. The default 16 texels per metre remains blurry
in [the settled baseline](brick-settled.png); this provides about five texels
across each 320 mm brick. Setting the existing prop chart override
`MATTER_VT_PROP_TEXELS_PER_METER=256` resolves the brick edges sharply. This is
a controlled quality experiment, not a change to global or terrain defaults.

The `brick-settled-*` and `brick-density256-*` manifests and logs record both
runs, with native exit 0, unchanged sources and unchanged executable. The
higher-density preview is still dark, overly regular and has very little paint
coverage. It demonstrates the procedural source path, not final visual quality.

## Still open

Persisted composed height/POM; height-aware layer semantics and roughness
filtering; shared splat contents/placement; filtered noise/SDF operators and
page-seam proofs; terrain recipe; source-artifact caching; selective physics
profiles and pose reuse; contact blending; native close/middle/far/RT views;
visual feedback and all deferred performance targets. Source v1 has full
coverage only, a single fallback identity, and the existing 96-op budget.
Height rejects interpolated receiver-field and normal/slope dependencies until
spatial derivatives are supported. Existing noise is not automatically filtered.
The brick preview uses provisional arithmetic structural variation, not the
final stable feature-ID API or general layer/splat stack.
