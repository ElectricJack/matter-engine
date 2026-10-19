# Historical VT native comparison: failures predate the graphics move

Task `nimble-rapids-30`, claim epoch 16. **The VT and VT-RT smoke failures are
pre-existing and not caused by the graphics producer move.** Pre-move dev
`58289c187` and move commit `90a19c669` fail the same assertions, line for
line, on the same host, toolchain and GPU, with zero validation errors on both:

| Commit | `vt` | `vt-rt` |
|---|---|---|
| `58289c187` (pre-move) | exit 1, **7 failures**, 0 validation errors | exit 1, **12 failures**, 0 validation errors |
| `90a19c669` (move) | exit 1, **7 failures**, 0 validation errors | exit 1, **12 failures**, 0 validation errors |

The `FAIL:` / `validation errors` / `FAILURE(S)` lines are byte-identical
between the commits for both modes. The only stdout differences are one
frame-cost line (`vt`) and one GPU-zone timing line (`vt-rt`). Because the move
did not introduce them, there is no forwarding/adapter regression to fix. The
failures belong to the deferred VT work, not to this task. The baseline `vt`
stats line shows the symptom: `pool=1/256 pinned=1 fills_total=0 queue=1
requests_last=0`, meaning the pinned tail request is queued but no fill
executes and the feedback pass emits no requests.

## How the build interruption was avoided

Epochs 14 and 15 lost the native build to `0xC000013A` (`STATUS_CONTROL_C_EXIT`;
stderr `User interrupt` in 4abb). Epoch 16 used the **same unchanged**
[`run_native_build.ps1`](../agent/evidence/2026-10-19-vt-native-pair/run_native_build.ps1)
and [`run_native_smoke.ps1`](../agent/evidence/2026-10-19-vt-native-pair/run_native_smoke.ps1).
The only difference was launching PowerShell from WSL detached from the
caller's session and terminal: `setsid nohup powershell.exe ... < /dev/null`
([driver](../agent/evidence/2026-10-19-vt-native-pair/epoch16/smoke.sh)). Both
builds and all four smokes then ran to completion. This fits the earlier
interrupts being a console Ctrl+C forwarded from the invoking WSL process group
rather than anything in the build. It is a workaround, not a proven root cause.

## Epoch-16 evidence

All under [`epoch16/`](../agent/evidence/2026-10-19-vt-native-pair/epoch16/):

- **Sources.** Baseline `D:\mv15-78a4\b` (the epoch-15 export) and a fresh
  move tree `D:\mv15-78a4\m16`, extracted from the epoch-15 move archive. Its
  SHA256 `8533667c...` equals a fresh `git archive 90a19c669`. After the builds,
  `verify_tree.py` re-hashed every tracked path against `git ls-tree`:
  10,419/10,419 (baseline) and 10,443/10,443 (move) match, including the one
  symlink ([receipt](../agent/evidence/2026-10-19-vt-native-pair/epoch16/tree-verification.txt)).
- **Builds.** Both run `tools/build-windows.ps1 -Config RelWithDebInfo -Target
  vulkan_smoke_tests` with an identical toolchain (MSVC 14.44.35207, SDK
  10.0.26100.0, Vulkan SDK 1.4.357.0). Baseline exit 0 in 356 s, move exit 0 in
  423 s. Receipts, toolchain and gzipped stdout are preserved.
- **Smokes.** RTX 4090, driver 610.88, Vulkan API 1.4.341. Executable SHA256:
  baseline `3759695f...`, move `e1d9bf8b...`. Receipts, stdout and stderr are
  preserved per run. stderr contains only the host's broken Epic overlay layer
  manifests.

## Unix `LocalLightGpuMeta` 32-vs-48 assertion

This is also pre-existing and unrelated to the move. `vk_scene_renderer.h`
asserts `sizeof(LocalLightGpuMeta) == 48` (three 16-byte lanes since
`e9da6034` added the primary-tile fields). The Make-only
`MatterEngine3/tests/vk_scene_renderer_tests.cpp:78` still asserted `== 32`, so
that suite could not compile on any of the three commits checked. The test now
asserts 48, matching the header. A GCC `-fsyntax-only` check against the real
header accepts 48 and rejects 32. The full suite was not compiled, because
this Linux host has no `glslc` for `shaders_gen/embedded_spirv.h`.

## Epoch-15 record (superseded)

Task `nimble-rapids-30`, claim epoch 15. The authorized native comparison is
**blocked before compilation**. No new VT or VT-RT runtime result exists, and
no forwarding regression or smoke restoration has been demonstrated.

Supervisor message `msg-d487a5e576894c3ab90b69ce0d8f9376` confirmed graphics
prerequisite delivery and authorized focused context reads after `aq prime`
failed with `context.required_over_budget`. The held branch was clean at
`4abb24a8a2b037b9b390bf7453b62c7b6649ebf7`; delivered graphics commit
`8c443542cbf836d0dac4eabaa7df17dbe0db2770` was already an ancestor, so no source
update was necessary. The [prior evidence](vt-smoke-provenance-2026-10-14.md)
remains preserved.

## Exact source and native placement

The [export driver](../agent/evidence/2026-10-19-vt-native-pair/export_native_pair.py)
created fresh Git archives for pre-move
`58289c187ef2565c7d0532fe28a8cfb903023e44` and move
`90a19c6698a03623da9a3e3c5235ad272531d462` from immutable local objects.
Their task-owned test placement is `D:\mv15-78a4\b` and `D:\mv15-78a4\m`,
outside live source checkouts and AQ slots. No UNC mapping, source adoption,
borrowed binary, graph edit, reset or rebase was used.

The baseline's 10,419 tracked files, including one symlink, passed archive-mode
and extracted-blob comparison against its Git tree before the build. Its
archive SHA256 is
`e5245b259a6840223a0f1c107fc54cc8b24db94968c5a018ed49637ec965f7d7`.
The [export receipt](../agent/evidence/2026-10-19-vt-native-pair/export-receipt.json)
and compressed full inventory preserve the tree OID, individual content hashes,
Git/archive modes, physical modes and symlink target.

## Actual native outcome

The [paired driver](../agent/evidence/2026-10-19-vt-native-pair/test_native_pair.py)
ran through AQ's test semaphore with pytest workers disabled. From the genuine
baseline source root, the unchanged supported wrapper ran:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
```

The unchanged [bounded native runner](../agent/evidence/2026-10-19-vt-native-pair/run_native_build.ps1)
used six build workers and a 1,800-second process-tree timeout. Preflight
resolved MSVC 14.44.35207, Windows SDK 10.0.26100.0, Vulkan SDK 1.4.357.0 and
native Python 3.13.14. The native host capture reports an RTX 4090 with driver
32.0.16.1088. No editor, smoke, compiler, Ninja or CTest process was running
at the before/after captures.

The wrapper exited **-1073741510 (`0xC000013A`) after 1.9015011 seconds** at
2026-10-19 21:34:27 UTC. Its stdout contains only the Visual Studio developer
environment banner; stderr is empty. There is no CMake or compiler diagnostic,
and the timeout did not fire. The WSL-facing driver returned 58; AQ pytest
recorded **one failed test in 3.57 seconds**. The
[native receipt](../agent/evidence/2026-10-19-vt-native-pair/baseline-build/build-receipt.json),
stdout, stderr, toolchain receipt and complete pytest failure are preserved.

This repeats the interruption status from the October 14 attempt, but at an
earlier stage. No separate native PowerShell invocation from this task
overlapped the build; the move export was a Linux Python process. The sender
or cause of the interruption remains unknown. It is neither a successful
build nor evidence of a historical source defect.

The experiment stopped at that failure. The owned move exporter was terminated
with SIGTERM and exit 143, and its incomplete tree was inventoried and preserved.
The [stopped receipt](../agent/evidence/2026-10-19-vt-native-pair/stopped-attempt-receipt.json)
records matching, absent and mismatching tracked files. That tree was never
build input: 4,740 tracked files match, 5,703 are absent and none mismatch.
There was no native retry, move build or smoke run. The
[paired result](../agent/evidence/2026-10-19-vt-native-pair/native-pair-result.json)
explicitly lists all four planned smokes as **unexecuted**. No fresh smoke
executable or engine shader product was produced to hash.

## Linux evidence and remaining scope

The [focused Linux checks](../agent/evidence/2026-10-19-vt-native-pair/linux-checks.json)
passed **3/3 in 1.80 seconds**: historical expanded renderer/VT source equality,
compiled historical VT budget bounds, and the existing VT residency suite.
Both historical budget probes again report 680 tiles per page, at most 14
tiles in seven frames and a minimum 340 slice frames. These are CPU/source
results; they do not supply the missing native comparison.

No production algorithm, forwarding adapter, test assertion, tolerance or skip
changed. The original seven VT and twelve VT-RT failures remain unresolved;
their full native failure provenance is still unverified. Windows CI waiver
does not waive this behavioral investigation. The repeated interruption needs
an execution-environment resolution before another native attempt.
