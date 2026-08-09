# Atmosphere Presentation and Lighting Curves — Design

## Summary

This focused follow-up to `2026-08-08-physical-atmosphere-volumetric-clouds-design.md` fixes sky presentation and separates it from world lighting. It retains the physical atmosphere, keeps the `192x108` sky-view LUT initially, adds static final-display dither, lowers noon sky ambient, and introduces a shared direct-world-light sunset curve.

It does not add auto exposure, a full time-of-day profile, a legacy sky, a new atmosphere model, or a larger LUT unless the prescribed metrics fail.

## Current Findings

`VkAtmosphere` creates a `192x108` RGBA16F sky-view LUT with a linear sampler for compute generation. The scene renderer subsequently binds that LUT through the shared nearest `composite_sampler_`, which also serves intentionally nearest G-buffer-style sampling. This nearest environment binding is the banding root; LUT resolution is not the initial remedy.

The current lighting assembly emits one `sky_color` that feeds surface ambient, RT/GI environment work, and volumetric scattering. Consequently a sky presentation edit can alter irradiance. This design makes those values separate.

## Architecture and Frame Data Flow

```text
Atmosphere settings + sun direction
  -> VkAtmosphere sky-view LUT and irradiance coefficients
  -> resolved lighting snapshot
       visible_sky_rgb       -> composite background only
       direct_world_sun_rgb  -> raster/RT/fog/cloud direct light
       sky_irradiance_rgb    -> raster/RT/GI/fog/cloud ambient
       sun_disc_rgb          -> analytic disc presentation only
  -> HDR composite
  -> display transform: tone map, gamma, deterministic dither
  -> 8-bit output
```

The renderer owns all policy-derived values. Raster push constants, RT/GI constants, environment helpers, and `VkVolumetrics::set_lighting` consume the same resolved snapshot; no shader reconstructs its own elevation or ambient curve.

## Sky-View Filtering and Seam

Add a dedicated `sky_view_linear_sampler_` owned by the environment-descriptor owner. It has linear min/mag filtering, nearest mip selection, repeat U, and clamp-to-edge V/W:

```text
minFilter/magFilter = VK_FILTER_LINEAR
mipmapMode          = VK_SAMPLER_MIPMAP_MODE_NEAREST
addressModeU         = VK_SAMPLER_ADDRESS_MODE_REPEAT
addressModeV/W       = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
```

Bind it only to the sky-view environment descriptor. Keep the current nearest sampler for G-buffer, material, depth, and ID attachments. Do not make `composite_sampler_` linear globally. Irradiance-coefficient and cloud-shadow images retain their existing, purpose-specific sampling contracts rather than inheriting sky U-repeat.

The common sky-view helper wraps azimuth with `fract` and samples texel centres:

```text
u = 0.5 / 192 + fract(azimuth_u) * (191 / 192)
v = clamp(v, 0.5 / 108, 107.5 / 108)
```

The sky-view compute pass writes periodic first/last azimuth columns. Thus repeat filtering crosses a continuous circular signal; it does not blend arbitrary left/right edges or duplicate a texel. `192x108` is retained. A later resolution increase is allowed only if the vertical/edge/seam gates below fail after this binding and mapping are present.

## Stable Output Dither

Implement dither in `display_transform.frag`, after tone mapping and gamma and immediately before writing the normalized 8-bit target. It never changes the HDR composite, G-buffer, LUTs, irradiance, RT/GI, denoiser, or volumetric inputs.

Use one achromatic scalar per final physical output pixel. Use `ivec2(floor(gl_FragCoord.xy))`, a fixed 8x8 blue-noise-style rank permutation, and a deterministic integer hash of the 8x8 tile coordinate to cyclically rotate the 64 ranks. Every complete 8x8 tile therefore contains ranks 0 through 63 exactly once. Do not use frame index, jitter, time, camera position, DLSS phase, or random state.

For rank `r` in `[0,63]`:

```text
d = ((r - 31.5) / 63.0) * (0.5 / 255.0)
rgb_output = clamp(rgb_after_gamma + d, 0, 1)
```

The offset is bounded by ±0.5 LSB, has exact zero mean per complete 8x8 tile, and is the same in RGB so neutrals stay neutral. It applies across the whole final frame, including UI, and remains byte-stable for unchanged output size/content. Raw HDR and intermediate validation readbacks remain before this pass.

## Lighting Split

The resolved frame ABI explicitly carries four values:

| Value | Consumers | Must not affect |
|---|---|---|
| `visible_sky_rgb` | Composite background | irradiance, direct light, RT/GI, fog, clouds |
| `direct_world_sun_rgb` | Raster direct BRDF, RT direct/secondary direct, fog/cloud direct scatter | analytic disc brightness |
| `sky_irradiance_rgb` | Raster ambient, RT/GI environment/secondary hits, fog/cloud ambient scatter | visible background |
| `sun_disc_rgb` | Existing analytic disc path | direct-world elevation ratio |

`visible_sky_rgb` uses the existing physical-sky presentation controls and mapping at defaults, preserving the present background appearance. `sun_disc_rgb` uses the existing atmospheric solar radiance and existing disc-size thresholds. If the disc currently inherits the shared direct-light field, split it before multiplying the direct-world curve; otherwise leave it unchanged.

The direct base RGB remains extraterrestrial solar RGB × atmospheric transmittance × authored sun modifier × live sun tint × sun multiplier. Apply the direct ratio last and as one scalar, preserving atmospheric chroma. The base ambient is physical irradiance coefficients × authored sky chroma; apply only the ambient ratio. Never derive ambient from post-tone-mapped background pixels.

Visible-sky controls are presentation-only. They update only composite/background constants and must not change irradiance, GI/volumetric history, or exposure. The physical LUT remains the environmental source for lighting paths, controlled by the separate ambient properties below.

## Direct World-Light Curve

Let `e` be established sun elevation in degrees and `s` be sanitized `sunset_direct_ratio`. The initial curve is:

```text
e <= 0:        0
0 < e < 5:     s * smoothstep(0, 5, e)
5 <= e < 45:  s + (1 - s) * smoothstep(5, 45, e)
e >= 45:       1
```

Initial anchors are exact: `90° = 1.00`, `5° = 0.25`, `0° = 0`, and every negative elevation is `0`. The initial `s` is `0.25`; it is user-editable, so only that 5° anchor changes when the property is edited. `0–5°` and `5–45°` are the complete transitions. There is no hidden below-horizon tail or discontinuity at 5°.

Apply this exact resolved scalar to raster primary direct lighting, RT primary/secondary direct lighting, shadowed direct terms, cloud-shadow inputs, and volumetric direct scattering. Solid/cloud shadows can attenuate direct light but cannot restore it when `e <= 0`. The analytic disc is intentionally separate.

## Day and Twilight Ambient

Let `d = day_ambient_multiplier` and `t = twilight_ambient_multiplier`, after sanitization. Initial defaults are `d = 0.25` and `t = 1.00`.

```text
twilight_mix(e)       = 1 - smoothstep(-6, 5, e)
sky_ambient_ratio(e)  = mix(d, t, twilight_mix(e))
sky_irradiance_rgb    = physical_sky_irradiance_rgb * sky_ambient_ratio(e)
```

At and above `+5°`, ambient is 25% of today's effective sky ambient. The `+5°` to `-6°` range smoothly transfers to twilight ambient. At `-5°`, direct world light is exactly zero, while physically derived sky irradiance remains positive and becomes primary light for upward surfaces and fog. Below `-6°`, the multiplier remains user-selected but is not a floor: physical atmosphere irradiance must continue to fade naturally through deep night. Do not inject a constant night term or compensate with auto exposure.

The same resolved ambient RGB is used by raster, RT/GI, and volumetrics; twilight cannot light one of these paths differently from the others.

## Controls, Sanitization, and Compatibility

Keep all values in the existing `render.lighting` group and generic Lighting panel/FIFO machinery. Add these properties after existing sky controls:

| Property | Label | Default | Range | Meaning |
|---|---|---:|---:|---|
| `render.lighting.day_ambient_multiplier` | Day ambient | `0.25` | `[0,4]` | Physical irradiance multiplier at `e >= +5°` |
| `render.lighting.twilight_ambient_multiplier` | Twilight ambient | `1.00` | `[0,4]` | Physical irradiance multiplier at `e <= -6°` |
| `render.lighting.sunset_direct_ratio` | Sunset direct | `0.25` | `[0,1]` | Direct-world ratio at `e = +5°` |

Register `MATTER_DAY_AMBIENT_MULTIPLIER`, `MATTER_TWILIGHT_AMBIENT_MULTIPLIER`, and `MATTER_SUNSET_DIRECT_RATIO` following existing headless-control practice. FIFO names are exactly the property paths. UI docs must say: visible sky does not change ambient; ambient does not recolour the visible sky; Sunset direct does not affect disc presentation.

Centralize sanitization beside the current Vulkan lighting override sanitizer. Non-finite day/twilight values fall back to their defaults then clamp to `[0,4]`; non-finite sunset direct falls back to `0.25` then clamps to `[0,1]`. Existing elevation fallback/clamp stays `[-90,90]`. Resolved curve values are always finite in `[0,1]`; invalid derived RGB falls back to zero direct and a zero-safe/last-valid irradiance value rather than reaching shaders as NaN.

Older worlds/property state omit the new fields and receive these defaults. Existing atmosphere values, authored sun/sky modifiers, `sky_multiplier`, `sky_tint`, exposure, sun angles, and sun-disc size remain loadable. Existing sky controls retain their paths and default presentation appearance, but no longer form a hidden irradiance control. No schema conversion, legacy-sky option, or TOD profile is introduced.

## History, Resource, and Path Consistency

- Atmosphere coefficient and sun-direction edits retain existing LUT dirty behavior. A successful changed LUT invalidates GI and volumetric lighting history once.
- Day ambient, twilight ambient, and Sunset direct are lighting-source edits: update constants without LUT regeneration, request one GI reset, and invalidate volumetric temporal scatter history. They do not invalidate cloud density/optical-depth history.
- Visible-sky-only edits and output dither alter neither irradiance nor direct light; they invalidate no GI, volumetric, or cloud history and never change exposure.
- Create/bind the dedicated sampler transactionally with environment descriptors and retain it through the normal frame-resource fence. Failure reports an error and retains the previous valid descriptor/sampler; it is not silently accepted as nearest filtering.
- Raster, RT, and volumetrics receive a single resolved `VkSceneLighting` snapshot per frame. In particular, the RT secondary direct term and fog/cloud direct scatter must use `direct_world_sun_rgb`, and RT/GI/environment plus fog/cloud ambient must use `sky_irradiance_rgb`.
- Dither has no history and no presented-frame dependency. A resize changes the pixel lattice normally; same-size static output is deterministic.

## Validation

### CPU and property gates

- Test default direct ratios at `90`, `45`, `5`, `0`, `-5`, and `-12`: `1`, `1`, `0.25`, `0`, `0`, `0`; dense `[-90,90]` samples are monotonic and continuous at `0`, `5`, and `45`.
- Verify noon ambient is 0.25 of the former effective lighting input, not 25% of tone-mapped screen colour. Verify the `+5..-6°` blend is finite/continuous and its endpoint values are exact.
- At `-5°`, verify resolved direct is zero and physical ambient is positive for an upward receiver and fog. At `-12°`, verify no artificial floor has been added.
- Verify sanitization/fallbacks, older settings defaults, and that presentation edits leave irradiance/direct RGB unchanged while ambient edits leave visible sky RGB unchanged.

### GPU/image gates

- Descriptor test proves sky-view uses dedicated linear/repeat-U sampling while a representative G-buffer binding remains nearest.
- For 432 fixed vertical samples away from disc/horizon, GPU sampling differs from a CPU bilinear 192x108 LUT oracle by at most `1e-3` per linear channel. Where reference luminance slope exceeds `1e-4`, no identical-output plateau spans more than two adjacent samples.
- V=0/V=1 samples are finite and within the min/max of their clamped edge texel pairs. For 256 direction pairs immediately across the U wrap, relative RGB difference is at most `0.5%` (absolute `1e-3` near black), and seam finite difference is no more than twice the median adjacent-U finite difference.
- For a uniform post-gamma image on a multiple-of-8 viewport, dither is byte-identical over two static frames, lies in ±`0.5/255` before clamp, has absolute 8x8 mean at most `1e-8`, uses equal RGB offset, and has frame mean within `1e-6` of undithered output. Shader/ABI inspection rejects a temporal input.
- Raster/RT/volumetric captures at `90/5/0/-5°` prove equal direct ratios. At `-5°`, fog and upward receiver are readable but direct/shadow contribution is zero. At noon, lit-versus-occluded receiver comparison proves restored shadow contrast after ambient reduction.

### CLI/FIFO captures

Extend the existing one-process atmosphere shot harness; do not add a protocol. Wait for `viewer: bake ready` and `MATTER_CMD_FIFO: listening`, hold one camera and exposure fixed, and issue for each `90`, `5`, `0`, `-5`, and `-12` degree capture:

```text
set render.lighting.exposure_ev -2
set render.lighting.day_ambient_multiplier 0.25
set render.lighting.twilight_ambient_multiplier 1
set render.lighting.sunset_direct_ratio 0.25
set render.lighting.sun_elevation_deg <elevation>
get render.lighting.day_ambient_multiplier
get render.lighting.twilight_ambient_multiplier
get render.lighting.sunset_direct_ratio
stats atmosphere-presentation-<elevation>
shot <absolute-png-path>
```

Wait for `<png>.done` before compare/inspection, capture raster and RT where available plus a fog-enabled `-5°` scene, send `quit`, and retain a kill trap only as cleanup. Store transient output in `MatterEditor/build/validation/atmosphere-presentation/`; commit test assertions/metadata rather than captures.

## Acceptance Criteria

- The `192x108` LUT is linear with a correct periodic U seam; G-buffer sampling stays nearest; resolution changes only after a metric failure.
- Dither is final-display-only, deterministic, bounded, whole-frame, neutral, and zero-mean without shimmer.
- Visible sky and irradiance are independent. Noon ambient defaults to 25% of today's effective value.
- Direct world lighting preserves atmospheric chroma and defaults to `90=100%`, `5=25%`, `0=0`, `<0=0`; analytic-disc presentation remains separate.
- At `-5°`, direct is zero while upward receivers/fog remain positive/readable; deep night has no permanent ambient floor.
- New controls are manual existing-Lighting properties, sanitize predictably, preserve old content, work through FIFO, and are consistent in raster, RT, and volumetrics.
