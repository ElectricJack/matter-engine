# Streaming Mountains RT controls regression — 2026-09-17

## Cause and correction

The review launch sent `render_path raster`. That command assigned a separate
session override which took priority over the live Ray tracing checkbox, even
when the checkbox remained checked. GI therefore had no active traced input,
and opaque mesh rock shadows did not run. Voxel foliage has a separate shadow
pass, which explains why tree shadows could still appear.

The cloud/fog froxel pass also requires a current traced TLAS and effective RT
(`MatterEngine3/src/render/vk_scene_renderer.cpp`, `volumetrics_ready` in the
raster recording function). Thus the raster override suppressed this pass too.
Clouds, volumetrics and cloud shadows were all enabled in the user's live
properties. The new capture `rt-on-before-check.png` visibly restores the cloud deck.

`render_path` now writes `render.gpu.ray_tracing` through the existing typed
property setter. The frame reads that same property; there is no hidden FIFO
override. Capability checks and environment-forced property locks remain in
force. The command reports a failure if the property cannot be changed.

## Evidence and validation

- Original review: `C:/tmp/matter-mountain-rock-review-20260917`.
  `regression-before.png` shows checked RT/GI controls alongside “Vulkan raster”.
  Its log reports effective RT false and no traced dispatches.
- Original editor was already gone when the close request was checked; the
  requested final issue capture therefore did not complete. The existing
  screenshot and saved world properties preserve the review reference.
- `user-props-before.json` records the user's saved world settings before the
  new launch; these were not overwritten with authored defaults.
- Native MSVC RelWithDebInfo build passed. Build log, manifest and source hashes:
  `../2026-09-16-shared-vt-pixels/mountain-rt-controls-v1-*`.
- New interactive launch: `C:/tmp/matter-mountain-rt-review-20260917`, UI visible,
  StreamMountain, RT and GI explicitly enabled. Camera reconstructed from the
  last review screenshot. The user subsequently moved the camera while reviewing.
- Actual runtime RT dispatches were observed. `render_path raster` changed the
  live property to false and stopped dispatches; a subsequent
  `set render.gpu.ray_tracing true` restored dispatches. GI was acknowledged
  enabled. See `runtime-verification.json` and `runtime-controls.log`.
- The populated RT capture shows clouds and a ground shadow at the large rock.
  These are visual checks, not isolated shadow/GI pixel acceptance. No new
  isolated GPU suite was run before handing the editor back.
- Editor PID 48316 remains open for user review with RT and GI enabled at
  handoff. Further isolated GPU checks must wait until that session ends.

This build also includes the separately compiled sector POM endpoint fix. Its
strict seam GPU acceptance is still pending and is not claimed by this report.
