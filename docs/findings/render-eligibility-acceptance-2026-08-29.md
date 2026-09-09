# Render eligibility and raster-only water acceptance

**Date:** 2026-08-29

**Task base:** `0a79c2a9` (`perf(water): keep animated water out of ray tracing`)

**Host:** Windows, native MSVC v143 build through the repository CMake/Ninja wrapper

## Result

The render-eligibility contract passed its native acceptance gates. A
false-only synthetic part remained raster-visible and produced no RT geometry,
BLAS, or TLAS work. Two placements of one shared part, one false and one true,
both remained in raster staging while only the true placement produced one RT
record and one shared per-part BLAS build. Active and fallback water remained
raster-only with zero animated-water RT decode.

This closes render eligibility and removal of animated-water BLAS caching. It
does **not** complete the forward-water optics work: refraction, depth-dependent
absorption/fog, turbulence-led foam, flow-aligned detail, scene/environment
reflection, representative screenshots, and matched performance captures are
the next committed plan.

## Exact commands

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
$env:MATTER_VK_SMOKE_MODE='water-animation'; & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe

tools/build-windows.ps1 -Config RelWithDebInfo
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -L cpu --output-on-failure
$env:MATTER_VK_SMOKE_MODE='water-animation'; & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
```

The host did not have the pinned Python 3.13.14 dependency. The successful
native builds used the Codex bundled Python 3.12.13 through temporary local
version/discovery accommodations. Those accommodations were fully restored
before staging; `git diff --exit-code` was clean for `CMakeLists.txt`,
`tools/windows/MatterWindowsToolchain.psm1`,
`cmake/tests/python_contract_tests.ps1`, and
`tools/tests/windows_toolchain_tests.ps1`.

## RED evidence

After the new acceptance scenarios were written, the renderer's production
`if (source.ray_traced)` filter was temporarily mutated to admit every
instance. The focused target built, and the `water-animation` run exited 1
with five expected failures and zero validation errors:

```text
false-only: raster=1 rt_instances=1 records=1 blas=1 tlas=0->1
mixed:      raster=2 rt_instances=2 records=2 blas=1 tlas=0->1
active water: decode=0 blas=1 records=1 tlas=0->1
validation errors: 0
5 FAILURE(S)
```

This proves the new assertions detect the wrong RT-membership branch while
leaving raster placement counts observable. The exact production predicate was
then restored with no retained production diff.

## GREEN evidence

### Native build and CPU suite

- Full `RelWithDebInfo` build: exit 0; the 136-action Ninja graph completed and
  produced `MatterEditor/build/windows-msvc/editor.exe`.
- CPU-labeled CTest suite: **58/58 passed**, 0 failed; 219.22 seconds real time.
- The build retained existing MSVC warnings (including C4005, C4099, C4100,
  C4127, C4324, C4459, C4702, and C4996); no warning was promoted to an error.

### Vulkan device

```text
Vulkan adapter: NVIDIA GeForce RTX 4090
driver: NVIDIA (610.74, 0x98928000)
API: 1.4.341
```

### Synthetic eligibility counts

| Scenario | Raster placements | RT instances | RT geometry records | BLAS builds | TLAS builds |
|---|---:|---:|---:|---:|---:|
| False-only, one triangle instance | 1 | 0 | 0 | 0 | 0 -> 0 |
| Mixed shared part, false + true | 2 | 1 | 1 | 1 | 0 -> 1 |

The mixed result demonstrates sharing at the part boundary: two raster
placements of the same registered triangle require exactly one eligible BLAS,
and only the true placement enters the TLAS record set.

### Water counters

| State | Raster evidence | RT instances/records | Decode dispatches | BLAS builds | TLAS builds |
|---|---:|---:|---:|---:|---:|
| Active animation | 1 direct animated draw recorded | 0 records | 0 | 0 | 0 -> 0 |
| Accepted static fallback | 1 raster placement | 0 instances | 0 | 0 | unchanged |

The final focused executable exited 0 with `ALL PASS` and
`validation errors: 0`. The Vulkan loader emitted its existing general
warnings about raster vertex attributes unused by a shader; these did not
increment the validation-error counter.

## Remaining scope

- Forward-water optical quality and its representative screenshots are not
  part of this acceptance and are not claimed complete.
- Matched RiverFloatLab camera timings at 1, 10, and 16 shadow samples remain
  to be captured with the forward-water work.
- Changed-only/stable-slot TLAS maintenance remains a separate renderer-scaling
  roadmap item; this acceptance covers eligibility membership, not that CPU
  scaling redesign.
