# Live Streamline DLSS SR Adapter Report

Date: 2026-07-14

## Outcome

The Vulkan renderer now evaluates NVIDIA DLSS Super Resolution through the
official signed Streamline 2.12 runtime. The production path uses the
`VulkanDevice`-owned `StreamlineBridge`, queries DLSS optimal render sizes,
tags the renderer's HDR, depth, motion-vector, and output images, supplies the
complete camera/constants contract, and records `slEvaluateFeature` on the
current Vulkan command buffer.

The SDK is not vendored. A Streamline-enabled build requires `STREAMLINE_PATH`
and copies the signed development DLL set from `bin/x64/development` beside the
viewer. Builds with `HAVE_STREAMLINE=0` retain the native Vulkan fallback and
do not include Streamline headers or calls.

## TDD and debugging evidence

The initial strict smoke compile failed because `DlssResource` did not expose
the Vulkan view, memory, usage, or aspect metadata required by Streamline and
because the renderer did not expose device-owned bridge usage. The contract
test was completed first, followed by the implementation.

Live validation then identified and drove three production fixes:

1. Streamline's Vulkan core creates a private-data slot, so the requested
   Vulkan 1.3 feature set now explicitly enables `privateData`.
2. `slEvaluateFeature` returned `eErrorMissingInputParameter` until the
   matching viewport handle was included in its input structure list.
3. `eOnlyValidNow` caused unnecessary Streamline clone copies whose internal
   images produced undefined-layout validation errors. The four resources are
   valid through the immediate evaluate call and are now tagged
   `eValidUntilEvaluate`.

The manual-hook integration creates the Vulkan instance and device natively,
registers them with `slSetVulkanInfo`, and routes the required WSI operations
through Streamline dispatch. This avoids proxy `vkCreateDevice` applying the
already-merged feature, extension, and queue requirements a second time.
Proxy-created surface/swapchain lifetime is tracked so the interposer remains
loaded until those objects are destroyed, while `slShutdown` runs with the
native device and instance still alive so Streamline can release its Vulkan
children.

## Live production gate

Configuration:

- Windows Vulkan viewer
- `HAVE_CUDA=1`
- `HAVE_STREAMLINE=1`
- official Streamline 2.12 development SDK
- NVIDIA GeForce RTX 4090
- validation layer enabled
- `MATTER_DLSS_MODE=quality`
- 1280x720 output

Observed output:

```text
DLSS selected=Quality active=Quality internal=853x480 output=1280x720 resets=0 reason=none
Vulkan RT observed effective=true dispatches=1 reason=none
screenshot written to .codex-tmp/quality-live.png
```

The process exited normally, the screenshot was visually inspected, and the
run emitted zero Vulkan validation errors and no fatal diagnostics. The
development Streamline/DLSS watermark is expected in this SDK build.

## Verification gates

- Streamline enabled, CUDA enabled: full Windows build passed.
- Streamline enabled: strict `-Wall -Wextra -Werror` smoke/ABI compile against
  the official 2.12 headers passed.
- Streamline disabled, CUDA enabled: forced full Windows rebuild passed.
- Streamline disabled: strict Vulkan/CUDA smoke suite passed all injected
  interop-failure cases plus native RT, RT-disabled, and RT-unavailable cases;
  every case reported `validation errors: 0` and `ALL PASS`.
- `MatterViewer/tools/check_vulkan_viewer.ps1` passed.
- `git diff --check` passed.

## Operator interface

F8 continues to cycle Native, Quality, Balanced, and Performance at runtime.
For deterministic demos and automation, `MATTER_DLSS_MODE` accepts `native`,
`quality`, `balanced`, or `performance` as the initial selection.

## Deliberate limitations

- This milestone is DLSS Super Resolution only. Frame generation, ray
  reconstruction, Reflex, and DLAA policy are not integrated.
- The development SDK displays NVIDIA's non-distribution watermark. Shipping
  requires NVIDIA-provided production binaries and the applicable licensing
  process.
- The SDK is intentionally external and is not committed to the repository.
- RT resolution scaling and BLAS compaction were explicitly outside this task.

## Post-review state-machine hardening

Whole-branch review found two additional transition/fallback gaps and one
resource-contract inconsistency. They were fixed with new regressions before
the implementation was accepted:

- Every selected mode now passes through `StreamlineBridge`. A live
  Quality-to-Native transition calls `slDLSSSetOptions` with `eOff`, updates
  the active mode to Native, direct-composites HDR, and requests exactly one
  temporal reset. Returning to Quality evaluates DLSS again. Renderer-owned
  output images and Streamline feature allocations are deliberately retained
  until normal renderer/Streamline teardown so rapid toggles do not perform
  unsafe in-flight destruction or allocation churn.
- Streamline initialization preserves an explicit native-retry requirement.
  Injected missing instance-dispatch and device-dispatch cases both tear down
  the partially created native Vulkan stack and successfully restart with the
  Native backend. Both report zero validation errors.
- Since the renderer does not provide a dedicated exposure texture, DLSS now
  consistently enables auto exposure. This live path exposed that SDK
  `sl::SubresourceRange` scalar fields are not zeroed by its default
  constructor; all aspect, mip, layer-base, and count fields are now assigned
  explicitly.

The fake renderer regression observes the exact Quality, Native, Quality
sequence, verifies Native does not allocate a DLSS output for its frame slot,
checks direct HDR composite rather than stale upscaled output, and verifies the
single reset/output lifecycle. The signed live gate observed:

```text
DLSS selected=Quality active=Quality internal=853x480 output=1280x720 resets=0 reason=none
DLSS selected=Native active=Native internal=1280x720 output=1280x720 resets=1 reason=none
DLSS selected=Quality active=Quality internal=853x480 output=1280x720 resets=1 reason=none
```

RTX remained effective with one dispatch, the final screenshot completed, and
the run emitted zero Vulkan validation or fatal diagnostics. The disabled
strict suite now contains eleven bounded modes: the two proxy-retry cases, six
CUDA ownership-fault cases, and the three native-RT availability modes.
