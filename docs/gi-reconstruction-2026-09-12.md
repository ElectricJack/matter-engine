# Low-resolution GI reconstruction — 2026-09-12

The user likes diffuse bounce lighting at approximately one eighth of the
render width/height, but nearest-neighbor indirect sampling covered the detailed
image with coarse squares. Glass/refraction shared that same resolution and
damaged window bars and boundaries.

## Result and controls

Diffuse lighting and reflection/refraction now have independent resolutions.
Reduced diffuse is reconstructed as smooth lighting, then applied to the
full-resolution material. Direct lighting, shadows, geometry, normal maps and
parallax retain their normal rendering path.

```text
set render.gi.enabled true
set render.gi.trace_scale 0.128
set render.gi.reflection_trace_scale 1
set render.gi.diffuse_multiplier 1
```

These live settings are session-only. Both resolution defaults remain 1.
The editor labels them **Diffuse GI resolution scale** and **Reflections and
glass resolution scale**. Existing `trace_scale` now controls diffuse only;
this is a deliberate change from its former shared-resolution meaning.

The diffuse strength slider controls the amount of fill. Nonzero values do
not reduce ray work; zero skips diffuse rays. The overall GI enable switch
still controls all three indirect signals together.

## Implementation

- Reduced diffuse raygen writes incident lighting before primary albedo,
  metalness and AO multiplication. The existing temporal and spatial filters
  therefore smooth illumination without smoothing brick colors or mortar AO.
  Black, metallic and AO-zero source texels still trace incident lighting so
  they can supply compatible neighboring receivers.
- Composite gathers four bilinear neighbors with material/instance, normal
  and metric receiving-plane weights. It uses the same snapped source texel
  coordinates as raygen. A bounded 3×3 fallback searches for compatible support
  at thin receivers; absent support contributes zero indirect instead of taking
  light from another surface. Geometry derivatives are trusted only inside a
  matching identity quad and fall back at incompatible boundaries.
- Full-resolution destination albedo × (1−metalness) × AO is applied after
  reconstruction. Gain is applied once in raygen, as before. Reduced diffuse
  spatial filtering also rejects different instances to prevent earlier mixing
  from defeating the final surface rejection.
- Reflection/transmission have their own raw, auxiliary, history and filtered
  extents. The shared raygen uses separate masked dispatches only when extents
  differ. Matching extents retain the existing combined dispatch. Primary local
  direct stays full rate. Per-image diagnostic readbacks respect each extent.
- Full-rate diffuse retains the prior radiance storage/sampling contract.
  Reduced raw/accumulated diffuse diagnostics instead represent incident light.
  The previous radiance-derived firefly cap is retained; reduced incident
  channels also clamp to ±255 so squared luminance moments fit in RG16F.

This is a reconstruction improvement, not a new lighting cache or extra GI
bounce implementation. No material texture is blurred and no new bake is needed.

## Visible acceptance and limitations

Before/after captures are under `C:/tmp/castle-gi-reconstruct/{before,after}/`.
Both use CastleUpgraded at visible 1280×720 Native resolution, the same hall
and gold/window cameras, diffuse strength 1 and diffuse scale 0.128. Before uses
the former shared scale; after keeps reflections/glass at 1. The after capture
enables Vulkan validation and additionally exercises scale 0.125, camera motion,
the reverse resolution split and return to full resolution.

Brick/mortar detail, floor joints, furniture edges and window bars are visibly
clearer. Warm bounced fill remains. The one-eighth diffuse signal can still show
soft blotchiness, incomplete fill on unsampled thin surfaces and temporal settling
after movement. Full-resolution reflections may retain their own sampling noise.
Lowering reflection resolution still loses sharp detail; its reconstruction has
not been changed into the diffuse filter.

## Validation and performance

Native MSVC editor and Vulkan smoke targets built successfully. Visible native
`raster`, `rt`, `rt-transmission` and `rt-local-direct` gates all passed with zero
Vulkan validation errors. Added checks cover odd 321×201 dimensions at 0.128,
independent reflection extent, reverse split and scale round trips. Existing
submission/history and smooth-transmission compatibility checks passed.

The old eight-probe fixed-seed rough-metal test initially failed because the new
full-resolution reflection default selected different source texels and random
seeds. Its established 0.5 reflection grid is now explicit, retaining the original
assertions; full-resolution glass is exercised separately. The AO-zero raw
radiance test explicitly uses full-rate diffuse because reduced buffers now store
incident lighting. Shader/renderer behavior was not loosened to pass those tests.

Same visible 1280×720 Native hall on RTX 4090, four primary local-area samples,
weighted secondary sampling, reference POM, MAILBOX presentation, 15s warmup and
20s sampling. Both windows were foreground throughout and unminimized.

| Mode | Median frame | p95 frame | Median FPS | Raw GPU median |
|---|---:|---:|---:|---:|
| Before: all indirect at 0.128, nearest sampling | 7.964ms | 9.043ms | 125.56 | 7.812ms |
| After: diffuse 0.128 reconstructed, reflections/glass 1 | 9.621ms | 10.958ms | 103.94 | 9.337ms |

This is a measured cost of about 1.7ms/frame for the combined reconstruction and
reflection-resolution change, not an isolated shader microbenchmark. GPU clocks
were not locked. Raw GI tracing medians were 0.694ms and 1.294ms; denoising medians
were 0.283ms and 0.579ms. Primary-direct timing also varied between runs, so those
individual deltas should not be attributed entirely to the new filter.

Exact commands, executable hashes, environment and raw performance JSON are in
`C:/tmp/castle-gi-reconstruct/perf-{before,after}/`. Native gate logs are in
`gates/`. The standalone `comparison.html` beside these folders provides a
before/after wipe for the hall and gold/window views. Python compilation and
diff whitespace checks passed.
