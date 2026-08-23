# PhysX PBD Fluid-Bake Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate NVIDIA PhysX 5.6.1 GPU PBD fluid simulation into the in-process Matter hydrology bake and accept one authored, dam-terminated ravine section that produces persistent visual, query, and gameplay water products.

**Architecture:** The official PhysX checkout and build stay outside this repository and are selected only by `MATTER_PHYSX_ROOT`; ordinary Matter builds remain PhysX-free. A private MSVC adapter owns PhysX/CUDA objects and implements a Matter-owned backend interface, while a pure orchestration layer owns validation, emitters, sensor completion, cancellation, product conversion, and immutable artifact publication. The accepted host particle snapshot is passed to the existing Vulkan visual mesher and coarse CPU mesher; PhysX is never used for isosurface extraction and never runs during gameplay.

**Tech Stack:** C++17, MSVC 19.44.35211, CMake/Ninja, NVIDIA PhysX SDK 5.6.1, CUDA Toolkit 12.8 (NVCC 12.8.61), Vulkan 1.4, MatterSurfaceLib, PowerShell dependency/build tests, CTest.

**Spec:** `docs/superpowers/specs/2026-08-22-physx-fluid-bake-integration-design.md`

## Global Constraints

- Pin `https://github.com/NVIDIA-Omniverse/PhysX.git` tag `107.3-physx-5.6.1` at commit `5ca9f472105a90d70d957c243cb0ef36fe251a9f`; never clone or update it implicitly.
- Build the Matter adapter with CUDA Toolkit `12.8` (NVCC `12.8.61`), MSVC compiler `19.44.35211`, v143 toolset `14.44.35207`, and Matter's pinned Windows SDK `10.0.26100.0`. The unmodified upstream PBF control selected Windows SDK `10.0.28000.0`; that measurement is recorded as control provenance and does not authorize changing Matter's toolchain pin.
- Keep the PhysX source checkout external and select it with `MATTER_PHYSX_ROOT`; because NVIDIA's 5.6.1 generator does not quote all paths, the control-build root must contain no whitespace.
- PhysX remains opt-in (`MATTER_ENABLE_PHYSX=OFF` by default); Linux, WSL-native, headless, hydrology authoring, and CPU meshing builds acquire no PhysX or CUDA dependency.
- The solver and adapter run in `editor.exe`; no bridge DLL, solver executable, temporary worker process, GenCase, or command-line handoff is permitted.
- PhysX owns all PBD mathematics. Matter code may marshal geometry, emitters, sensors, particles, settings, and products but may not implement or modify a fluid constraint solver.
- PhysX isosurface APIs are never created or called. Matter's GPU visual mesher and CPU query mesher remain the only water surface extractors.
- The first accepted scope is one upstream section with one main inlet, an explicit virtual dam, and a fill sensor. Interfaces preserve multiple emitters, but tributary and sequential-section acceptance are deferred.
- Section bounds and dry collars are validation/culling regions, never collision walls. Only terrain, boulders, optional inlet backing geometry, and the authored virtual dam contain water.
- Runtime gameplay consumes baked artifacts only and does not load PhysX.

---

### Task 1: Pin and Reproduce the Official PBF Control

**Files:**
- Create: `tools/deps/physx.lock.json`
- Create: `tools/physx/MatterPhysxDependency.psm1`
- Create: `tools/physx/build-physx-control.ps1`
- Create: `tools/tests/physx_dependency_contract_tests.ps1`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `docs/superpowers/specs/2026-08-22-physx-fluid-bake-integration-design.md`

**Interfaces:**
- Consumes: `MATTER_PHYSX_ROOT`, the external git checkout, Visual Studio's bundled CMake, CUDA 12.8, and the checked-in JSON lock.
- Produces: `Resolve-MatterPhysxDependency -RepositoryRoot <path> -PhysxRoot <path> -CudaRoot <path>` returning a validated object with `RepositoryRoot`, `SdkRoot`, `Commit`, `CudaVersion`, `BinaryDirectory`, and `ControlExecutable`; `build-physx-control.ps1 -Mode Validate|Generate|Build|Run` as the reproducible P0 entry point.

- [x] **Step 1: Write the failing dependency-contract test**

Create a temporary git checkout containing the locked version header and exercise the real module. The test must prove that the validator accepts the exact commit/version, rejects a wrong commit, rejects CUDA other than 12.8.61, rejects whitespace in the external root for `Generate`, and never downloads anything in `Validate` mode.

```powershell
$resolved = Resolve-MatterPhysxDependency `
    -RepositoryRoot $repositoryRoot `
    -PhysxRoot $fakeCheckout `
    -CudaRoot $fakeCuda
Assert-Equal $resolved.Commit '5ca9f472105a90d70d957c243cb0ef36fe251a9f'
Assert-Equal $resolved.SdkVersion '5.6.1'
Assert-Throws { Resolve-MatterPhysxDependency @wrongCommit }
Assert-Throws { Resolve-MatterPhysxDependency @wrongCuda }
```

- [x] **Step 2: Run the contract test and verify RED**

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/tests/physx_dependency_contract_tests.ps1
```

Expected: FAIL because `tools/physx/MatterPhysxDependency.psm1` and `Resolve-MatterPhysxDependency` do not exist.

- [x] **Step 3: Add the exact dependency lock and validator**

The lock must contain literal, reviewable identity rather than machine paths:

```json
{
  "schema": 1,
  "url": "https://github.com/NVIDIA-Omniverse/PhysX.git",
  "tag": "107.3-physx-5.6.1",
  "commit": "5ca9f472105a90d70d957c243cb0ef36fe251a9f",
  "sdkVersion": "5.6.1",
  "physicsVersionHex": "0x05060100",
  "cudaToolkitVersion": "12.8",
  "cudaNvccVersion": "12.8.61",
  "windowsPreset": "vc17win64",
  "binaryAbi": "win.x86_64.vc143.mt",
  "license": "LICENSE.md"
}
```

The module validates the JSON schema, exact detached-HEAD commit, `PxPhysicsVersion.h`, `nvcc --version`, required preset/script/license files, and that the root is external to the Matter repository. It returns stable paths but performs no clone, fetch, pull, checkout, or package download.

- [x] **Step 4: Implement the explicit control runner**

`Validate` calls only the module. `Generate` prepends Visual Studio's bundled CMake, converts the CUDA root to a no-space short path, invokes NVIDIA's unchanged `generate_projects.bat vc17win64`, and requires `PhysXSDK.sln`. `Build` invokes CMake's build mode for `SnippetPBF`/`release` and requires the executable plus `PhysXGpu_64.dll`. `Run` starts the official interactive control, requires it to remain alive for a bounded observation window, records `nvidia-smi` GPU identity/utilization, and closes only the process it created.

```powershell
switch ($Mode) {
    'Validate' { $dependency }
    'Generate' { Invoke-PhysxGenerate $dependency }
    'Build'    { Invoke-PhysxBuild $dependency -Target SnippetPBF }
    'Run'      { Invoke-PhysxControl $dependency -Seconds $ObservationSeconds }
}
```

- [x] **Step 5: Register and pass the offline CTest contract**

Add `physx_dependency_contract_tests` as a `compiler-policy` test. It must not depend on `MATTER_PHYSX_ROOT`, CUDA, a GPU, or network access.

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_dependency_contract_tests
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R physx_dependency_contract_tests --output-on-failure
```

Expected: PASS.

- [x] **Step 6: Record the measured P0 control and commit**

Change the design status from draft to approved/in progress and record the measured control: RTX 4090, driver 610.74, CUDA compiler 12.8.61, 900,000 source particles, 59–61 rendered FPS, about 80% GPU utilization, about 6,158 MiB used, and screenshot `MatterEditor/build/baselines/msvc/physx-p0/snippet-pbf.png`.

```powershell
git add tools/deps/physx.lock.json tools/physx/MatterPhysxDependency.psm1 tools/physx/build-physx-control.ps1 tools/tests/physx_dependency_contract_tests.ps1 cmake/MatterEngine.cmake docs/superpowers/specs/2026-08-22-physx-fluid-bake-integration-design.md docs/superpowers/plans/2026-08-23-physx-fluid-bake-integration.md
git commit -m "build: pin the PhysX PBF control"
```

### Task 2: Define Matter-Owned Fluid Adapter Contracts

**Files:**
- Create: `MatterEngine3/src/hydrology/physx_fluid_types.h`
- Create: `MatterEngine3/src/hydrology/physx_runtime.h`
- Create: `MatterEngine3/src/hydrology/physx_fluid_bake.h`
- Create: `MatterEngine3/src/hydrology/physx_fluid_bake.cpp`
- Create: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: `matter::RiverNetworkDefinition`, `hydrology::RiverGeometry`, world-space indexed collision triangles, and callback-based cancellation/progress.
- Produces: `IFluidBakeBackend::probe()`/`run()`, `PhysxFluidBake::run()`, stable `FluidBakeCode`, `FluidParticle`, `FillSensorResult`, `FluidBakeStats`, and `FluidBakeOutput` types with no PhysX/CUDA headers.

- [x] **Step 1: Write failing fake-backend contract tests**

Cover invalid indices/non-finite values before backend invocation, unavailable-backend status translation, callback progress monotonicity, cancellation propagation, stable particle-id sorting, multiple emitter preservation, and rejection of backend output containing escaped or non-finite particles.

```cpp
RecordingBackend backend;
FluidBakeOutput output{};
FluidBakeError error{};
CHECK(!PhysxFluidBake::run(input_with_nan, backend, {}, output, error));
CHECK(backend.run_calls == 0);
CHECK(error.code == FluidBakeCode::InvalidInput);
```

- [x] **Step 2: Run and verify RED**

Run the new target; expect compilation to fail because the Matter-owned contracts do not exist.

- [x] **Step 3: Implement the minimal contracts and pure validation/orchestration**

Use `std::function<bool()> cancelled` and `std::function<void(const FluidBakeProgress&)> progress` so the adapter does not depend on `matter_async::CancelToken`. `IFluidBakeBackend` is the only injected seam; tests use a real in-memory fake implementation and assert orchestration results rather than mock call text.

- [x] **Step 4: Preserve compiler neutrality and pass MSVC plus WSL GCC tests**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R physx_adapter_contract_tests --output-on-failure
```

```bash
make -C MatterEngine3/tests run-physx-adapter-contract
```

Expected: both PASS with `MATTER_ENABLE_PHYSX` absent.

- [x] **Step 5: Commit**

```powershell
git add MatterEngine3/src/hydrology/physx_fluid_types.h MatterEngine3/src/hydrology/physx_runtime.h MatterEngine3/src/hydrology/physx_fluid_bake.h MatterEngine3/src/hydrology/physx_fluid_bake.cpp MatterEngine3/tests/physx_adapter_contract_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: define PhysX fluid bake contracts"
```

### Task 3: Build the Private Native PhysX Runtime

**Files:**
- Create: `integrations/physx_adapter/CMakeLists.txt`
- Create: `integrations/physx_adapter/physx_runtime.cpp`
- Create: `integrations/physx_adapter/physx_raii.h`
- Create: `cmake/MatterPhysx.cmake`
- Create: `MatterEngine3/tests/physx_fluid_integration_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `tools/build-windows.ps1`

**Interfaces:**
- Consumes: the Task 1 validated external checkout and Task 2 `IFluidBakeBackend` contract.
- Produces: `PhysxRuntime` with a private implementation, lazy PhysX/CUDA creation, exact header/runtime version checks, CUDA device identity, error-callback translation, and idempotent destruction.

- [x] **Step 1: Write a GPU-tagged failing runtime probe test**

The test is registered only when `MATTER_ENABLE_PHYSX=ON`. It asserts exact SDK version `5.6.1`, a valid CUDA context, RTX device identity, create/destroy loops, no exception escape, and stable `BackendUnavailable` when the GPU module path is deliberately removed.

- [x] **Step 2: Verify RED with the opt-in configuration**

```powershell
$env:MATTER_PHYSX_ROOT='D:\PhysX-5.6.1'
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_fluid_integration_tests
```

Expected: configuration or link failure because `MatterPhysx.cmake` and the adapter do not exist.

- [x] **Step 3: Add the opt-in external build and private adapter**

`MatterPhysx.cmake` requires the lock validator before enabling targets, builds official static core/foundation/common/cooking/extensions libraries into the active CMake build tree's `physx/` directory, keeps `PhysXGpu_64.dll` as the staged GPU runtime, and exposes one internal target `matter_physx_adapter`. No PhysX include directory is `PUBLIC` or `INTERFACE` on an engine target.

Use release-aware ownership:

```cpp
template <class T> struct PxRelease {
    void operator()(T* value) const noexcept { if (value) value->release(); }
};
template <class T> using PxOwner = std::unique_ptr<T, PxRelease<T>>;
```

- [x] **Step 4: Implement lazy probe/lifetime/error translation**

`probe` creates only the lightweight foundation, physics, and CUDA context
needed to validate the actual GPU runtime; cooking, scene, and particle buffers
remain per-bake `run` resources. Convert PhysX callback severities and caught
exceptions to stable Matter failures; never throw across `IFluidBakeBackend`.

- [x] **Step 5: Pass focused GPU and default-off build gates**

Run the opt-in probe, then configure/build `matter_editor` without `MATTER_ENABLE_PHYSX` or `MATTER_PHYSX_ROOT` and confirm the default graph is unchanged.

Result (2026-08-23): Matter builds the official static core/foundation/common/
cooking/extensions closure under the CMake build tree and stages only the
official `PhysXGpu_64.dll`. The native probe passed on the RTX 4090 through
three create/destroy cycles, success-then-missing GPU-path rejection, injected
exception containment, and exact 5.6.1 identity checks. `dumpbin /dependents`
reported only `KERNEL32.dll` for the probe executable. The default editor,
Windows/WSL wrapper contracts, and WSL GCC adapter contract passed with the
PhysX target absent from the default build graph.

- [x] **Step 6: Commit**

```powershell
git add integrations/physx_adapter cmake/MatterPhysx.cmake CMakeLists.txt cmake/MatterEngine.cmake tools/build-windows.ps1 tools/physx/build-physx-control.ps1 tools/physx/configure-physx-static.ps1 MatterEngine3/src/hydrology/physx_runtime.h MatterEngine3/tests/physx_fluid_integration_tests.cpp docs/superpowers/plans/2026-08-23-physx-fluid-bake-integration.md
git commit -m "feat: embed the PhysX GPU runtime"
```

### Task 4: Cook Matter Collision Geometry and Prove the Chute

**Files:**
- Create: `MatterEngine3/src/hydrology/physx_collision_input.h`
- Create: `MatterEngine3/src/hydrology/physx_collision_input.cpp`
- Modify: `integrations/physx_adapter/physx_runtime.cpp`
- Modify: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- Modify: `MatterEngine3/tests/physx_fluid_integration_tests.cpp`

**Interfaces:**
- Consumes: world-space terrain triangle mesh, boulder geometry, virtual dam plane/mesh, and dry-collar AABB.
- Produces: validated indexed `FluidCollisionMesh`; cooked PhysX triangle mesh/static actors; collision probe results and contact/escape counters.

- [x] **Step 1: Add failing pure collision-input tests**

Test winding-preserving deduplication, invalid/out-of-range indices, degenerate triangles, non-finite transforms, bounds derivation with dry margin, explicit dam tagging, and absence of generated AABB wall triangles.

- [x] **Step 2: Implement minimal collision input assembly and pass CPU tests**

The output contains only authored terrain/boulders/backing/dam surfaces. A face-count assertion proves no six-face domain box is synthesized.

- [x] **Step 3: Add failing GPU collision fixtures**

Run one triangle, one box, one sloped ramp, then a short Matter chute. Assert probe particles settle within one collision voxel, the water center of mass moves downhill, all particles remain finite, and the dry collar produces escape failure instead of reflection.

- [x] **Step 4: Implement PhysX cooking and static actor installation**

Cook with exact scale/unit settings, reject warning/error callbacks as categorized results, and retain cooked resources until the scene is destroyed. Do not change PBD constraints or timestep equations.

- [x] **Step 5: Pass P1 collision and P2 chute tests and commit**

```powershell
git add MatterEngine3/src/hydrology/physx_collision_input.h MatterEngine3/src/hydrology/physx_collision_input.cpp integrations/physx_adapter/physx_runtime.cpp MatterEngine3/tests/physx_adapter_contract_tests.cpp MatterEngine3/tests/physx_fluid_integration_tests.cpp
git commit -m "feat: collide PhysX water with Matter terrain"
```

### Task 5: Implement Emitters, Fixed-Step Batches, and Fill Completion

**Files:**
- Create: `MatterEngine3/src/hydrology/fluid_emission.h`
- Create: `MatterEngine3/src/hydrology/fluid_emission.cpp`
- Create: `MatterEngine3/src/hydrology/fill_sensor.h`
- Create: `MatterEngine3/src/hydrology/fill_sensor.cpp`
- Modify: `MatterEngine3/src/hydrology/physx_fluid_bake.cpp`
- Modify: `integrations/physx_adapter/physx_runtime.cpp`
- Modify: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- Modify: `MatterEngine3/tests/physx_fluid_integration_tests.cpp`

**Interfaces:**
- Consumes: multiple authored emitters, fixed timestep/batch/capacity settings, particle positions, and a grid-based sensor volume.
- Produces: deterministic per-step activation counts with fractional carry, stable particle ids, sensor wet fractions/windows, completion step, and terminal `Ready|Cancelled|CapacityExceeded|Escaped|NonFinite|SensorNotReached|DeviceLost` results.

- [x] **Step 1: Write failing emission and CPU sensor-reference tests**

Use hand-derived sequences such as a 2.5-particles-per-step emitter yielding `2,3,2,3`; assert independent carry per emitter, start/stop boundaries, stable ids, broad-sensor rejection of a narrow jet, consecutive-step reset, and exact stable-window completion.

- [x] **Step 2: Implement pure emission/sensor references and pass CPU tests**

No forces, pressures, neighbor search, or integration math may appear in these files.

- [x] **Step 3: Add failing adapter batch-loop tests**

Assert activation precedes each simulate step, cancellation is sampled between batches, progress is monotonic, capacity is checked before writes, and only bounded sensor counts return per batch.

- [x] **Step 4: Implement PhysX PBD setup from `SnippetPBF` formulas**

Derive rest/contact/fluid offsets and particle mass exactly from particle spacing and density, create fluid/self-collide phase, upload initial/activated particles, step `simulate`/`fetchResults`, run the occupancy reduction, and copy final position/velocity/id once after completion.

- [x] **Step 5: Pass control parity and failure-injection tests and commit**

Result (2026-08-23): the in-process adapter now creates the official PhysX
5.6.1 GPU PBD system/material/phase, activates deterministic multi-emitter
particles before fixed steps, checks occupancy on device, and copies only a
bounded count batch until one accepted final particle snapshot. The fixed
SnippetPBF-parameter control, authored-floor collision, cancellation,
pre-write capacity, hardware-error/DeviceLost, CUDA-OOM/CapacityExceeded,
sensor telemetry, default-off MSVC graph, and WSL/GCC contracts pass on the
RTX 4090. The native test executable imports only `KERNEL32.dll`.

```powershell
git add MatterEngine3/src/hydrology/fluid_emission.h MatterEngine3/src/hydrology/fluid_emission.cpp MatterEngine3/src/hydrology/fill_sensor.h MatterEngine3/src/hydrology/fill_sensor.cpp MatterEngine3/src/hydrology/physx_fluid_bake.cpp integrations/physx_adapter/physx_runtime.cpp MatterEngine3/tests/physx_adapter_contract_tests.cpp MatterEngine3/tests/physx_fluid_integration_tests.cpp
git commit -m "feat: fill river sections with PhysX PBD water"
```

### Task 6: Convert and Persist Final Water Products

**Files:**
- Create: `MatterEngine3/src/hydrology/fluid_gameplay_field.h`
- Create: `MatterEngine3/src/hydrology/fluid_gameplay_field.cpp`
- Modify: `MatterEngine3/src/hydrology/water_visual_products.*`
- Modify: `MatterEngine3/src/hydrology/hydrology_artifact.*`
- Modify: `MatterEngine3/src/hydrology/physx_fluid_bake.cpp`
- Modify: `MatterEngine3/tests/hydrology_artifact_tests.cpp`
- Modify: `MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp`
- Modify: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`

**Interfaces:**
- Consumes: accepted, stable-id-sorted final particles and terrain-height sampling.
- Produces: visual `ParticleJob`, coarse CPU mesh, section-local height/depth/wet/velocity field, PhysX provenance/stats/sensor metadata, particle snapshot, semantic key, and immutable `MHYDMSH2` artifact.

- [x] **Step 1: Write failing product and artifact migration tests**

Assert radius/velocity/id conversion, empty samples invalid rather than zero-current, volume-weighted velocity literals, independent product keys, exact semantic invalidation inputs, V1 rejection or explicit migration, V2 round-trip, corruption rejection, and no artifact publication on failed/cancelled simulation.

- [x] **Step 2: Implement gameplay-field conversion and product keys**

Keep visual-only settings out of coarse/gameplay keys. Include PhysX/adapter/PBD/collision/network/dam/sensor versions in the hydrology semantic key.

- [x] **Step 3: Extend the artifact atomically**

Use bounded lengths and finite validation for every new payload, preserve write-temp/reopen/atomic-replace, and record acceptance explicitly. Debug particles may be stripped only after all accepted products exist.

- [x] **Step 4: Connect the existing GPU and CPU meshers**

Post the final host snapshot through `vk_particle_visual_bake`, use material 4, build the coarse CPU mesh separately, and fail the hydrology result if either required accepted product fails. Never call a PhysX isosurface symbol.

- [x] **Step 5: Pass artifact/mesher/orchestration tests and commit**

```powershell
git add MatterEngine3/src/hydrology MatterEngine3/tests/hydrology_artifact_tests.cpp MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp MatterEngine3/tests/physx_adapter_contract_tests.cpp
git commit -m "feat: persist PhysX river bake products"
```

### Task 7: Add Imperative DSL Settings and Editor Bake Orchestration

**Files:**
- Modify: `MatterEngine3/include/matter/hydrology.h`
- Modify: `MatterEngine3/include/matter/river_network.h`
- Modify: `MatterEngine3/src/dsl_bindings.cpp`
- Modify: `MatterEngine3/src/script/world_definition_loader.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.*`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `projects/world_demo/scenes/RiverHydrology/RiverHydrology.js`
- Modify: `MatterEngine3/tests/river_network_tests.cpp`
- Modify: `MatterEngine3/tests/dsl_determinism_tests.cpp`
- Modify: `MatterEngine3/tests/async_bake_tests.cpp`

**Interfaces:**
- Consumes: imperative builder calls for backend, particle spacing, density, fixed step, iterations/neighbors, caps, sensor grid, dam, dry collar, and emitters.
- Produces: canonical world definition/hash; worker-owned PhysX bake; renderer GPU-mesher handoff; `HydrologyStatus`/events; dry-world fallback on failure; stale-generation protection.

- [ ] **Step 1: Write failing DSL canonicalization tests**

Author the exact builder surface and assert defaults, invalid ranges, call-order independence where intended, multiple emitter preservation, canonical text/hash changes for every solver-relevant setting, and no hidden environment override.

- [ ] **Step 2: Implement the builder and update the ravine scene**

The scene explicitly declares one upstream inlet, the virtual dam, fill sensor, dry margin, particle spacing, fixed timestep, batch/max steps, capacity, and visual/coarse/gameplay quality settings.

- [ ] **Step 3: Write failing async lifecycle tests**

Assert PhysX runs on the bake worker, Vulkan meshing runs through the existing renderer callback, progress/cancellation events remain thread-safe, dry terrain loads on failure, superseded generations cannot publish, and cache hits submit neither PhysX nor Vulkan work.

- [ ] **Step 4: Implement orchestration and status mapping**

Lazy-create `PhysxRuntime` only for requested PhysX bakes. Compare CUDA/Vulkan LUID before full allocation. Copy the final snapshot to host, idle/release PhysX, then invoke Vulkan meshing. Publish `Ready` only after artifact validation/atomic save.

- [ ] **Step 5: Pass CPU/default-off/editor opt-in tests and commit**

```powershell
git add MatterEngine3/include/matter/hydrology.h MatterEngine3/include/matter/river_network.h MatterEngine3/src/dsl_bindings.cpp MatterEngine3/src/script/world_definition_loader.cpp MatterEngine3/src/provider/local_provider.cpp MatterEngine3/src/provider/local_provider.h MatterEngine3/src/matter_engine.cpp projects/world_demo/scenes/RiverHydrology/RiverHydrology.js MatterEngine3/tests/river_network_tests.cpp MatterEngine3/tests/dsl_determinism_tests.cpp MatterEngine3/tests/async_bake_tests.cpp
git commit -m "feat: orchestrate PhysX river bakes in the editor"
```

### Task 8: Accept the Authored Ravine, Cache, and Distribution

**Files:**
- Modify: `MatterEngine3/tests/physx_fluid_integration_tests.cpp`
- Modify: `tools/stage-windows-msvc-package.py`
- Modify: `tools/check-windows-msvc-package.ps1`
- Modify: `tools/tests/test_windows_package_stage.py`
- Modify: `tools/tests/windows_package_tests.ps1`
- Modify: `cmake/MatterPackaging.cmake`
- Modify: `docs/superpowers/specs/2026-08-22-physx-fluid-bake-integration-design.md`

**Interfaces:**
- Consumes: the complete opt-in editor, authored `RiverHydrology` scene, pinned external build outputs, and package manifest.
- Produces: accepted `.mhyd` ravine artifact, four camera captures, performance/repeat/cache measurements, staged required PhysX GPU/runtime DLLs and notices, and a clean-PATH editor package.

- [ ] **Step 1: Add failing P3/P4 acceptance assertions**

Require at least 100 m inlet-to-dam, about 15% overall fall with reach variation, variable-width rounded-V channel, dry-collar clearance, fill completion before max steps, finite/non-escaped particles, connected wet path, nonzero downstream velocity through the curve, no truncation, no more than two million active particles, and under five wall-clock minutes on the RTX 4090.

- [ ] **Step 2: Run a bounded declared particle-spacing sweep**

Test only the spec-declared spacings/settings, persist every result and rejection reason, and select the coarsest setting that passes all geometry, sensor, gameplay, visual, capacity, and time gates. Do not tune or replace PhysX solver mathematics.

- [ ] **Step 3: Capture and inspect the accepted water**

Generate overview, principal curve, downstream/dam, and low-water captures using the normal editor and existing glass material. Inspect the files for connected flow, terrain containment, boulder interaction, bank clearance, and visible absence of hidden domain walls.

- [ ] **Step 4: Prove cache and lifecycle behavior**

Repeat on the same machine within declared sensor/envelope/volume/velocity tolerances; prove the second load submits no PhysX or GPU meshing work; invalidate independently for terrain/network/PBD/sensor/mesher versions; cancel/reload/shutdown under GPU load without partial publication or device loss.

- [ ] **Step 5: Stage and verify the self-contained editor**

The package manifest sets `physx=true`, `cuda=true`, lists every staged DLL by hash, includes NVIDIA license/notice files, rejects extra runtime aliases, and starts/runs the cached scene under a System32-only PATH without CUDA Toolkit, PhysX source, Python, CMake, or Visual Studio on PATH.

- [ ] **Step 6: Run complete verification and commit acceptance**

```powershell
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo --output-on-failure
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_dist
tools/check-windows-msvc-package.ps1 -DistPath MatterEditor/build/dist/world_demo
```

```bash
make -C MatterEngine3/tests run-physx-adapter-contract
```

Record hardware, driver, SDK/CUDA/adapter versions, particle count, steps, timing, memory, completion fractions, output digests, repeat tolerances, cache-hit evidence, and screenshot paths in the design.

```powershell
git add MatterEngine3/tests/physx_fluid_integration_tests.cpp tools/stage-windows-msvc-package.py tools/check-windows-msvc-package.ps1 tools/tests/test_windows_package_stage.py tools/tests/windows_package_tests.ps1 cmake/MatterPackaging.cmake docs/superpowers/specs/2026-08-22-physx-fluid-bake-integration-design.md docs/superpowers/plans/2026-08-23-physx-fluid-bake-integration.md
git commit -m "test: accept the PhysX ravine fluid bake"
```
