# Vulkan Hybrid GI and Material-Driven Ray Tracing Design

**Date:** 2026-07-14  
**Status:** Approved  
**Target:** MatterViewer Vulkan renderer, RTX hardware, 2560x1440 output at
60 FPS using DLSS Quality as the initial performance target

## 1. Goal

Bring the complete useful lighting feature set from the legacy GLSL and OptiX
renderers into the Vulkan RTX renderer without making every material pay for or
enable every effect.

The renderer remains hybrid:

- Rasterization owns primary visibility, GPU instancing, depth, stable material
  identity, and motion vectors.
- Vulkan ray tracing owns visibility, indirect diffuse lighting, glossy
  reflection, dielectric transmission, thin-surface scattering, and emissive
  secondary lighting.
- Engine-owned temporal accumulation and signal-specific denoising clean the
  stochastic lighting before DLSS Super Resolution reconstructs the final
  output resolution.

The result is not a literal transcription of either legacy implementation.
The old GLSL tracer had the broader material feature set, while the later
OptiX path had a simpler one-bounce model and unfinished temporal/denoising
code. This design preserves their intended features while using the canonical
Vulkan matrices, motion vectors, acceleration structures, and Streamline
integration already established by the current renderer.

## 2. Scope

### In scope

- Direct sun and sky lighting with ray-traced visibility.
- Diffuse global illumination and colored indirect bounce.
- GGX dielectric and metallic reflection, including rough reflection.
- Fresnel-weighted reflection and transmission.
- Refraction, index of refraction, total internal reflection, thickness, and
  Beer-Lambert absorption for glass, water, and other closed dielectrics.
- Thin-walled forward/back scattering for leaves and similar foliage.
- Emissive surfaces contributing at primary and secondary hits.
- Existing baked per-vertex ambient occlusion, including at secondary hits.
- Optional short-range ray AO only when baked AO is missing or explicitly
  requested.
- Temporal accumulation, disocclusion detection, firefly suppression, and
  signal-specific denoising.
- DLSS inputs and masks appropriate to dynamic reflection, transmission, and
  translucency.
- Runtime feature and quality controls, debug views, instrumentation, and a
  raster fallback.

### Out of scope for the first production milestone

- Replacing raster primary visibility with a full path tracer.
- DLSS Ray Reconstruction or Frame Generation.
- Spectral rendering, caustic photon mapping, or physically exact volumetric
  multiple scattering.
- A dedicated water shader model. Water must first be expressible as the same
  parameterized dielectric model used by glass.
- Optimizing beyond the initial 60 FPS at 1440p DLSS Quality target before the
  feature-complete reference scenes are correct.

## 3. Chosen Architecture

Use one hybrid, material-driven lighting integrator rather than independent
full-screen ray passes for every named effect.

The opaque raster pass produces the authoritative primary hit. A Vulkan ray
generation shader reconstructs that hit, evaluates direct lighting, chooses a
continuation lobe from the material, and traces a bounded stochastic path.
Closest-hit shaders return a reusable surface record rather than hard-coding a
particular lighting effect. This lets the ray-generation shader perform an
iterative path loop with a small, configurable bounce count and avoids
unbounded shader recursion.

This architecture preserves the fast GPU-instanced raster path and avoids the
cost and consistency problems of separate GI, reflection, refraction, and SSS
pipelines. A material with zero transmission never launches a transmission
continuation; an opaque rough surface does not pay for glass behavior; and a
thin leaf uses a scattering lobe instead of pretending to be a closed volume.

## 4. Material Contract

### 4.1 Authoring properties

Extend the canonical material definition with the following logical groups:

| Group | Properties |
|---|---|
| Base | albedo, roughness, metallic, opacity, emission color/strength |
| Specular | specular strength, optional specular tint |
| Transmission | transmission weight, IOR, absorption color, absorption distance, thickness |
| Scattering | subsurface weight, scattering color, scattering distance, anisotropy |
| Clearcoat | clearcoat weight, clearcoat roughness |
| Visibility | alpha cutoff, shadow opacity |
| Structure | a small surface-flags field |

Feature activation is value-driven:

- `transmission > 0` enables the transmission lobe.
- Nonzero absorption with a positive absorption distance enables colored
  attenuation.
- `subsurface > 0` enables thin-surface scattering.
- `clearcoat > 0` enables the clearcoat lobe.
- Nonzero emission enables emissive contribution.
- Zero weights disable their lobes and their associated ray work.

Flags are reserved for behavior that values cannot identify without
ambiguity:

- `THIN_WALLED`
- `DOUBLE_SIDED`
- `ALPHA_TESTED`
- `VOLUME_BOUNDARY`

Water should use generic dielectric values: IOR near that of water, low
roughness, transmission, and wavelength-dependent absorption. A water-only
flag or shader branch is added only if profiling or a demonstrated visual
requirement cannot be represented by the generic contract.

### 4.2 Compatibility and asset migration

Existing source materials remain valid by defaulting new lobe weights to zero,
opacity and shadow opacity to one, IOR to the existing value or one, and flags
to opaque non-volume behavior.

`MaterialDef` is embedded in current part artifacts and compared byte-for-byte
when loading. Expanding it therefore requires an explicit material-schema
version/fingerprint change and a one-time safe invalidation/rebake of cached
parts. The loader must reject old material payloads diagnostically rather than
misinterpreting their shorter layout. Material indices remain stable and
registry additions remain append-only.

The CPU authoring structure and GPU shading structure may have different
physical layouts, but `MaterialRegistryPackForGPU` remains the single explicit
conversion boundary. The GPU layout uses aligned `vec4`/`uvec4` records; shaders
must not depend on C struct padding or reinterpret integer flags as floats.

### 4.3 Energy and lobe rules

The shader computes normalized lobe probabilities from material values and
view-dependent Fresnel. Sampling one lobe per continuation does not mean all
lobes are evaluated every frame. The chosen contribution is divided by its
selection probability to remain unbiased within the implemented model.

- Metallic energy is assigned to specular reflection, not diffuse or
  transmission.
- Dielectric reflection and transmission split according to Fresnel.
- Total internal reflection redirects all transmission energy into reflection.
- Thin-walled foliage does not alter the ray's medium stack.
- Closed transmissive volumes update the current medium and apply absorption
  over traveled distance.
- Emission is accumulated before continuation termination.
- Clearcoat is an optional top lobe and defaults off.

## 5. Geometry and Secondary-Hit Contract

### 5.1 Raster vertex data

The current Vulkan vertex contains position, normal, baked/tinted albedo, and
ORM with baked AO and encoded emission. It lacks a material index, so a ray hit
cannot recover the authored material reliably.

Extend the Vulkan scene vertex/triangle contract to make these values available
to both raster and ray tracing:

- Position and shading normal.
- Material index.
- Tint or resolved base-color inputs required to match raster shading.
- Baked AO, with `1.0` meaning valid and fully unoccluded.
- UVs when alpha-tested or textured ray hits require them.

Material identity is flat per triangle even if stored alongside vertices. The
conversion path must verify that all three vertices of a generated triangle
resolve to the same material index or produce a dedicated triangle metadata
record. Interpolated properties are limited to normals, UVs, tint, and AO.

### 5.2 Device-addressable part table

BLAS geometry already resides in per-part pinned, device-addressable buffers,
and TLAS `instanceCustomIndex` already identifies the renderer part slot. Add a
GPU part table indexed by that custom index. Each entry supplies:

- Geometry buffer device address.
- Vertex stride/count and primitive count.
- Optional triangle-metadata address.
- Validity/generation information for diagnostics.

Closest-hit shaders use `gl_InstanceCustomIndexEXT`,
`gl_PrimitiveID`, and barycentrics to fetch the triangle, interpolate its
attributes, transform normals correctly, and return a compact surface payload.
The part table and every referenced buffer follow the same frame-fence lifetime
contract as BLAS/TLAS resources. Part-slot reuse cannot occur while an in-flight
TLAS can still reference the previous entry.

### 5.3 Alpha and transmissive visibility

The current BLAS marks all geometry opaque and the shadow trace forces opaque
ray flags. That remains the fast path for ordinary materials. Alpha-tested or
partially shadow-transmitting materials require non-opaque geometry plus an
any-hit shader that can:

- Reject alpha-cutout intersections.
- Attenuate or continue through configured shadow-transmitting surfaces.
- Terminate once remaining visibility is negligible.

Geometry build flags should be selected per part/geometry class where possible
so opaque parts keep the fastest traversal behavior. A mixed-material part may
require multiple BLAS geometries or conservative non-opaque handling; profiling
decides which representation is retained.

## 6. G-buffer Contract

Rasterization continues to produce:

- Linearizable depth in the canonical Vulkan zero-to-one convention.
- World-space shading normal.
- Base color and ORM/baked AO values.
- Motion vectors using current and previous jitter-aware transforms.
- Stable material index.
- Stable instance identity or a history-compatible hash sufficient for
  temporal rejection.

The exact attachment packing is chosen during implementation from supported
formats and bandwidth measurements, but material index must be integer-exact;
it must not be normalized into an 8-bit color channel. Emission may move out of
the currently overloaded normal alpha when the new layout is introduced.

Background pixels use an explicit invalid material/instance value and do not
trace surface continuations.

## 7. Lighting Integrator

### 7.1 Primary work

For each internal-resolution shaded pixel:

1. Reconstruct the raster primary surface from depth and G-buffer data.
2. Read the material from the GPU material table.
3. Accumulate emission.
4. Evaluate direct sun/sky lighting and trace required visibility rays.
5. Select one continuation lobe using material weights and Fresnel.
6. Trace the continuation and receive a surface record or environment miss.
7. Repeat until the bounce limit, negligible throughput, or Russian roulette
   terminates the path.

The initial quality target uses one stochastic continuation sample per shaded
pixel, up to two indirect bounces, and Russian roulette after the first bounce.
The bounce count remains runtime-configurable.

### 7.2 Direct lighting and shadows

Sun lighting uses next-event estimation at relevant path vertices. Shadow rays
reuse the existing RTX visibility foundation but add alpha/transmission-aware
behavior. Sky/environment lighting is sampled on misses and may later use
importance sampling without changing the material contract.

Direct sun is not multiplied by baked AO. Shadow opacity and alpha-test rules
affect visibility; diffuse AO does not.

### 7.3 Diffuse GI

Opaque nonmetallic surfaces can select a cosine-weighted diffuse continuation.
Secondary hit albedo, emission, baked AO, and direct sun/sky illumination create
colored bounce. Baked AO modulates local indirect diffuse/ambient response, not
the entire path throughput indiscriminately.

### 7.4 Reflection

Reflection uses a GGX microfacet distribution for metals and dielectrics.
Roughness controls the sampled lobe and denoising requirements; it is not
implemented as arbitrary cone jitter around a mirror vector. Extremely rough
specular paths may be disabled by a global quality cutoff only as an explicit
performance setting.

### 7.5 Transmission and refraction

Closed dielectric volumes use Snell refraction, Fresnel reflection,
front/back-face classification, and a bounded medium state. Distance traveled
inside the medium applies Beer-Lambert absorption using authored absorption
color/distance. Total internal reflection is handled without producing an
invalid direction.

Authored thickness is a fallback for thin or imperfect geometry. Closed volume
geometry should prefer actual entry-to-exit path length. The initial medium
implementation supports the air/current-material transition needed by common
glass and water scenes; deeply nested heterogeneous media are a later extension
unless a reference scene requires them.

### 7.6 Thin-surface foliage scattering

`THIN_WALLED` materials with nonzero subsurface weight use a dedicated leaf
model:

- Front lighting retains ordinary diffuse response.
- Back lighting uses a wrapped/forward scattering term controlled by scattering
  color, distance/thickness, and anisotropy.
- Sun visibility is sampled from the appropriate side of the surface.
- The ray does not search for a volume exit or modify the medium state.
- Double-sided normal orientation is made explicit to avoid camera-dependent
  flips.

This provides the legacy backlit leaf glow without treating leaves as solid
glass or paying for volumetric random walks.

### 7.7 Ambient occlusion

Baked per-vertex AO remains the default:

- It is interpolated for raster primary hits.
- The same AO is available through secondary-hit geometry fetches.
- It modulates indirect diffuse and local ambient/sky response.
- It does not darken direct sun, mirror reflection, emission, or transmission.
- Missing/unbaked AO resolves to `1.0` through an explicit validity convention.

Optional short-range ray AO is available only for dynamic/procedural surfaces
without baked AO or when explicitly enabled. It is not an unconditional ray per
pixel. Bent normals are a compatible future baker output but not required for
this milestone.

## 8. Signal Outputs and Denoising

Do not denoise one already-composited noisy color with a universal filter.
Produce at least these logical signals:

- Diffuse indirect radiance.
- Specular reflection radiance plus hit distance/roughness metadata.
- Transmission/refraction radiance plus hit distance/confidence metadata.
- Direct shadow visibility.
- Thin-surface scattering contribution when separate history improves quality.

Signal storage may alias images between sequential stages, but the logical
separation must remain observable in debug modes.

### 8.1 Temporal accumulation

Each signal reprojects with the existing Vulkan motion vectors and previous
canonical transforms. History is rejected or reduced using:

- Out-of-bounds reprojection.
- Depth/plane distance mismatch.
- Normal mismatch.
- Material index mismatch.
- Stable instance identity mismatch.
- Disocclusion or invalid motion/history markers.
- Camera cuts, resize, world reload, renderer recovery, DLSS mode changes, and
  material/lighting changes that invalidate the signal.

History length and moments are tracked per signal. Neighborhood clipping limits
reprojected radiance to a local luminance/chroma envelope before blending.
Firefly clamping occurs before a large outlier can persist in history.

### 8.2 Spatial filtering

- Diffuse GI uses edge-aware depth/normal/material filtering.
- Rough reflection uses hit-distance- and roughness-aware filtering.
- Sharp reflection and refraction use shorter history and narrower kernels.
- Thin foliage uses conservative history and strong disocclusion checks.
- Shadow filtering remains independent from indirect radiance filtering.

The first implementation may use an A-trous/SVGF-style compute filter. The
architecture keeps the denoiser engine-owned so a later vendor denoiser or DLSS
Ray Reconstruction experiment can replace stages without changing material or
geometry contracts.

## 9. Composition and DLSS

Filtered RTX signals are combined with raster surface response, direct
lighting, emission, and baked AO into a pre-tonemap HDR scene at the internal
render extent. DLSS Super Resolution runs after Monte Carlo denoising and before
UI composition.

DLSS continues to receive color, depth, motion vectors, jitter, exposure, reset
state, and the correct internal/output extents through Streamline. Reactive and
transparency/composition masks are generated for rapidly changing glass, water,
emission, particles, and foliage where the installed Streamline/DLSS version
supports those tags. Unsupported optional tags degrade cleanly; they do not
prevent native rendering or DLSS evaluation.

DLSS is a reconstruction stage, not the primary Monte Carlo denoiser. Raw noisy
radiance must not be passed to it in place of temporal/spatial lighting cleanup.

## 10. Runtime Controls and Instrumentation

Expose engine settings for:

- Master RTX lighting enable.
- Maximum indirect bounces.
- Lighting trace scale or checkerboard mode.
- Samples per pixel / adaptive sampling mode.
- Denoiser quality and iteration count.
- Maximum roughness for traced reflection.
- Global diffuse GI, reflection, transmission, scattering, emission, and AO
  multipliers.
- Optional fallback ray AO.
- Per-signal and history debug views.

Add GPU timestamps and counters for TLAS work, primary lighting rays, shadow
rays, continuation lobe counts, miss/hit counts, alpha any-hit work, temporal
passes, spatial passes, composite, and DLSS. Counter collection must be optional
and avoid CPU synchronization in the normal frame loop.

Useful debug views include material index/flags, raw and filtered signal AOVs,
path/lobe choice, bounce count, hit distance, AO source, history length, history
weight, rejection reason, reactive mask, and final pre-DLSS HDR.

## 11. Resource Lifetime and Failure Behavior

All new buffers, images, descriptor sets, part-table entries, and pipelines
follow the renderer's frame-slot fence ownership. No immediate submit, device
idle, or CPU readback is introduced in the frame path.

If Vulkan ray tracing is unsupported or initialization fails, the renderer uses
the existing raster lighting path. If an optional lobe or denoising stage fails
to initialize, the renderer reports the exact disabled feature and falls back
to the nearest valid lighting mode rather than presenting uninitialized data.
DLSS failure continues to fall back to native composite independently of RTX
lighting availability.

Shader reload, world reload, material-schema change, resize, and renderer
recovery explicitly reset affected temporal histories.

## 12. Delivery Sequence

1. Expand/version the material schema, GPU material table, and compatibility
   defaults; invalidate/rebake old artifacts safely.
2. Add stable material identity, secondary-hit geometry attributes, and the
   device-addressable part table.
3. Generalize the shadow-only RT shaders into reusable surface queries while
   preserving a working shadow milestone.
4. Add unified direct lighting and one-bounce diffuse GI with baked AO.
5. Add temporal accumulation, history diagnostics, and diffuse denoising.
6. Add GGX reflection and reflection-specific temporal/spatial filtering.
7. Add closed-dielectric transmission/refraction, Fresnel, IOR, thickness, and
   absorption for glass and water.
8. Add thin-walled foliage scattering/translucency.
9. Add emissive secondary lighting and optional fallback ray AO.
10. Add the second bounce, Russian roulette, adaptive quality controls, and
    detailed ray/timestamp instrumentation.
11. Generate supported DLSS reactive/transparency masks and validate the full
    pre-tonemap HDR path at 1440p output.
12. Tune against reference scenes to meet the 60 FPS acceptance target without
    weakening the correctness/debug contracts.

Each step must leave the viewer demoable. Feature flags permit incomplete later
lobes to remain disabled while already-completed lighting paths continue to
work.

## 13. Validation and Acceptance Criteria

### CPU and shader-contract tests

- Material defaults preserve existing opaque materials.
- GPU packing round-trips every field and flag with explicit offsets.
- Old part/material payloads fail with a clear rebake reason.
- Fresnel limits, Snell refraction, total internal reflection, Beer-Lambert
  absorption, lobe probabilities, and energy bounds have deterministic tests.
- Missing baked AO resolves to one; baked AO never affects excluded lighting
  terms.

### GPU scene tests

- Secondary hits recover the correct part, instance, primitive, barycentrics,
  transformed normal, material index, tint/UV, and baked AO.
- Opaque geometry stays on the opaque traversal fast path.
- Alpha cutouts ignore transparent intersections; shadow-transmitting surfaces
  attenuate without infinite any-hit loops.
- Resource replacement and part-slot reuse do not produce stale device-address
  reads under multiple frames in flight.

### Deterministic reference scenes

Provide small selectable scenes for:

- Opaque diffuse color bounce and baked AO.
- Metal, mirror, and rough reflection.
- Clear and colored glass, including total internal reflection.
- Water with Fresnel reflection and depth-dependent absorption.
- Emissive lighting.
- Front-lit and backlit thin foliage.
- Dynamic-object AO fallback.
- Camera translation/rotation, disocclusion, resize, and mode switching.

Where stochastic output prevents exact image equality, use fixed seeds and
bounded numeric/image metrics for test captures. Manual sign-off is still
required for temporal stability and material appearance.

### Runtime gates

- Zero Vulkan validation errors in native, RTX-disabled, per-feature RTX, DLSS
  Quality, and Native-to-DLSS mode-transition runs.
- No history trails, invented edges, or reversed motion during camera movement
  in reference scenes.
- Correct fallback on non-RTX hardware and when Streamline/DLSS is unavailable.
- CUDA-enabled Windows build remains supported and ships the required signed
  Streamline/DLSS runtime files.
- Initial performance acceptance: 60 FPS at 2560x1440 output using DLSS Quality
  on the reference RTX system, with GPU timings identifying any missed budget.

## 14. Key Risks and Mitigations

- **Material schema invalidates cached assets.** Version/fingerprint it and
  perform an explicit one-time rebake; never parse old bytes as the new layout.
- **Secondary hits read stale addresses.** Pin geometry and retire part-table
  entries only after all referencing frame fences complete.
- **Mixed alpha/opaque geometry slows traversal.** Classify geometry and retain
  opaque BLAS flags wherever possible; measure mixed-part strategies.
- **One sample serves several lobes noisily.** Use probability-correct lobe
  sampling, separated AOV histories, and adaptive budgets based on measured
  variance.
- **Transmission history smears disocclusions.** Use stable material/instance
  rejection, shorter history, hit-distance metadata, and DLSS masks where
  supported.
- **Foliage explodes ray counts.** Use thin-walled analytic scattering,
  alpha-aware traversal, conservative bounce limits, and counters.
- **A literal legacy port preserves old defects.** Validate behavior against
  reference scenes and equations, not byte-for-byte shader similarity.

