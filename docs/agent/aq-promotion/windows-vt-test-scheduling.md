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

### Composition with CI tier labeling

The separately held `keen-current-19` checkpoint `2e67` introduces
`cmake/MatterCiTests.cmake`. Its `matter_label_ci_tests` helper currently
assigns `RESOURCE_LOCK matter-vulkan`, replacing any existing locks. That
assignment must be reconciled when the integration owner combines the work;
the frozen checkpoint is not changed by this task.

The composition contract is:

- Use `matter_gpu` as the single canonical GPU lock name. Consolidate the
  root-directory loop and the CI helper's GPU-lock assignment into one final
  recursive helper, called after all tests and their labels are registered.
  Every `gpu` or `vulkan` test in every directory must acquire that lock,
  regardless of CI tier.
- Append the canonical lock while retaining any existing unrelated resource
  locks. Remove the superseded `matter-vulkan` assignment during composition;
  a later labeling helper must not replace or split the shared GPU lock.
- After all helpers execute, inspect `ctest --show-only=json-v1` from the
  composed native configuration. Check every GPU test has `matter_gpu`,
  unrelated locks are preserved, CPU-only tests do not gain the GPU lock,
  and no tests are disabled. Check the three affected tests retain their
  timeouts, assertions and validation-error gates. The fast CPU tier alone
  does not satisfy this task's required native `dev` acceptance.
- Run the focused `-j 6` command below and the required native Windows gate
  on the exact composed `dev` SHA. Keep the staging/main requirements intact.

The 26-test lock inventory above verifies this task's standalone policy.
It does not verify the future composed helper: both its effective native
properties and execution remain pending with the integration owner. Native
launch retries are paused while the daemon owner handles the shared WSL
interop failure and the current fleet qualification remains in progress.

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

## Final native `dev` acceptance

The integration owner merged the published source into `dev` at
`72cda2a3f64f32c7562d32f33afb8114142130e8`. Native Windows run
[38131013670](https://github.com/ElectricJack/matter-engine/actions/runs/38131013670)
passed on 2026-10-11, including the full MSVC build, all 189 CTests and package
validation. Its uploaded `ctest.xml` reports zero failures, disabled tests or
skips. CTest retained parallelism six; the execution log shows all 26 GPU
tests ran without overlapping one another while overall concurrency reached
six. Test execution took 1060.51 seconds.

| Previously failing test | Native `dev` result |
| --- | --- |
| `static_surface_vt_tests` | Pass, 3.08 s |
| `smoke_vt_normal_frame` | Pass, 67.90 s |
| `smoke_vt_module_residency` | Pass, 5.96 s |

This satisfies the task's exact-`dev`-SHA native Windows acceptance without
changing renderer behavior, test assertions or timeouts. The separate future
CI-helper composition must still preserve the contract above.
