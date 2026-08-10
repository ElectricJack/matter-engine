# Task 13 report — cloud shadows across world lighting

Status: **DONE WITH ONE TOOLING CONCERN**. Cloud transmittance now attenuates
primary raster direct lighting, native-RT secondary/transmission direct sun,
and enhanced low-fog direct sun. Evaluated 9SH ambient, environment/miss light,
indirect illumination, and the Current volumetric path remain unchanged.

## TDD evidence

Real receiver assertions were added before the production shader changes.
Their RED results were:

- raster: `overhead cloud slab attenuates raster object direct lighting`;
- native RT: `overhead cloud slab attenuates native RT secondary-hit direct sun`;
- enhanced fog: `overhead cumulative cloud slab attenuates enhanced low-fog direct sun`.

The final deterministic GPU fixtures use a sliced, uniform overhead slab and
sun-on minus sun-off measurements. They prove attenuation below the slab,
unchanged SH ambient, clear behavior above the slab, disabled-state recovery,
and finite output. The raster receiver is the real alpha-tested mixed-material
G-buffer path (metallic 0.698), so it exercises diffuse, analytic metallic, and
vegetation-style geometry through the shared composite receiver shader. The
native RT check runs in a fresh, isolated atmosphere fixture, and the fog check
reuses the seeded Task 12 froxel readback seam.

Final raster diagnostics were `direct clear=0.2582683`,
`shadow=0.0349621`, and sampled transmittance `0.13525` (the expected
`exp(-2)`). A second production-fragment profile uses a 1000 m cloud top and
nonzero filter scale: sharp tau is `0..2.2505` (mean `1.1252`, zero transition
samples), while filtered tau is `0.9002..1.8001` (mean `1.3501`, all 40 samples
in the penumbra). The gate requires the wider transition, reduced contrast,
and mean drift below 12% of the original optical-depth range.

## Implementation

- Appended camera X/Z to the existing composite push prefix and reconstructed
  primary world position with reversed-Z view-axis depth and the off-axis ray
  correction. Raster diffuse/subsurface and metallic GGX consume one shared
  `visible_direct_sun`; ambient and emission do not.
- Added a fail-clear receiver-to-active-cloud-top helper. The cloud producer
  publishes the maximum finite enabled layer top through the previously free
  `cloud_state.w` lane, without changing `EnvironmentBlock` size or offsets.
  It uses the same contiguous active prefix as GPU cloud-layer packing.
- Multiplied both native-RT secondary direct estimators by the same set-1 cloud
  sample. This covers secondary surface bounces and the shared transmission
  entry/exit sunlit-hit path while leaving miss/environment radiance alone.
- Multiplied only enhanced `fog_sun` by cloud transmittance. The TLAS visibility
  query remains separate, `fog_ambient` is untouched, and Current stays on its
  established no-cloud-shadow scatter path.
- Added the smallest renderer test forwarder needed to generate sliced cloud
  density/cumulative fields and refresh the receiver descriptors.

## ABI reconciliation

The brief's 120-byte starting size was stale. The authoritative post-atmosphere
push prefix was 96 bytes. Every existing offset is preserved; `camera_pos_x`
and `camera_pos_z` are appended at offsets 96 and 100, producing exactly 104
bytes. CPU static assertions pin all three facts. Disassembled production
`composite.frag.spv` independently reports member offsets 96 and 100. No
sun/sky push lanes were restored; direct light continues to come only from
`environment.direct_world_sun_ratio.rgb`.

## Verification

- PASS: `make -C MatterEngine3/tests run-cloud-shadows` (`ALL PASS`).
- PASS: `make -C MatterEngine3/tests run-shader-source`.
- PASS: `make -C MatterEngine3 vulkan-spirv` and generated embedded SPIR-V.
- PASS: full `make -C MatterEngine3` and full `make -C MatterEditor windows`.
- PASS: final `raster`, isolated `atmosphere` native-RT, and `froxel-resize`
  real-Vulkan modes (`ALL PASS`, zero validation errors in each).
- PASS: `git diff --check`.

## Visual acceptance

The bounded non-streaming `AtmospherePresentationFixture` pass retained six
inspected 1280x720 PNGs under
`MatterEditor/build/validation/atmosphere-clouds/receivers/`:

- `receivers_ground_object_disabled.png`
- `receivers_ground_object_enabled.png`
- `receivers_cross_layer_disabled.png`
- `receivers_cross_layer_enabled.png`
- `receivers_low_fog_disabled.png`
- `receivers_low_fog_enabled.png`

Settings were an authored 4–6 m cloud deck (density 0.35, coverage 0.58,
zero wind), sun elevation 60 degrees / diameter 0.53 degrees, near/far
clipmaps resolving to their registered 250 m minimum coverage, filter 1, full
update, and low fog density 0.025 / floor 0 / falloff 18. The disabled ground
view contains only the object's geometric shadow; the enabled view adds the
broad cloud-deck cast shadow. The fog pair shows the corresponding attenuated
sunlit fog/receiver band. Cloud volume self-shadowing is present in both
enabled/disabled captures and is distinct from these cast receiver shadows.
FFmpeg pair metrics were 29.86 dB PSNR for ground/object and 34.27 dB for fog.
The cross-layer pair adds a second 8–10 m deck (density 0.22, coverage 0.62)
and visibly resolves its additional broad cast-shadow structure. The retained
harness ceilings are enforcing rather than unconditional: 30% ground/object,
70% cross-layer, and 90% low-fog pixels over channel tolerance 2.

The only tooling concern is that the optional legacy `img_diff.py` post-step
reported missing Pillow after all six PNGs and `.done` markers were written.
The single permitted rerun added and retained the cross-layer pair; direct
visual inspection and independent FFmpeg metrics were used instead.

## Scope

Only Task 13 production shaders/renderer support, deterministic receiver tests,
generated SPIR-V, the receiver capture lane, and this report are intended for
the commit. Protected pre-existing shader symlink deletions and `.tmp-task8/`
and `.tmp-task9-base/` were neither modified nor staged. Task 14 was not
started.

## Review fix round 1

Three Important findings were addressed before commit:

1. Receiver cloud top is now the maximum finite top of the active packed
   prefix, not every enabled array slot. A real renderer regression pins a
   disabled-layer-1 / enabled-900–1000-m-layer-2 case to layer 0's 5 m top.
2. The high-cloud/nonzero-filter real-GPU profile above directly proves the
   receiver-distance path widens penumbra while approximately conserving mean
   optical depth.
3. The visual lane now includes the inspected two-deck cross-layer pair and
   enforcing diff ceilings. The raster fixture explicitly proves the shared
   alpha-tested vegetation-style geometry path, avoiding streaming-dependent
   vegetation.

Focused review-fix verification passed the `-Werror` smoke rebuild, final
`raster` mode (`ALL PASS`, validation 0), shell syntax, visual inspection, and
`git diff --check`. The broader pre-existing `cloud_layer_tests` executable
still contains one stale Task 9 source-string expectation for
`enhanced_clouds_requested_`; it is unrelated to Task 13 and was not changed.
