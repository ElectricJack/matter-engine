# Hybrid GI Task 6 Report: Temporal Reprojection, Rejection, and Moments

## Implementation

- Added a test-first CPU mirror of the GI temporal contract with presentation-token candidate/commit semantics, current-minus-velocity pixel reprojection, unique rejection bits, 32-frame history cap, luminance moments, and the diffuse alpha floor.
- Added `gi_temporal.comp`. It consumes raw diffuse GI plus full-resolution velocity/depth/normal/material/instance inputs, scales pixel-space motion into the GI resolution, rejects bounds/depth/normal/material/instance/reset discontinuities, clips reprojected history to a 3x3 YCoCg neighborhood, and writes accumulated radiance, moments, history length, previous geometry/identity, and rejection reason.
- Added two complete retained GPU history sets. Per-frame descriptor sets avoid descriptor mutation while another frame slot is live. The compute pass records after the RT lighting dispatch on the acquired frame command buffer; no immediate submit or wait was added.
- The renderer records into the non-presented ping-pong set and publishes that index only in `finish_ray_tracing_frame(..., true)`. A failed presentation leaves the prior index and attempt token intact. Resize, temporal reset/camera cut/world reload, material revision, and Native/DLSS mode changes request a reset that remains pending until a successful presentation.
- The composite samples the current candidate accumulated radiance for the frame being presented. Raw diffuse readback remains available as the pre-denoiser diagnostic signal.

## TDD Evidence

RED strict build failed on the intentionally missing `GiTemporalState`, `GiTemporalSurface`, `GiTemporalResult`, and six rejection-bit APIs. The tests covered three static frames, translated-pixel coordinate selection, depth/normal/material/instance rejection, failed-present rollback, and one-frame invalidations.

GREEN strict build passed after the minimal CPU contract implementation. GPU assertions additionally prove that an RT dispatch targets the opposite ping-pong set, a failed present does not publish it, and a successful retry does.

## Verification

```text
C:\msys64\usr\bin\make.exe build/windows/vulkan_smoke_tests.exe -j1
```

- PASS with `-Wall -Wextra -Werror` and `MATTER_HAVE_STREAMLINE=0`.

```text
C:\msys64\usr\bin\make.exe vulkan-spirv matter_engine.o vk_temporal.o -j2
C:\msys64\usr\bin\make.exe build/windows/vk_scene_renderer.o -j1
```

- PASS. `gi_temporal.comp` compiled for Vulkan 1.3 and was embedded; production renderer and temporal objects compiled.

```text
MATTER_VK_SMOKE_MODE=rt build/windows/vulkan_smoke_tests.exe
MATTER_VK_SMOKE_MODE=rt-unavailable build/windows/vulkan_smoke_tests.exe
MATTER_VK_SMOKE_MODE=rt-disabled build/windows/vulkan_smoke_tests.exe
MATTER_VK_SMOKE_MODE=raster build/windows/vulkan_smoke_tests.exe
build/windows/vulkan_smoke_tests.exe
```

- All five modes: exit 0, `ALL PASS`, `validation errors: 0`.
- The RT mode covers real RTX dispatch, failed-present retry, GPU ping-pong publication, and raw GI fixtures.
- RT-unavailable covers replacement/resize fallback; default covers Native and fake-DLSS Quality/fallback transitions.
- The loader still emits the pre-existing stale Epic overlay/ReShade host warnings; these do not increment Vulkan validation errors.

## External SDK Status

`HAVE_STREAMLINE=1` remains truthfully unverified: `STREAMLINE_PATH`/`SL_SDK_PATH` are unset, no workspace `sl.h` exists, and `Libraries/streamline` is absent. The enabled build therefore continues to use and report `MATTER_HAVE_STREAMLINE=0`; no SDK-backed success is claimed.

## Concerns for Review

- The temporal resources are allocated whenever raster targets are created, including RT-unavailable mode, so mode changes do not need a separate allocation path.

## Review-Fix Verification

The first review identified five Important gaps; all were addressed with focused RED/GREEN tests:

- The temporal dispatch now refreshes the acquired frame-slot composite descriptor after selecting the candidate output. A real RTX pixel reads back accumulated radiance and final HDR; reconstructing the composite equation matches only when the accumulated term is included.
- Motion vectors now use the negative-height viewport's top-left pixel convention on both CPU and GPU. The real raster attachment proves `(+8,-8)` for simultaneous positive X/Y object motion, and the real temporal shader proves a `(1,1)` current-to-previous velocity selects `(x-1,y-1)` via a patched GPU history texel.
- G-buffer producer transitions now expose albedo/normal/ORM to RT, normal and material/instance identity to temporal compute, and velocity to compute, while retaining fragment consumers. GPU validation remains zero.
- A test-only real `gi_temporal.comp` fixture covers XY reprojection, history length, moments, 3x3 clipping, and exact bounds/depth/normal/material/instance/reset rejection values. Its immediate submission/wait is compiled only under `MATTER_VK_TEST_FAULT_INJECTION`; production still records exclusively on acquired frame command buffers.
- RT disable/re-enable, Quality/Native selection through a fake Streamline bridge, and subsequent stable frames prove reset counts advance exactly once. Streamline's later reset notification is suppressed only when an actually recorded GI candidate already consumed the same mode reset; transitions without a GI candidate still propagate to `TemporalState`.

Review-fix RED evidence:

```text
strict smoke compile
error: gbuffer_sampled_stages_for_test is not a member
error: VkSceneRenderer has no member test_composite_uses_gi_temporal
error: VkSceneRenderer has no member test_gi_history_reset_count
```

```text
strict smoke link
undefined reference to VkSceneRenderer::test_dispatch_gi_temporal_fixture(...)
```

The first GPU-fixture execution intentionally exposed its missing test harness synchronization: descriptor-set-in-use VUID `03047` and four sampled-image layout mismatches. The fixture now recycles the test frame slot and transitions its synthetic inputs before dispatch. This wait is test-only. The corrected run has zero validation errors.

Final commands and results:

```text
C:\msys64\usr\bin\make.exe build/windows/vulkan_smoke_tests.exe -j1
C:\msys64\usr\bin\make.exe vulkan-spirv matter_engine.o vk_temporal.o -j2
C:\msys64\usr\bin\make.exe build/windows/vk_scene_renderer.o -j1
```

- PASS. Strict smoke uses `-Wall -Wextra -Werror`; production objects and both updated shaders compile/embed.

```text
MATTER_VK_SMOKE_MODE=rt
MATTER_VK_SMOKE_MODE=rt-unavailable
MATTER_VK_SMOKE_MODE=rt-disabled
MATTER_VK_SMOKE_MODE=raster
default Native/fake-DLSS
```

- Every mode exits 0 with `ALL PASS` and `validation errors: 0`.
- `HAVE_STREAMLINE=1` remains SDK-header blocked and is not claimed; all final builds truthfully report `MATTER_HAVE_STREAMLINE=0`.
