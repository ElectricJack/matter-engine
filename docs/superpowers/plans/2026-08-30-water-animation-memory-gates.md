# Water Animation Memory Gates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enforce the existing complete-animation-file caps consistently before Ready publication, cached Ready admission, and playback activation, with exact-boundary regressions.

**Architecture:** The artifact module owns public limits and allocation-free size arithmetic. Existing package validation and playback use them before decoding candidate assets. Provider timings and the continuity checker count actual complete files, independently from sidecar/build/decoded memory.

**Tech Stack:** C++17, native MSVC v143/CMake/Ninja, filesystem-based immutable artifacts, Python 3 unittest, existing PhysX-enabled build.

**Spec:** [Water-animation memory admission](../specs/2026-08-30-water-animation-memory-gates-design.md).

**Status:** Prepared while character integration runs; not executed. Do not run this plan concurrently with another source/build writer. It closes only the roadmap's memory item, not overall Task 12 or visual/performance acceptance.

## Global Constraints

- Each complete `.mhwa` file, including its 32-byte outer header, is at most 1,073,741,824 bytes. All section and handoff animation files admitted for one network total at most 734,003,200 bytes.
- Boundary sidecars, decoded heap, and peak-build residency remain separate metrics; do not add them to the runtime complete-file sum or relabel that sum as peak memory.
- C++17 only. Use pointer/count interfaces, not `std::span`; introduce no file-format/cache-key/DSL changes.
- Preserve accepted water geometry, flow interpolation, shaders, section ownership, and zero animated-water RT participation. Do not restore rejected waterfall experiments.
- Record dirty/staged files before each task. Preserve unrelated changes, including current dirty hydrology/provider/tests. Use apply_patch and surgical staging; no reset/clean/checkout/stash or broad add.
- Every build/preflight invocation includes `-EnablePhysx -PhysxRoot 'D:/PhysX-5.6.1'`. Use `MatterEditor/build/cmake/windows-msvc/relwithdebinfo`, RelWithDebInfo, native MSVC; never build while an editor using the binary is running.
- No new bake, SDK download, global environment change, or source-cache mutation is needed for these gates. Package fixtures own unique temporary directories; validate cleanup targets, never remove an existing user cache.
- Exact arithmetic tests allocate only small arrays. Integration over-budget fixtures use sparse logical lengths and must be rejected before decoder allocation.

## Verification setup

Resolve native tools through the supported wrapper, with host escalation if the sandbox identity cannot see the user's Python launcher:

```powershell
$memoryToolchain = (& ./tools/build-windows.ps1 -EnablePhysx -PhysxRoot 'D:/PhysX-5.6.1' -PreflightOnly | Out-String) | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'MSVC preflight failed' }
$memoryCTest = Join-Path (Split-Path $memoryToolchain.CMake) 'ctest.exe'
$memoryBuild = 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo'
```

## Task 1: Centralize exact size limits and apply them to artifacts/playback

**Files:** Modify `MatterEngine3/src/hydrology/water_mesh_animation_artifact.h/.cpp`, `MatterEngine3/src/render/water_mesh_animation_playback.cpp`, `MatterEngine3/src/matter_engine.cpp`; test `MatterEngine3/tests/water_mesh_animation_artifact_tests.cpp` and `water_mesh_animation_playback_tests.cpp`. Create the shared header-only fixture helper `MatterEngine3/tests/water_animation_memory_test_utils.h` for Task1/2 sparse-file tests; no production manifest addition is needed.

**Consumes:** Existing binary envelope, serializer, private checked-add helpers, playback candidate transaction and caller budget.

**Produces:** These exact interfaces in namespace `hydrology`:

```cpp
inline constexpr std::uint64_t kWaterAnimationArtifactFileLimitBytes =
    1073741824ull;
inline constexpr std::uint64_t kWaterAnimationArtifactHeaderBytes = 32ull;
inline constexpr std::uint64_t kWaterAnimationNetworkBudgetBytes = 734003200ull;

bool water_animation_artifact_file_bytes(
    std::uint64_t serialized_payload_bytes, std::uint64_t& file_bytes,
    gpu_meshing::Error& error);
bool water_animation_network_file_bytes(
    const std::uint64_t* artifact_file_bytes, std::size_t count,
    std::uint64_t& total, gpu_meshing::Error& error);
```

The test-only header owns fixture creation and resizing together; no API accepts an arbitrary existing root:

```cpp
namespace water_animation_test {
class SparseFixture {
public:
    static std::unique_ptr<SparseFixture> create(std::string& error);
    const std::filesystem::path& root() const noexcept;
    bool set_sparse_size(const std::filesystem::path& relative_path,
                         std::uint64_t bytes, std::string& error);
    bool cleanup(std::string& error);
    SparseFixture(const SparseFixture&) = delete;
    SparseFixture& operator=(const SparseFixture&) = delete;
private:
    SparseFixture() = default;
};
}
```

`create` exclusively creates a new `matter-water-memory-<unique>` directory beneath the OS temporary directory and records its canonical path/identity. Construction cannot adopt a caller's directory. Callers write their small fixtures beneath `root()`, then pass only relative paths to `set_sparse_size`. Reject rooted/traversal paths, canonical targets outside that root, any symlink/reparse component, nonregular targets, and files whose hard-link count is not one before mutation. Check the owned root identity has not changed. On Windows use a non-following existing-file handle, verify its attributes/link count and containment before `FSCTL_SET_SPARSE` and resizing that same handle; POSIX likewise validate the opened regular-file descriptor before `ftruncate`. Do not reopen by a merely checked path to mutate it. Report errors instead of skipping. `cleanup` revalidates the exact owned root identity/containment, never follows link targets, and reports failure rather than deleting a replacement directory. There is no destructor-driven broad cleanup. Tests explicitly check cleanup's result; both suites reuse this one helper.

Test the guard itself using two independently owned small fixtures: reject an absolute path, parent traversal, a hard-linked target, and a symlink/reparse target where the host supports creating it; prove the other fixture's bytes/length stay unchanged. Record any unavailable link-creation case, never weaken the production guard. No test references the project's shared cache.

- [ ] **1.1 Add declarations and inert definitions, then exact-boundary tests.** Keep caller outputs sentinel-valued on failure. Test the following concrete matrix; a failed intended boundary assertion, not a linker error, establishes RED:

```cpp
std::uint64_t output = 777u;
gpu_meshing::Error error{};
CHECK(water_animation_artifact_file_bytes(1073741792ull, output, error) &&
      output == 1073741824ull, "exact complete-file cap accepted");
output = 777u;
CHECK(!water_animation_artifact_file_bytes(1073741793ull, output, error) &&
      output == 777u, "one-over preserves output");
CHECK(!water_animation_artifact_file_bytes(UINT64_MAX, output, error) &&
      output == 777u, "header addition overflow rejected");
const std::uint64_t exact[]{367001600ull, 367001600ull};
CHECK(water_animation_network_file_bytes(exact, 2u, output, error) &&
      output == 734003200ull, "exact network cap accepted");
const std::uint64_t over[]{367001600ull, 367001601ull};
output = 777u;
CHECK(!water_animation_network_file_bytes(over, 2u, output, error) &&
      output == 777u, "aggregate one-over preserves output");
```

Also test empty pointer/count0 -> total0 success; null pointer/nonzero count -> InvalidInput; individual complete size0/31 -> InvalidInput; size over 1GiB, UINT64_MAX, and overflowing pairs -> rejection without output mutation. Test a small real serialized artifact: `file.size() == serialized_payload_size + 32`, with metadata/directory/identity included; changing identity length changes complete size correspondingly. Do not assume raw `frame_payload.size()+32` is complete accounting.

- [ ] **1.2 Run RED.** Build each target with the wrapper and run the two exact CTest names:

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -PhysxRoot 'D:/PhysX-5.6.1' -Target water_mesh_animation_artifact_tests
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -PhysxRoot 'D:/PhysX-5.6.1' -Target water_mesh_animation_playback_tests
& $memoryCTest --test-dir $memoryBuild -C RelWithDebInfo -R '^(water_mesh_animation_artifact_tests|water_mesh_animation_playback_tests)$' --output-on-failure --no-tests=error
```

- [ ] **1.3 Implement helpers and replace private cap literals.** Compute into locals, check addition before performing it, reject individual invalid sizes and aggregate limits, then assign output only on success. Error codes: InvalidInput for invalid pointers/truncated sizes, Overflow for attempted arithmetic overflow, LimitExceeded for budget violations. No allocation is needed inside either helper. Serializer/read paths keep their existing envelope and validations; use the public constants and checked envelope count at serialization. Do not change preexisting serializer output-reset semantics incidentally.

- [ ] **1.4 Apply the hard network cap to playback preflight.** Retain current path/reference checks. Collect actual complete file sizes for section and handoff references before loading candidates, fail stat/truncated/over-limit inputs, and call the shared network helper. Reject totals over `min(cpu_budget_bytes, kWaterAnimationNetworkBudgetBytes)` with CpuBudgetExceeded. Preserve the current MissingArtifact/CorruptArtifact distinctions, active playback object, frame spans, and retained/peak memory calculations. Replace the engine's literal caller budget with the public constant.

- [ ] **1.5 Test and commit.** Existing valid playback fixtures pass; lower caller budget rejection preserves prior playback counters/assets/selection. To prove an oversized caller budget cannot bypass the hard cap, populate a newly created `SparseFixture` with the existing valid fixture data, preserve a prior playback object, and use its root-bound method to give two candidate files logical lengths 367001600 and 367001601 bytes. Call activation with `cpu_budget_bytes=UINT64_MAX`. Require CpuBudgetExceeded and unchanged prior playback. Before running this new RED case against the old decoder, extract the new size-preflight seam as an inert non-accepting CorruptArtifact result; never let old code allocate the sparse payloads. Then implement the real preflight and prove the budget-specific GREEN result. Exact cap arithmetic remains tested without large buffers. Re-run both focused tests, inspect the diff, stage only task-owned hunks, and commit `fix(water): centralize complete animation memory limits`.

## Task 2: Enforce shared Ready package admission and truthful byte diagnostics

**Files:** Modify `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp` and `MatterEngine3/src/provider/local_provider.cpp`; test `MatterEngine3/tests/hydrology_network_artifact_tests.cpp` and relevant existing `physx_adapter_contract_tests.cpp` cases. Do not introduce a generic filesystem abstraction.

**Consumes:** Task1 public size helpers/constants and shared sparse-file test helper. Existing `validate_animation_package` is called by both `save_network_artifact_atomic_impl` and `load_network_artifact_validated_impl`; preserve those transactions and their prior path/field validation.

**Produces:** Ready save/load cannot admit an over-budget animation network, and provider `animationBytes` / handoff `animationFileBytes` are actual complete-file counts or explicit product failure.

- [ ] **2.1 Add a real over-budget package RED test.** Create a new `SparseFixture`; populate only its owned root using `animated_fixture_manifest`, `animation_fixture`, and `save_field_pair`. Save a valid Ready manifest and snapshot its bytes plus loaded object. Use the fixture's guarded relative-path `set_sparse_size` method to give the first two animation files logical sizes of 367001600 and 367001601 bytes. Do not resize directly or adopt files from another cache. Never allocate a vector of those sizes. A fixture-creation/ownership error fails the test explicitly; explicitly check the fixture's scoped cleanup result afterward.

Call Ready save at the same path and validated Ready load into the prior object. Require `ErrorCode::LimitExceeded` from both, identical original manifest-file bytes, and unchanged prior loaded object. That specific error proves aggregate rejection happened before decoding intentionally resized payloads. Restore the small artifacts and verify ordinary save/load success. Also cover missing/stat-failed file rejection and an animation-disabled legacy Ready manifest. Only the temporary test fixture is removed.

- [ ] **2.2 Run RED and inspect the failure.**

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -PhysxRoot 'D:/PhysX-5.6.1' -Target hydrology_network_artifact_tests
& $memoryCTest --test-dir $memoryBuild -C RelWithDebInfo -R '^hydrology_network_artifact_tests$' --output-on-failure --no-tests=error
```

The pre-change implementation may attempt payload validation; observe failure of the expected aggregate LimitExceeded behavior. Do not allow the test to allocate/decode the sparse payloads during RED: first introduce the private size-preflight seam as an inert rejection with a non-LimitExceeded error, keeping the production gate non-accepting until implemented. This tests the missing behavior safely rather than feeding large files to the old decoder.

- [ ] **2.3 Add a two-pass package check.** For a Ready animated manifest, collect file sizes for all section/handoff references under the existing validated cache root, then call `water_animation_network_file_bytes`. Missing/stat errors fail with an artifact error. Return before decoding any asset if admission fails. Only then run existing identity/digest/frame checks. Preserve animation-disabled and non-Ready diagnostic behavior. Existing save/load callers enforce publication/cache policy; do not duplicate the cap in a provider-only path.

- [ ] **2.4 Remove diagnostic fallbacks.** In provider section and cached handoff animation paths, a failed file-size query returns the existing explicit ProductFailure/Invalid status path. Do not substitute raw frame bytes or leave a zero diagnostic and continue. Reuse actual counts already read at successful paths; keep cold/hit values consistent and sidecar/build memory separate. Add focused provider contract coverage through its existing fixture seam; no production test-only branch.

- [ ] **2.5 Run GREEN and commit.**

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -PhysxRoot 'D:/PhysX-5.6.1' -Target physx_adapter_contract_tests
& $memoryCTest --test-dir $memoryBuild -C RelWithDebInfo -R '^(hydrology_network_artifact_tests|physx_adapter_contract_tests|water_mesh_animation_artifact_tests|water_mesh_animation_playback_tests)$' --output-on-failure --no-tests=error
```

Review that old cached manifests and new publication share the budget gate, all failures preserve prior Ready/playback state, and no caller trusts timing fallback counts. Stage only reviewed task hunks and commit `fix(water): gate Ready packages on total animation bytes`.

## Task 3: Check reported memory strictly and record the bounded acceptance

**Files:** Modify `MatterEngine3/tools/water_mesh_continuity_acceptance.py`, `MatterEngine3/tools/tests/test_water_mesh_continuity_acceptance.py`; create `docs/findings/water-animation-memory-gates-2026-08-30.md`; update only the memory item in `ROADMAP.md` and execution note in this plan after evidence exists.

**Consumes:** Actual per-section `animationBytes` and per-handoff `animationFileBytes` in existing timing JSON; Task1/2 native tests. Existing Stage1 comparator structure/cameras/geometry predicates remain intact.

**Produces:** `_validate_animation_memory(trace, label, failures)` returns the checked complete-file total or None and appends strict errors. `compare_stage1` includes cold/cache/edit memory totals in its summary and rejects unequal cold/cache totals for the same assets. No new runner or visual acceptance stage is added.

- [ ] **3.1 Add failing Python cases.** Extend valid synthetic section rows with integer `animationBytes`; retain integer handoff `animationFileBytes`. Directly test totals exactly 734003200, one-over, an individual 1073741824+1, missing fields, bool, float, negative, zero, malformed rows, and unequal cold/cache totals. Check sidecar/peak changes alone do not alter the runtime sum. Use small integers/arrays only.

```python
trace = {"sections": [{"id": "upper", "animationBytes": 367001600}],
         "handoffs": {"pool-one": {"animationFileBytes": 367001600}}}
failures = []
assert _validate_animation_memory(trace, "cold", failures) == 734003200
assert not failures
trace["handoffs"]["pool-one"]["animationFileBytes"] += 1
assert _validate_animation_memory(trace, "cold", failures) is None
assert failures
```

- [ ] **3.2 Run RED, implement strict validation, and run GREEN.** Accept `int` but not `bool`, require positive complete-file sizes at least32, enforce each file cap, then sum/check network cap. Preserve old Stage1 predicates and make new memory failures explicit rather than silently ignoring absent data.

```powershell
& $memoryToolchain.Python -m unittest discover -s MatterEngine3/tools/tests -p test_water_mesh_continuity_acceptance.py
```

- [ ] **3.3 Run the four native suites from Task2 plus Python suite, then build the editor.** Use the same pinned PhysX-enabled wrapper. A test/build failure is not acceptance. No full river bake is required to verify these code-level admission gates; do not claim newly captured visuals, cold/cache bake behavior, or performance from unit fixtures.

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -PhysxRoot 'D:/PhysX-5.6.1' -Target matter_editor
git diff --check
```

- [ ] **3.4 Record and commit the bounded result.** Findings list actual commits, dirty-file scope, exact commands/exits, helper boundary table, publication/cache/playback transactional evidence, provider byte semantics, comparator cases, and remaining visual/performance gates. Mark only ROADMAP's memory enforcement item complete after independent review; leave full Task12/cold-cache/shadow performance acceptance open. Commit `test(water): verify animation memory admission gates` with surgical staging.

## Final review

- [ ] Size arithmetic has one authority; no raw-frame-byte substitution or larger caller budget bypass.
- [ ] New Ready, cached Ready, and playback failure preserve prior state.
- [ ] C++17, immutable formats, shader/geometry/physics behavior, and unrelated dirty changes are preserved.
- [ ] Sparse fixture failures are explicit, targeted cleanup is safe, and no oversized decoder allocation occurs even during RED.
- [ ] Native/Python tests and final PhysX-enabled build have actual recorded exits.
- [ ] Documentation closes only memory enforcement; it does not claim overall water appearance, new bake screenshots, or performance acceptance.
