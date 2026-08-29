# Cloud Terrain Occlusion Design

## Goal

Mountains and other TLAS geometry must cast direct-sun shadows into enhanced volumetric clouds when the occluder is farther than the current 300 m volumetric shadow-ray limit. The change must remain cheap enough for interactive use and must not add a second ray or a new rendering pass.

## Scope

This is a narrow extension of the existing ray query in `vol_scatter.comp`.

- Enhanced froxels that contain finite, positive cloud extinction use a terrain-shadow reach matching the 3 km froxel volume.
- Fog-only froxels and Current-cost volumetrics retain the existing 300 m reach.
- Each actively updated froxel still launches at most one TLAS shadow ray.
- The existing Bayer update schedule, temporal accumulation, cloud self-shadowing, atmosphere lighting, and cloud-shadow clipmaps are unchanged.
- This does not add a sun-space terrain clipmap, screen-space shadow approximation, or shadowing of sky ambient.

## Shader Design

`vol_common.glsl` will retain `VOL_SHADOW_FAR = 300.0` and define a distinct cloud-terrain reach equal to `VOL_FROXEL_FAR` (currently 3000 m). Keeping separate names makes the performance policy explicit.

In the enhanced specialization, `vol_scatter.comp` will sample and sanitize the R16F cloud-extinction value before initializing the existing ray query:

1. Non-finite extinction resolves to zero.
2. Finite extinction is clamped to `[0, media_extinction]` exactly as it is today.
3. Values above `1e-6 m^-1` select the 3 km cloud-terrain reach; zero or negligible values select 300 m.
4. The sanitized value is reused later for cloud scattering rather than sampled twice.

The Current-cost specialization has no cloud-density image and always selects 300 m. The shader still calls `rayQueryInitializeEXT` once per active froxel; only its maximum distance changes.

## Lighting Semantics

The ray result continues to feed the existing shared geometry visibility term. Consequently:

- Direct cloud sunlight is attenuated by mountains over the extended reach.
- Direct fog sunlight in a cloud-containing froxel uses the same visibility, preserving the current single-ray behavior.
- Sky-irradiance ambient remains unoccluded by this ray.
- Existing cloud multiple-scattering/order weighting is preserved; this milestone changes ray reach, not the established lighting equation.
- Cloud-to-world shadow clipmaps and cloud self-shadow optical depth remain independent inputs and are not modified.

This intentionally avoids introducing a second visibility term or a second ray solely to separate colocated fog from cloud media.

## Performance Boundary

Long rays are restricted to enhanced froxels whose sanitized cloud extinction exceeds `1e-6 m^-1`. History-only Bayer columns do not gain work. Empty-cloud and fog-only regions keep the 300 m traversal cap, which preserves the optimization originally added to avoid dense terrain TLAS stalls.

The 3 km cap matches the current froxel coverage, so rays do not search beyond media represented by the volume. If the froxel range changes later, the cloud-terrain reach follows `VOL_FROXEL_FAR` rather than introducing another independently tuned distance.

## Failure and Edge Behavior

- NaN, infinity, negative density, and negligible density select the short ray.
- A missing or empty TLAS follows the existing volumetric fallback path; this change does not alter renderer availability rules.
- Allocation, descriptor, or atmosphere failures retain their existing transactional behavior.
- An occluder beyond 3 km does not shadow the froxel.
- The change does not invalidate volumetric history merely because the chosen ray distance differs between neighboring froxels.

## Verification

Implementation will follow RED-to-GREEN coverage:

1. Add a focused shader/source contract that proves the enhanced path selects the long reach from sanitized cloud extinction while Current-cost remains short, and that there is still one ray-query initialization site.
2. Add a deterministic real-Vulkan fixture with a cloud-bearing froxel and an occluder positioned beyond 300 m but within 3 km along the sun ray. The occluded direct-scattering result must be materially darker than the clear control.
3. Add controls proving fog-only/Current-cost behavior retains the 300 m policy and sky ambient is not multiplied by terrain visibility.
4. Regenerate and audit SPIR-V, rebuild the focused smoke target, and require Vulkan validation errors to remain zero.
5. Run a bounded command-line screenshot comparison if a deterministic non-streaming scene can expose the effect without expanding scope. The GPU numerical fixture is the authoritative correctness gate.

## Acceptance Criteria

- A mountain-equivalent TLAS occluder between 300 m and 3 km reduces enhanced cloud direct radiance.
- The implementation launches no additional per-froxel ray and creates no new render pass or persistent GPU resource.
- Fog-only and Current-cost froxels retain the 300 m maximum ray distance.
- Sky ambient remains unchanged between occluded and clear controls.
- Focused CPU/source, shader compilation, and real-Vulkan tests pass with zero validation errors.

## Files Expected to Change

The implementation should normally remain within:

- `MatterEngine3/shaders_vk/vol_common.glsl`
- `MatterEngine3/shaders_vk/vol_scatter.comp`
- generated embedded SPIR-V output required by the existing build
- focused shader/source tests
- `MatterEngine3/tests/vulkan_smoke_tests.cpp`

Any renderer API or resource-lifetime change requires a design amendment because the approved approach does not need one.
