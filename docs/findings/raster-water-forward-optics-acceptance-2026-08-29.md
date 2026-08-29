# Raster-water forward-optics acceptance — blocked

**Date:** 2026-08-29
**Implementation base:** `fdd53b85c1c11d21d21d515c5fbe7cda0997f085`
with source fixes `cbc09b71` and `24015965`; the acceptance tooling and this
evidence are in this finding's containing commit
**Verdict:** **NOT ACCEPTED** — automated implementation gates pass, but the
waterfall/plunge and section-handoff visual criteria do not.

`ROADMAP.md` remains unchanged. In particular, current `Now` items 3 and 4
stay open, and no `test(water): accept forward optics in RiverFloatLab`
checkpoint was created.

## Environment

- Native MSVC RelWithDebInfo build, Vulkan 1.4.341
- NVIDIA GeForce RTX 4090
- NVIDIA driver 610.74 (`0x98928000`)
- Performance internal/output resolution: 1280 x 720, native DLSS mode
- Visible acceptance capture: 1280 x 720 editor image; 496 x 495 live viewport

## Automated evidence

- Strict comparator/runner tests: **13/13 passed** in 0.209 s. The suite
  rejects a wrong world, a wrong shadow-sample tag, and a runner that can
  reuse a pre-existing performance file.
- Complete native MSVC build: **passed**.
- CPU-labeled suite: **59/59 passed** in 200.46 s.
- RTX Vulkan smoke modes `water-forward`, `water-animation`, and default:
  **ALL PASS**, each with zero Vulkan validation errors.
- `water-animation` smoke: water RT decode = 0, water BLAS = 0, water TLAS
  insertions = 0, water RT records = 0.
- The final clean candidate completed the 1-shadow performance row before the
  visual failure stopped the remaining long runs:

| Samples | Baseline median | Candidate median | Median limit | Baseline p95 | Candidate p95 | p95 limit | Result |
|---:|---:|---:|---:|---:|---:|---:|:---|
| 1 | 2.91865 ms | 3.12280 ms | 3.91865 ms | 7.01110 ms | 7.01170 ms | 9.01110 ms | pass |

That clean row also reported:

- `water_forward_image_bytes = 11,059,200`, exactly
  `1280 * 720 * 12` logical bytes;
- `water_animation_decode_dispatch_delta = 0`;
- `water_animation_steady_state_allocation_delta = 0`;
- `validation_errors = 0`; and
- `gpu_water_forward_ms = 0.37117`.

The definitive 10- and 16-shadow rows were **not completed** after the native
visual review failed. The candidate directory contains older/mixed-generation
JSON at those names, so it must not be passed to the comparator or quoted as
final evidence. A future rerun must use a newly cleared candidate directory.

## Visual evidence and verdicts

All five final material-remap plus bounded-foam-lobe images were inspected at
their original 1280 x 720 size. These are local ephemeral artifacts in this
worktree, not portable files tracked by this finding:

- `build/qa/raster-water-forward-2026-08-29/candidate/screenshots/shallow-player-low.png`
- `build/qa/raster-water-forward-2026-08-29/candidate/screenshots/upper-rapids.png`
- `build/qa/raster-water-forward-2026-08-29/candidate/screenshots/waterfall-side.png`
- `build/qa/raster-water-forward-2026-08-29/candidate/screenshots/plunge-pool.png`
- `build/qa/raster-water-forward-2026-08-29/candidate/screenshots/section-handoff.png`

| Criterion | Verdict | Evidence |
|---|---|---|
| Shallow bed legibility and depth absorption | **Pass** | The bed remains visible and deeper pockets darken/shift toward teal. |
| Rapids foam lanes and downstream advection | **Partial / fail overall** | Fine foam/highlight filaments follow the channel. Two same-camera frames 15 rendered frames apart changed 191,744 of 246,512 viewport pixels, with mean absolute RGB change 6.11 / 5.23 / 7.87. This proves temporal scene change only: moving props and the unmasked whole-viewport comparison cannot establish downstream displacement of foam features. Local frames: `build/qa/raster-water-forward-2026-08-29/diagnostic/advection-a.png` and `advection-b.png`. |
| Continuous waterfall/plunge whitewater without a proxy seam | **Fail** | The cascade is a pale, blocky/faceted curtain; impact whitewater is not convincingly continuous or localized. |
| Section-handoff optics, normal, foam, and animation continuity | **Fail** | A jagged change in coverage/detail remains at the join; normal/mesh continuity is not established. |
| Plausible reflection and clean screen-edge miss fallback in every view | **Partial / fail overall** | Sky/specular response and edge fallback are plausible, but the waterfall and join geometry prevents a convincing result in every required view. |
| One coherent shadow set | **Pass** | No duplicate animated/static-proxy shadow is visible. |

## Root cause and fixes retained

The original nearly uniform gray result was not a forward-optics tuning
failure. Animation artifacts retain legacy bake material 4, while the packed
water field records the dynamically authored RiverFloatLab material. Playback
was drawing with artifact material 4, so `water_sample_field` fail-closed on
the material mismatch and skipped refraction, depth optics, animated normals,
foam, and reflection.

The retained integration fix sends the authored runtime water material through
`AuthoredFluidRenderBinding` into every synchronized animation draw, while the
artifact/decode material remains unchanged as bake provenance. A regression
fixture explicitly uses artifact material 4 and authored material 19. The
forward shader also applies a tested, bounded whitewater radiance lobe driven
only by the existing Task 2 foam coverage; it does not change the accepted
foam driver or thresholds.

An explicit draw-ordinal diagnostic then isolated the remaining visual defect:

- Local ephemeral `build/qa/raster-water-forward-2026-08-29/diagnostic/identity-waterfall.png`:
  the entire visible cascade is one section draw. The faceting is **not**
  caused by overlapping section/handoff draws.
- Local ephemeral `build/qa/raster-water-forward-2026-08-29/diagnostic/identity-handoff.png`:
  upstream section, collar, and downstream section are three contiguous,
  non-overlapping ownership bands. The visible transition coincides with
  independently meshed collar cuts.

The remaining work is mesh-side: improve/smooth the animated cascade surface
and make section/collar boundary positions and shading normals continuous
across the independently generated products. That belongs with the roadmap's
existing reliable-longer-sections/spillway-handoff work. The Task 8 optics
acceptance must be rerun after that geometry work; changing foam thresholds or
weakening the visual gate would only hide the defect.

## Task 8 changed-file scope

Acceptance tooling and documentation:

- `MatterEngine3/tools/raster_water_forward_acceptance.timeline`
- `MatterEngine3/tools/raster_water_forward_acceptance.py`
- `MatterEngine3/tools/tests/test_raster_water_forward_acceptance.py`
- `MatterEngine3/tools/run_raster_water_forward_acceptance.ps1`
- `docs/agent/qa-cookbook.md`
- this finding

Integration regressions/fixes discovered by the real RiverFloatLab run:

- `MatterEngine3/src/matter_engine.cpp`
- `MatterEngine3/src/render/water_mesh_animation_playback.{h,cpp}`
- `MatterEngine3/tests/water_mesh_animation_playback_tests.cpp`
- `MatterEngine3/src/render/water_surface_reference.{h,cpp}`
- `MatterEngine3/shaders_vk/water_surface.glsl`
- `MatterEngine3/shaders_vk/water_forward.frag`
- `MatterEngine3/tests/water_surface_reference_tests.cpp`
- one narrow Task 7 assertion update in
  `MatterEngine3/tests/shader_source_tests.cpp`

No temporary diagnostic shader/render changes remain in source.
