# Hybrid GI Task 7 Report: Variance-Guided A-trous Denoising

## Implementation

- Added `gi_atrous.comp`, a reusable diffuse signal filter selected by explicit
  signal-mode, kernel-radius, and step-width push constants.
- Added five variance-guided A-trous iterations with widths 1, 2, 4, 8, and
  16. The filter uses temporal luminance moments plus depth-plane distance,
  normal agreement, and exact material identity.
- Added two retained RGBA16F ping-pong images. Three immutable descriptor sets
  per acquired frame slot describe temporal-to-A, A-to-B, and B-to-A, avoiding
  mutation of descriptors already referenced by an earlier recorded dispatch.
- Added compute write-to-read barriers after every iteration and exposes the
  fifth result to fragment sampling before refreshing the acquired frame's
  composite descriptor. Raster readback now reports the same filtered signal
  consumed by the composite.
- Invalid history, background material identity, non-finite signals/depth, and
  invalid normals copy through without filtering. No production immediate
  submission or wait was added; the immediate synthetic fixture is test-only.

## TDD Evidence

The first strict smoke build failed before implementation on the deliberately
missing `GiAtrousGpuFixture`, `GiAtrousGpuResult`,
`test_dispatch_gi_atrous_fixture`, shader, and pipeline/readback support. After
correcting one CHECK macro parenthesization in the new test itself, the failure
was exclusively the missing A-trous feature surface.

The GREEN real-GPU test uploads a synthetic noisy RGBA16F signal and guide
images, dispatches the actual embedded compute shader, and reads back the fifth
iteration. It proves variance decreases independently on both sides of a
depth/normal/material discontinuity, boundary leakage remains below 2 percent,
every output is finite, invalid/background pixels pass through, constant color
is an identity, and the exact step sequence is 1/2/4/8/16.

## Verification

```text
C:\msys64\usr\bin\bash.exe -c "...; make vulkan-spirv -j2"
C:\msys64\usr\bin\bash.exe -c "...; make build/windows/vulkan_smoke_tests.exe -j1"
C:\msys64\usr\bin\bash.exe -c "...; make build/windows/vk_scene_renderer.o -B -j1"
```

- PASS: `gi_atrous.comp` compiled for Vulkan 1.3 and was embedded.
- PASS: strict smoke executable compiled with `-Wall -Wextra -Werror`.
- PASS: the production Vulkan renderer object rebuilt.

```text
MATTER_VK_SMOKE_MODE=rt
MATTER_VK_SMOKE_MODE=rt-unavailable
MATTER_VK_SMOKE_MODE=rt-disabled
MATTER_VK_SMOKE_MODE=raster
default Native/fake-DLSS
```

- All five fresh modes exited 0 with `ALL PASS` and `validation errors: 0`.
- RT mode executed the real RTX 4090 filter/readback fixture and the
  diffuse-GI reference scene.
- Default mode retained Native and fake-DLSS Quality/fallback compatibility.
- The Vulkan loader still reports the pre-existing ReShade injection warning;
  it does not increment validation errors.

## External SDK Status

The SDK was subsequently supplied at
`/d/SDKs/streamline-sdk-v2.12.0`. Vulkan preflight passed with
`HAVE_CUDA=1 HAVE_STREAMLINE=1`, and both the strict smoke executable and
production Vulkan renderer object compiled with `MATTER_HAVE_STREAMLINE=1`.

## Concerns for Review

- The filter is intentionally diffuse-only for this task. Its signal-mode and
  kernel controls are explicit so later reflection/transmission tasks can reuse
  the dispatch contract without changing the five-pass scheduling.

## Review-Fix Evidence

The first review found that the reported step sequence was CPU-authored and the
9x9 extent made width 16 unable to reach any off-center texel. The focused RED
test required missing `gpu_step_widths` and `penultimate` GPU results and failed
to compile before the instrumentation existed.

The shader now writes its actual step width into a per-frame storage-buffer
slot from invocation `(0,0)` of each dispatched pass. The test clears that GPU
buffer, executes the production dispatch loop, applies a compute-to-host
barrier, and reads the five slots. Removing a dispatch leaves its slot zero;
reordering step constants changes the observed array.

The fixture is now 65x9 so width 16 has off-center support. Temporal moments
contain nonzero variance, and the test requires at least 25 percent variance
reduction in each region. Actual RTX 4090 evidence was:

```text
GPU step widths: 1 2 4 8 16
left variance:  0.122500 -> 0.001019
right variance: 0.062499 -> 0.000374
pass-5 maximum delta from pass 4: 0.044922
```

The untouched pass-4 ping-pong image is read back beside the final pass-5
image, proving the width-16 pass materially contributes. A separate
bright-left/black-right fixture measures actual energy arriving on the black
side and retains the less-than-2-percent requirement. Finite output,
constant-color identity, and invalid/background passthrough remain covered.

Fresh verification after the fix:

```text
make vulkan-preflight HAVE_CUDA=1 HAVE_STREAMLINE=1 \
  STREAMLINE_PATH=/d/SDKs/streamline-sdk-v2.12.0
make -W ../MatterEngine3/src/render/streamline_bridge.cpp \
  build/windows/vulkan_smoke_tests.exe HAVE_CUDA=1 HAVE_STREAMLINE=1 ...
make -B build/windows/vk_scene_renderer.o \
  HAVE_CUDA=1 HAVE_STREAMLINE=1 ...
```

- Enabled preflight, strict smoke compile, and production renderer compile:
  PASS with the supplied Streamline 2.12 SDK and CUDA 13.3.
- Fresh `rt`, `rt-unavailable`, `rt-disabled`, `raster`, and default
  Native/fake-DLSS modes: all `ALL PASS`, all `validation errors: 0`.
