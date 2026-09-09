# Physical Atmosphere, Scalable Volumetrics, and Lit Clouds — Design

## Summary

Replace the stylized procedural sky with one shared, physically inspired
Rayleigh/Mie atmosphere; make the existing froxel volume resolution and
lighting quality runtime-configurable; and add cloud self-shadowing,
approximated multiple scattering, and world-space cloud shadows without
replacing the current camera-froxel renderer.

The design deliberately uses three representations at three scales:

1. Compact atmosphere lookup textures for planetary-scale sky and solar
   transmittance.
2. The existing camera-aligned froxel volume for visible fog and detailed
   cloud lighting.
3. A camera-centered, sun-aligned optical-depth clipmap for kilometre-scale
   cloud illumination and shadows on terrain, objects, and fog.

The shipped default is visibly better than the current renderer. A
`Current cost` preset retains today's volumetric pass structure and resource
size as an explicit performance escape hatch. The new physical atmosphere is
the only sky model; there is no user-selectable legacy sky mode.

## Current Renderer

The Vulkan renderer already has the right foundations:

- `sky_common.glsl` is shared by the composite and RT environment paths, but
  currently generates a fixed artistic gradient and procedural cloud tint.
- `VkVolumetrics` owns a `160×90×128` camera-frustum grid containing
  `vol_media`, two temporal `vol_scatter` volumes, and `vol_integrated`, all
  RGBA16F.
- `vol_density.comp` evaluates exponential ground fog, up to four authored
  cloud layers, and local volume emitters.
- `vol_scatter.comp` traces one TLAS sun-visibility query for one quarter of
  froxel columns per frame, applies a single Henyey-Greenstein scattering
  term, and fills the remaining columns temporally.
- The renderer already exposes volumetric debug views and `gpu_vol_ms`.
- The editor property registry and `MATTER_CMD_FIFO` already support generic
  `set/get`, deterministic camera placement, stats capture, screenshots, and
  clean process termination.

The missing pieces are atmospheric optical depth, medium transmittance toward
the sun, higher-order cloud illumination, a representation that exists beyond
the view frustum, and runtime-selectable grid dimensions.

## Goals

- Approximate an Earth-like Rayleigh/Mie sky and derive direct-sun color and
  intensity from solar elevation.
- Use the same atmospheric result in the background, direct surface light,
  RT/GI environment, reflections, fog, and clouds.
- Preserve authored sun/sky colors and multipliers as artistic modifiers.
- Preserve exponential low-lying haze and fog as a first-class medium.
- Give clouds convincing lit edges, dark cores, self-shadows, cross-layer
  shadows, and selectable approximated multiple scattering.
- Cast temporally stable cloud shadows on terrain, objects, vegetation, and
  ground fog over roughly 2–4 km around the camera.
- Parameterize froxel XY scale and depth slices through the existing property
  system and Lighting window.
- Provide manual presets plus visible expert controls, memory estimates, and
  per-pass GPU timings.
- Keep allocation failures and unsupported options non-fatal.
- Make engine-CLI-driven tests and screenshots part of every implementation
  milestone, not a final manual exercise.

## Non-Goals

- Full spectral atmospheric rendering.
- Full Monte Carlo volumetric path tracing.
- Weather simulation, fluid clouds, precipitation, or lightning.
- Planet-from-orbit rendering.
- Replacing view froxels with a world-space cloud renderer.
- Automatic quality scaling in the first version.
- A separate physical clear-air aerial-perspective pass. Authored fog remains
  responsible for near- and mid-distance haze in this phase.

## Decisions

| Topic | Decision |
|---|---|
| Overall architecture | Hybrid atmosphere LUTs + view froxels + sun-space optical-depth clipmaps |
| Sky compatibility | Physical atmosphere is the only sky model |
| Volumetric compatibility | `Current cost` disables the new cloud-lighting work while retaining the new sky |
| Quality control | Manual properties and presets; no automatic scaler |
| Froxel resolution | XY scale and depth slices are independent discrete properties |
| Cloud multiple scattering | Selectable 1–4 scattering octaves |
| Cloud self-shadowing | Short full-density local sun march plus remaining coarse optical depth from the sun-space volume |
| World shadows | Two camera-centered sun-space clipmap levels, approximately 2–4 km total coverage |
| Ground fog | Existing exponential fog remains; single-scattered by default |
| UI | Every setting is registered in the existing properties system and drawn in the Lighting window |
| Validation | Headless tests + GPU tests + CLI-driven screenshots and stats |

## Architecture

### Components

#### `VkAtmosphere`

Owns atmosphere parameters, pipelines, lookup textures, and the derived solar
radiance and sky irradiance data. It exposes immutable frame descriptors and a
small constants block; consumers do not read its private state.

#### `VkCloudShadows`

Owns two camera-centered, sun-aligned cloud optical-depth clipmap levels,
their update history, coordinate transforms, and filtering parameters. It
exposes world-position-to-transmittance sampling to volumetrics, composite
surface lighting, and RT lighting.

#### `VkVolumetrics`

Continues to own density, scatter, integration, temporal history, and
composition resources. It gains runtime grid dimensions, local cloud
self-shadow sampling, separate cloud and fog phase behavior, and selectable
cloud scattering octaves. Enhanced cloud lighting allocates an additional
R16F `vol_cloud_density` image containing full-resolution cloud extinction
only. `vol_media` remains the combined scattering/extinction field for fog,
clouds, and emitters. The separate scalar channel is what lets the scatter
pass apply cloud-only phase and multiple-scattering behavior without trying to
infer the medium type from a mixed color. `Current cost` does not allocate or
sample this extra image.

#### Shared cloud-density GLSL

Cloud height profiles, coverage, weather noise, base FBM, and detail erosion
move into a binding-free shared include. The froxel density and sun-space
clipmap passes must evaluate the same authored cloud definition. The shared
function returns both coarse and full density so long-range optical depth can
avoid expensive detail octaves while visible clouds retain them.

### Frame Flow

```text
Update atmosphere transmittance/multiple-scattering LUTs if dirty
  -> update sky-view LUT and irradiance coefficients if dirty
  -> reproject/update cloud optical-depth clipmap tiles
  -> inject froxel fog/cloud/emitter density
  -> evaluate geometry visibility + cloud transmittance
  -> accumulate selected cloud multiple-scattering octaves
  -> integrate froxel columns
  -> composite surfaces, cloud shadows, volumes, and physical sky
```

Atmospheric parameter changes invalidate atmosphere-dependent lighting and
GI history. Cloud-shape changes invalidate cloud-shadow and volumetric
history. Froxel dimension changes recreate only the froxel bundle and its
descriptors.

## Physical Atmosphere and Solar Lighting

### Model

Use a spherical Earth-like atmosphere with Rayleigh scattering, Mie
scattering, and ozone absorption. The atmosphere is physically inspired, not
spectral: coefficients are represented in linear RGB and scaled by a small
artist-facing property set.

The GPU resources are:

- A 2D transmittance LUT indexed by observer altitude and zenith cosine.
- A compact atmospheric multiple-scattering LUT.
- A sky-view LUT indexed by view direction relative to the sun.
- Nine irradiance coefficients derived from the sky view for diffuse surface
  and fog illumination.

The transmittance and atmospheric multiple-scattering LUTs update only when
atmosphere parameters change. The sky-view LUT and irradiance coefficients
also update when sun direction or observer altitude changes.

### Direct Sun

Direct solar radiance is assembled exactly once:

```text
extraterrestrial solar RGB
  × atmospheric transmittance at the current elevation
  × authored sun RGB modifier
  × render.lighting sun tint
  × render.lighting sun multiplier
```

Near zenith the sun remains bright and nearly neutral. Near the horizon it
becomes warmer and dimmer. Planetary occlusion removes direct sun below the
horizon while the sky-view LUT continues through twilight.

### Shared Consumers

- `composite.frag` samples the sky-view LUT and adds the solar disc.
- Primary surface lighting uses the derived direct-sun radiance.
- RT/GI misses and reflection environments sample the same physical sky.
- RT secondary-hit direct lighting uses the same sun radiance.
- Volumetric fog and clouds consume the same direct sun and sky irradiance.
- Cloud shadows attenuate that same direct-sun value.

Existing world `sun_color` and `sky_color` fields remain loadable. They become
artistic RGB modifiers rather than independent base sky models, avoiding a
schema break while removing the old procedural sky branch.

### Atmosphere Properties

Register `render.atmosphere` through the existing property system:

- `sea_level_y`
- `rayleigh_scale`
- `mie_scale`
- `mie_anisotropy`
- `ozone_scale`
- `ground_albedo`

Earth-like defaults are authoritative. The group is drawn by the generic
property renderer in the Lighting window, beside `render.lighting`.

## Fog, Cloud Density, and Phase Behavior

### Ground Fog and Haze

The existing exponential ground-fog injection remains unchanged in purpose:

- Density, floor, and vertical falloff define low-lying haze.
- Color remains scattering albedo.
- Wind and noise break up uniform layers.
- TLAS visibility creates geometry shadows and shafts.
- Physical sky and elevation-adjusted sun replace the old light inputs.
- Cloud optical depth attenuates direct sun reaching fog beneath clouds.

Ground fog remains single-scattered by default. Its lower optical depth does
not justify the cloud multiple-scattering cost. The existing `phase_g`
property becomes explicitly the fog/haze anisotropy control.

### Cloud Shape Extensions

Retain the existing four-layer schema and height/coverage controls. Add
optional per-layer fields with neutral defaults:

- Weather scale and influence.
- Detail scale and erosion strength.
- Density-shape bias for softer or sharper bodies.

Coarse density uses height, coverage, weather, and lower FBM octaves. Full
density adds detail erosion and remaining octaves. New fields default to no
effect so existing worlds retain their authored density until opted in.

In enhanced mode the density pass writes full cloud extinction to
`vol_cloud_density` while continuing to add cloud scattering and extinction
to `vol_media`. Clouds use a near-white water-droplet single-scattering albedo;
their sunset color comes from the physical sun and sky rather than inheriting
the ground-fog color. Ground fog and local emitters retain their authored
albedos. Per-layer colored cloud albedo is intentionally deferred.

### Cloud Phase

Clouds use a cloud-specific forward/backward phase approximation rather than
the ground-fog `phase_g`. This supports strong forward edge lighting and a
weaker backward lobe without making ground haze unnaturally directional.

## Sun-Space Optical-Depth Clipmaps

### Representation

`VkCloudShadows` owns a near and far shallow 3D texture. Each texture uses a
sun-aligned coordinate frame:

- Depth points along the direction of incoming sunlight.
- The two lateral axes cover the world around the camera.
- Each texel stores cumulative cloud optical depth from the sunward boundary.

The initial conservative configuration is approximately:

- Near: `256×256×32`.
- Far: `128×128×24`.
- Total useful coverage: 2–4 km, with a guard band at the boundary.

The actual dimensions, coverage, and per-frame update fraction are properties.

### Generation and Updates

The density stage evaluates coarse cloud density only. A prefix-integration
stage produces cumulative optical depth along the sun axis.

- Reprojection retains valid texels as the camera moves.
- Newly exposed tiles update first.
- Remaining tiles update in a rotating pattern.
- Cloud-authoring changes invalidate both levels.
- Large sun-direction changes invalidate the sun-space transform.
- Small sun changes may reproject, but correctness wins over retaining stale
  history.
- Generation pauses when direct sun is below the horizon.

Cloud motion still requires regular tile refresh. `update_fraction` exposes
that cost/latency trade-off through the property system.

### Sampling and Softness

Any world-space point can sample cumulative optical depth. Therefore the same
field provides:

- Broad cloud self-shadowing.
- One cloud layer shadowing another.
- Shadows on terrain, objects, and vegetation.
- Cloud attenuation of direct light in ground fog.
- Cloud-aware direct lighting at RT secondary hits.

Transmittance is continuous, `T = exp(-optical_depth)`, not binary. Filtering
grows with solar angular size and estimated cloud-to-receiver separation, so
high-cloud ground shadows are softer than internal cloud shadows. Clipmap
edges fade to clear transmittance rather than revealing a square boundary.

Ground fog is never injected into this clipmap and cannot cast kilometre-scale
projected shadows.

## Detailed Cloud Self-Shadowing

The clipmap captures broad, long-range optical depth but intentionally omits
expensive erosion detail. Visible cloud froxels refine it:

1. Start at the froxel world position.
2. March a configurable short segment toward the sun through full-resolution
   `vol_cloud_density`.
3. At the end of the local segment, sample remaining cumulative coarse optical
   depth from the sun-space volume.
4. Add the detailed local optical depth to the remaining coarse path.
5. Convert the total to direct-sun transmittance.

This replaces the near coarse segment rather than multiplying two complete
paths, preventing double counting. Sample positions are jittered and reused
temporally. The existing TLAS ray query remains a separate solid-geometry
visibility factor.

## Approximated Cloud Multiple Scattering

Expose `multiple_scattering_orders` from 1–4:

- Order 1 is direct single scattering.
- Each added octave reduces effective extinction, allowing light to penetrate
  farther into the cloud.
- Each added octave reduces phase anisotropy, approximating loss of direction
  across repeated events.
- Each added octave reduces energy so the sum remains bounded.

Expose `multiple_scattering_strength`; keep the extinction, anisotropy, and
energy decay constants internal in the first implementation. Add a modest
`powder_strength` property for edge/interior shaping, but do not use the
powder term as a substitute for optical-depth self-shadowing.

Multiple-scattering changes invalidate volumetric lighting history. Higher
orders apply only to cloud media, not ordinary ground fog.

## Runtime Froxel Resolution

Replace `VOL_W`, `VOL_H`, and `VOL_D` compile-time assumptions with dimensions
carried in renderer parameters. Dispatch sizes, slice mapping, composite
sampling, and bounds checks all consume those dimensions.

Register discrete properties under `render.volumetrics`:

- `froxel_xy_scale`: `0.5`, `0.75`, `1.0`, `1.5`, `2.0`.
- `froxel_depth_slices`: `64`, `96`, `128`, `192`, `256`.

XY scale multiplies the current `160×90` plane as a pair. Depth is independent.
The UI shows resulting dimensions, estimated persistent memory, and GPU time.
Four RGBA16F volumes consume approximately 59 MB at `160×90×128`; the UI must
make superlinear memory growth obvious. Enhanced cloud lighting adds one R16F
cloud-density volume (about 3.5 MB at the base grid), and the displayed memory
estimate includes it and the selected cloud-shadow clipmaps. `Current cost`
retains the four-volume footprint.

### Resource Replacement

Changing a dimension creates a complete replacement froxel bundle, including
images and dimension-dependent descriptors. The renderer swaps the new bundle
at a safe frame boundary and retires the old bundle through existing lifetime
tracking. History is invalid after a successful swap.

If creation fails:

- Keep the previous bundle active.
- Restore the last valid property value.
- Report requested dimensions and memory.
- Continue rendering.

Discrete values prevent repeated allocation while a slider is dragged.
Step-count and scattering-order changes require no resource recreation.

## Properties and Lighting Window

All settings are properties first; the UI is a view over the registry.

- `render.atmosphere`: atmosphere coefficients and sea level.
- `render.volumetrics`: enable, fog phase, temporal blend, froxel dimensions,
  local sun-march steps/distance, multiple-scattering orders/strength, and
  powder strength.
- `render.cloud_shadows`: enable, near/far coverage and resolution, depth
  slices, filtering, and update fraction.
- `render.clouds`: existing layer fields plus optional weather/detail fields.

The Lighting window claims and draws all four groups through the existing
generic property renderer. Do not add duplicate hand-written backing values.
The FIFO `set/get` commands must automatically reach every new property.

### Manual Presets

Preset buttons apply ordinary property edits as one transaction. Every value
remains visible and editable afterward; editing any field makes the displayed
configuration `Custom`.

| Preset | Froxels | Local sun march | Cloud scattering | Clipmap |
|---|---:|---:|---:|---|
| Current cost | `1× / 128` | Off | 1 order | Off |
| Improved, default | `1× / 128` | 8 steps | 2 orders | Conservative |
| High | `1.5× / 192` | 12 steps | 3 orders | Higher resolution/update rate |
| Ultra | `2× / 256` | 24 steps | 4 orders | Maximum configured |

The `Current cost` preset preserves today's density/scatter/integrate
structure and base volume size. It does not allocate `vol_cloud_density` or
cloud-shadow textures. It still uses the new physical atmosphere.

## Failure Behavior

- Sanitize all non-finite and out-of-range properties before GPU upload.
- Failed froxel allocation retains the previous bundle.
- Failed cloud-shadow allocation supplies transmittance `1` and reports the
  failure.
- Failed atmosphere regeneration retains the last valid LUTs.
- Initial atmosphere failure uses a neutral emergency sky and reports a
  renderer error; it does not expose a selectable legacy sky.
- Missing ray-query support retains the renderer's existing volumetric
  capability behavior.
- Non-finite optical depth resolves to clear transmittance and emits a
  diagnostic instead of blacking out the frame.
- Sun below the direct-light horizon skips cloud-shadow generation and direct
  sunlight while preserving twilight sky radiance.

## Testing and Agent Visual-Progress Contract

### Engine CLI Is the Primary Visual Harness

Implementation agents must automate visual and performance validation through
the engine's existing CLI/FIFO surface. Interactive mouse-driven inspection is
supplemental, not an acceptance gate.

The harness must launch one editor process, poll for `viewer: bake ready` and
`MATTER_CMD_FIFO: listening`, and then use commands such as:

```text
set render.lighting.sun_elevation_deg 5
set render.volumetrics.froxel_xy_scale 1.5
set render.volumetrics.froxel_depth_slices 192
set render.volumetrics.multiple_scattering_orders 3
get render.volumetrics.multiple_scattering_orders
cam <px> <py> <pz> <tx> <ty> <tz>
stats <label>
shot <absolute-png-path>
quit
```

The driver waits for `<png>.done` before reading or comparing a screenshot and
always sends `quit`, with a process-kill trap as a final cleanup. It should
extend `MatterEngine3/tools/viewer_shots.sh` or add a focused sibling script,
not create an unrelated control protocol.

New scripted matrices must cover:

- Atmosphere at sun elevations `90`, `45`, `5`, `0`, and `-5` degrees.
- `Current cost`, `Improved`, `High`, and at least one custom configuration.
- Cloud self-shadow, cross-layer shadow, ground shadow, and low fog.
- Near/far clipmap boundaries while the camera translates.
- A moving-cloud sequence with fixed camera and lighting.
- All supported froxel XY scales and depth-slice counts for smoke/stability
  coverage; representative combinations are sufficient for screenshots.

### Screenshot Progress Requirement

Screenshots are a required implementation artifact, not merely files produced
at the end. At each visual milestone, the agent must:

1. Run the engine CLI harness itself.
2. Inspect the generated PNGs and relevant stats/logs.
3. Provide representative screenshots directly in the conversation using
   absolute local paths and concise captions.
4. Include before/after or quality-tier comparisons when the milestone changes
   an existing visual result.
5. State the exact CLI property configuration represented by each image.

An agent may not claim a visual milestone complete by reporting that PNGs
exist without showing them in the conversation.

Store transient captures under a gitignored validation directory such as
`MatterEditor/build/validation/atmosphere-clouds/<milestone>/`. Deterministic
golden inputs or compact baseline metadata may be committed separately when
the implementation plan calls for them.

### Automated Tests

Headless and GPU tests must cover:

- Atmospheric transmittance against a double-precision CPU reference.
- Noon, horizon, sunset, and below-horizon solar behavior.
- Shared CPU/GLSL cloud-height and density invariants where practical.
- Cumulative optical depth is monotonic and transmittance stays in `[0,1]`.
- Constant-density slabs produce expected self-shadow values.
- Additional scattering orders brighten shadowed cloud regions without
  exceeding an energy bound.
- Cloud layers shadow themselves and one another.
- `FogLab` has no cloud-only contribution and retains low-lying haze.
- Froxel integration remains monotonic at every supported dimension.
- Repeated live resolution changes do not leak or use retired descriptors.
- Cloud-shadow reprojection has no hard clipmap seam during camera motion.
- Atmosphere, cloud, or resolution changes invalidate the correct histories.
- Vulkan validation reports no descriptor, layout, or lifetime hazards.

Use `MatterEngine3/tools/img_diff.py` for deterministic image comparisons.
CLI `stats` output is the performance record for automated runs.

## Performance Acceptance

Add separate GPU zones for:

- Atmosphere LUT generation.
- Cloud-shadow generation/update.
- Froxel density/scatter/integration.

Show each zone and their combined total in the Lighting/Performance UI and
emit them through CLI stats.

- `Current cost` must stay within 5% or 0.1 ms of today's volumetric cost at
  the same dimensions.
- `Improved` targets no more than approximately 2 ms combined on an RTX
  3070-class GPU at 1440p with DLSS Quality.
- Atmosphere steady-state cost should be negligible because LUTs are
  dirty-driven.
- High and Ultra are intentionally uncapped manual choices, but must remain
  stable and report their memory and time honestly.

## Acceptance Criteria

- The procedural sky is removed from every production lighting consumer.
- Moving the sun changes sky color, direct-sun color, direct-sun intensity,
  reflections, GI misses, fog lighting, and cloud lighting coherently.
- Low-lying exponential haze remains authorable and visibly lit.
- Clouds show direct edge lighting, dark optical-depth cores, self-shadows,
  and selectable brighter interiors from higher scattering orders.
- Clouds cast filtered shadows on other layers, fog, terrain, vegetation, and
  objects over the configured clipmap coverage.
- `Current cost` is a reliable performance escape hatch.
- Every froxel dimension option can be applied live or fails without losing
  the previous valid renderer state.
- All new settings are registered properties, persist through the existing
  property system, appear in the Lighting window, and are controllable through
  FIFO `set/get`.
- Automated CLI scripts produce screenshots, stats, and clean process exit.
- Implementation progress includes representative screenshots embedded in
  the conversation at each visual milestone.
- Headless, GPU, validation-layer, visual, and performance gates pass.
