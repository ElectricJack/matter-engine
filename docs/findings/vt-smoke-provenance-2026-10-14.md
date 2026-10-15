# VT smoke provenance investigation, 2026-10-14

Task: `nimble-rapids-30`, claim epoch 8.

The pre-move source already contains a scheduling/fixture mismatch sufficient
to prevent the pinned tail from completing within the plain VT smoke's seven
frames. The graphics move preserves that code. This is evidence of a
pre-existing timing mismatch, **not a native runtime baseline**: the exact
seven/twelve failure counts and zero validation errors remain the original
post-move results. Neither smoke mode is claimed restored or passing.

## Revisions and dependency source

- Historical baseline: `58289c187ef2565c7d0532fe28a8cfb903023e44`.
- Original graphics move: `90a19c6698a03623da9a3e3c5235ad272531d462`.
- Current verification checkout: `c57ba76866d811e285457439380b57a01eebe717`.

The supervisor authorized `aq git pull --branch dev --json` from the held task
branch after checking the claim and clean tracked state. It succeeded, with
both resulting HEAD and `origin/dev` at `c57ba768`. Accepted graphics delivery
`8c443542cbf836d0dac4eabaa7df17dbe0db2770` and tested source
`e2bf6305115ff05101b1d8b4e193a25cc881a18e` are ancestors, and the standalone
graphics sample paths exist. This resolves the initial stale-checkout blocker;
it does not establish a passing VT result. No historical salvage was adopted.

## Historical comparison

The [reproducible diagnostic](../agent/evidence/2026-10-14-vt-provenance/test_provenance.py)
reads historical objects with `git show`; it never checks out or resets a branch.

1. Expanding the four moved renderer fragments reproduces the entire original
   `vk_scene_renderer.cpp` byte for byte. Moved raster mesh and impostor mip
   files match after normalizing the geometry header's relative include path.
2. The VT residency/compositor implementation, budget headers, chart constants,
   renderer public header and smoke test are byte identical between revisions.
   [Source report](../agent/evidence/2026-10-14-vt-provenance/source-comparison.json)
   records the compared paths and hashes.
3. Raster, G-buffer, skin and water shader variants compile to identical bytes
   between revisions using one Linux `glslc`, `--target-env=vulkan1.3 -O` and
   the historical engine include directory. The
   [SPIR-V report](../agent/evidence/2026-10-14-vt-provenance/spirv-comparison.json)
   records compiler version, flags, sizes and hashes. This is Linux historical
   equality, not parity with the original Windows compiler or GPU execution.

## Why the short fixtures cannot settle under the default budget

These references describe **both historical revisions**, before the later
graphics extraction replaced engine files with forwarding headers:

- `MatterEngine3/include/matter/vt_budgets.h` defaults `fill_budget_ms` to 4 ms.
- `VtCompositor::supports_incremental_fill()` returns true.
- `VtGpuWorkBudget::tiles()` clamps an enabled budget to at most two tiles.
- `VtResidency::record_frame()` limits an incremental batch to one request
  (`time_quota = 1`) and sets `request.work_tiles` from that budget.
- A page has 680 tiles (`kVtPageStride = 136`, five tiles per row). The compositor
  accumulates slices with `mark_pending(tile_count)` and returns. Normal resolve
  and encoding run in a separate frame after the last slice.

Compiling the actual historical budget/constants headers at both revisions gives
the same [CPU result](../agent/evidence/2026-10-14-vt-provenance/cpu-bound.json):

```text
page_tiles=680 max_tiles_in_7_frames=14 max_tiles_in_6_frames=12 min_slice_frames=340
```

That is an optimistic lower bound of 340 slice frames plus completion work for
one page; preparation, feedback and paired enrichment can add delay. The plain
`run_vt_path` renders seven frames before asserting `fills_total >= 1` and that
feedback has drained a page beyond the tail. The plain `run_vt_rt_path` settles
for six frames before testing page-backed raster/RT results. Their comments
still assume a complete tail fill in frame one. Specialized modes have separate
settling paths and are not covered by this conclusion.

The original [VT log](../agent/evidence/2026-10-11-graphics-producer-move/vt.txt)
reports `fills_total=0`, one pinned slot and a nonempty queue, consistent with
this mismatch. The [VT-RT log](../agent/evidence/2026-10-11-graphics-producer-move/vt-rt.txt)
reports unresolved VT slots/pages. This explains a shared prerequisite failure;
it does not prove the complete failure set or exclude additional runtime issues.
The tile pacing code predates the move (including
`f10ae70193842b81a57d00cae6c8a6eb40d6b7a2`); this investigation is not a runtime bisect.

## Verification and remaining runtime gap

Linux checks passed:

- Historical source equality and compiled budget probes: two checks passed.
- Existing `make -C MatterEngine3/tests -j2 run-vt-residency`: `ALL PASS`;
  [full output](../agent/evidence/2026-10-14-vt-provenance/vt-residency.log).
- Historical shader compilation/equality: one check passed, four variants.

The final diagnostic contains all four checks in one driver. Run it
through `aq test -n 0 docs/agent/evidence/2026-10-14-vt-provenance/test_provenance.py -q`;
set `MATTER_PROVENANCE_GLSLC` if the recorded staged compiler is absent.

Native Windows preflight found the supported MSVC/SDK toolchain. The actual
`tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests` command exited
1 before configuration: CMD rejected this checkout's UNC working directory,
defaulted to `C:/Windows`, and CMake could not find `C:/Windows/CMakePresets.json`.
The [build log](../agent/evidence/2026-10-14-vt-provenance/windows-build.log)
preserves that failure. No path guard was bypassed or source tree adopted into
the held branch. Windows acceptance is waived by the supervisor, but no historical
native VT/VT-RT runtime receipt has thereby been created.

The Linux host exposes llvmpipe; the initially unset display variables and
[Vulkan inventory](../agent/evidence/2026-10-14-vt-provenance/environment.txt)
are environment observations, not proof the smoke modes cannot run. The supported
root CMake graph requires Windows/MSVC; the Linux smoke Make target delegates to
the Windows binary. No new Linux renderer build port was introduced. The separate
historical renderer-unit-test assertion expects `LocalLightGpuMeta` to be 32
bytes while the unchanged public header asserts 48. It remains unfixed and is
not used as a claim that the smoke binary itself cannot compile.

## Authorized native historical attempt

Supervisor message `msg-55fa5898a0d54a0e86e8e3be10694467` authorized fresh,
task-owned historical source artifacts on the mounted Windows filesystem at
`D:\matter-agent-artifacts\matter-engine-cpp\nimble-rapids-30\epoch8-230ea608\`,
with each source directory named by its full commit OID. This permits a bounded
test-only comparison and supersedes the earlier UNC execution gap. No mapping,
`subst`, source adoption into the branch or canonical wrapper change was used.

The [export receipt](../agent/evidence/2026-10-14-vt-provenance/native-attempt/export-receipt.json)
records the source/tree OIDs, archive SHA256 and compressed extracted-file
inventory. The baseline's 10,419 tracked files, including one symlink, passed
blob-content and archive Git-mode comparison against `git ls-tree` before its
build started. The inventory records extracted modes and symlink targets as well.
The [export driver](../agent/evidence/2026-10-14-vt-provenance/export_native_pair.py)
uses only immutable objects from the held repository and refuses an existing
artifact root.

From the baseline's actual native `D:` source-root working directory, the
unchanged `tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests`
ran with its resolved MSVC/SDK/Python/glslc toolchain, six build workers and a
30-minute process-tree timeout. It **failed with exit 1 after 6.22 seconds** in
CMake's basic C compiler check, before engine compilation:

```text
The object file directory ... has 241 characters.
The maximum full path to an object file is 250 characters ...
LINK : fatal error LNK1104: cannot open file
'CMakeFiles\cmTC_fbc28.dir/intermediate.manifest'
```

The path-length warning is consistent with the manifest failure, but the exact
environmental cause has not been independently isolated. Full
[stdout](../agent/evidence/2026-10-14-vt-provenance/native-attempt/baseline-build/build.stdout.txt),
[stderr](../agent/evidence/2026-10-14-vt-provenance/native-attempt/baseline-build/build.stderr.txt),
[receipt](../agent/evidence/2026-10-14-vt-provenance/native-attempt/baseline-build/build-receipt.json)
and CMake configure diagnostics are preserved beside the export receipt.

The comparison stopped at that failure, as instructed. The move snapshot's
concurrent export was terminated and preserved as an incomplete artifact; its
partial inventory and [stopped-export receipt](../agent/evidence/2026-10-14-vt-provenance/native-attempt/stopped-export-receipt.json)
record 4,824 complete files, 5,619 absent files and no mismatched present files.
No second
native build or smoke run was attempted, and the historical source/graph was
not repaired or retried. No smoke executable or generated engine shaders exist
to hash. The
[paired result](../agent/evidence/2026-10-14-vt-provenance/native-attempt/native-pair-result.json)
explicitly records all four planned runtime runs as unexecuted.

The [current native host capture](../agent/evidence/2026-10-14-vt-provenance/native-attempt/native-host-before.txt)
reports an RTX 4090 with NVIDIA 610.88 and no editor/CTest/Vulkan smoke process
at capture time. The resolved toolchain receipt is retained. The optional
SDK `vulkaninfo.exe` inventory command was unavailable; this is separately
recorded, not replaced with the earlier RTX/Vulkan observations.

No renderer forwarding repair is supported by the comparison so far. No production
algorithm, smoke assertion, tolerance or skip changed. The native baseline
experiment is blocked before engine compilation, so native failure provenance
remains unverified. The concrete failure is reported once to the supervisor
with a request for supported pause/release. This is not a static-only pass;
the original native failures remain failures.

## One authorized short-path attempt

Supervisor message `msg-f8ee60b5f905490c860e418aae1581fe` authorized one
placement-only diagnostic using fresh `D:\mv8e8\b` and `D:\mv8e8\m` directories.
The same immutable tar archives were reused, with their SHA-256 hashes checked
before extraction. The baseline's 10,419 files and one symlink verified against
its Git tree before building; the exporter also verifies the archive's Git
executable modes and records the physical extracted modes. The native baseline
source-root length fell from 117 to 10 characters. Both previous artifact trees,
archives and inventories remain preserved. See the
[short export receipt](../agent/evidence/2026-10-14-vt-provenance/short-native-attempt/export-receipt.json)
and [full baseline inventory](../agent/evidence/2026-10-14-vt-provenance/short-native-attempt/58289c187ef2565c7d0532fe28a8cfb903023e44-inventory.json.gz).

The unchanged canonical wrapper used the same native toolchain, target,
RelWithDebInfo configuration, six workers and 30-minute bound. Native compiler
and linker probes **passed**, and CMake generated the engine graph. The first
attempt's `LNK1104` did not recur in this layout, which supports the path-length
hypothesis without establishing the original error's cause. After the last
logged build step **128/429**, the native wrapper instead exited
**-1073741510 (`0xC000013A`)**, with stderr **`User interrupt`**, after 72.349 seconds.
The WSL-facing driver returned 58 and AQ pytest recorded **one failed test**.
The timeout did not fire. The logs identify no compiler diagnostic causing this
stop, and the source of the interruption is unknown. This is an interrupted
build, not a successful build or an established historical source defect.
While the build was active, a separate native PowerShell syntax-only parser
check of a proposed diagnostic runner completed with exit zero; that runner
was never executed and was discarded after this stop. The build receipts do
not identify the interrupt sender, so interaction between overlapping WSL/native
PowerShell invocations cannot be excluded. No retry was used to test that
hypothesis.

The [build receipt](../agent/evidence/2026-10-14-vt-provenance/short-native-attempt/baseline-build/build-receipt.json),
[stdout](../agent/evidence/2026-10-14-vt-provenance/short-native-attempt/baseline-build/build.stdout.txt),
[stderr](../agent/evidence/2026-10-14-vt-provenance/short-native-attempt/baseline-build/build.stderr.txt)
and full CMake configure log are retained. The subsequent
[native process capture](../agent/evidence/2026-10-14-vt-provenance/short-native-attempt/native-processes-after.json)
showed no remaining native build or renderer process. The owned move export was
stopped and its partial inventory preserved; no move build or smoke mode was
run. That incomplete tree contains 5,642 matching files, 4,800 missing files,
and one empty, mismatching file left by the interrupted extraction; it is
preserved without repair and was never build input. A subsequent check of
all 10,419 recorded baseline prebuild physical modes confirms their normalized
Git modes also match. The [stopped-attempt receipt](../agent/evidence/2026-10-14-vt-provenance/short-native-attempt/stopped-attempt-receipt.json)
records those gaps. No further build/layout retry or historical graph change
was made. The paired native runtime comparison remains blocked, and the
original seven VT and twelve VT-RT failures remain open.
