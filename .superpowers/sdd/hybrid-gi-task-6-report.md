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

- The GPU rejection image is produced and retained for later debug-view/counter work (Task 11); Task 6 validates exact rejection reasons through the CPU shader-contract mirror rather than adding a production synchronous readback.
- The temporal resources are allocated whenever raster targets are created, including RT-unavailable mode, so mode changes do not need a separate allocation path.
