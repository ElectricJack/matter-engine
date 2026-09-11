# swift-flare.3 reopen acceptance — 2026-09-11

Code checkpoints: `f94946ca` fixes the scene-GI dispatch flag and uses the
POM-lifted world position only for local-light visibility origins;
`76a383a3` isolates the diagnostic local-direct readback from the established
multi-attachment raster/GI readback; `05f6a424` keeps the analytic sources
clear of their RT-visible fixture housings.

## Automated/native checks

- Canonical `RelWithDebInfo` builds of `vulkan_smoke_tests` (bounded with
  `CMAKE_BUILD_PARALLEL_LEVEL=4`) and `matter_editor` passed.
- `MATTER_VK_SMOKE_MODE=rt-local-direct` passed all checks with Vulkan
  validation errors `0`. The measured glass lanes were GI-off
  `0.7160/0.5060/0.31128`, GI-on `0.55420/0.41553/0.27710`; measured/expected
  diffuse weights were `0.232/0.200`. See `vulkan-smoke-local-direct.log`.
- Shader-source, local-light-index, and LocalLightRtGallery Node fixture tests
  passed. The native capture logs contain no rejected property, bake error, or
  Vulkan validation error.

## Fixed-camera visual evidence

`capture.timeline` drove one native editor after its numeric `[bake-timing]`
publication line. Every comparison uses one camera, an explicit history reset,
and equal 96-frame settling:

- `corner-gi-off.png` / `corner-gi-on.png`: primary direct remains in both,
  but the outlined receiver is not visually unambiguous enough to close the
  colored-bounce gate.
- `materials-gi-off.png` / `materials-gi-on.png`: the warm gold and closed
  glass remain locally lit, but all native-lit surfaces expose a stable
  black-pepper pattern. These six shots are diagnostic evidence, not final
  visual acceptance.
- `proxy-visible.png` / `proxy-hidden.png`: only the cosmetic emitter mesh
  disappears; analytic illumination and physical fixture bodies remain.

The bounded follow-up uses the same non-streamed fixture. A permanently
authored `ForestFloor` detail slab is lit by the unobstructed warm point source.
`pom-native-rt.png` and `pom-raster.png` share the grazing camera and show the
same visibly parallaxed relief retaining local illumination; this exercises the
lifted RT visibility origin without streamed terrain/cache work.

`zero-before-reload.png` and `zero-after-reload.png` were captured in the same
editor process and camera. Between them only the default root/light factory
arguments were temporarily changed from `true` to `false`, followed by
`reload`; the after commands were not appended until the second numeric
`[bake-timing]` publication line. The log records `published 0 lights, 0
cells/0 buckets`; the after image is black. Both source arguments were restored
to `true` before this evidence was committed.

## Corrected local-direct grain controls

The final six controls were captured with the corrected 2026-09-11 09:49:44
MSVC editor supplied by the renderer owner. That build uses frame-varying
finite-radius samples, a separate full-resolution local-direct temporal
history, and one narrow edge-aware spatial pass. All three controls use the
same GI-off material camera and independent history resets at one and 96
presented frames:

- `old-frame-1.png` / `old-frame-96.png` transiently recreate the former
  intersecting housing. Sampling noise converges, but the invalid emitter/body
  intersection leaves visibly dark, fuzzy residuals at the fixtures.
- `cleared-frame-1.png` / `cleared-frame-96.png` use the committed housing
  clearance. The noisy first sample converges to smooth soft shadows while
  direct illumination remains present across the floor, gold, glass, and pale
  receivers.
- `zero-radius-frame-1.png` / `zero-radius-frame-96.png` temporarily set only
  the six source radii to zero. Both frames show the expected stable hard-shadow
  control without black pepper.

`grain-old.log`, `grain-cleared.log`, and `grain-zero-radius.log` record six
published lights, native RT effective, zero bake errors, and clean editor exit.
They contain no rejected property, Vulkan validation error, device loss, or
fatal renderer message. The authored housing positions and all six source
radii were restored before archiving the evidence.

The WSL invocation passed
`WSLENV=...:MATTER_WORLD:MATTER_CMD_FIFO/p:...` so the Windows child received
both the selected world and translated command-file path. The editor wrote all
PNG/`.done` pairs and exited 0; the POSIX `drive.py` post-check nevertheless
reported a false-negative because it treated the timeline's `C:/...` shot
paths as relative Linux paths. The files above were verified directly at
`/mnt/c/tmp/local-light-rt-grain-controls/` before copying them here.
