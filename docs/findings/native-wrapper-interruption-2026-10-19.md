# Native wrapper interruption before CMake

Task `azure-lantern-65`, which blocks `nimble-rapids-30`. This fixes the
launcher that let the historical VT native build die with `0xC000013A`. It
does **not** run VT or VT-RT; that comparison stays with `nimble-rapids-30`.

## Symptom

Both historical attempts launched the unchanged supported command,
`tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests`,
via `run_native_build.ps1` in the nimble evidence
(`docs/agent/evidence/2026-10-19-vt-native-pair/` and
`docs/agent/evidence/2026-10-14-vt-provenance/` on that branch):

| attempt | export | child exit | elapsed | last output |
|---|---|---|---|---|
| 2026-10-19 | `D:\mv15-78a4\b` | `-1073741510` (`0xC000013A`) | 1.90 s | VS banner only, stderr empty |
| 2026-10-14 | `D:\mv8e8\b` | `0xC000013A` | 72.3 s | ninja step 128/429, stderr `User interrupt` |

`0xC000013A` is `STATUS_CONTROL_C_EXIT`: the default console control handler
ends a process that way on `CTRL_C`, `CTRL_BREAK` or `CTRL_CLOSE`. In both
runs the parent driver survived and wrote its receipt. Only the child build
tree died, at unrelated points, one inside `VsDevCmd.bat` and one mid-ninja.
That rules out a compiler, CMake or source failure. The common factor is
a console control event delivered to the child's console and to nothing else.

## Boundary and cause

The interruption boundary is the console the launcher created for the build,
not `tools/build-windows.ps1`. The nimble drivers start the build with:

```powershell
Start-Process -FilePath powershell.exe ... -PassThru `
    -RedirectStandardOutput ... -RedirectStandardError ...
```

Without `-NoNewWindow`, Windows PowerShell 5.1 starts the child with
`CREATE_NEW_CONSOLE`. This host is Windows 11 10.0.26200 and has no
`HKCU\Console\%%Startup` delegation override, so the default terminal is
Windows Terminal 1.24.12741.0. A new console therefore becomes a **visible,
focusable Windows Terminal window**. The WSL caller's own console has no
window at all. Closing that window or tab, or pressing Ctrl+C in it, sends
`CTRL_CLOSE`/`CTRL_C` to every process attached to it: the whole build tree,
but not the driver.

The receipts cannot show which of these events ended each run, or who sent
it. This finding does not claim to know.

## Startup reproducer (one bounded probe)

[`probe_parent.ps1`](../agent/evidence/2026-10-19-native-wrapper-interruption/probe_parent.ps1)
launched [`probe_child.ps1`](../agent/evidence/2026-10-19-native-wrapper-interruption/probe_child.ps1)
from WSL three ways under one driver (pid 3512). It used a scratch directory,
`D:\azl65-probe\run1`, outside source checkouts and AQ slots. Results are in
[`console-probe/`](../agent/evidence/2026-10-19-native-wrapper-interruption/console-probe/):

| launch | console window | visible | class | console members |
|---|---|---|---|---|
| direct from WSL (as `build-windows-from-wsl.sh` does) | 0 | — | — | child + driver 3512 |
| historical `Start-Process -Redirect*` | 1968112 | **true** | `PseudoConsoleWindow` | child only |
| corrected `ProcessStartInfo.CreateNoWindow` | 0 | false | — | child only |

All three children exited 0. The historical form is the only one that gives
the build a window someone or something on the desktop can close.

## Fix

[`tools/run-windows-build-bounded.ps1`](../../tools/run-windows-build-bounded.ps1)
runs a source root's `tools/build-windows.ps1` with these properties:

- **Console.** It uses `UseShellExecute=false` and `CreateNoWindow=true`. The
  build gets its own console with no window, which also keeps it out of
  Ctrl+C broadcasts on the caller's console. Stdin is redirected and closed,
  so nothing can block on "Terminate batch job?".
- **Bounds.** The timeout defaults to 1800 s. On timeout it kills the tree
  with `taskkill /T /F`, records the result in `timeout-kill.txt`, and exits
  124. The worker limit is `CMAKE_BUILD_PARALLEL_LEVEL`, default 6.
- **Inputs.** It rejects UNC source roots and requires a fresh evidence
  directory, so earlier receipts are never overwritten.
- **Output.** It writes `build.stdout.txt`, `build.stderr.txt` and
  `build-receipt.json`. The receipt records the command, pid, status, exit
  code and timing. The runner exits with the build's own exit code.

The same `CreateNoWindow` idiom already exists in
`MatterEditor/tools/smoke_vulkan_faults.ps1` and
`tools/check-windows-msvc-package.ps1`. The build wrapper,
assertions, tolerances, timeouts and package/runtime coverage are unchanged.

[`tools/tests/test_windows_bounded_runner.py`](../../tools/tests/test_windows_bounded_runner.py)
is the regression test. It is not registered with ctest, so it adds no CI
gate.

- **Static contract (runs anywhere).** The runner keeps
  `CreateNoWindow`/redirected stdin and never uses `Start-Process`.
- **Native checks.** These run only when `MATTER_WINDOWS_SCRATCH` points at a
  mounted Windows drive. They use a stub wrapper and check that:
  - the child sees `console_window=0` and `CMAKE_BUILD_PARALLEL_LEVEL=6`;
  - quoted arguments survive and both streams are captured;
  - a non-zero exit propagates;
  - a rerun cannot overwrite a receipt;
  - a timeout kills the tree and exits 124.

```
MATTER_WINDOWS_SCRATCH=/mnt/d/azl65/tests python3 tools/tests/test_windows_bounded_runner.py -v
Ran 3 tests in 10.4s — OK
```

## Corrected native probe (one targeted run)

Source: a fresh `git archive` of `58289c187ef2565c7d0532fe28a8cfb903023e44`
extracted to `D:\azl65\b`. Its archive SHA256,
`e5245b259a6840223a0f1c107fc54cc8b24db94968c5a018ed49637ec965f7d7`, is
identical to nimble's baseline export receipt. nimble's exports under
`D:\mv15-78a4` and `D:\mv8e8` were not touched.

Toolchain, from `-PreflightOnly` through the runner
([preflight](../agent/evidence/2026-10-19-native-wrapper-interruption/preflight/)):

- VS 2022 Community 17.14.7, MSVC 14.44.35207 (cl 19.44.35211), SDK
  10.0.26100.0;
- VS-bundled CMake and Ninja;
- Vulkan SDK 1.4.357.0;
- Python 3.13.14.

The exact previously failing command ran through the runner:

```
powershell.exe -File D:\azl65\run-windows-build-bounded.ps1 -SourceRoot D:\azl65\b
  -EvidenceDirectory D:\azl65\probe-build -TimeoutSeconds 1800
  -Config RelWithDebInfo -Target vulkan_smoke_tests
```

[Receipt](../agent/evidence/2026-10-19-native-wrapper-interruption/corrected-build/build-receipt.json):

- `console: CreateNoWindow`, six workers.
- **Exit 0 after 413.8 s.** Stderr is empty.
- **No `0xC000013A`.** The build went past the 1.9 s VsDevCmd point,
  configured with MSVC 19.44.35211 (`Configuring done (4.3s)`), passed the
  step-128 death point of 2026-10-14, and linked `vulkan_smoke_tests.exe`
  (`[428/429]`).
- The [stdout head, step lines and hashes](../agent/evidence/2026-10-19-native-wrapper-interruption/corrected-build/)
  are retained. The full 241,817-byte stdout stays at `D:\azl65\probe-build`.

This shows that the build gets through configure and compile and finishes
under the corrected launcher. It is one run. It does not prove that the
historical interruptions can no longer happen under the old launcher. It
also does not show that the VT smoke passes.

## Adoption by nimble-rapids-30

nimble's `run_native_build.ps1` and `run_native_smoke.ps1` should stop using
`Start-Process -Redirect*`. Either:

- build through `tools/run-windows-build-bounded.ps1` (copy it beside the
  export, as this probe did, when the export predates it), or
- start processes with a `ProcessStartInfo` that sets
  `UseShellExecute=$false` and `CreateNoWindow=$true`.

Keep the 1800 s bound and the six-worker limit. VT and VT-RT remain
unexecuted until that task runs them.
