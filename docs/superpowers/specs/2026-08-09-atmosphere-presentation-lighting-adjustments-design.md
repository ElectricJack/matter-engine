# Atmosphere Presentation and Lighting Curves — Design

## Summary

This focused follow-up to `2026-08-08-physical-atmosphere-volumetric-clouds-design.md` fixes sky presentation and separates it from world lighting. It retains the physical atmosphere, keeps the `192x108` sky-view LUT initially, adds static scene-display dither, lowers noon sky ambient, and introduces a shared direct-world-light sunset curve.

It does not add auto exposure, a full time-of-day profile, a legacy sky, a new atmosphere model, or a larger LUT unless the prescribed metrics fail.

## Current Findings

`VkAtmosphere` creates a `192x108` RGBA16F sky-view LUT with a linear sampler for compute generation. The scene renderer subsequently binds that LUT through the shared nearest `composite_sampler_`, which also serves intentionally nearest G-buffer-style sampling. This nearest environment binding is the banding root; LUT resolution is not the initial remedy.

The current lighting assembly emits one `sky_color` that feeds surface ambient, RT/GI environment work, and volumetric scattering. Consequently a sky presentation edit can alter irradiance. This design makes those values separate.

## Architecture and Frame Data Flow

```text
Atmosphere settings + sun direction
  -> VkAtmosphere sky-view LUT and irradiance coefficients
  -> atomic resolved atmosphere-and-lighting snapshot
       sky_display_modifier_rgb    -> background, RT misses, reflections
       sky_irradiance_modifier_rgb -> 9-SH diffuse/GI/fog/cloud ambient
       direct_world_sun_rgb        -> raster/RT/fog/cloud direct light
       sun_disc_rgb                -> analytic disc presentation only
  -> HDR composite
  -> display transform: tone map, gamma, deterministic dither
  -> 8-bit output
```

The renderer owns all policy-derived values. Raster push constants, RT/GI constants, environment helpers, and `VkVolumetrics::set_lighting` consume the same resolved snapshot; no shader reconstructs its own elevation or ambient curve. The 3x3 atmosphere irradiance texture remains nine directional SH coefficients, not a flat RGB replacement.

## Sky-View Filtering and Seam

Add a dedicated `sky_view_linear_sampler_` owned by the environment-descriptor owner. It has linear min/mag filtering, nearest mip selection, repeat U, and clamp-to-edge V/W:

```text
minFilter/magFilter = VK_FILTER_LINEAR
mipmapMode          = VK_SAMPLER_MIPMAP_MODE_NEAREST
addressModeU         = VK_SAMPLER_ADDRESS_MODE_REPEAT
addressModeV/W       = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
```

Bind it only to the sky-view environment descriptor. Keep the current nearest sampler for G-buffer, material, depth, and ID attachments. Do not make `composite_sampler_` linear globally. Irradiance-coefficient and cloud-shadow images retain their existing, purpose-specific sampling contracts rather than inheriting sky U-repeat.

The sky-view LUT is a periodic set of 192 centred azimuth bins. The common helper samples U exactly as follows:

```text
u = fract(azimuth_u)
v = clamp(v, 0.5 / 108, 107.5 / 108)
```

The compute pass evaluates each U column at its centred periodic bin, `(column + 0.5) / 192`; it does not write a duplicate endpoint column. Repeat-U filtering then blends the last and first real bins across the seam. V is independently centre-clamped as above. `192x108` is retained. A later resolution increase is allowed only if the vertical/edge/seam gates below fail after this binding and mapping are present.

## Stable Output Dither

Implement dither in `display_transform.frag`, after tone mapping and conversion to encoded display code, immediately before that pass stores its scene result. It never changes the HDR composite, G-buffer, LUTs, irradiance, RT/GI, denoiser, or volumetric inputs. This is deliberately viewport/scene-only: the display pass runs before ImGui, so ImGui is composited afterward and receives no dither.

Use one achromatic scalar per display-pass pixel, `ivec2(floor(gl_FragCoord.xy))`. There is no temporal hash: use this exact, row-major 8x8 rank table indexed by `(pixel.y & 7) * 8 + (pixel.x & 7)`:

```text
37 12 54  1 46 27 61  8
18 43  5 58 31 50 14 40
63 22 35 10 48  3 56 29
16 45  7 60 25 52 11 38
33  0 47 20 57 15 42 30
 9 53 24 62  4 36 19 51
41 13 55 28 59  6 44 21
26 49  2 39 17 34 23 32
```

The table is a permutation of 0 through 63. Its required byte-table oracle is FNV-1a-32 `0xdc0d948b` (offset basis `2166136261`, byte-wise XOR/multiply by `16777619`, unsigned wrap). This prevents a reordered or substituted pattern from becoming an unreviewed visual change. Do not use frame index, jitter, time, camera position, DLSS phase, or random state.

For rank `r` in `[0,63]`, in encoded/code space before quantization:

```text
d = ((r - 31.5) / 31.5) * (0.5 / 255.0)
code_dithered = clamp(code + vec3(d), 0, 1)
```

This has exact extrema `-0.5/255` and `+0.5/255`, exact zero mean per complete tile, and one equal RGB offset. `code` is the explicit sRGB OETF result of the tone-mapped scene RGB. For a UNORM swapchain, write `code_dithered` directly. For an sRGB swapchain, write `srgb_eotf(code_dithered)` from the shader so the attachment's fixed-function sRGB OETF stores exactly `code_dithered`; do not add noise to linear values before fixed-function encoding. The existing `srgb_output` display setting selects these two branches. Raw HDR/intermediate readbacks remain before this pass.

## Lighting Split

The resolved frame ABI explicitly carries the following independent values:

| Value | Exact consumers | Must not affect |
|---|---|---|
| `sky_display_modifier_rgb` | Composite sky background, RT miss radiance, reflection environment | 9-SH diffuse/GI/fog/cloud ambient |
| `sky_irradiance_modifier_rgb` | Multiplies the evaluated directional 9-SH result for raster diffuse, diffuse GI, fog ambient scatter, cloud ambient scatter | background, RT miss/reflection display radiance |
| `direct_world_sun_rgb` | Raster direct BRDF, RT primary/secondary direct, fog/cloud direct scatter | analytic disc brightness |
| `sun_disc_rgb` | Existing analytic disc path | direct-world elevation ratio |

The atmosphere irradiance image remains nine directional coefficients. Every diffuse consumer first evaluates `sky_irradiance_sh(normal_or_direction)` and only then multiplies RGB by `sky_irradiance_modifier_rgb`; it must not collapse SH to a flat colour. `sky_display_modifier_rgb` is the existing visible-sky modifier path (authored display sky chroma times live sky tint and sky multiplier) and preserves its default appearance in composite, misses, and reflections. `sky_irradiance_modifier_rgb` is the independent authored irradiance chroma times the day/twilight ambient ratio below; it does not read the display modifier.

Use these exact direct equations, with component-wise multiplication written as `*`:

```text
direct_base_rgb = extraterrestrial_solar_rgb * atmospheric_transmittance_rgb
                * authored_sun_rgb * live_sun_tint_rgb * sun_multiplier
sun_disc_rgb = direct_base_rgb
direct_world_sun_rgb = direct_base_rgb * direct_world_ratio(e)
```

`sun_disc_rgb` deliberately does not inherit `direct_world_ratio`. Existing disc size/edge/core presentation remains unchanged. `direct_world_sun_rgb` receives the scalar last, preserving atmosphere chroma. Never derive any of these values from post-tone-mapped pixels.

Display-modifier edits must leave resolved SH irradiance, diffuse GI, fog/cloud ambient, direct-world RGB, and exposure unchanged. They do change background/miss/reflection radiance, so they invalidate only reflection/miss temporal history where that history exists; they do not reset diffuse-GI or volumetric history. Irradiance-modifier edits do not change background/miss/reflection display radiance and do reset diffuse-GI and volumetric ambient history.

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
sky_irradiance_modifier_rgb = authored_irradiance_chroma_rgb
                              * sky_ambient_ratio(e)
sky_irradiance_rgb(direction) = evaluate_9sh(direction)
                                   * sky_irradiance_modifier_rgb
```

At and above `+5°`, ambient is 25% of today's effective sky ambient. The `+5°` to `-6°` range smoothly transfers to twilight ambient. At `-5°`, direct world light is exactly zero, while physically derived sky irradiance remains positive and becomes primary light for upward surfaces and fog. Below `-6°`, the multiplier remains user-selected but is not a floor: physical atmosphere irradiance must continue to fade naturally through deep night. Do not inject a constant night term or compensate with auto exposure.

`evaluate_9sh(direction)` always evaluates the committed physical nine-coefficient field; it is never replaced by one flat RGB. Raster diffuse uses its shading normal, diffuse GI uses its incident/environment direction, and fog/cloud ambient uses its phase/integration direction. All share the same committed `sky_irradiance_modifier_rgb`, so twilight cannot light one path differently from another.

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

Atmosphere coefficient and sun-direction changes use an explicit candidate transaction. A candidate contains the complete LUT image set, its settings and normalized sun direction, its 9 SH coefficients, `direct_base_rgb`, both sky modifiers, resolved ambient ratio, and direct ratio. Generate/validate all of it off the currently committed descriptor set. Only after successful generation and descriptor publication may the renderer atomically replace the committed transaction and its direction. Every consumer then sees the new direction and all new light values together. If an ambient/direct/display control changes while an atmosphere candidate is pending, fold its current sanitized value into that candidate before publication; until then, retain the complete committed transaction rather than mixing old LUTs with new controls.

On candidate generation, validation, allocation, or descriptor-publication failure, retain the entire last-valid atmosphere transaction: old atmosphere LUTs, old direction, and old environment descriptors. Do not advance one light direction while retaining LUTs from another direction, and do not partially dim direct light. Immediately after recording the failure, resolve the *current sanitized non-atmosphere controls* (`sky_display_modifier_rgb`, day/twilight ambient controls, and `sunset_direct_ratio`) against that last-valid committed atmosphere direction/SH/direct base in a separate constants-only transaction. It may update display, ambient, or direct-ratio constants, but never LUT handles, atmosphere direction, or coefficients.

The constants-only transaction follows normal narrow replay rules: ambient or sunset-direct changes request one diffuse-GI reset and invalidate volumetric history; a display-modifier change invalidates reflection/miss history only. It does not increment atmosphere generation serial. An atmosphere failure by itself resets no history. On successful full atmosphere commit only, increment atmosphere generation serial and request exactly one diffuse-GI and volumetric-history reset. This replaces any partial dirty/reset behavior for atmosphere-linked updates.

FIFO `set render.lighting.*` and `get render.lighting.*` always expose the requested, sanitized property value, including a newly requested sun elevation that has not committed. The read-only status fields defined in **CLI/FIFO captures** expose only the currently resolved transaction. Thus after a failed LUT candidate the requested elevation can differ from `viewer.atmosphere_status.resolved_elevation_deg`, while current ambient/direct/display controls are visible in both their requested values and their last-valid-atmosphere resolved result.

- When no atmosphere candidate is pending, day ambient, twilight ambient, and Sunset direct edits update resolved constants without regenerating the LUT, request one diffuse-GI reset, and invalidate volumetric temporal scatter history. While a candidate is pending or has failed, they follow the candidate/constants-only rules above. They do not invalidate cloud density/optical-depth history.
- Display-modifier edits follow the narrow reflection/miss history rule in **Lighting Split**; output dither changes no lighting history. Neither changes exposure.
- Create/bind the dedicated sampler transactionally with environment descriptors and retain it through the normal frame-resource fence. Failure reports an error and retains the previous valid descriptor/sampler; it is not silently accepted as nearest filtering.
- Raster, RT, and volumetrics receive one committed snapshot per frame. RT secondary direct and fog/cloud direct use `direct_world_sun_rgb`; raster diffuse/diffuse GI/fog/cloud ambient evaluate 9 SH then apply `sky_irradiance_modifier_rgb`; background, RT misses, and reflections apply `sky_display_modifier_rgb`.
- Dither has no history and no presented-frame dependency. A resize changes the viewport pixel lattice normally; same-size static scene output is deterministic.

## Validation

### CPU and property gates

- Test default direct ratios at `90`, `45`, `5`, `0`, `-5`, and `-12`: `1`, `1`, `0.25`, `0`, `0`, `0`; dense `[-90,90]` samples are monotonic and continuous at `0`, `5`, and `45`.
- Verify noon ambient is 0.25 of the former effective lighting input, not 25% of tone-mapped screen colour. Verify the `+5..-6°` blend is finite/continuous and its endpoint values are exact.
- At `-5°`, verify resolved direct is zero and physical ambient is positive for an upward receiver and fog. At `-12°`, verify no artificial floor has been added.
- Verify sanitization/fallbacks and older-settings defaults. Changing `sky_display_modifier_rgb` must leave all nine SH coefficients, `sky_irradiance_modifier_rgb`, direct ratio, and `direct_world_sun_rgb` byte-identical; changing `sky_irradiance_modifier_rgb` must leave background/miss/reflection display RGB byte-identical.
- Candidate failure test injects LUT-generation and descriptor-publication failure. It proves the committed direction, all LUT handles, direct/ambient ratios, direct RGB, and modifiers are the exact previous transaction, and proves no history-reset counter advances. A successful candidate proves one atomic serial advance and one reset.

### GPU/image gates

- Descriptor test proves sky-view uses dedicated linear/repeat-U sampling while a representative G-buffer binding remains nearest.
- For 432 fixed vertical samples away from disc/horizon, GPU sampling differs from a CPU bilinear 192x108 LUT oracle by at most `1e-3` per linear channel. Where reference luminance slope exceeds `1e-4`, no identical-output plateau spans more than two adjacent samples.
- V=0/V=1 samples are finite and within the min/max of their clamped edge texel pairs. For 256 direction pairs immediately across the U wrap, relative RGB difference is at most `0.5%` (absolute `1e-3` near black), and seam finite difference is no more than twice the median adjacent-U finite difference.
- Dither oracle test checks the exact table and FNV checksum above. On uniform interior encoded values `code=(0.5,0.5,0.5)` in a multiple-of-8 viewport, measure `code_dithered-code` before quantization: every value is within `[-0.5/255,+0.5/255]`, extrema occur, RGB offsets are equal, each 8x8 mean has absolute value at most `1e-8`, and two static frames match exactly. Rail cases `code=0` and `code=1` are separate: after clamp they may have non-zero mean, but every code stays in `[0,1]` and the un-clamped offset still meets the same bound. The test also proves the sRGB and UNORM branches yield the same pre-quantization encoded code values. Shader/ABI inspection rejects temporal inputs.
- Use the deterministic `AtmospherePresentationFixture`: white Lambert up-facing 8 m x 8 m ground receiver centred at origin; 2 m vertical occluder centred at `(0,1,0)`; camera exactly `cam 0 2 12 0 1 0`; clouds disabled; fog enabled with `density=0.002`, `floor=0`, `falloff=30`, `color=(0.9,0.92,0.95)`, `wind=(0,0,0)`. The lit ROI is pixels `[420,300]..[460,340]`, shadow ROI `[500,300]..[540,340]`, upward/fog ROI `[430,260]..[470,290]` at 1280x720.
- Run both raster and native RT fixture paths at `90`, `5`, `0`, `-5`, and `-12` degrees after three warm-up frames plus a forced history reset. Emit and assert the resolved elevation, `direct_world_ratio`, `direct_base_rgb`, `direct_world_sun_rgb`, `sky_ambient_ratio`, `sky_display_modifier_rgb`, and `sky_irradiance_modifier_rgb`. Ratios must match CPU within `1e-6`; raster/RT direct RGB channels within `2e-3`; and fog/upward ROI mean at `-5` must exceed `1e-4` linear while direct contribution is exactly zero. Noon lit ROI must exceed shadow ROI by at least 10% after the day-ambient reduction. Native RT unavailable is an explicit acceptance-fixture failure, not a skipped RT comparison.

### CLI/FIFO captures

Implementation scope adds the following read-only session properties to the existing property registry; they are `get`-only, never serialized, and are formatted by the existing `get: <path> = <value>` grammar:

| Read-only property | Type / exact value |
|---|---|
| `viewer.session.render_path` | enum string: `raster`, `native_rt`, or `native_rt_unavailable` |
| `viewer.session.presented_frame_serial` | unsigned integer, incremented only after successful present |
| `viewer.session.native_rt_available` | boolean |
| `viewer.atmosphere_status.generation_serial` | unsigned committed-atmosphere generation serial |
| `viewer.atmosphere_status.resolved_elevation_deg` | committed atmosphere direction elevation, decimal degrees |
| `viewer.atmosphere_status.direct_world_ratio` | resolved scalar |
| `viewer.atmosphere_status.direct_base_rgb` | resolved RGB triple, `(r,g,b)` |
| `viewer.atmosphere_status.direct_world_sun_rgb` | resolved RGB triple, `(r,g,b)` |
| `viewer.atmosphere_status.sky_ambient_ratio` | resolved scalar |
| `viewer.atmosphere_status.sky_display_modifier_rgb` | resolved RGB triple, `(r,g,b)` |
| `viewer.atmosphere_status.sky_irradiance_modifier_rgb` | resolved RGB triple, `(r,g,b)` |

Implementation scope also adds these typed FIFO commands alongside existing `cam`, `set`, `get`, `stats`, and `shot`; their success lines are part of the harness protocol:

```text
render_path raster|native_rt
history_reset
wait_frames <positive-integer>
shot_now <absolute-png-path>
```

`render_path native_rt` fails with `render_path: native_rt unavailable` unless `viewer.session.native_rt_available = true`; the harness treats that line as failure, never a skip. `history_reset` prints `history_reset: requested` after queuing the one-shot temporal reset. `wait_frames N` completes only after N successful presents and prints `wait_frames: complete N frame_serial=<M>`; it is the sole frame-wait mechanism. `shot_now` writes the next successfully presented viewport image without its own warm-up, then writes the existing `<png>.done` sentinel. Existing `shot <path>` keeps its compatibility settle behavior and is not used by this acceptance fixture.

Extend the existing one-process atmosphere shot harness using these commands; do not add another control protocol. It loads `AtmospherePresentationFixture`, selects 1280x720, waits for `viewer: bake ready` and `MATTER_CMD_FIFO: listening`, uses `cam 0 2 12 0 1 0`, and holds exposure fixed. Run the complete matrix once with `render_path raster` and once with `render_path native_rt`. Before each elevation, read `viewer.atmosphere_status.generation_serial` as `S0`, then use this exact ordering:

```text
set render.lighting.exposure_ev -2
set render.lighting.day_ambient_multiplier 0.25
set render.lighting.twilight_ambient_multiplier 1
set render.lighting.sunset_direct_ratio 0.25
set render.lighting.sun_elevation_deg <elevation>
wait_frames 1
get viewer.atmosphere_status.generation_serial
get viewer.atmosphere_status.resolved_elevation_deg
```

Repeat `wait_frames 1` followed by the two atmosphere-status `get` commands until `generation_serial > S0` and `abs(resolved_elevation_deg - requested_elevation_deg) <= 1e-4`; only then issue this second, ordered block:

```text
history_reset
wait_frames 3
get viewer.session.render_path
get viewer.session.presented_frame_serial
get viewer.atmosphere_status.direct_world_ratio
get viewer.atmosphere_status.direct_base_rgb
get viewer.atmosphere_status.direct_world_sun_rgb
get viewer.atmosphere_status.sky_ambient_ratio
get viewer.atmosphere_status.sky_display_modifier_rgb
get viewer.atmosphere_status.sky_irradiance_modifier_rgb
stats atmosphere-presentation-<elevation>
shot_now <absolute-png-path>
```

The harness first sends `render_path <path>` and then requires `get viewer.session.render_path` to echo that path; for `native_rt` it also requires `get viewer.session.native_rt_available = true` before any elevation work. It parses all other `get:` values exactly as the types above, records requested `render.lighting.*` values separately from `viewer.atmosphere_status.*` resolved values, and fails on a timeout of 240 such polls. It waits for `<png>.done` before compare/inspection, measures the named ROIs/tolerances above, sends `quit`, and retains a kill trap only as cleanup. Store transient output in `MatterEditor/build/validation/atmosphere-presentation/`; commit test assertions/metadata rather than captures.

## Acceptance Criteria

- The `192x108` LUT is linear with a correct periodic U seam; G-buffer sampling stays nearest; resolution changes only after a metric failure.
- Dither is deterministic, scene-viewport-only, code-space correct for UNORM and sRGB presentation, bounded at exact plus/minus 0.5 LSB before quantization, and zero-mean on interior values without shimmer. ImGui is unaffected because it is composited after the display pass.
- `sky_display_modifier_rgb` and post-SH `sky_irradiance_modifier_rgb` are independent. Noon ambient defaults to 25% of today's effective value while the physical 9 SH remain directional.
- Direct world lighting preserves atmospheric chroma and defaults to `90=100%`, `5=25%`, `0=0`, `<0=0`; analytic-disc presentation remains separate.
- At `-5°`, direct is zero while upward receivers/fog remain positive/readable; deep night has no permanent ambient floor.
- Atmosphere direction/LUT/direct/ambient changes commit atomically or retain a complete last-valid transaction. New controls are manual existing-Lighting properties, sanitize predictably, preserve old content, work through FIFO, and are consistent in raster, RT, and volumetrics.
