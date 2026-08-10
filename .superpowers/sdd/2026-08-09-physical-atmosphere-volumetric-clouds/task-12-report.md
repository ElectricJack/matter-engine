# Task 12 report — detailed cloud self-shadowing and selectable scattering

Status: **DONE WITH CONCERNS**. Numerical, source/ABI, SPIR-V, full-build, and
real-RTX gates pass. The implementation is complete; the required visual matrix
could not be retained as credible evidence because the bounded StreamMountain
settle gate never stabilized in this environment (details below).

## TDD evidence

RED was captured before implementation:

- `make -C MatterEngine3/tests run-cloud-shadows ...` failed on the missing
  `cloud_self_shadow_constant_slab_reference`, `CloudLightingReference`, and
  `cloud_lighting_reference` interfaces.
- `make -C MatterEngine3/tests run-shader-source ...` failed on the absent
  enhanced-scatter specialization, density binding, and 240-byte ABI contract.
- Real RTX `froxel-resize` exercised the old production scatter and reported
  identical order luminance: `0.0861740 0.0861740 0.0861740 0.0861740`
  (expected RED monotonic-order assertion).

GREEN coverage now pins:

- constant-slab `exp(-sigma * distance)` within one half-float/step tolerance;
  endpoint coarse tau (`4 + 2.5` overlap case resolves to `4.5`, not `6.5`);
  and frustum-exit local truncation with the coarse remainder preserved;
- strict cloud brightening for orders 1/2/3/4, bounded normalized energy,
  order-1 strength independence, fog-only order independence, and a zero
  cloud-only channel with nonzero haze;
- the production RTX pipeline, with luminance
  `0.0212559 0.0567397 0.0815918 0.0953862`, no resource-generation change,
  order-1 strength invariance, fog-only invariance, finite outputs, and zero
  Vulkan validation errors.

## Implementation

- Added a specialization-selected Current/enhanced scatter pipeline pair over
  one superset descriptor layout. Current binds the existing 1x1x1 density
  dummy; enhanced binds the full R16F cloud grid. Pipeline selection changes do
  not recreate a bundle unless the derived enhanced footprint changes.
- Added camera-basis world-to-froxel mapping, frustum-bounded jittered local sun
  marching (0–32 steps, 0–1000 m), and endpoint sampling of the remaining
  cumulative sun-space optical depth without start-point overlap.
- Split cloud scattering (`0.99 * cloud_extinction`) from fog/emitter
  scattering, retained one TLAS query and single-HG/SH fog lighting, and added
  the exact cloud dual lobe, 1–4 bounded scattering orders, and powder factor.
- Direct lighting reads `environment.direct_world_sun_ratio.rgb`; ambient reads
  post-9SH `sample_sky_irradiance` / `sky_irradiance_ambient_ratio`. No legacy
  `sun_color`, `sky_color`, or scatter `sun_intensity` consumer was restored.
- Cloud-shape and scatter-lighting changes invalidate history. Cloud-density
  write visibility and per-frame lifetime now cover enhanced compute sampling.
- Extended `cloud-lighting` with deterministic order 1–4, march 0/8, and an
  explicitly authored contiguous two-layer case. The fixed enhanced caption is:
  march distance 250 m, strength 0.6, powder 0.35, sun elevation 45 degrees,
  froxels 1x/128 (observed 160x90x128), near clipmap 256x256x32 over 1800 m,
  far clipmap 128x128x24 over 4000 m, filter 1, full update. Orders vary only
  the order count; the march pair uses order 1; cross-layer uses march 8/order 2.

## ABI reconciliation

The brief described a 208-byte starting block and a 256-byte result, but the
committed atmosphere-adjustment ABI had already removed duplicated sun/sky
lanes and made the live prefix 192 bytes. Per parent approval, Task 12 preserves
that 192-byte prefix, reuses the dead `sun_intensity`/padding lanes for the five
registered controls, and appends the exact 48-byte camera tail, yielding 240
bytes. Static asserts pin size 240 and offsets 128/156/192/236; source tests
prove the reclaimed `sun_intensity` name has no shader/header consumer. This
stays below the guaranteed 256-byte ceiling without repacking EnvironmentBlock.

## Verification

- PASS: `make -C MatterEngine3 vulkan-spirv ...`
- PASS: full `make -C MatterEngine3 ...`
- PASS: full `make -C MatterEditor windows ...`
- PASS: `make -C MatterEngine3/tests run-cloud-shadows ...` (`ALL PASS`)
- PASS: `make -C MatterEngine3/tests run-shader-source ...`
  (`shader_source_tests: all passed`)
- PASS: RTX `MATTER_VK_SMOKE_MODE=froxel-resize` from `MatterEditor`
  (`ALL PASS`, validation errors 0; values above)
- PASS: final `bash -n MatterEngine3/tools/atmosphere_cloud_shots.sh` and
  `git diff --check`.

## Visual acceptance concern

Run 1 generated the order/march matrix, but inspection rejected it: streamed
mountain sectors visibly changed between order 1 and order 4 and confounded the
lighting comparison. The run later stopped at the pre-existing harness image
gate because the available MSYS Python has no Pillow:
`ERROR: img_diff requires Pillow`.

One bounded rerun added the existing depth/draw-count settle gate. It exhausted
all 30 probes: the depth-producing draw count kept increasing (76, 107, ...,
670 by probe 26), then stopped with
`ERROR: streaming did not settle for cloud-shadow capture`. Rerun cleanup had
removed run-1 images, so no accepted Task 12 PNGs remain. Per instruction, no
third run was attempted. The final harness preserves diagnostic captures after
a bounded settle miss instead of exiting early, but this final behavior was not
rerun. Visual acceptance therefore remains outstanding despite the stronger
deterministic numerical and real-GPU assertions above.

## Scope/self-review

Only Task 12 implementation, tests, generated SPIR-V, harness, and this report
are intended for the commit. Protected pre-existing deletions
`MatterEditor/shaders`, `MatterEngine3/shaders` and untracked `.tmp-task8/`,
`.tmp-task9-base/` were neither edited nor staged. No Task 13 work was started.

## Review fix round 1: froxel eye and seeded overlap proof

Both Important review findings were reproduced. The host populated
`camera_pos` by unprojecting NDC near-center while `world_to_froxel_uvw`, the
frustum-exit slab equations, and the phase direction all subtracted it as the
eye. The earlier real-GPU Task 12 check also passed `cloud_shadows.enabled =
false`, so its analytical endpoint assertion was not production evidence.

Exact RED evidence, captured before the fix:

- `make -C MatterEngine3/tests run-cloud-shadows` failed to compile the new
  center/near/edge assertions because `FroxelCameraReference`,
  `volumetric_camera_eye`, `world_to_froxel_reference`, and
  `froxel_to_world_reference` did not exist.
- `make -C MatterEditor build/windows/vulkan_smoke_tests.exe` failed under
  `-Werror` because the new production fixture could not find
  `froxel_ray_exit_distance_reference`,
  `set_cloud_shadow_density_layers_for_test`,
  `readback_volumetrics_scatter_voxel_for_test`, or
  `clear_cloud_shadow_density_override_for_test`.

The fix now reconstructs the rigid-transform eye as `-R^T t` once on the host
and passes that value through the unchanged density/scatter push-constant ABI.
The shader accepts exact near/lateral boundaries within `1e-5` and clamps them
to the sampled domain. CPU tests pin translated-eye recovery plus center,
exact-near, and lateral-edge mappings and inverse round trips; the source gate
pins the production helper call and boundary tolerance. No production
descriptor, resource, or push-constant layout changed, so no full rebuild was
required for this review round.

The real RTX fixture uses the existing Task 11 density-layer override to seed
nonuniform cumulative tau, enables cloud shadows, and reads the production
scatter volume. Its GREEN diagnostics are `center local=2.4999 total=4.5000`
(endpoint remainder 2, more than 1 away from the incorrect 6.5),
`near local=0.0874 total=2.0881 alpha=0.6250`, and
`edge local=0.0213 total=4.0212`. Both boundary cases retain more than 1.5
coarse tau; the lateral endpoint is outside the froxel frustum.

Final GREEN evidence:

- PASS: `make -C MatterEngine3/tests run-cloud-shadows` (`ALL PASS`).
- PASS: `make -C MatterEngine3/tests run-shader-source`
  (`shader_source_tests: all passed`).
- PASS: `make -C MatterEngine3 vulkan-spirv`.
- PASS: `make -C MatterEditor build/windows/vulkan_smoke_tests.exe`
  (`-Wall -Wextra -Werror`).
- PASS: RTX `MATTER_VK_SMOKE_MODE=froxel-resize` (`ALL PASS`, validation
  errors 0, seeded diagnostics above).
