# VT normal-frame smoke readiness — 2026-09-29

Task `clear-summit-46` diagnoses the 32 failures found while validating
`clear-ridge.7` (cache-ordering implementation `4a1697e2`, evidence commit
`794c029a`). The failure is in fixture readiness: the normal-frame test samples
before its pinned VT tail can finish production. The normal transforms and
directional oracle are correct once the pages are resident.

## Unchanged baseline

The assigned branch starts at `2f328bd6`, without the cache-ordering changes.
A fresh canonical MSVC RelWithDebInfo `vulkan_smoke_tests` build at that commit
reproduces all 32 assertions from the original log: eight each for applied VT,
raster oracle, RT oracle and raster/RT parity. It exits 1 with zero Vulkan
validation errors. Every per-pose cosine matches the original
`/tmp/clear-ridge-7-smoke-vt-normal-frame.log`.

For example, yaw 0 gives raster/RT oracle cosines `0.924795` / `0.927751` and
parity `0.915039`. The RT cosine is the oracle's Z component: the ray sees the
geometric normal. Raster instead shows live tileset fallback detail, whose
world-space triplanar frame does not test the object's VT normal transform.
These fallback results cannot establish VT normal correctness.

## Cause and fix

`render/vt_types.h` defines 680 horizontal work tiles per physical page.
`VtGpuWorkBudget::tiles()` in `render/vt_work_budget.h` permits at most two
tiles/frame with the default enabled GPU time target. A newly produced tail
therefore needs at least 340 frames, followed by tail activation and feedback
retirement. `vt_residency.h` deliberately gates raster and RT VT slots until
that tail has been submitted.

The fixture previously rendered twelve frames per pose plus one RT probe:
104 frames over all eight yaw/roll poses. Even the first tail cannot complete
within that entire run. Cache ordering is unrelated; the hand-built fixture
also bypasses `build_vulkan_part`, where that ordering is applied.

`run_vt_rt_path` now advances presenting frames until production has completed
at least one page and the fill queue, dirty-page count and last-frame fills
stay empty for five consecutive frames. This covers asynchronous feedback
retirement as well as the fill itself. The wait is bounded at
`4 * vt::kVtPageTiles` frames per pose (2,720 with the current page geometry),
and failure to settle adds a failing readiness assertion with counters in the
log. The original material ownership, valid RT hit, applied VT, invalid-record,
directional oracle and parity assertions remain unchanged, including the
`0.995` oracle and `0.998` parity thresholds. Renderer, shader and scheduling
behavior are unchanged.

The previously unregistered mode is now CTest `smoke_vt_normal_frame`, with
the standard `ALL PASS` / validation-error gates and a 120-second timeout.

## Native verification

MSVC builds and GPU runs use RTX 4090 / NVIDIA 610.74. GPU checks ran serially.

| Configuration | Cold settle frames | Later poses | Result |
|---|---:|---:|---|
| Unchanged `2f328bd6`, original twelve-frame fixture | 12 per pose | 12 per pose | 32 failures, validation 0 |
| `2f328bd6` with readiness fix, default sliced producer | 693 | 5 | ALL PASS, validation 0 |
| Readiness fix with temporary `4a1697e2` source overlay, CTest | 695 | 5 | 1/1 passed, validation 0 |
| Same overlay, `MATTER_VT_FILL_BUDGET_MS=0` whole-page producer | 13 | 5 | ALL PASS, validation 0 |

All passing runs settle two pages with queue and dirty counts zero. Across
the eight poses, minimum raster oracle cosine is `0.999774`, RT oracle cosine
`0.999998`, and raster/RT parity `0.999776`. The whole-page run establishes that
the readiness check follows completion rather than assuming a particular
number of frames. The temporary cache-ordering overlay is removed from the
delivered branch; this fix has no dependency on it.

`node tests/vt_normal_frame_reference_tests.mjs` also passes all 480 independent
directional probes, including neutral normals, principal axes, nonuniform
scale, reflection and back-face cases.

The temporary cache-ordering build's `cull` mode with
`MATTER_GBUFFER_WORKLOAD=1` passes its query/frame-reuse scenarios with zero
validation errors.

After removing the overlay, the final MSVC builds and CTest run pass both
`vt_residency_tests` (including tile-budget/progress contracts) and
`smoke_vt_normal_frame`: 2/2 passed, with zero Vulkan validation errors. The
normal fixture again settles cold in 693 frames and later poses in five.

Repeatable commands from the repo root:

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests
WSLENV=MATTER_VK_SMOKE_MODE:TMP:TEMP MATTER_VK_SMOKE_MODE=vt-normal-frame \
  TMP=C:/tmp TEMP=C:/tmp \
  ./MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
WSLENV=MATTER_VK_SMOKE_MODE:MATTER_VT_FILL_BUDGET_MS:TMP:TEMP \
  MATTER_VK_SMOKE_MODE=vt-normal-frame MATTER_VT_FILL_BUDGET_MS=0 \
  TMP=C:/tmp TEMP=C:/tmp \
  ./MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
node tests/vt_normal_frame_reference_tests.mjs
./tools/build-windows-from-wsl.sh RelWithDebInfo vt_residency_tests
```

Alternatively, from native Windows with the resolved toolchain's `ctest.exe`:

```powershell
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo `
  --output-on-failure -R '^(smoke_vt_normal_frame|vt_residency_tests)$'
```

Local logs are `/tmp/clear-summit-46-baseline-normal.log`,
`/tmp/clear-summit-46-readiness-normal.log`,
`/tmp/clear-summit-46-cache-order-ctest.log` and
`/tmp/clear-summit-46-cache-order-unsliced-normal.log`. The final CTest summary
and per-pose output are retained as `/tmp/clear-summit-46-final-ctest.log` and
`/tmp/clear-summit-46-final-ctest-detail.log`; the build also holds
`Testing/Temporary/LastTest.log`.
