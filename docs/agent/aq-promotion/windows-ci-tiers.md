# Native Windows CI tiers and persistent builds

Implementation task: `keen-current-19`. This document describes the proposed
source change; it does not activate an AQ trust policy. Hosted acceptance remains
open until the owner delivers the reconciled source and retains its run artifacts.

## Tier selection

| Source/event | Windows tier | Check name | Packaging |
| --- | --- | --- | --- |
| `dev` push | fast advisory | `Native Windows fast build and tests` | no |
| task/parent/batch/integration/promotion/backmerge AQ push; PR to dev/staging/main | full v1 qualification | `Native Windows build and tests` | yes |
| `staging`, `main` push | full | `Native Windows build and tests` | yes |
| nightly schedule | full, forced clean, compiler cache disabled | `Native Windows build and tests` | yes |
| dispatch | automatic by ref, or explicitly full | corresponding name | full only |

An explicit fast request on staging/main, a PR to staging/main or a scheduled run
fails. The script defaults to **full** when `-Tier` is omitted, preserving the
meaning of legacy reusable callers. Only the standalone feedback workflow
explicitly opts into automatic dev-fast selection. Reusable callers require an
explicit `fast`/`full` choice and reject `auto`. The fast CTest label is `ci-fast`; full is
`ci-full`. Every registered test, including third-party subdirectory tests, receives
the full label. The inventory has 193 tests, with 159 selected for fast.

Fast selects existing CPU, foundation, Python, compiler-policy, third-party and
safety labels, plus the shader input-list contract. Five measured expensive CPU
fixtures remain full: `world_definition_tests`, `viewer_logic_tests`,
`sector_streamer_tests`, `part_flatten_tests`, `script_host_tests`. Vulkan/GPU,
editor census, shader rebuild and package execution tests remain full. CPU tests
with GPU-related names are still CPU contracts according to their existing
registration. No fixture size, assertion, timeout or failure rule is reduced.
The fast external-shader regression reuses the full shader dependency fixture on
different source/build drives. It serializes with the original same-drive fixture
because both create the same checkout-local source fixture.

`matter_ci_fast` builds the executable targets selected by these labels; it avoids
the editor and full-only executable targets. Shared engine dependencies still
compile on the first run. Both tiers use `ctest --no-tests=error`. Full packaging
runs even after a test failure so package evidence is available; the original test
failure still fails the job. Vulkan/GPU tests use the `matter-vulkan` resource lock
to avoid simultaneous device use and shared smoke outputs. CPU tests retain their
configured parallelism. Workflow concurrency remains per-ref, with an additional
exclusive filesystem lock protecting the persistent tree across refs.

## Reuse and invalidation

The default external root is `C:\matter-ci\matter-engine`, with
`build-relwithdebinfo` and a sibling ownership/state file. `actions/checkout` can
clean its checkout without removing these objects. The root must be outside the
checkout on a local drive; an unowned nonempty build directory or reparse-point
root/build directory is rejected. The clean operation is bounded to the owned
configuration directory.

The fingerprint includes root CMake files, all `cmake/` CMake/source manifests,
the build wrapper, Windows tool modules/locks, resolved tool paths and compiler,
CMake, Ninja and glslc binary hashes. Ordinary source/header changes reuse the
tree and Ninja's dependency graph. CMake or toolchain changes, a moved checkout,
an incomplete clean build, the first build, the first run of a new UTC day or
`-ForceClean` rebuild the tree. A successful build records completion; cancelled
or failed clean builds do not establish a reusable clean baseline. Tests always
run, including after a prior failed test run. `-DisableCompilerCache` clears both
compiler launchers and restores the normal PDB format.

The first local warm run exposed an existing glslc depfile limitation: an external
`C:` tree and a spaced `D:` checkout produce absolute dependency paths with
unescaped spaces. Ninja consequently recompiled shaders on each invocation.
`normalize-glslc-depfile.py` escapes those cross-drive paths before CMake reads
the depfile, and rejects unexpected/missing dependencies. Native same-drive and
cross-drive shader fixtures verify no-op stability and selective source/include
invalidation, with complete long-path shader inventory assertions retained.

`tools/windows/toolchain.lock.json` pins the native toolchain and sccache 0.18.0.
CI rejects a version mismatch. Installation verifies SHA-256 of both the release
archive and executable, including an already installed executable. Cache data is
under `sccache/<toolchain-lock-sha256>`, capped at 20 GB. A root-specific server
port and the exclusive root lock isolate the server; cancelled servers are
retired on the next run. Launcher failures fail the build. CMake CMP0141 and
`Embedded` debug information select `/Z7`, which MSVC sccache supports. See the
[sccache documentation](https://github.com/mozilla/sccache/blob/main/README.md)
and [CMake MSVC debug information documentation](https://cmake.org/cmake/help/latest/prop_tgt/MSVC_DEBUG_INFORMATION_FORMAT.html).

The nightly clean run disables the compiler cache to exercise the complete
compiler/toolchain path. The next cached run changes compiler flags back to `/Z7`
and Ninja rebuilds affected objects. Retaining the nightly PDB build does not
silently reuse objects with the cache-enabled flags.

## Promotion and trust reconciliation

At the task's base `f823151d8a06670efb007cee9a4b89ae48434535`, the only CI workflow
is `native-windows.yml`; the active trust manifest and Linux/reusable workflow are
absent. Read-only inspection of pending `aq/fleet-ridge-94` shows a v1 manifest
requiring `Native Windows build and tests` and `Linux foundation build and tests`,
and a `promotion-source-audit` set prefixed with `unattested-ci /`. That branch's
acceptance run and deployed policy must remain untouched by this task.

Owner reconciliation must implement the following binding before enabling dev
attestations for this implementation:

| Policy purpose | Exact Windows check | Linux check | Requirement |
| --- | --- | --- | --- |
| Existing `matter-engine-ci-v1` full source proof | `Native Windows build and tests` | `Linux foundation build and tests` | preserve full meaning; fast is never a substitute |
| Dev-fast feedback | `Native Windows fast build and tests` | Linux remains in reusable workflow | advisory only; never satisfies the current v1 gate |
| Staging/release source audit | `unattested-ci / Native Windows build and tests` | `unattested-ci / Linux foundation build and tests` | force full reusable-workflow input on the exact source SHA |
| Unattested direct staging/main fallback | full check, with caller prefix if reusable | Linux foundation check | no missing/verifier-error path may select fast |

Concrete owner composition templates are [tests.workflow.proposal.yml](tests.workflow.proposal.yml),
[main-attestation.workflow.proposal.yml](main-attestation.workflow.proposal.yml)
and [native-feedback.workflow.proposal.yml](native-feedback.workflow.proposal.yml).
Install them together with the pending fleet verifier/tests/trust assets, after
qualifying the reconciled source. They are stored outside `.github/workflows`
and do not change current qualification or deployed policy. The reusable `tests.yml`
defaults to full, has typed tier/clean/cache inputs, preserves both platforms and
rejects automatic selection. The attestation caller always runs an independent
explicit full audit on the exact dev SHA, including after a successful advisory
proof. Staging/main fallback remains full on missing/error/invalid attestation.

The reviewed AQ v1 source has one global check version; named sets select names,
not an independently authenticated version. Both current promotion gates inherit
root checks. Keep full-v1 gating and fast advisory. A separately trusted dev-fast
version requires demonstrated **deployed** support across daemon, generator,
selector, payload/verifier, App/protection and reviewed bindings; published source
alone is insufficient. Do not rename v1 checks or reinterpret existing full proofs.

The composed `tests.yml` owns task (`aq/*`), `aq/parent/**`, `aq/batches/**`,
`aq/integration/**`, `aq/promote/**`, `aq/backmerge/**` push and all protected-base
PR qualification. These always emit plain full-v1 Windows/Linux names. The
feedback wrapper owns dev advisory (`advisory-ci / Native Windows fast build and tests`)
and nightly/dispatch maintenance (`maintenance-ci / Native Windows build and tests`).
The attestation workflow owns dev source audit and staging/main fallback with the
existing `unattested-ci /` full aliases. Replace the standalone native workflow
when installing the templates; leaving both full producer definitions would
duplicate jobs. Current standalone source retains full candidate/PR execution
and 12/6 parallelism, but the pending fleet qualification source is unchanged.
AQ App identity,
repository/SHA, check conclusion and provenance verification remain required.

## Evidence and timing

Every workflow uploads `artifacts/ci/` even on failure. Files include the native
transcript, resolved toolchain, fingerprint inputs, full CTest inventory, selected
JUnit results, slowest 20/over-60-second report, sccache JSON/text statistics and
`run-summary.json` with source SHA, hosted/ref/tier, cleanup reasons, script hash,
parallelism and phase durations. Failed phases and test failures remain visible.

The historical hosted baseline is
[run 38100939715](https://github.com/ElectricJack/matter-engine/actions/runs/38100939715),
source `25a6601606bb1ed2fc730b9a710b5bf975bc306a`: 31m06s job, 30m41s build/test
step, 848s CTest, 189 tests and seven failures. Packaging did not run after those
failures. This is a historical different-source baseline, not a controlled
before/after performance comparison.

| Historical slowest test | Seconds | Decision |
| --- | ---: | --- |
| world_definition_tests | 322.686 | full; complete world fixtures retained |
| viewer_logic_tests | 223.966 | full; existing failure retained |
| sector_streamer_tests | 150.889 | full; full streaming fixture retained |
| part_flatten_tests | 126.302 | full; complete geometry/determinism fixture retained |
| vt_feedback_visibility_tests | 92.754 | full; serialize device use, retain failure |
| script_host_tests | 90.986 | full; complete script fixture retained |
| vt_direct_source_tests | 90.860 | full; serialize device use |
| vulkan_smoke_tests | 90.121 | full; serialize device use |
| windows_package_tests | 47.553 | full; package tests retained |
| vt_surface_material_tests | 41.811 | full; serialize device use |
| windows_build_wrapper_contract | 33.650 | fast; native/WSL contracts retained |
| obj_export_golden_tests | 31.109 | fast; existing failure retained |
| sparse_voxel_gpu_tests | 29.448 | full; serialize device use |
| mesh_simplifier_tests | 26.078 | fast |
| smoke_vt_normal_frame | 22.423 | full; serialize device use |
| vt_input_snapshot_tests | 18.354 | full; serialize device use |
| event_hub_tests | 17.269 | fast |
| geometry_pages_gpu_tests | 17.078 | full; serialize device use |
| solid_face_projection_gpu_tests | 14.241 | full; serialize device use |
| viewer_graph_tests | 11.647 | fast |

The >60s CPU fixtures are tiered rather than shortened. Their runtime still needs
profiling for algorithmic improvements; the historical data alone does not prove
a cause. GPU serialization addresses contention, but an improved full-tier
duration must be measured. The seven baseline failures were export golden,
viewer logic, shader source, VT feedback visibility, asset root, Windows/WSL build
wrapper and editor registration census. They are not waived by this change.

Read-only inspection of the pending Linux job identifies MemoryLib,
MeshChartingLib and AssetStoreLib behavioral suites as overlaps with the Windows
foundation tests. Keep them on Linux because the GCC/platform coverage differs;
removing them solely because Windows runs similar assertions would lose compiler
and ABI coverage. Its attestation-verifier tests also remain independent. No
Linux job exists in this task's base checkout, so none is edited here.

## Local reproduction and hosted acceptance

Use a task-specific external root for local measurements so the hosted runner's
tree/server are not modified:

```powershell
./tools/ci-windows.ps1 -Tier fast -CiRoot C:\matter-ci\keen-current-19
# Copy artifacts/ci to a separate measurement directory before the next run.
./tools/ci-windows.ps1 -Tier fast -CiRoot C:\matter-ci\keen-current-19
# Force compiler invocations against the retained cache to measure real hits.
./tools/ci-windows.ps1 -Tier fast -CiRoot C:\matter-ci\keen-current-19 -ForceClean
./tools/ci-windows.ps1 -Tier full -CiRoot C:\matter-ci\keen-current-19
```

An incremental no-op build can have zero compiler requests; that is reuse, not a
cache hit-rate measurement. Use the forced-clean cached run for the latter and
retain both summaries. Local runs have `hosted: false` and do not satisfy hosted
acceptance. Owner delivery must first reconcile the promotion workflows/policy,
then retain exact-source warm dev and full staging Actions runs and artifact
links. The target is fast CTest <10m and warm dev job approximately <20m. Record
the measured full staging duration, failures, package validation and cache stats;
do not label historical or local evidence as an accepted hosted run.
