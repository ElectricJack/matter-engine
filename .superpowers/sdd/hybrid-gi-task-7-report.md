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
  submission or wait was added; the immediate 9x9 fixture is test-only.

## TDD Evidence

The first strict smoke build failed before implementation on the deliberately
missing `GiAtrousGpuFixture`, `GiAtrousGpuResult`,
`test_dispatch_gi_atrous_fixture`, shader, and pipeline/readback support. After
correcting one CHECK macro parenthesization in the new test itself, the failure
was exclusively the missing A-trous feature surface.

The GREEN real-GPU test uploads a synthetic noisy 9x9 RGBA16F signal and guide
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
- RT mode executed the real RTX 4090 9x9 filter/readback fixture and the
  diffuse-GI reference scene.
- Default mode retained Native and fake-DLSS Quality/fallback compatibility.
- The Vulkan loader still reports the pre-existing ReShade injection warning;
  it does not increment validation errors.

## External SDK Status

`STREAMLINE_PATH` and `SL_SDK_PATH` remain unset and no workspace `sl.h` is
present. The verified build truthfully uses `MATTER_HAVE_STREAMLINE=0`; no
SDK-backed `HAVE_STREAMLINE=1` success is claimed.

## Concerns for Review

- The filter is intentionally diffuse-only for this task. Its signal-mode and
  kernel controls are explicit so later reflection/transmission tasks can reuse
  the dispatch contract without changing the five-pass scheduling.
