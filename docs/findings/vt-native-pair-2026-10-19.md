# Historical VT native comparison: repeated build interruption

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
