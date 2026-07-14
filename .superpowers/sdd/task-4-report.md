# Task 4 Report: Asynchronous Culling and Grouped Indirect Rasterization

## Status

Complete in commit `c0359e9` (`perf(vulkan): record asynchronous instanced frame`).

## Implementation

- Required and enabled `VkPhysicalDeviceFeatures::multiDrawIndirect`, with a
  named preflight diagnostic and a `VulkanDevice` capability query.
- Added an atomic immediate-submit diagnostic counter for smoke assertions.
- Added `VkSceneRenderer::record_cull_and_render`, which records the compute
  dispatch, the compute-to-indirect/vertex synchronization2 dependency, and
  G-buffer/HDR rasterization directly into the acquired frame command buffer.
- Preserved GPU ownership of culling, LOD selection, transform expansion, and
  indirect instance counts. The production `WorldSession::render` path does no
  cull-stat or indirect-command readback and performs no submit/wait boundary
  between dispatch and rasterization.
- Persisted per-part active-instance counts and command ranges. Rasterization
  issues one contiguous multi-draw indirect range per active part, splitting
  only when the device `maxDrawIndirectCount` requires it.
- Retained newly created raster attachments for the active frame, preserving
  the frame-lifetime contract from Task 1.

## Tests

- Added smoke coverage for multiDrawIndirect enablement, absence of immediate
  submissions in the record path, two active grouped ranges, and forced
  `maxDrawIndirectCount = 3` splitting with contiguous coverage.
- Strict CUDA 13.3 Windows Vulkan smoke build passed with `-Wall -Wextra
  -Werror`.
- A production-only strict compile of `vk_scene_renderer.cpp` without
  `MATTER_VK_TEST_FAULT_INJECTION` passed, confirming immediate diagnostic
  APIs are excluded from the production build.
- `MATTER_VK_SMOKE_MODE=cull`, `raster`, and `default` each reported
  `validation errors: 0` and `ALL PASS`.

## Concerns

- The host Vulkan loader emits pre-existing EOS overlay/ReShade discovery
  messages after successful tests; smoke validation remains zero.
