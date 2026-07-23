# Froxel Volumetric Lighting & Smoke — Design

## Summary

Add froxel-based volumetric lighting to the Vulkan RT renderer: world-level height fog, ray-traced god rays, and DSL-authored localized emitters (chimney smoke, waterfall spray/mist). Three compute passes fill, light, and integrate a camera-aligned 3D froxel grid; the composite pass applies the result with one trilinear sample per pixel. Sun visibility per froxel is traced against the existing TLAS via `VK_KHR_ray_query`. Budget: ≤1.5 ms on the RTX 4090 at DLSS render resolution. Vulkan path only; the GL raster path is unaffected.

## Motivation

- No fog/atmosphere system exists; sky is analytic-only in `rt_lighting.rgen`
- God rays and lit fog are the highest-payoff visual features unlocked by the RT lighting infrastructure already on main
- Localized plumes (house chimneys, waterfall mist) make procedurally-placed parts feel alive, and the generic part system means one DSL verb covers all of them
- The froxel grid is reusable infrastructure (e.g. future clustered local lights)

## Decisions (brainstorm outcomes)

| Question | Decision |
|---|---|
| Platform | Vulkan RT path only |
| Emitter authoring | DSL verb on parts (`emitVolume`), rides instance resolve |
| Plume animation | Procedural curl-noise advection; no fluid sim (possible later phase) |
| Fog lighting v1 | Sun (RT shadowed via TLAS) + analytic sky; emissive-part fog lighting deferred |
| Perf budget | ~1.5 ms total for all volumetric passes |
| Validation | Extend Demo world; FIFO-harness screenshots + HUD perf gate |
| Approach | Classic froxel chain (density → scatter → integrate), not screen-space raymarch, not shadow maps |

## Architecture

### Module & shaders

- New module `VkVolumetrics` (`MatterEngine3/src/render/vk_volumetrics.{h,cpp}`), owned by `VkSceneRenderer`
- New compute shaders in `MatterEngine3/shaders_vk/`:
  - `vol_density.comp` — media injection (height fog + emitters)
  - `vol_scatter.comp` — per-froxel sun/sky lighting + temporal reuse
  - `vol_integrate.comp` — front-to-back integration along Z
  - `vol_common.glsl` — grid constants, slice↔depth mapping, shared structs
- Modified: `composite.frag` (apply pass), `vk_scene_renderer.{h,cpp}` (orchestration), `world_session.h` (options), `MatterViewer/ui.cpp` (controls)

### Frame order

```
cull → G-buffer raster → RT trace → GI temporal → GI à-trous
     → vol_density → vol_scatter → vol_integrate
     → composite (samples vol_integrated) → DLSS/display
```

Volumetric passes are recorded sequentially with image barriers between them. They have no dependency on the GI passes (only on depth + TLAS), leaving room to overlap later; v1 does not attempt overlap.

### Froxel grid

- Dimensions: **160 × 90 × 128** (W×H×D), compile-time constants in `vol_common.glsl`
- Camera-frustum-aligned; Z slices exponentially distributed from near plane to **300 m**
- Persistent 3D images (RGBA16F, ~59 MB total):
  - `vol_media` — rgb: scattering albedo × density, a: extinction (density pass output)
  - `vol_scatter[2]` — rgb: in-scattered light, a: extinction; ping-pong pair for temporal history
  - `vol_integrated` — rgb: accumulated inscatter camera→slice, a: transmittance (sampled by composite)

## Emitter authoring (DSL)

New session-polymorphic verb, records metadata (no geometry) in any session type:

```js
emitVolume({
  pos: [0, 5.2, 0],          // part-local; transforms with instance
  dir: [0, 1, 0],            // plume axis
  radius: 0.4,               // base radius at origin (m)
  spread: 0.15,              // radius growth per meter along axis
  length: 12,                // axial fade-out distance (m)
  density: 0.8,
  color: [0.85, 0.85, 0.9],  // scattering albedo
  rise: 1.5,                 // buoyant drift along dir (m/s)
  turbulence: 0.6            // curl-noise warp strength
});
```

- Typical presets: chimney smoke = narrow/high-rise/gray; waterfall mist = wide/no-rise/white/high-turbulence with `dir` down-and-out
- Storage: metadata block in the `.part` file (like collision proxies); untouched by the LOD ladder; parameters salt the bake hash
- Runtime: emitters resolve with instances. Each frame the CPU gathers emitters within 300 m of the camera into an SSBO (cap **256**; nearest-to-camera wins on overflow, logged once). `vol_density.comp` iterates a per-tile culled subset (screen-tile frustum vs. emitter bounding cone).

## Density evaluation (`vol_density.comp`)

Per froxel, world position at froxel center:

1. **Height fog:** `base_density · exp(-(y - fog_floor) / falloff)` with world fog color. World-level params: base density, floor, falloff, color, wind vector — new world-schema fields plus runtime override multipliers (same pattern as `VulkanLightingOverrides`).
2. **Emitters:** sample position warped by two octaves of curl noise (from a 32³ tiling noise texture generated at init), scrolled along the plume axis at `rise` m/s plus world wind. Warped position tested against cone `radius + spread·t` with smooth radial falloff and axial fade over `length`. Contributions sum into `vol_media`.

All animation lives in this pass (scrolling noise phase), so temporal lighting reuse never lags plume *motion*, only lighting.

## Lighting & temporal reuse (`vol_scatter.comp`)

- Per froxel with non-zero extinction (empty froxels early-out):
  - **Sun:** one `rayQueryEXT` shadow ray from froxel center toward `sun_direction`, opaque-only, tMax 300 m, against the existing TLAS. Contribution = `sun_color · visibility · HG_phase(θ, g)`; Henyey-Greenstein `g = 0.3` default, world-tunable.
  - **Sky:** analytic ambient matching `sky_environment()` in `rt_lighting.rgen`, fixed hemispheric weight.
- **Subsampling:** each frame traces 1 of 4 froxels in a rotating 2×2 XY Bayer pattern.
- **Temporal reprojection:** froxel world position reprojected through the previous frame's `world_to_clip` (`FrameMatrices`); trilinear history sample from the ping-pong volume; blend ~85 % history / 15 % fresh. Fallback to fresh sample when out-of-frustum or on large extinction change. First frame / resize / grid teleport force blend weight 0.

## Integration & composite

- `vol_integrate.comp`: one thread per XY column marches the 128 slices front-to-back: `inscatter += T · scatter · step`, `T *= exp(-extinction · step)`, writing every slice to `vol_integrated`.
- `composite.frag`: one trilinear sample at `(uv, slice_from_depth(depth))`; `color = color·T + inscatter`, applied after all surface lighting, before tone-map. Sky pixels sample the far slice, so god rays against sky work with no special path. DLSS consumes the fogged HDR image unchanged.

## Options & UI

- `VulkanVolumetricsSettings` in `RenderOptions` (`world_session.h`): `enabled` (default **off** until validated), `temporal_blend`, `phase_g`, plus world fog override multipliers
- ImGui section in `MatterViewer/ui.cpp` mirroring the lighting controls
- Volumetric debug modes added to the existing `debug_view` enum: raw density, raw scatter, integrated result

## Failure modes

| Condition | Behavior |
|---|---|
| No `VK_KHR_ray_query` support | Volumetrics disabled at init, one-line log (matches RT init fallback pattern) |
| >256 emitters in range | Nearest-to-camera win; overflow logged once |
| Invalid history (first frame/resize/teleport) | Temporal blend forced to 0 for that frame |
| Zero fog density and zero emitters | All three dispatches skipped; `vol_integrated` stays cleared (T=1, inscatter=0) so the composite sample is safe |

## Testing

1. **Headless unit tests:** emitter metadata round-trips through `.part` bake/load and survives the LOD flatten ladder; `emitVolume` parameter validation at bake; bake-hash changes when emitter params change
2. **GPU tests (`GALLIUM_DRIVER=d3d12` suite):** density-fill vs. CPU-reference plume evaluation at known froxel centers; integrate-pass invariant — transmittance monotonically non-increasing along Z
3. **Visual gate:** Demo world extended with a chimney-smoke house part, a waterfall-mist emitter, and low height fog; `viewer_shots.sh` FIFO shots at fixed cameras (low-sun god-ray angle, camera inside fog, plume close-up); self-terminating runs
4. **Perf gate:** HUD frame-time delta ≤1.5 ms with volumetrics toggled, Demo world, Windows build

## Constraints & build notes

- SPIR-V embeds (`shaders_gen/embedded_spirv.h`) regenerate only via the MSYS2 Windows build — shader tasks require a Windows rebuild pass before GPU validation
- Always `make windows` after engine changes (stale viewer.exe ships old engine); clean-rebuild after struct/header changes
- Test suites run sequentially, never in parallel (WSL2 OOM)
- Grid dimensions fixed in v1; revisit only if the perf gate fails

## Deferred (explicitly out of scope for v1)

- Emissive-part fog lighting (per-froxel GI ray) — phase 2 toggle candidate
- Local fluid-sim hero emitters layered over procedural plumes
- GL raster path support
- Clustered local lights on the froxel grid (infrastructure enabler only)
- Pass overlap/async compute scheduling
