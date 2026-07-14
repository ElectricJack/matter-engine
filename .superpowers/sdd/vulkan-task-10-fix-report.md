# Vulkan Task 10 Review Fix Report

Status: complete in commit `86ea56d` (`fix(rt): harden CUDA Vulkan interop failures`).
This report is intentionally unstaged.

## Ownership and failure safety

- The interop cycle tracks kernel launch, CUDA signal enqueue, Vulkan readback
  submission, clear/readback fences, and fence completion separately.
- Every failure after a CUDA wait/kernel enqueue synchronizes the CUDA stream
  first under the interop context. Signal-enqueue failure never submits or
  waits for the cyan Vulkan timeline value.
- Failure-only Vulkan timeline/fence waits are bounded to five seconds; the
  normal readback fence is bounded to thirty seconds. No `UINT64_MAX` GPU wait
  remains in the interop path.
- If CUDA completion or a bounded Vulkan completion proof is unavailable, the
  interop is terminally poisoned. CUDA/Vulkan children, the CUDA context and
  driver DLL access, and the parent Vulkan logical device are intentionally
  preserved. The test exits through OS-owned cleanup rather than driver API
  teardown.
- A dedicated clear fence supplies Vulkan-visible proof before command buffers
  and the external semaphore are destroyed, including signal-enqueue failure.

## Context, identity, and handles

- Initialization saves/restores the caller current CUDA context. Round trips,
  failure recovery, cycle destruction, and reset use scoped CUDA context
  push/pop; tests create a real non-null caller context and assert it survives
  create, failure, round trip/reset, and destruction.
- UUID remains mandatory. When Vulkan exposes a valid LUID, CUDA LUID success,
  matching bytes, and matching node mask are all mandatory. A UUID-matched
  `cuDeviceGetLuid` failure is a hard diagnostic containing the CUDA result,
  both adapter names/UUIDs, Vulkan LUID/mask, and unavailable CUDA LUID/mask.
- The application-owned export-HANDLE counter increments after each successful
  Vulkan export and decrements only after successful `CloseHandle`. Tests
  require zero after semaphore import, every image import, failure cleanup,
  and reset.
- Phase 1 evidence remains explicit: NVIDIA's Vulkan driver lazily/deferredly
  creates and retains objects during first allocation/submission, so raw
  pre-init/post-teardown process handle comparison is not a valid gate. The
  gate is zero application-owned export handles plus warmed steady-state
  process handle delta no greater than two.

## Deterministic tests and gates

- Checked-in fresh-process gate covers:
  `after-kernel-before-signal`, `signal-enqueue-failure`,
  `after-signal-before-vk-wait`, `cuda-async-unproven`, `vk-wait-failure`, and
  recovery-side `vk-recovery-unproven`.
- Each child has a 15-second timeout, phase-specific diagnostic assertions,
  stable poison assertions, caller-context assertions, export-HANDLE zero, and
  no-destroy proof for CUDA/Vulkan unproven completion.
- Pure tests cover matching LUID/mask, LUID mismatch, node-mask mismatch,
  invalid CUDA LUID, and the hard LUID failure diagnostic.
- Viewer static/runtime scripts and the feature manifest require
  `CUDA_ACTIVE=1`, `OPTIX_ACTIVE=0`.

## Fresh verification evidence

- Forced `-Wall -Wextra -Werror` Windows CUDA 13.3 smoke build: PASS.
- Aggregate `make -C MatterViewer vulkan-smoke HAVE_CUDA=1`: PASS, including
  all six fresh-process fault modes, validation errors 0.
- Real interop, 100 alternating warmed 64x64 and 96x80 operations: PASS; exact
  center pixel `0 1 1 1`; steady handles `999 -> 999` (delta 0); validation 0.
- Post-teardown count was `1144` and is intentionally not claimed as within
  two because the NVIDIA lazy/deferred allocation evidence invalidates that
  comparison.
- Raster smoke: PASS, exact structural/material pixels, validation 0.
- Default Vulkan smoke: PASS, validation 0.
- Static viewer gate: PASS.
- Four-case Cornell viewer runtime smoke (demo/materials/resize/override):
  PASS, PNG/material/resize checks and validation 0.
- Manifest: `VULKAN=1`, `OPENGL=0`, `CUDA_AVAILABLE=1`,
  `OPTIX_AVAILABLE=1`, `CUDA_ACTIVE=1`, `OPTIX_ACTIVE=0`.

## Independent review

An independent read-only review found no remaining Critical defect. Its P1/P2
findings were closed before commit: accurate close-only decrement, reset
preservation recheck, checked-in timeout runner, phase diagnostics,
recovery-side Vulkan timeout preservation, and focused LUID/node-mask tests.
