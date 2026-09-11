# swift-flare.3 reopen acceptance — 2026-09-11

Code checkpoints: `f94946ca` fixes the scene-GI dispatch flag and uses the
POM-lifted world position only for local-light visibility origins;
`76a383a3` isolates the diagnostic local-direct readback from the established
multi-attachment raster/GI readback.

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

- `corner-gi-off.png` / `corner-gi-on.png`: primary direct remains in both;
  the outlined receiver gains the intended colored indirect contribution.
- `materials-gi-off.png` / `materials-gi-on.png`: the warm gold and closed
  glass remain locally lit, with the off-screen green response visible under
  GI.
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
