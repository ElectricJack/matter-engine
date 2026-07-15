# Final Review Fix Report: Temporal History and Visibility Lifetime

## Scope

This change closes the two non-SDK Important findings from the whole-branch
review. It does not change the Streamline bridge or attempt the live DLSS SDK
integration assigned separately.

## A. Presented Temporal Jitter

Root cause: `TemporalState` retained only the last presented unjittered camera.
For the next candidate it reconstructed `previous_jittered` using the current
candidate's Halton offset, so motion vectors did not reference the projection
that was actually presented.

The temporal regression now asserts:

- A static rigid instance on the second presented frame has the known Halton
  delta `(-0.25, 1/3)` pixels.
- `previous_jittered` exactly matches the first successfully committed
  projection and jitter.
- Camera and rigid motion include their corresponding presented Halton deltas.
- A discarded attempt repeats its jitter and does not advance presented history.

RED evidence: the focused executable failed all four presented-jitter velocity
and projection assertions. GREEN implementation stores the candidate's
`current_jittered` matrix only in `commit_presented` and consumes that stored
matrix on the next non-reset frame. Failed attempts still cannot publish or
advance history.

## B. Fallback Visibility and Descriptor Ownership

Root cause: `visibility_.lifetime` was retained only inside the successful RT
trace path. Disabled/unavailable fallback frames submitted clear and composite
commands without retaining visibility. In addition, every frame shared one
composite descriptor set, so an extent change updated descriptors still owned
by the preceding in-flight frame.

The regression submits two frames without an intervening `wait_idle`: forced RT
unavailable at `160x100`, then RT disabled at `96x64`, on different frame slots.
It verifies current fallback reasons, extents, visibility usage, and zero
validation errors after both submissions.

RED evidence:

- Before lifetime retention: validation reported `vkDestroyImageView` and
  `vkDestroyImage` while the first submission still used them.
- After retaining only visibility: validation reported four
  `vkUpdateDescriptorSets` errors because the shared composite set was pending.

GREEN implementation unconditionally retains visibility with the other raster
attachments and allocates/updates one composite descriptor set per frame slot.
The same two-frame transition then passed with zero validation errors.

## Verification

```text
Focused MATTER_VK_SMOKE_MODE=rt-unavailable
PASS: temporal Halton/history assertions, overlapping extent/mode transition,
ALL PASS, validation errors: 0

make -C MatterViewer windows HAVE_CUDA=1 HAVE_STREAMLINE=0 \
  CUDA_PATH=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3 -j1
PASS: CUDA=1, OptiX=1 fallback viewer build

make -C MatterViewer vulkan-smoke HAVE_CUDA=1 HAVE_STREAMLINE=0 \
  CUDA_PATH=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3 -j1
PASS: six interop fault modes plus RT enabled/disabled/unavailable;
all bounded processes report ALL PASS and validation errors: 0

powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File MatterViewer/tools/check_vulkan_viewer.ps1
PASS

powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File MatterViewer/tools/smoke_vulkan_viewer.ps1
PASS: five viewer cases, resize and RT toggle, validation errors: 0
```

The Vulkan loader continues to print unrelated stale Epic overlay/ReShade
registration diagnostics; Vulkan validation remains at zero in every passing
gate.
