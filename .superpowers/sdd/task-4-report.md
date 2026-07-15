# Task 4 Report

Status: IMPLEMENTED; CONTROLLER VERIFICATION PENDING

## Implemented

- Added public `DlssMode` selection and truthful selected/active mode, internal/output extent, reset-count, and fallback-reason frame statistics.
- Added compile-safe DLSS constants/resource tagging and `evaluate_dlss` bridge contract. Native never evaluates. The test fake validates row-major matrix payloads, jitter and motion-vector metadata, exact resource layouts, distinct images, and one-shot failure reset.
- Kept production truthful without proprietary Streamline evaluation artifacts: the renderer advertises only Native and directly composites HDR when no linked evaluation adapter exists.
- Added lazy, extent-aware `R16G16B16A16_SFLOAT` output images per in-flight frame slot, frame retention, transitions, fake evaluation, and output composite before UI.
- Wired mode-specific temporal internal extents, evaluation failure fallback plus following-history invalidation, and F8 viewer reporting/cycling restricted to supported modes.

## Review fixes

- Extended the bridge contract with exact Vulkan format, extent, layout, stage,
  and access metadata, explicit HDR/auto-exposure options, an optimal-settings
  query, and an evaluation result that proves the output was written and
  describes its final synchronization state.
- Made depth and velocity attachment writes visible to DLSS compute reads, and
  made the output-to-blit barrier consume the evaluator's reported stage,
  access, and layout.
- Made the fake evaluator clear the real output image and verified that those
  pixels reach the swapchain readback. Added replacement coverage proving the
  old per-slot output stays retained through delayed completion and is released
  only when that slot is recycled.
- Fixed a pre-existing transform-only update regression: `update_instances`
  moved the command/layout arrays into rollback storage even when layout was
  unchanged, emptying later raster draws. The non-layout path now updates only
  instance/RT state and preserves command templates.
- Corrected the raster velocity expectation using the shared CPU temporal
  oracle. A 0.2 world-space move at z=-2 under the test's 90-degree perspective
  is 8 input pixels, matching the shader's 7.996 half-float readback, not 16.

## TDD evidence

- RED: smoke compile failed after adding tests for missing `DlssMode`, constants/resources, `evaluate_dlss`, fake evaluator, evaluation count, active mode, and history-reset APIs.
- GREEN: focused bridge seams and renderer fake path pass in the `cull` smoke; distinct per-slot outputs are asserted across in-flight slots.
- REVIEW RED: the extended bridge tests initially failed to compile because
  resource synchronization metadata, options, optimal settings, and output
  state were absent. Raster diagnostics also exposed an empty indirect-command
  template after a transform-only update.
- REVIEW GREEN (pre-final test edit): the strict smoke executable rebuilt; the
  command-template fix reduced raster failures from five to one with validation
  errors 0. The final remaining assertion was shown by both shader math and the
  CPU temporal oracle to contain an incorrect 16-pixel expectation.

## Verification

- `make -C MatterViewer build/windows/vulkan_smoke_tests.exe windows HAVE_STREAMLINE=0 CUDA_PATH=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3 -j4` - PASS.
- `MATTER_VK_SMOKE_MODE=default MatterViewer/build/windows/vulkan_smoke_tests.exe` - ALL PASS, validation errors 0.
- `MATTER_VK_SMOKE_MODE=cull MatterViewer/build/windows/vulkan_smoke_tests.exe` - ALL PASS, validation errors 0; includes fake DLSS evaluation/output/per-slot lifetime coverage.
- `git diff --check` - PASS.
- Final strict CUDA build and default/cull/raster executions were delegated to
  the controller environment after stale local smoke processes held the output
  executable open. No final pass is claimed here until that run completes.

## Concerns

- No Streamline SDK/evaluation adapter is present, so this task intentionally does not claim or expose live DLSS. `HAVE_STREAMLINE=1` runtime evaluation remains disabled with an explicit diagnostic; no proprietary artifacts were added.
- The machine's Vulkan loader logs unrelated stale EOS/ReShade layer
  diagnostics. Vulkan validation itself remained at zero in the last completed
  raster run.
