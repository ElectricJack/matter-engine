# CI selection, reuse and the one-minute target

Task `keen-current-19` requires the complete PR build/test path to finish in less
than 60 seconds on warm Linux and native Windows runners. The implementation and
local measurements below do **not** yet establish full Windows acceptance.
The prior implementation/evidence is preserved in
[windows-ci-history.md](windows-ci-history.md).

## Active workflow

`.github/workflows/native-windows.yml` is the sole active CI workflow in this
checkout. PRs target `dev`, `staging` or `main`; pushes trigger only on those three
branches. AQ branch pushes no longer queue a second native run before their PR.
Concurrency uses the PR head SHA (otherwise push SHA). Checkout uses that exact
SHA. Existing runs are allowed to finish; canceling a same-SHA run must not abort
an unrelated source measurement or leave a partial build as a reusable baseline.
Required names remain `Linux foundation build and tests` and
`Native Windows build and tests`. There is no active advisory fast check.

| Event | Behavioral selection | Source/build lint | Packaging |
| --- | --- | --- | --- |
| PR, dev push, ordinary dispatch | `ci-pr` | separate phase, required | no |
| staging/main push, nightly | `ci-full` | separate phase, required | yes |
| local explicit `-Tier fast` | historical `ci-fast` subset | separate phase | only with full tier |
| local full with `-Package` | `ci-full` | separate phase | yes |

Both full and automatic tier choices retain the full required-check name. The
five historically slow CPU fixtures and all GPU tests still participate in
`ci-pr`; they have **not** been silently discarded to claim a fast suite.
`ci-full` preserves the behavioral tests plus promotion package tests. Source
contracts run in lint, and the two contracts below move to custom lint targets. Existing composition proposals
are historical, inactive files, and must not be installed as duplicate producers.
The global supervisor's manual branch integration replaces prior train paperwork;
this change does not edit daemon configuration or any human gate.

## Coverage shifts and data inputs

| Check or behavior | Current disposition | Per-PR coverage given up |
| --- | --- | --- |
| `windows_ci_contract_tests` | renamed `tools/lint/windows_ci_contract_lint.ps1`, `matter_ci_lint` build target; required PR lint phase | no behavior removed; excluded from CTest timing |
| `shader_external_build_tests` | `matter_ci_promotion_lint` build target, staging/main/nightly packaging phase | per-PR cross-drive rebuild fixture; complete fixture remains on promotion |
| exact 11 qualifiers in [ci-checks.json](../../../cmake/ci-checks.json) | reviewed inventory commands run by `ci-source-lint.py` in a required separate lint phase | no assertions removed; source/build and mixed disk qualifiers execute outside behavioral CTest |
| tests labeled `package`, `matter_dist` and package validation | staging/main/nightly or explicit `-Package` | per-PR packaging execution, retained on promotion |
| `ci_test_report_tests` | XML literals supplied with `StringIO`, rather than temporary XML files | none |
| `matter_engine_headless_consumer_tests`, `matter_mesh_allocator_policy_tests` | explicit behavioral policy exemptions, retained in `ci-pr`/`ci-full` | none; the allocator dispatch and public provider-interface probes execute as runtime tests |

The conversion manifest records each exact name, command suffix, original labels
and preserved semantics. CMake rejects a missing listed qualifier or a newly
unclassified shader/compiler-policy test. Post-build lint rejects duplicate,
missing, unreviewed, relabeled or substituted commands and records the manifest
SHA-256; an empty or incomplete qualifier set cannot pass. The prior broad-label
implementation is superseded: two compiler-policy executables have runtime
assertions and remain behavioral CTest.

`editor_registration_census` retains a real editor census, its reviewed disk
manifest and the unregistered PE-string mutation probe. It is a **mixed** runtime
and disk qualifier, not a pure source check. `compiler_return_address_gnu_tests`
likewise still compiles and executes its caller-identity probe. These mandatory
qualifiers remain intact while `steady-harbor-62` owns pure-runtime conversions;
no disk-free claim is made for them here. `vt_link_inventory_tests` inspects only
Ninja link commands; its former CTest dependency on `vt_compositor_tests` was
ordering, not a GPU input. The compositor runtime remains in behavioral CTest.

Lint executes the configured commands, working directories, environment,
timeouts and pass/fail regexes from the post-build inventory. A missing command,
failed child, absent success marker or forbidden failure marker fails lint. Lint
failure does not suppress the behavioral run or become a passing test result.
Source/build checks retain command registrations as the inventory authority.
They are excluded from both behavioral selections and execute as separate lint
commands, so promotion does not run the same source contract twice. Complete removal/conversion of shared core source and
disk consumers is coordinated with `steady-harbor-62`; do not describe this branch
alone as a wholly memory-fixture test suite. No runtime fixture/iteration/assertion
reduction has been made by this orchestration change.

## Incremental and compiler cache contract

Native objects live outside the checkout, under
`C:\matter-ci\matter-engine\build-relwithdebinfo`. Toolchain/CMake fingerprints,
checkout identity, ownership markers and an incomplete clean baseline still force
a clean rebuild. Date rollover alone no longer deletes a valid build: the nightly
workflow explicitly requests a clean build and disables the compiler cache.
Unowned nonempty trees and reparse points remain rejected. Failed clean builds do
not establish a successful reusable baseline; failed tests always run again.

The existing SHA-256-verified sccache 0.18.0 launcher and toolchain lock are retained.
Both C and C++ use the launcher with embedded MSVC debug information (`/Z7`).
Cache requests/hits are retained in run artifacts. Zero requests during a no-op
build are build reuse, not a 100% cache hit rate. Use a uniquely owned root for
local measurements; do not alter another worker's native source/build tree.
Canonical `tools/build-windows.ps1` resolves and enters the pinned MSVC toolchain.

CTest parallelism defaults to the processor count, unless the caller explicitly
limits it. The existing Vulkan resource lock still serializes shared device use;
changing job count does not remove that safety or accelerate its serialized sum.

## Evidence and acceptance

The native summary includes `qualification_under_60_seconds`, requiring both a
passing outcome and the entire script wall time (including lint/setup) below 60;
build and test subset durations remain separate diagnostic fields. Missing lint
or JUnit evidence fails the outcome.

Artifacts retain source identity, hosted/local status, toolchain/fingerprint,
complete inventory, JUnit, all timing rows, slowest 20, missing/extra/duplicate
selected tests, serialized resource sums, cache statistics and all phase durations.
The measured scheduling lower bound is the largest individual duration, summed
work divided by job count, or each serialized resource sum, whichever is larger.
This is a bound for those measured fixtures on that platform, not a forecast for
smaller inputs or a different runner. Skips and incomplete selection fail timing
validation. Test success and the 60-second target are separate evidence fields;
a slow passing test run does not prove task acceptance.

The Linux job requires the existing GNU invalid-format compile probe to fail for
the expected `-Werror=format` diagnostic, compiles and executes the existing GNU
caller-identity probe, and runs report/lint/depfile/embedding qualifiers. Their
exit codes, diagnostics and timings are retained outside behavioral CTest. The
Windows-specific MSVC/editor/package contracts remain native qualification; this
is not Linux parity with the engine/editor suite. A lint failure fails the whole
Linux job while all foundation suites still execute.

The Linux job runs the existing portable foundations (MemoryLib with ASan/UBSan
and allocation interposition, MeshChartingLib, AssetStoreLib) in independent,
bounded jobs. Each suite retains all existing assertions. MemoryLib's phony
`test` target still compiles and executes together; its time is reported as
`build_and_test`, never as an incremental no-op. This is the complete current Linux
foundation job, not Linux parity with every Windows engine/Vulkan test.

| Source-bound local measurement | Build plus tests | Result |
| --- | ---: | --- |
| epoch59 working diff, Linux cold portable foundations | 25.611 s | all three suites passed |
| same Linux inputs, warm portable foundations | 4.728 s | all three suites passed; MemoryLib recompilation included |
| clean commit `92936ed0`, warm portable foundations, two jobs | 4.468 s | all three suites passed; build/test commands and script hash retained |
| clean commit `764ad8e9`, warm portable foundations plus mandatory Linux lint, two jobs | 7.976 s | all three suites and seven qualification phases passed; lint 2.045 s |
| preserved checkpoint2e67 forced-clean/cached Windows fast run | 480.605 s total | historical failed run: 158/159, hydrology failure; 892/892 cache hits |
| epoch59 native current PR inventory | not measured | canonical build interrupted for supervisor resource order after 1298/1305 actions; CTest never started |

Linux measurements are local WSL, `hosted: false`. The first cold/warm pair used
three independent jobs and a recorded working diff; the final warm run used two
jobs on clean commit `92936ed0c6fbe22f25a0d5d6edb079972d8e7223`, with the CI
script hash and each build/test duration recorded. The final scoped run at clean
commit `764ad8e9ea9558a0a8321a1975da2c4ddfa0b08c` includes the new mandatory
portable compiler/source qualification and retains all diagnostics, child exit
codes and per-phase timings. Neither local measurement proves hosted or full
engine/native acceptance. The historical Windows fast result is not a controlled before/after
comparison. Hosted source qualification and native warm build/test timings remain
required. The original full-suite target is less than 60 seconds, not the earlier
10/20-minute advisory target. If the measured full test floor exceeds 60 seconds,
retain the failures and slowest 20, then shorten or relocate the identified heavy
fixtures with explicit coverage accounting and cheap regression equivalents.

Reproduce with `python3 tools/ci-linux.py` and
`tools/ci-windows.ps1 -Tier full -CiRoot <unique-local-root>`. Preserve artifacts
before a warm rerun. Use `-Package` only for explicit promotion measurement.

The supplied graphics-head CI run [38514005777](https://github.com/ElectricJack/matter-engine/actions/runs/38514005777)
failed at source `2fb388d7739fc38437cfdbc1e14230409c8914ad`: 188 of 189 tests
passed, with `shader_source_tests` aborting on
`water_forward_uses_world_normal(water_forward, water_raster)` (native exit
`0xc0000409`, CTest exit 8, CI exit 1). It recorded CTest wall time 1901.03 s.
The [verified raw JUnit and diagnostic inventory](evidence/ci-performance-20261014/graphics-38514005777/diagnostic-summary.json)
retain every executed name and the slowest 20. The largest diagnostics are
`vulkan_smoke_tests` 949.457 s, `windows_package_tests` 418.478 s,
`world_definition_tests` 310.125 s and `viewer_logic_tests` 243.245 s.
These timings overlapped local native builds, so they establish neither a clean
floor nor a controlled before/after result. No independent build-phase timing
was supplied. Source SHA and artifact hashes are preserved; the 189 executed
names belong to that graphics source, not the current branch's registrations.

Migrating `shader_source_tests` to mandatory lint does not waive this failure:
its exact executable and working directory remain selected, its abort fails lint
and the native outcome, and the existing positive oracle and negative mutations
are unchanged by this branch. `steady-harbor-62` owns its source/fixture fix. After
that qualification and a free host, measure the complete exact-source PR path,
then target the largest individual and serialized GPU durations with smaller
synthetic fixtures and retained regression assertions. Package duration is
already removed from the PR path; the four heavy CPU fixtures above remain
required and await their peer-owned conversion. Do not discard them to report
an orchestration-only success.

Native resource coordination: supervisor message `msg-dd168540d72e48b2a8ee54d63361101f`
identified active hosted graphics CI and ordered local qualification after clear99
and core62. The local build was interrupted; its cache/build evidence is retained.
The process-name probe before launch had shown no compiler/CTest process, but
that probe did not establish hosted-runner availability. Future local large builds
are capped at two jobs and require that actual measurement order to finish.

Retained local evidence: [Linux cold](evidence/ci-performance-20261014/linux-cold.json),
[Linux warm](evidence/ci-performance-20261014/linux-warm.json),
[clean-commit Linux warm](evidence/ci-performance-20261014/linux-92936ed0-warm.json),
[clean-commit Linux with mandatory lint](evidence/ci-performance-20261014/linux-764ad8e9/run-summary.json),
[exact-name manifest classification](evidence/ci-performance-20261014/ci-manifest-lint.json),
[missing-negative-probe rejection](evidence/ci-performance-20261014/ci-manifest-failure.json),
[interrupted native source manifest](evidence/ci-performance-20261014/native-cold-source.json),
[interruption record](evidence/ci-performance-20261014/native-interrupted-summary.json),
[cache statistics](evidence/ci-performance-20261014/native-interrupted-cache.json),
[Windows CI lint](evidence/ci-performance-20261014/windows-ci-lint.log) and
[CMake label lint](evidence/ci-performance-20261014/cmake-label-lint.json).
The cold native cache had 1,047 requests, zero hits, zero failed compilations and
zero cache errors. There is no native CTest slowest-20 list for this interrupted
run; the historical list stays in the preserved history document.
