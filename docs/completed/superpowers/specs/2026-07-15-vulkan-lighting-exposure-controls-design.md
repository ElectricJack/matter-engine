# Vulkan Lighting and Exposure Controls Design

**Date:** 2026-07-15  
**Status:** Approved design, pending written-spec review  
**Target:** MatterViewer Vulkan hybrid RTX renderer with Native and Streamline
DLSS Super Resolution output

## 1. Goal

Make direct light, emissive surfaces, shadows, and indirect bounces readable in
the Vulkan viewer without weakening the renderer's linear-HDR lighting model.
The viewer will expose temporary lighting overrides for diagnosis and apply a
real display transform after DLSS rather than relying on swapchain clipping.

The immediate reference case is CornellBox. Its authored sun color is
`(3, 3, 3)`, sky color is `(0.6, 0.65, 0.75)`, and the canonical light material
has emission strength `5`. Those values legitimately exceed display range.
The current Vulkan path composites them in linear HDR but has no explicit
post-DLSS exposure and tone-mapping stage, so bright terms saturate and hide
shadow and bounce detail.

## 2. Scope

### In scope

- A post-DLSS exposure and ACES-style tone-mapping display transform.
- Temporary per-session multipliers for sun, sky, and material emission.
- Viewer UI controls for exposure and the three source multipliers.
- A one-click reset to the active world's authored lighting.
- Resetting overrides when switching or reloading worlds.
- Consistent behavior in Native, DLSS, RT-enabled, and raster-fallback modes.
- Tests proving ordering, monotonic response, finite output, and reset behavior.

### Out of scope

- Persisting overrides into `world.manifest` or material definitions.
- Automatic exposure, eye adaptation, histograms, or exposure pumping.
- Replacing authored world-light values with globally reduced constants.
- Per-light editing for spotlights or adding new light types.
- Color grading, white-balance controls, LUTs, bloom, or HDR-monitor output.

## 3. Chosen Approach

Use two separate control layers:

1. **Lighting-source debug overrides** change the energy entering the lighting
   integrator. Sun, sky, and emission multipliers default to `1.0`.
2. **Display exposure** changes only how the completed linear-HDR image is
   mapped to the SDR swapchain. It is expressed in exposure values (EV), with
   a CornellBox-friendly initial value of `-2 EV`.

This separation is intentional. Source multipliers help isolate direct sun,
environment light, and emissive bounce behavior. Exposure changes visibility
without altering light transport. A source-only solution would still clip HDR
highlights, while automatic exposure would make frame-to-frame GI comparisons
unstable.

## 4. Runtime Contract

Add a small viewer-owned lighting override value object:

- `sun_multiplier`, default `1.0`, range `0.0` to `4.0`.
- `sky_multiplier`, default `1.0`, range `0.0` to `4.0`.
- `emission_multiplier`, default `1.0`, range `0.0` to `4.0`.
- `exposure_ev`, default `-2.0`, range `-6.0` to `+6.0`.

The object is copied into `RenderOptions` each frame. The engine combines sun
and sky multipliers with the active manifest values when constructing
`VkSceneLighting`. The emission multiplier is passed explicitly to both the
primary composite and secondary-hit lighting shader so visible emission and
emissive bounced radiance scale together.

All inputs must be finite. The viewer clamps UI values to their declared
ranges. The engine also sanitizes external/API values so NaN, infinity, or
out-of-range input cannot poison temporal history or HDR output.

Overrides are diagnostic state, not world state. Selecting another world,
reloading the current world, or pressing **Reset to World** restores the four
defaults. Camera motion and DLSS mode changes do not reset these values.

## 5. Rendering Order

The required data flow is:

```text
raster G-buffer
  -> direct light + emission + RTX signals
  -> temporal accumulation and A-trous filtering
  -> linear-HDR composite
  -> DLSS Super Resolution (when active)
  -> exposure and ACES-style SDR tone map
  -> UI overlay
  -> swapchain present
```

DLSS must consume the pre-tone-mapped linear-HDR image. Tone mapping before
DLSS would discard highlight information and violate the existing HDR/auto-
exposure resource contract. The display transform therefore operates on the
actual selected final image: native HDR when DLSS is inactive or Streamline's
output when DLSS succeeds.

The UI overlay is rendered after tone mapping so ImGui colors are not modified
by exposure. The final transform clamps or sanitizes non-finite values and
outputs display-range color appropriate for the existing SDR swapchain.

## 6. Viewer UI

Add a **Lighting** section to the existing Vulkan debug panel:

- `Exposure (EV)` slider, `-6` to `+6`, displayed with two decimals.
- `Sun` slider, `0` to `4`.
- `Sky` slider, `0` to `4`.
- `Emission` slider, `0` to `4`.
- `Reset to World` button.

The controls update live without rebaking geometry or probes. Tooltips clarify
that Sun, Sky, and Emission change lighting energy, while Exposure changes only
display mapping. Existing unused sun-direction fields in `ViewerStats` are not
part of this milestone; direction editing can be designed separately if it is
still useful after the brightness problem is resolved.

## 7. Temporal and Reset Behavior

Changing display exposure does not invalidate GI, reflection, denoiser, or
DLSS history because it occurs after those systems.

Changing sun, sky, or emission energy invalidates engine-owned lighting
history exactly once. Otherwise old accumulated samples would fade over
multiple frames and make the controls appear laggy or incorrect. The next
successfully presented frame becomes the new history origin. Failed presents
must not publish history under the new values.

World switch and reload already form history boundaries. Resetting overrides
as part of those transitions must not introduce a second reset on the following
frame.

## 8. Fallback and Error Handling

- Raster and RT fallback use the same sun, sky, emission, and exposure values.
- If DLSS evaluation fails, the native linear-HDR image receives the same final
  display transform; brightness must not jump because of fallback.
- If RT is disabled or unavailable, stale indirect/specular history contributes
  no light, but source controls and tone mapping remain functional.
- Shader or resource setup failure follows the renderer's existing truthful
  fallback/error path; it must not silently present unclamped HDR as SDR.

## 9. Verification

### CPU and contract tests

- Defaults and clamp/sanitize behavior, including NaN and infinity.
- EV conversion is `exp2(exposure_ev)` and is monotonic across the range.
- World switch, reload, and Reset to World restore all defaults.
- Source changes request one lighting-history reset; exposure-only changes
  request none.

### GPU tests

- Fixed HDR inputs produce finite ACES-style SDR outputs in display range.
- Increasing exposure, sun, sky, or emission produces a monotonic response in
  an isolated fixture without changing unrelated terms.
- Emission multiplication matches at primary visibility and a secondary hit.
- Tone mapping happens after the selected Native/DLSS output, never on a DLSS
  input resource.
- Native and fake/real-DLSS paths agree within the expected reconstruction
  tolerance for a constant HDR image.
- RT enabled, disabled, unavailable, resize, world-switch, and DLSS-transition
  sequences produce zero Vulkan validation errors and no double history reset.

### Visual acceptance

In CornellBox at the default `-2 EV`, the ceiling emitter retains highlight
shape, the directly lit surfaces do not saturate to featureless white, box and
wall shadows remain distinguishable, and colored indirect bounce can be seen.
The user can separately reduce Sun or Emission to inspect their contributions.

## 10. Alternatives Rejected

**Only lower CornellBox source values.** This helps one world but leaves the
missing display transform and other HDR scenes unresolved.

**Source multipliers without tone mapping.** This provides debug isolation but
continues to clip values above swapchain range.

**Automatic exposure.** It is useful for a later production camera, but its
adaptation makes controlled GI comparisons harder and adds state, tuning, and
reset behavior not needed for this milestone.

