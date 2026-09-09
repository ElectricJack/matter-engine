# Task 2 Report: Immutable River Runtime Publication

## Outcome

Implemented the complete Task 2 slice on base `728bd1e11e0804130cd682642cd45d836271adfd` and committed it as `d9c4fb43` (`feat: publish immutable river runtime bindings`).

The implementation adds artifact v5 presentation persistence, network manifest v2 field digests and exact typed package closure, deterministic handoff presentation assembly, section-local presentation production, a provider/renderer-free public runtime binding, WorldSession access, and a single cancellation-safe publication record pairing the CPU binding with the existing render binding.

## Contract Decisions Applied

- Manifest v2 Ready closure contains exactly one Runtime and one Presentation typed field-product reference. Missing, duplicate, stale, or extra references are rejected.
- `RiverRuntimeBinding::generation()` is the accepted manifest `payload_digest`; runtime and presentation digests remain separate.
- Internal `hydrology::RiverFeature` conversion to public `matter::RiverFeature` is an exhaustive switch and rejects unknown values.
- The renderer and CPU binding share one immutable publication record and one atomic swap. Task 7 can extend that record without creating a second publication boundary.
- Candidate storage, render binding, and failed-debug binding are fully allocated before `hydrology_generation_mutex`; the locked region performs the stale-token check, provider commit, atomic swap, status update, and event emission.
- Failed candidates preserve the prior accepted publication. Cancelled candidates publish neither CPU nor renderer state. Failed-debug water remains visual-only and is installed only when no accepted publication exists.
- The temporary v4 three-key cache predicate was removed; v5 cache validation requires full `ProductKeys` equality including presentation.
- Per Ruling 9, the PhysX product path retains `GameplayFieldStatistics`, samples finite terrain records, and invokes the Task 1 presentation builder with neutral marker, wake, and local-override inputs.

## TDD Evidence

### RED

Tests were added before production implementation.

- `hydrology_artifact_tests` failed to compile because `HydrologyArtifact::presentation_field` did not exist.
- `hydrology_network_artifact_tests` failed to compile because manifest runtime/presentation digests, typed field references, and the field-product enum did not exist.
- `hydrology_handoff_products_tests` failed to compile because section/network presentation products did not exist.
- `async_bake_tests` failed to compile because `matter/river_runtime.h` did not exist.

The initial post-implementation focused run also usefully exposed:

- strict positive reconstructed normal-Y validation had temporarily been relaxed; the artifact negative fixture caught it and strict validation was restored;
- CTest did not inherit a Visual Studio developer PATH, so the dependency probe was made to discover and enter the MSVC environment explicitly; and
- the cancellation test originally waited without pumping queued GPU jobs, then stopped on generation A's expected cancellation event. It now pumps while approaching the publication barrier and waits through A cancellation for generation B's `BakeFinished`.

### GREEN

All builds used the required MSVC entry point.

Focused targets built successfully:

- `hydrology_artifact_tests`
- `hydrology_network_artifact_tests`
- `hydrology_handoff_products_tests`
- `river_runtime_tests`
- `async_bake_tests`
- `physx_dependency_contract_tests`
- `physx_adapter_contract_tests`

Renderer-conditioned compile verification:

- `tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_engine_viewer_objects`
- Result: success; all 166 viewer product sources, including the renderer half of `matter_engine.cpp`, compiled.

Final focused CTest command:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'hydrology_artifact_tests|hydrology_network_artifact_tests|hydrology_handoff_products_tests|river_runtime_tests|async_bake_tests|physx_dependency_contract_tests|physx_adapter_contract_tests' --output-on-failure
```

Result: **7/7 passed**, 0 failed, 7.56 seconds.

Exactly one full CPU suite was run after focused GREEN:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -L cpu -j 8 --output-on-failure
```

Result: **37/37 passed**, 0 failed, 102.49 seconds real time.

## Changed Files

Public API and publication:

- `MatterEngine3/include/matter/river_runtime.h`
- `MatterEngine3/include/matter/world_session.h`
- `MatterEngine3/src/hydrology/river_runtime.cpp`
- `MatterEngine3/src/hydrology/river_runtime_internal.h`
- `MatterEngine3/src/matter_engine.cpp`

Persistence, products, and provider assembly:

- `MatterEngine3/src/hydrology/hydrology_artifact.h`
- `MatterEngine3/src/hydrology/hydrology_artifact.cpp`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.h`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- `MatterEngine3/src/hydrology/hydrology_handoff_products.h`
- `MatterEngine3/src/hydrology/hydrology_handoff_products.cpp`
- `MatterEngine3/src/hydrology/physx_fluid_bake.cpp`
- `MatterEngine3/src/hydrology/water_visual_products.h`
- `MatterEngine3/src/provider/local_provider.cpp`

Tests and dependency/build registration:

- `MatterEngine3/tests/river_runtime_tests.cpp`
- `MatterEngine3/tests/hydrology_artifact_tests.cpp`
- `MatterEngine3/tests/hydrology_network_artifact_tests.cpp`
- `MatterEngine3/tests/hydrology_handoff_products_tests.cpp`
- `MatterEngine3/tests/async_bake_tests.cpp`
- `MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp`
- `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- `tools/tests/physx_dependency_contract_tests.ps1`
- `cmake/manifests/engine-core.sources`
- `cmake/MatterEngine.cmake`
- `cmake/MatterViewer.cmake`
- `cmake/tests/viewer_graph_tests.cmake`
- `MatterEngine3/tests/Makefile`

## Self-Review

- Artifact v5 writes exactly 21 bytes per gameplay record and 22 bytes per presentation record, validates exact section lengths and final remaining bytes, validates feature/wet bytes before conversion, and assigns only after complete digest and structural validation.
- Presentation persistence checks finite channels, `[0,1]` normalization, strict positive reconstructed normal Y, dimension equality, and gameplay/presentation wet-flag equality.
- Manifest v2 validates both nonzero field digests, exactly two distinct cache-relative typed references, exact digest correspondence, and canonical ordering. Deserialization is transactional.
- Handoff presentation uses gameplay-equivalent ownership selection, full positive-hemisphere normal reconstruction and renormalization, linear continuous channels, explicit nearest feature ownership, and rejects gameplay/presentation wet disagreement.
- Runtime construction recomputes both canonical field digests, rejects zero/mismatched metadata and layouts, deep-copies immutable storage, and performs exhaustive enum conversion. Sampling rejects empty, dry, non-finite, out-of-bounds, mismatched, or invalid storage. Batch null/count contracts are covered.
- Publication readers load one immutable record. No reader can combine a CPU generation with a renderer generation. Superseding a bake no longer clears the prior accepted record.
- The public header dependency probe rejects PhysX, Vulkan, Flecs, provider, render, and hydrology dependencies.
- Core/viewer hard-coded source censuses were updated and exercised by configuration plus the viewer object build.
- `git diff --check` passed before commit. Unrelated pre-existing untracked files were not staged or modified.

## Concerns

No known blockers or remaining Task 2 concerns. Task 6 is expected to replace the intentionally neutral presentation marker/wake/local-override inputs with authored values, and Task 7 is expected to extend the existing publication record rather than introduce a second swap boundary.

## Independent Review Repair (2026-08-24)

### Outcome

The three Important review findings were repaired in code/test commit
`ba542f02` (`fix: harden river runtime publication`) on top of `0bcb87bf`.
The repair adds retained-binding lifetime rejection, real typed field-product
files and Ready package validation, and transactional preservation across all
artifact/network decode and load failures.

### TDD RED Evidence

Tests were changed before the corresponding production repairs.

- `tools/build-windows.ps1 -Config RelWithDebInfo -Target river_runtime_tests`
  failed to compile because `RiverRuntimePublicationLease`, the internal
  binding access seam, and the lease-bearing build input did not exist.
- `tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests`
  failed to compile because `hydrology_field_artifact.h` and the typed field
  persistence API did not exist.
- After building `hydrology_artifact_tests`, Visual Studio CTest with
  `-R '^hydrology_artifact_tests$'` failed 11 preservation assertions: corrupt,
  truncated, digest-invalid, version-invalid, stale-key, and file-load failures
  all cleared the caller's valid sentinel.
- A later focused `hydrology_artifact_tests` RED run failed the additional
  `load_or_build_artifact` sentinel check, proving its failed-load/failed-build
  path still cleared prior output.

The first async repair run exposed a test-fixture issue rather than a
publication defect: changing only the network seed reused an accepted section
cache, so the injected visual failure never ran. The fixture now changes a
simulation-keyed PBD value for the failed generation; the corrected test proves
cancelled and failed replacements preserve the incumbent lease.

### Implementation

- `AuthoredFluidPublication` now owns a shared atomic publication lease also
  retained by immutable runtime storage. Sampling checks that lease without
  allocation. Under `hydrology_generation_mutex`, a successful replacement
  invalidates the prior lease and swaps the single CPU/render publication
  record; cancelled or failed candidates do neither. Session destruction also
  invalidates its final retained publication.
- The internal analytic runtime fixture covers exact bilinear scalar values,
  batch partial counts and null contracts, OOB/dry rejection, zero/stale/null
  metadata, invalid layout/feature rejection, and retained-lease invalidation.
- Field products use a bounded little-endian wire format: a 32-byte typed
  header, a 24-byte layout, and exact 21-byte Runtime or 22-byte Presentation
  records. Decode validates version, reserved bytes, kind, dimensions, exact
  remaining bytes, enums, wet bytes, finite/channel/normal constraints, and a
  recomputed canonical field digest before assignment.
- Each field file is written through a unique temporary, flushed, reopened,
  fully decoded and digest/type checked, then atomically replaced. The provider
  writes and revalidates both content-addressed files before publishing Ready.
  A cancellation recheck occurs after the pair and before manifest publication.
- Ready save/load validation requires exactly one canonical cache-relative
  Runtime ref and one distinct Presentation ref. It loads both actual files,
  checks their typed structure and manifest digests, then checks exact layout,
  count, and wet-mask agreement. Missing, corrupt, truncated, swapped,
  stale-digest, duplicate, and extra closure cases are covered.
- `deserialize_artifact`, `load_artifact_validated`,
  `load_or_build_artifact`, `deserialize_network_artifact`, and
  `load_network_artifact_validated` now parse/load into local candidates and
  assign only after all checks succeed.
- No new core translation unit was added: the field wire implementation lives
  with the network artifact implementation, so the established 123/166 source
  census remains unchanged.

### GREEN Evidence

All repair builds used only the required Windows entry point:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_runtime_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target async_bake_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_dependency_contract_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_engine_viewer_objects
```

Every target succeeded. The dependency target reported provider-free river
runtime API PASS, and the renderer-conditioned viewer object graph compiled.

Final focused command, run from
`MatterEditor/build/cmake/windows-msvc/relwithdebinfo`:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' -C RelWithDebInfo -R '^(hydrology_artifact_tests|hydrology_network_artifact_tests|hydrology_handoff_products_tests|river_runtime_tests|async_bake_tests|physx_dependency_contract_tests|physx_adapter_contract_tests)$' --output-on-failure
```

Result: **7/7 passed**, 0 failed, 7.79 seconds.

Per the repair instruction, the broad CPU suite was not rerun. The prior
post-feature evidence remains exactly one full `-L cpu -j 8` run with
**37/37 passed**.

### Repair Files

- `MatterEngine3/include/matter/river_runtime.h`
- `MatterEngine3/include/matter/world_session.h`
- `MatterEngine3/src/hydrology/hydrology_artifact.cpp`
- `MatterEngine3/src/hydrology/hydrology_field_artifact.h`
- `MatterEngine3/src/hydrology/hydrology_handoff_products.h`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.h`
- `MatterEngine3/src/hydrology/river_runtime.cpp`
- `MatterEngine3/src/hydrology/river_runtime_internal.h`
- `MatterEngine3/src/matter_engine.cpp`
- `MatterEngine3/src/provider/local_provider.cpp`
- `MatterEngine3/tests/async_bake_tests.cpp`
- `MatterEngine3/tests/hydrology_artifact_tests.cpp`
- `MatterEngine3/tests/hydrology_network_artifact_tests.cpp`
- `MatterEngine3/tests/river_runtime_tests.cpp`

### Process and Self-Review Audit

- No subagents or reviewers were spawned for the repair.
- No Make, GCC, g++, MinGW, MSYS2, or collect2 command was invoked. Builds used
  `tools/build-windows.ps1`; tests used Visual Studio CTest in the prescribed
  build tree. The first sandboxed build preflight could not execute the native
  Python launcher, so the same approved build script was rerun with the needed
  host-tool access.
- `git diff --check` and the staged diff check passed. Only the listed repair
  files were staged; unrelated pre-existing untracked files were preserved.
- Sampling remains renderer/provider-free and allocation-free. The lease load
  is the only new hot-path operation.
- The successful replacement boundary is deliberately fail-closed: the old
  lease is invalidated immediately before the one atomic record store while the
  generation lock is held. Thus a concurrent reader can observe an invalid old
  binding during the handoff, but can never keep sampling old data after the new
  CPU/render record is visible.

### Remaining Concerns

No known Task 2 repair blocker remains. Unreferenced content-addressed field
files may remain after cancellation or a second-file write failure, but no Ready
manifest can reference an incomplete or invalid pair; later cache maintenance
may garbage-collect such orphan files.

## Independent Re-review Repair Round 2 (2026-08-24)

### Outcome

The remaining two Important findings were repaired in code/test commit
`ee0f955b` (`fix: linearize river publication and field storage`) plus the
contained IO-audit follow-up `b9828d70` (`fix: anchor immutable field
publication`). The final implementation has one observable CPU/render
publication identity, transactionally rejects sampling across replacement, and
uses exact digest-derived, confined, immutable, durably ordered field blobs.

### TDD RED Evidence

The retained-identity, mid-batch replacement, prior-gap, canonical-path,
immutable-overwrite, growth/trailing-byte, concurrent mutation, and reparse
tests were authored before the corresponding production changes.

The captured RED command was:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_runtime_tests
```

MSVC failed compilation on the intentionally missing
`RiverRuntimePublicationSlot`, `RiverRuntimePublicationIdentity`, `publish`,
and batch-hook contracts. This was the expected RED rather than an environment
or unrelated failure. The first sandboxed invocation could not discover native
Windows Python; the same required build entry point was rerun with host-tool
access to capture the compiler RED.

The first post-implementation persistence run also exposed a fixture regression:
the test still enumerated `root/fields` after canonical paths moved blobs to
`root/hydrology/fields`. After fixing that stale fixture, the focused runtime
and persistence pair passed. A later audit test run exposed that holding a
non-sharing Windows directory handle through `MoveFileEx` prevents the rename;
the final follow-up uses a handle-relative, create-new native rename anchored
to the validated fields directory instead.

### Implementation and Contract Audit

- `RiverRuntimePublicationSlot::current` is the sole atomic publication point.
  The immutable `AuthoredFluidPublication` is that identity and contains both
  CPU runtime and renderer state. There is no separate validity boolean.
- CPU and renderer readers load the slot, validate that the binding carries the
  same slot/identity, and retry unless the slot remains unchanged. A successful
  replacement's single store is the only event that stales the incumbent;
  cancellation, failed candidates, and pre-publication work preserve it.
- Scalar sampling validates identity before computation and again before
  assignment. Batch sampling validates once at entry, samples without
  per-element lease checks or allocations, then validates once at exit; any
  mid-batch replacement clears every output and returns zero.
- Session teardown clears the same slot atomically. The public runtime header
  remains renderer/provider-free; the dependency contract target passed.
- Runtime and Presentation references must equal the one canonical path helper:
  `hydrology/fields/<kind>-<16 lowercase digest hex>.mhydfield`. Alternate,
  parent-traversing, absolute, duplicate, extra, stale, or type-swapped paths
  cannot serialize or load as Ready.
- Ready validation holds trusted cache/directory handles, rejects every
  symlink/reparse component, opens the final file without following its leaf,
  reads size/content/EOF from one handle, and verifies stable file identity.
  Concurrent write/delete replacement is denied on Windows; POSIX uses
  `openat`/`O_NOFOLLOW` plus descriptor and namespace identity checks.
- Field publication is immutable and create-if-absent. Existing canonical
  blobs are accepted only when their complete bytes exactly match; different
  bytes are never replaced. Windows flushes the temporary file and performs a
  handle-relative native rename into the trusted directory. POSIX uses
  `fsync`, `linkat`, and directory `fsync` before and after temporary unlink.
- Manifest temporaries are OS-flushed before rename. Windows uses
  `FlushFileBuffers` plus write-through replacement; POSIX uses file `fsync`,
  rename, and containing-directory `fsync`. Both field blobs are reopened,
  parsed, type/digest checked, and package-closure checked before Ready becomes
  visible. A partial pair can leave only unreferenced immutable blobs.
- Prior transactional artifact/network destination preservation remains
  covered and passed unchanged.

### Final GREEN Evidence

All final builds used only:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target <target>
```

These seven required targets built successfully:

- `hydrology_artifact_tests`
- `hydrology_network_artifact_tests`
- `hydrology_handoff_products_tests`
- `river_runtime_tests`
- `async_bake_tests`
- `physx_dependency_contract_tests`
- `physx_adapter_contract_tests`

`matter_engine_viewer_objects` also built successfully after the final IO
follow-up, compiling the renderer-conditioned publication reader.

The definitive focused test command against the exact committed code was:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R '^(hydrology_artifact_tests|hydrology_network_artifact_tests|hydrology_handoff_products_tests|river_runtime_tests|async_bake_tests|physx_dependency_contract_tests|physx_adapter_contract_tests)$' --output-on-failure
```

Result: **7/7 passed**, 0 failed, 7.72 seconds. Per instruction, no broad
suite was rerun; the retained prior evidence remains the single 37/37 CPU run.

### Round 2 Files

- `MatterEngine3/include/matter/world_session.h`
- `MatterEngine3/src/hydrology/hydrology_field_artifact.h`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- `MatterEngine3/src/hydrology/river_runtime.cpp`
- `MatterEngine3/src/hydrology/river_runtime_internal.h`
- `MatterEngine3/src/matter_engine.cpp`
- `MatterEngine3/src/provider/local_provider.cpp`
- `MatterEngine3/tests/async_bake_tests.cpp`
- `MatterEngine3/tests/hydrology_network_artifact_tests.cpp`
- `MatterEngine3/tests/river_runtime_tests.cpp`

### Process, Self-review, and Remaining Concerns

- No subagents or reviewers were spawned. No Make, GCC, g++, MinGW, MSYS2,
  or collect2 command was invoked. Builds used the prescribed Windows script;
  tests used Visual Studio CTest in the prescribed tree.
- `git diff --check` and staged diff checks passed. Only scoped files were
  staged; unrelated untracked files were preserved. Test-owned temporary
  directories left by a diagnostic failure were resolved beneath the Windows
  temp root before their exact Task 2-prefixed paths were removed.
- Sampling adds shared-pointer atomic loads only; it performs no dynamic
  allocation in the scalar or batch hot path.
- Windows handle-relative rename is dynamically resolved from the native NT
  API and fails closed if unavailable. The exercised Windows 11/MSVC host
  supports it. POSIX durable/openat branches were source-reviewed but could not
  be executed under the mandated MSVC-only verification policy; that is the
  only platform-specific verification limitation.

## Independent Re-review Repair Round 3 (2026-08-24)

### Outcome

The final two IO findings were repaired in code/test commit `70a4d1e3`
(`fix: confine hydrology field publication`). Field publication now performs
all descendant creation, temporary IO, validation, publication, and cleanup
relative to a verified cache-root/directory handle, and every production native
resource is immediately RAII-owned across allocation failure and early return.

### TDD RED Evidence

Tests were changed before production code to require:

- a deterministic Windows junction from `cache/hydrology` to an external
  directory whose `fields` child did not exist, with no external side effect;
- a separate canonical-leaf symlink/reparse fixture whose external target bytes
  remain unchanged;
- corruption of the actual digest-named canonical immutable leaf followed by
  save rejection without replacement;
- deterministic allocation failures after cache-root, descendant-directory,
  and temporary/file handle acquisition, with no canonical field, no temporary,
  no Ready manifest, immediate tree deletion, and exact load-output
  preservation.

The focused RED command was:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests
```

After correcting the Windows-only test fixture to use its own SDK-compatible
mount-point buffer declaration, MSVC reached the intended RED and failed at
link with `LNK2019` for the deliberately missing
`set_hydrology_field_io_failure_for_test` implementation. The initial sandboxed
wrapper invocation could not launch the installed native Python; the same
required build entry point was rerun with host-tool access. No alternate
compiler or build path was used.

### Implementation and Resource Audit

- The caller-created cache root is the explicit trust boundary. Save opens that
  existing root without following a reparse point and performs zero path-based
  descendant mutation before it is verified.
- Windows opens or creates `hydrology`, `fields`, and the unique temporary with
  handle-relative `NtCreateFile`, always using reparse-point-open semantics and
  validating directory/file attributes on the acquired handle. The same
  temporary handle is written, OS-flushed, rewound, byte/EOF validated, and
  create-new renamed relative to the trusted fields handle with
  `NtSetInformationFile`. Failed publication cleanup is deletion-by-handle;
  existing canonical bytes are read through a non-sharing relative handle and
  are accepted only when exactly identical.
- POSIX uses root `open(O_DIRECTORY|O_NOFOLLOW)`, `mkdirat`/`openat` for both
  descendants, `openat(O_CREAT|O_EXCL|O_NOFOLLOW)` for the temporary, and the
  same descriptor for write, `fsync`, rewind, byte/EOF validation, `linkat`,
  `unlinkat`, and directory `fsync`. Its temporary guard unlinks relative to the
  trusted directory on every exception or early return.
- `UniqueNativeHandle` and `UniqueNativeFd` are move-only and close in
  `noexcept` destructors. Directory-handle containers reserve before acquiring
  the root. All artifact read handles, durable-write handles, directory-flush
  descriptors, confined field handles, temporary handles, and existing-file
  comparison handles are RAII-owned before any allocation or injected throw.
- All public bool field/network serialize, deserialize, save, and load entry
  points translate allocation, filesystem, standard, and unknown exceptions to
  `gpu_meshing::Error`. Deserialize/load candidates remain transactional and
  are assigned only after complete validation.
- Ready validation still requires the two exact digest-derived typed paths and
  reopens and verifies both payloads. A failed or partial field pair cannot make
  a Ready manifest visible.

The exercised Windows host created both the directory junction and leaf
symlink/reparse fixtures; the verbose focused run printed no `SKIP` and ended
`ALL PASS`. The POSIX branch was source-reviewed but not executed because this
task explicitly required MSVC-only verification. Windows has no portable
directory-`fsync` analogue; field data is flushed with `FlushFileBuffers`, the
temporary is opened write-through, and publication is a handle-relative native
rename before Ready validation/publication.

### Final GREEN Evidence

All final builds used only the required entry point:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_runtime_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target async_bake_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_dependency_contract_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_engine_viewer_objects
```

All eight targets succeeded. The dependency contract again reported the
provider-free river runtime API PASS, and the renderer-conditioned viewer graph
compiled. The definitive focused test command against the final audited source
was:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R '^(hydrology_artifact_tests|hydrology_network_artifact_tests|hydrology_handoff_products_tests|river_runtime_tests|async_bake_tests|physx_dependency_contract_tests|physx_adapter_contract_tests)$' --output-on-failure
```

Result: **7/7 passed**, 0 failed, 7.22 seconds. Per instruction, no broad CPU
suite was rerun; the retained earlier Task 2 evidence remains the single 37/37
CPU run.

### Round 3 Files and Process Audit

- `MatterEngine3/src/hydrology/hydrology_field_artifact.h`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- `MatterEngine3/tests/hydrology_network_artifact_tests.cpp`

No subagents or reviewers were spawned. No Make, GCC, g++, MinGW, MSYS2, or
collect2 command was invoked. `git diff --check` and the staged diff check
passed. Only the three scoped code/test files were committed; unrelated
pre-existing untracked files were preserved. No known Task 2 blocker remains.

## Independent IO Re-review Repair Round 4 (2026-08-24)

### Outcome

The three remaining IO findings were repaired in code/test commit `4c90e133`
(`fix: harden hydrology artifact publication`). POSIX immutable field
publication now retains an anonymous validated descriptor through atomic
create-new linking, Windows Ready loading opens field leaves relative to the
held trusted directory, and manifest replacement uses the same confined,
same-handle validation and cleanup discipline.

### TDD RED and Debugging Evidence

The identity-decision, held-directory namespace-swap, manifest allocation,
manifest parent/leaf/temporary-reparse, and manifest parent-swap tests were
added before the production seams and implementation. The captured RED command
was:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests
```

MSVC compiled the new tests, then failed the link with `LNK2019`/`LNK1120` for
the intentionally absent `set_hydrology_namespace_validation_test_hook` and
`hydrology_file_identity_stable` symbols.

The first implemented persistence run then terminated with an access violation.
Focused CDB isolation identified the exact failing test expression as an index
into an empty rejected-manifest byte vector. A temporary unbuffered diagnostic
run established that the preceding baseline manifest save had failed with
`could not publish hydrology network manifest`. The root cause was the held
Windows parent-directory handle's restrictive sharing mode: native
handle-relative replacement requires delete/write-compatible sharing. The
manifest parent guard now opts into share-read/write/delete, and reopens the
named parent before and after publication to require the same native identity.
The diagnostic buffering change was removed before the final build.

### Implementation and Native-resource Audit

- Linux/POSIX immutable field save uses `openat(..., O_TMPFILE)` and retains the
  RAII-owned descriptor through write, `fsync`, rewind, exact byte/EOF
  validation, and `linkat(temp_fd, "", fields_fd, canonical,
  AT_EMPTY_PATH)`. If either secure primitive is unavailable at compile or run
  time, publication fails closed. A newly linked entry is checked against the
  retained descriptor's device/inode and the containing directory is flushed.
- An existing POSIX canonical blob is opened with `openat`/`O_NOFOLLOW`; the
  descriptor, directory entry before IO, and directory entry after IO must all
  have one device/inode identity, and bytes must match exactly. The shared
  platform-neutral identity helper rejects either before- or after-identity
  substitution.
- Windows Ready validation and the direct public field loader share one
  trusted-parent implementation. The leaf is opened only with handle-relative
  `NtCreateFile`, reparse following is disabled, sharing prevents mutation, and
  size/content/EOF/type/digest checks all use that one RAII handle. A
  deterministic namespace hook proves the acquired directory remains the
  authority even if its path namespace is attacked after acquisition.
- Manifest save treats the already-existing caller parent as its explicit
  trust boundary. It does not create directories or use a path-based
  temporary, reopen, delete, or rename after verification. Windows creates the
  temporary relative to the held parent, writes, flushes, rewinds, rereads,
  deserializes, and replaces the final name through that same handle; its
  deletion guard remains armed until the final parent-identity check. POSIX
  uses `openat(O_CREAT|O_EXCL|O_NOFOLLOW)`, retains the validated descriptor,
  proves the named temporary identity, uses relative `renameat`, proves the
  published identity, and `fsync`s the parent directory.
- `UniqueNativeHandle`, `UniqueNativeFd`, `WindowsTemporaryFile`, and
  `PosixNamedTemporaryFile` own every native resource immediately. Container
  capacity is reserved before root acquisition. The POSIX named-temp cleanup
  guard unlinks only when the current directory entry is still the regular file
  with its captured device/inode; it cannot remove an attacker's replacement or
  double-unlink a disarmed publication. Windows cleanup is deletion-by-handle.
- Allocation failures injected after manifest-parent acquisition, after
  manifest-file acquisition, and immediately before rename are caught by the
  public bool API, preserve the old manifest byte-for-byte, and leave no
  temporary. Existing transactional artifact/network destination preservation
  and the single CPU/render publication identity were not changed.

On the exercised Windows host, directory namespace replacement, manifest
parent junction, manifest leaf symlink, and manifest temporary symlink fixtures
all ran; the verbose focused test emitted no `SKIP`. External targets remained
unchanged. The POSIX implementation was source-audited but could not be
compiled or executed under the task's mandatory MSVC-only policy. POSIX
manifest replacement necessarily uses a trusted-parent-relative named
`renameat`; the retained descriptor plus before/final identity checks make
replacement fail closed, while field publication uses the stronger anonymous
`O_TMPFILE`/`AT_EMPTY_PATH` primitive required by the finding.

### Final GREEN Evidence

All final targets were rebuilt using only:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target <target>
```

The seven required focused targets and `matter_engine_viewer_objects` all
succeeded on the final source. The dependency contract reported PASS and the
viewer-conditioned objects, including the network artifact implementation,
compiled. The single final focused test command was:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R '^(hydrology_artifact_tests|hydrology_network_artifact_tests|hydrology_handoff_products_tests|river_runtime_tests|async_bake_tests|physx_dependency_contract_tests|physx_adapter_contract_tests)$' --output-on-failure
```

Result: **7/7 passed**, 0 failed, 8.27 seconds. Per instruction, no broad CPU
suite was rerun; the retained Task 2 full-suite evidence remains the earlier
single **37/37** CPU run.

### Round 4 Files and Process Audit

- `MatterEngine3/src/hydrology/hydrology_field_artifact.h`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- `MatterEngine3/tests/hydrology_network_artifact_tests.cpp`

No subagents or reviewers were spawned. No Make, GCC, g++, MinGW, MSYS2, or
collect2 command was invoked. One sandboxed final-gate wrapper invocation could
not execute the installed native Python launcher; the identical required
wrapper was rerun with host-tool access. `git diff --check`, the staged diff
check, all focused builds, viewer-object compilation, and the seven-test CTest
gate passed. Only the three scoped code/test files were included in the repair
commit; unrelated untracked files were preserved. No known Task 2 blocker
remains.

## Confined IO Re-review Repair Round 5 (2026-08-24)

### Architecture Decision and Outcome

The three manifest-publication findings were repaired in code/test commit
`5e4c2480` (`fix: publish immutable hydrology manifests`). The production
manifest path is already a semantic cache slot derived from `network_key` and
`terrain_revision`:

`hydrology/network-<network-key>-<terrain-revision>.mhyn`

No production caller requires in-place overwrite. Manifest save therefore now
uses an immutable create-if-absent contract without changing the path scheme:
an identical existing file is accepted, while different bytes at the same
semantic slot fail closed and require cache maintenance to remove the stale or
corrupt entry. This avoids a new path migration while eliminating the POSIX
mutable-name replacement race.

### TDD RED and Focused Correction

Tests were added first for same-key/terrain differing-payload collision,
identical existing success, post-validation Windows temporary write/delete/
replace denial, exact-byte commit, and after-commit parent namespace swap.
The captured RED command was:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests
```

MSVC compiled the new tests and then failed at link with `LNK2019`/`LNK1120`
for the deliberately missing `set_hydrology_manifest_publication_test_hook`.

The first focused GREEN attempt exposed one obsolete fixture assumption: a test
persisted a Ready manifest, then overwrote that same path with an Incomplete
diagnostic manifest. Save correctly rejected the differing immutable bytes.
The diagnostic now uses its own distinct manifest path, preserving its original
"persisted but never Ready" assertion without weakening immutable publication.
The rebuilt verbose persistence test then printed `ALL PASS` with no `SKIP`.

### Publication Protocol and Audit

- Windows creates the manifest temporary relative to the held trusted parent
  with `FILE_SHARE_READ` only. The owner retains read/write/delete access, but
  external post-validation opens for write or delete, `DeleteFile`, and
  replace-existing `MoveFileEx` all fail. The same owner handle is written,
  flushed, rewound, reread with exact EOF/bytes, and deserialized.
- The deterministic BeforeCommit hook runs after validation. The allocation
  failure seam and the final named-parent/native-identity check run before the
  create-new native rename. The handle-relative rename with
  `replace_if_exists = FALSE` is the sole Windows commit point.
- Immediately after successful rename, the temporary deletion guard is
  disarmed and its handle is closed. The noexcept AfterCommit hook is purely
  observational; no fallible check follows the commit and save returns success.
  The executed hook renamed the manifest parent after commit, and the API still
  returned true with the exact committed file retained in the renamed parent.
- On Windows create-new collision, the existing leaf is opened relative to the
  held parent with reparse rejection and `FILE_SHARE_READ` only, then complete
  size/content/EOF bytes are compared through that same handle. Exact bytes
  succeed; different bytes or a non-regular/reparse entry fail without
  replacement.
- POSIX creates an anonymous manifest using
  `openat(parent_fd, ".", O_TMPFILE | O_RDWR | O_CLOEXEC)`, retains the
  RAII-owned descriptor through write, `fsync`, rewind, exact byte/EOF
  validation, and deserialization, then publishes only with
  `linkat(temp_fd, "", parent_fd, target, AT_EMPTY_PATH)`. There is no named
  temporary and no mutable source-name interval.
- POSIX `EEXIST` opens the canonical leaf using `openat`/`O_NOFOLLOW`, requires
  one descriptor/directory-entry device-and-inode identity before and after the
  same-handle read, and accepts only exact bytes. A successful new link verifies
  the retained descriptor identity and flushes the containing directory. Hosts
  without `O_TMPFILE` plus `AT_EMPTY_PATH` fail closed.
- Ready field closure is still reopened and fully validated before any
  manifest temporary is created. Field data is durably published first, and a
  differing/partial manifest can never replace an accepted Ready slot. The
  closed confined field paths and single CPU/render publication identity were
  unchanged.

Every Windows native resource is immediately owned by existing move-only RAII
wrappers. All allocations, injected failures, namespace checks, and exact-byte
comparison failures before create-new leave the guard armed; cleanup is
deletion-by-handle. No path-based manifest mutation was introduced.

### Final GREEN Evidence

All final targets were built only with:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target <target>
```

The seven required focused targets and `matter_engine_viewer_objects` all
succeeded. The dependency contract reported PASS and the renderer-conditioned
network artifact source compiled. The one final focused test command was:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R '^(hydrology_artifact_tests|hydrology_network_artifact_tests|hydrology_handoff_products_tests|river_runtime_tests|async_bake_tests|physx_dependency_contract_tests|physx_adapter_contract_tests)$' --output-on-failure
```

Result: **7/7 passed**, 0 failed, 14.54 seconds. Per instruction, no broad CPU
suite was rerun; the earlier single **37/37** CPU-suite result remains retained.

### Round 5 Files, Process, and Limitation

- `MatterEngine3/src/hydrology/hydrology_field_artifact.h`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.h`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- `MatterEngine3/tests/hydrology_network_artifact_tests.cpp`

No subagents or reviewers were spawned. No Make, GCC, g++, MinGW, MSYS2, or
collect2 command was invoked. Builds used the prescribed Windows wrapper and
tests used Visual Studio CTest in the prescribed build tree. `git diff --check`
and the staged diff check passed; unrelated untracked files were preserved.

Windows sharing, collision, and pre/post-commit interference behavior was
executed on the mandated MSVC host. The POSIX branch was source-audited but
could not be compiled or executed without violating the MSVC-only instruction;
at that review point its deliberate platform limitation was fail-closed
publication where Linux did not expose both `O_TMPFILE` and `AT_EMPTY_PATH`.
Round 6 below supersedes that capability-dependent route. No known Task 2
blocker remains.

## POSIX Manifest Portability Repair Round 6 (2026-08-24)

### Outcome

The remaining POSIX portability finding was repaired in code/test commit
`44b5fd96` (`fix: publish POSIX hydrology files without capabilities`). Both
immutable field and manifest publication now call one retained-descriptor
create-new helper using the documented unprivileged Linux procfs route. No
`AT_EMPTY_PATH` use remains in production source.

This round supersedes the preceding report's statement that publication needs
`AT_EMPTY_PATH`. Linux still needs `O_TMPFILE`, a mounted/usable
`/proc/self/fd`, and `AT_SYMLINK_FOLLOW`; missing runtime support fails closed.

### TDD RED Evidence

Tests were added before production for exact fixed-buffer proc-fd formatting,
exact-capacity success, truncation clearing/failure, negative-descriptor
rejection, and the explicitly requested source contract that field and manifest
publication call one shared helper with no `AT_EMPTY_PATH` remaining.

The captured RED command was:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests
```

MSVC compiled the tests, then failed the link with `LNK2019`/`LNK1120` for the
intentionally missing `format_hydrology_proc_fd_path` implementation.

### Shared Unprivileged Publication Protocol

- `format_hydrology_proc_fd_path` is allocation-free and `noexcept`. It writes
  `/proc/self/fd/<decimal-fd>` using a fixed prefix plus `std::to_chars`, reserves
  the terminator explicitly, rejects negative descriptors, and clears the first
  byte on truncation. Production uses a 64-byte stack buffer, which exceeds the
  maximum representation required for an `int` descriptor.
- `publish_posix_retained_fd_create_new` is the sole POSIX linking helper and is
  called by both field and manifest save. The source descriptor remains owned
  by its surrounding RAII object from validation through helper return, so it
  cannot close or be reused between formatting, link, and identity checks.
- The helper calls
  `linkat(AT_FDCWD, proc_fd_path, held_destination_fd, canonical_name,
  AT_SYMLINK_FOLLOW)`. Symlink following applies only to the fixed, internally
  generated `/proc/self/fd/<fd>` source; the destination remains a canonical
  leaf relative to the already-confined held directory handle.
- A successful link is accepted only after `fstat(source_fd)` and
  `fstatat(destination_fd, canonical_name, AT_SYMLINK_NOFOLLOW)` prove that the
  validated source, retained source, and new directory entry are the same
  regular device/inode. The held destination directory is then `fsync`ed.
- The low-level helper performs no allocation and never throws. It captures and
  restores the exact `linkat`, `fstat`, `fstatat`, or `fsync` error in `errno`;
  locally detected invalid identity uses `ESTALE`, path-format failure uses
  `EBADF` or `ENAMETOOLONG`, and `EEXIST` is returned as a typed result before
  higher-level work can overwrite it.
- On `EEXIST`, both callers retain the anonymous source descriptor and use the
  existing same-handle canonical-leaf validation: `openat` with `O_NOFOLLOW`,
  stable directory-entry identity before and after the full byte/EOF read, and
  exact expected-byte comparison. A different existing blob is never replaced.
- If procfs is absent, inaccessible, or does not resolve to the retained
  descriptor, `linkat` or the following identity check fails closed. There is no
  privileged `AT_EMPTY_PATH` fallback.

The source-contract test reads the production translation unit because the
task's MSVC-only rule prevents compiling the POSIX branch. It verifies exactly
one helper definition plus the field and manifest call sites, requires
`AT_SYMLINK_FOLLOW`, and rejects any remaining `AT_EMPTY_PATH` occurrence. The
platform-neutral formatter and identity-decision tests execute under MSVC.

### Final GREEN Evidence

The final focused persistence target built successfully and its verbose CTest
run ended `ALL PASS` in 0.38 seconds. All required targets were then rebuilt
using only:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target <target>
```

The seven focused targets and `matter_engine_viewer_objects` all succeeded. The
single final test command was:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R '^(hydrology_artifact_tests|hydrology_network_artifact_tests|hydrology_handoff_products_tests|river_runtime_tests|async_bake_tests|physx_dependency_contract_tests|physx_adapter_contract_tests)$' --output-on-failure
```

Result: **7/7 passed**, 0 failed, 7.88 seconds. No broad CPU suite was
rerun; the earlier single **37/37** CPU-suite result remains retained.

### Round 6 Files, Process, and Runtime Limitation

- `MatterEngine3/src/hydrology/hydrology_field_artifact.h`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- `MatterEngine3/tests/hydrology_network_artifact_tests.cpp`

No subagents or reviewers were spawned. No Make, GCC, g++, MinGW, MSYS2,
collect2, WSL, or other POSIX command was invoked. Builds and tests used only
the mandated Windows MSVC paths. `git diff --check` and the staged diff check
passed; unrelated untracked files were preserved.

The remaining limitation is verification, not a known production defect: the
Linux/WSL `O_TMPFILE` plus procfs `linkat` route was source-audited but not
compiled or executed because the task explicitly forbids non-MSVC validation.
Runtime success therefore depends on ordinary Linux procfs being mounted and
accessible. Failure in that environment is explicit and closed. No known Task
2 blocker remains.
