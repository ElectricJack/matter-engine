# RT Material Transport Design (Transmission, Thin Scattering, Colored Emission, Second Bounce)

Date: 2026-07-15
Branch: feature/rt-lighting-phase2
Status: approved by Jack (brainstorming session 2026-07-15)

## 1. Problem

The lighting sculpture garden (commits 9669259/42f48b8/e39e491/cf1f860) authors and
packs full transport data for glass, smoke glass, water, wax, and thin foliage into
the 144-byte `MaterialGpuRecord`, but the RT shading path never consumes it. The
garden plan deferred this deliberately ("no fake Task 9/10 shader behavior"), so
those sculptures render as flat diffuse. Screenshot evidence 2026-07-15: glass and
wax spheres are matte; emissive `lightCool` cubes glow white instead of the
authored blue.

Master design: `docs/superpowers/specs/2026-07-14-vulkan-hybrid-gi-materials-design.md`.
Its §12 delivery sequence steps 1-6 are implemented; this spec covers steps 7, 8,
and 9, plus a fixed second diffuse GI bounce pulled forward from step 10.

### Current shader consumption (audited 2026-07-15)

- Consumed today: base color/roughness, metallic, emission **strength** (not
  color), clearcoat amount/roughness, specular strength and tint
  (rt_lighting.rgen:258-260), alpha cutoff, shadow opacity, `MATERIAL_ALPHA_TESTED`;
  transmission weight is used only as a shadow tint in `rt_visibility.rahit`.
- Never consumed: IOR, thickness, absorption color/distance, subsurface weight,
  scattering color/distance/anisotropy, emission color.
- No transmission/refraction rays exist; all rays trace `gl_RayFlagsOpaqueEXT`.
  Diffuse GI is exactly one bounce.

## 2. Approach (chosen)

Extend the existing passes in place. No new pipelines, passes, or denoiser lanes.
Rejected alternatives: a dedicated transmission pass with its own temporal/atrous
lane (plumbing not justified while all garden glass is smooth), and a unified
stochastic path-loop rewrite (pulls in step 10's denoising burden; worst fit for
the no-local-shader-compile constraint).

## 3. Dielectric transmission (design step 7)

Materials: greenGlass (6), water (7), glassSmoke (27, `MATERIAL_VOLUME_BOUNDARY`),
legacy glass.

In `rt_lighting.rgen`, after the specular lobe, if the primary pixel's material has
`transmission.x > 0`:

1. Refract the view ray at the G-buffer surface, `eta = 1/IOR` (`transmission.y`).
   Entry Fresnel `F` via Schlick with `f0 = ((ior-1)/(ior+1))^2`. The existing GGX
   specular lane already renders the reflected component; the transmission lane
   carries `(1-F)`.
2. Deterministic medium walk, up to 4 events. Rays use `gl_RayFlagsOpaqueEXT`,
   mask `0xff`, the existing `rt_surface.rchit` hit group and `RtSurfacePayload`.
   At each interior hit: exit iff `dot(ray_dir, surface.normal) > 0`; attempt exit
   refraction with `eta = IOR`; `refract()` returning the zero vector means total
   internal reflection, so reflect internally and continue; accumulate in-medium
   path length from `hit_t`.
3. Beer-Lambert transmittance `pow(absorption_color, path_len / absorption_distance)`
   from `absorption_pad.rgb` and `transmission.w`. Authored thickness
   (`transmission.z`) is the fallback path length when the walk finds no backface
   or hits the event cap.
4. On exit, one radiance evaluation along the exit ray: `hit_radiance()` on hit
   (colored emission included per §5) or `environment()` on miss.

Output: new `raw_transmission_image` (rgba16f). rgb = transmitted radiance x (1-F)
x Beer-Lambert; a = transmission coverage weight (`transmission.x`). The walk is
deterministic and the exit shading analytic, so the signal is near noise-free and
**bypasses temporal/atrous**, feeding composite directly.

Composite blend: `(ambient + sun + raw_diffuse) * (1 - trans.a) + emission +
specular + trans.rgb`. Surface diffuse fades out as transmission takes over;
reflection stays. NaN/inf guard mirrors the specular guard (rt_lighting.rgen:308).

## 4. Thin-walled scattering and wax (design step 8)

Analytic, zero new rays; implemented in `composite.frag`, which today computes
direct sun as `max(dot(N, to_sun), 0.0)` (hard zero when backlit). Composite gains
three bindings: identity texture, `rt_materials` storage buffer, and the
transmission texture from §3.

**Thin foliage** (`MATERIAL_THIN_WALLED` and `subsurface > 0`: foliageThin 29, leaf):
- Front lighting: unchanged diffuse.
- Back lighting: wrapped/forward term. `backlit = clamp(-NdotL, 0, 1)` sharpened
  toward the sun by anisotropy (`scattering_shape.y` shaping
  `dot(view, -to_sun)`), tinted `scattering_color * subsurface`, attenuated by a
  falloff from `scattering_shape.x`, multiplied by the shadow visibility texture.

**Shadow reclassification** (the one structural change): `rt_material_is_opaque()`
in `vk_scene_renderer.cpp` currently keys only on transmission, so thin-scattering
geometry sits in the opaque TLAS layer (mask 0x01); a backlit closed blob's sun
ray starts inside its own geometry, self-hits, and reads fully shadowed. Fix:
classify `THIN_WALLED && subsurface > 0` into the non-opaque layer (mask 0x02) and
teach `rt_visibility.rahit` to attenuate such hits by
`scattering_color * (1 - shadow_opacity)` instead of blocking. GI and reflection
rays still treat foliage as a solid hit.

**Wax** (28: `subsurface > 0`, not thin-walled): stays opaque everywhere. Front
wrap only: `NdotL_wrapped = (NdotL + w) / (1 + w)` with `w = subsurface`, the
wrapped-in region tinted by scattering color. No reclassification, no rays.

Accepted limitation: this is the master design §7.6 analytic leaf model, not real
SSS. The garden's thick foliage blobs glow when backlit regardless of actual
thickness; that is the authored thin-walled contract.

## 5. Colored emission (design step 9)

`hit_radiance()` (rt_lighting.rgen:159) and `composite.frag:55` both compute
emission from albedo x strength, ignoring the authored emission color
(`emission_strength.rgb`) — hence white `lightCool` cubes.

1. **Pack-time normalization** (`material_registry.c`): when packing, if
   `emission_strength > 0` and the authored emission color is black (all legacy
   emissive materials), write the material's albedo into `emission_strength.rgb`.
   Shaders then read `emission_strength.rgb` unconditionally; legacy lights keep
   their exact current color.
2. **`hit_radiance()`**: `emission = mix(material.emission_strength.rgb, tint.rgb,
   tint.a) * strength` — the same tint blend base color gets, so tinted emissive
   parts keep working. GI bounce and reflection rays already add `hit_radiance()`
   emission, so emissive secondary lighting becomes colored automatically.
3. **`composite.frag`**: replace `albedo.rgb * strength` with
   `emission_strength.rgb * decoded strength` via the identity + material-buffer
   bindings. The G-buffer `normal.w` scalar strength encoding is untouched.

## 6. Second diffuse GI bounce (from design step 10)

Restructure the diffuse GI section of `rt_lighting.rgen` (currently lines 195-232)
into a 2-iteration loop with a running `throughput` multiplied by each vertex's
base color. Each vertex keeps the existing pattern: cosine-sample, trace, sun NEE
with the two-layer shadow trace, `hit_radiance()` sky/emission terms. Feeds the
same `raw_diffuse` signal; the existing temporal/atrous chain absorbs the added
1-spp variance.

Cost: roughly +2 rays/pixel in the diffuse lane. Explicitly excluded: Russian
roulette, adaptive quality, ray instrumentation (remain in step 10). If the noise
or cost is unacceptable in testing, this lands as its own commit and reverts
cleanly.

## 7. Renderer plumbing

- `raw_transmission_image`: same lifecycle as `raw_specular_image` (resize/
  recreate, cleared via `clear_color_image_for_use`), new binding in the
  rt_lighting descriptor set, sampled binding in composite.
- Composite descriptor-set layout grows three bindings: transmission texture,
  identity texture, `rt_materials` buffer.
- `rt_material_is_opaque()` gains the thin-scattering clause (§4), moving those
  instances to TLAS mask 0x02.
- No new push constants or UI tunables; tuning happens from Jack's test feedback.

## 8. Delivery and workflow

Four commits, in order:

1. **Colored emission** — packer normalization + `hit_radiance` + composite,
   including the new composite bindings (riskiest plumbing with the smallest
   shader change).
2. **Transmission** — rgen walk + `raw_transmission_image` + composite blend.
3. **Thin foliage + wax** — composite wrapped terms + opacity reclassification +
   `rt_visibility.rahit` attenuation.
4. **Second diffuse bounce** — rgen loop restructure; independently revertible.

Workflow per Jack (2026-07-15): sonnet subagents execute from self-contained
prompts; adversarial opus agents review each diff; **no verification gates
in-plan**. Free `-fsyntax-only` compiles on touched C++ TUs only; shaders
are static-review-only (no glslc in WSL). Jack runs the MSYS2 Windows build
(regenerating `embedded_spirv.h`, which also covers the three shader commits
stacked from the tech-debt queue) and does all runtime testing at the end,
providing screenshots and feedback.

## 9. Non-goals (deferred to master design steps 10-12)

- Rough/frosted transmission denoising and a dedicated transmission denoiser lane.
- Nested/heterogeneous media.
- Russian roulette, adaptive quality controls, ray instrumentation.
- DLSS reactive/transparency masks — glass may ghost or smear under DLSS motion
  until step 11.
- Performance tuning to the 60 FPS acceptance target (step 12).
- Real volumetric subsurface scattering.
