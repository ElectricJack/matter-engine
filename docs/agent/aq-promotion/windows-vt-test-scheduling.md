# Windows VT test scheduling — 2026-10-11

Task `swift-vault-98` investigates the three Vulkan failures in native Windows
run [38124103019](https://github.com/ElectricJack/matter-engine/actions/runs/38124103019).
The corrective policy is exclusive GPU use within CTest. Every `vulkan` or
`gpu` labelled test takes `RESOURCE_LOCK matter_gpu`; CPU tests retain the
workflow's parallelism of six. All three tests remain required on `dev`,
`staging` and `main`, with their original assertions, validation-error gates
and timeouts. No environment-dependent exemption is introduced.

## Evidence and cause

The failing job ran overlapping Vulkan processes on one RTX 4090, using
NVIDIA 610.74. Its normal-frame test overlapped the geometry-page,
face-projection and material-domain tests. The existing loader filter did
disable OBS, RenderDoc, Steam and EOS overlays. Missing EOS manifest messages
were loader diagnostics; the two completed failing tests both reported zero
Vulkan validation errors.

| Test | Failed CI run | Fresh MSVC build, serial native CTest |
| --- | --- | --- |
| `static_surface_vt_tests` | Generic GPU contract failure, 29.61 s | Pass, 5.50 s |
| `smoke_vt_normal_frame` | Killed at 124.97 s under its 120 s timeout | Pass, 106.69 s |
| `smoke_vt_module_residency` | Material-read fixture did not evict both source receiver pages, 33.68 s | Pass, 7.55 s |

The serial runs used the same native GPU/driver and overlay filter, with no
renderer or test changes. The source checkpoint was `f823151d8`; builds used
the canonical `tools/build-windows-from-wsl.sh RelWithDebInfo` wrapper.
The static-surface and material-read fixtures use five-second readiness
windows through `vt_prepare_tests::until`. The static test's original failure
does not identify which setup operation returned false. Serial success and
the large concurrent slowdown support contention as the cause, without
evidence of a component-separation VT regression.

The normal-frame test still uses the production sliced producer: its first
pose settled in 695 frames, with two filled pages and an empty, clean queue;
later poses settled in five frames. Minimum raster/oracle, RT/oracle and
raster/RT cosines were `0.999774`, `0.999998` and `0.999776`, respectively.
Module residency proved that two evicted receiver pages retained identical
compressed color/height bytes and that stale publication was rejected.
Both smoke modes printed `ALL PASS` and `validation errors: 0`.

## Policy and verification

`CMakeLists.txt` applies the common lock after registering the native tests.
The generated CTest inventory contains 26 locked GPU-labelled tests and 163
other tests without that lock. All 189 tests remain enabled. The normal-frame
timeout is still 120 seconds, and its `ALL PASS` and validation-error regular
expressions are unchanged. The lock coordinates tests in one CTest invocation;
separate invocations and interactive GPU applications must still be scheduled
by the runner owner.

The independent CPU oracle also passes:

```sh
node tests/vt_normal_frame_reference_tests.mjs
# 480 transformed directional probes passed
```

The focused verification command uses the same concurrency as CI:

```powershell
ctest --preset windows-msvc-relwithdebinfo --output-on-failure --no-tests=error `
  -j 6 -R '^(static_surface_vt_tests|smoke_vt_(normal_frame|module_residency|queue)|vt_(residency|compositor)_tests)$'
```

The post-fix launch of that command failed before CTest started with WSL
`UtilAcceptVsock: accept4 failed 110`; even subsequent `cmd.exe /c ver`
launches failed. This is an unavailable check, not a pass. The successful
post-fix CMake regeneration and property inventory above preceded the interop
failure. Follow-up execution and source publication are recorded in the task
comments.

Local native checks do not establish a green hosted run on `dev`. Both the
post-fix execution and exact-`dev`-SHA native Windows acceptance remain pending
with the integration owner; tests and timeouts stay required throughout.
