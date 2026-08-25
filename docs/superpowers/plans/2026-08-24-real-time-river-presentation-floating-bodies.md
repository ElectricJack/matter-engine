# Real-time River Presentation and Floating Bodies Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the accepted static river drive passive Box3D crates and rafts while one dedicated water material animates flow-following waves and automatic whitewater and uses MatterEngine's existing ray tracer for reflection, refraction, depth absorption, and in-water scattering in real time.

**Architecture:** Extend the accepted hydrology products with deterministic runtime and presentation fields, publish one immutable generation-safe binding beside the accepted water mesh, and consume that same binding from a fixed-tick float-force system and an eight-slot Vulkan field table. Water geometry, collision, simulation, and BLAS remain static; motion is shader-only through a shared raster/RT water evaluator. The implementation proceeds as reviewable vertical slices: field publication, floating-body proof, dedicated material identity, flow animation, foam/temporal integration, RT optics, and measured RiverFloatLab acceptance.

**Tech Stack:** C++17, QuickJS imperative builder DSL, Flecs ECS, Box3D, MSVC 19.44/v143, CMake/Ninja, NVIDIA PhysX 5.6.1 GPU PBD, CUDA 12.8, Vulkan 1.3 ray tracing, GLSL 460, PowerShell/CTest, Node.js scene tests, and Python FIFO screenshot automation.

**Spec:** `docs/superpowers/specs/2026-08-24-real-time-river-presentation-floating-bodies-design.md`

## Global Constraints

- The accepted particle snapshot, visual mesh, CPU query mesh, runtime field, presentation field, collision, and water BLAS are immutable during play.
- Runtime coupling is one-way: the field applies forces to Box3D bodies; bodies never feed wakes, displacement, or momentum back into the water.
- PhysX remains the only fluid solver. Matter code derives bounded products from accepted results but does not add a second fluid simulation.
- A runtime/presentation generation publishes atomically with the matching accepted visual mesh. Physics, raster, and RT may never observe different water generations.
- Public APIs expose no provider, PhysX, Vulkan, or Flecs implementation types.
- Invalid or dry river samples fail; they are never interpreted as zero-velocity water.
- Water identity and field slot are explicit. No path infers water from material number 4, material number 7, or a single global river.
- The renderer supports eight simultaneously resident water-network bindings and retains the last valid binding on upload or slot failure.
- Raster and RT call the same GLSL water evaluation functions with the same animation time, field generation, and parameter record.
- Wave animation changes normals and optical response only. It writes no vertex displacement, collision update, motion-vector displacement, TLAS update, or BLAS rebuild.
- All backtrace loops, refraction walks, ray counts, and Ultra options are compile-time or specialization bounded.
- RiverFloatLab authors at least 24 ordinary dynamic bodies. One named crate and one named raft must pass upper rapids, waterfall, first pool, spillway, and enter the lower reach; additional bodies may ground or snag.
- Stage only files owned by the active task before each commit; preserve all unrelated worktree changes and untracked files.

## File Responsibility Map

- `MatterEngine3/include/matter/river_runtime.h`: renderer/provider-free public river sample and immutable binding API.
- `MatterEngine3/src/hydrology/fluid_gameplay_field.{h,cpp}`: authoritative height/depth/velocity extraction and strict wet-cell sampling.
- `MatterEngine3/src/hydrology/river_presentation_field.{h,cpp}`: deterministic normals, turbulence, aeration, foam potential, and feature classification.
- `MatterEngine3/src/hydrology/{hydrology_artifact,hydrology_network_artifact,hydrology_handoff_products}.{h,cpp}`: versioned section/network field persistence and handoff aggregation.
- `MatterEngine3/src/matter_engine.cpp`: generation-guarded publication of the public CPU binding and matching renderer binding.
- `MatterEngine3/include/matter/physics.h` and `MatterEngine3/src/ecs/physics_context.{h,cpp}`: queued force-at-world-point command.
- `MatterEngine3/src/ecs/river_float_system.{h,cpp}`: allocation-free probe generation, sampling, force calculation, tracing, and diagnostics.
- `MatterEngine3/src/ecs/{ecs_runtime,physics_systems,scene_registry}.{h,cpp}`: component registration, DSL recipe instantiation, and fixed-pipeline ordering.
- `MatterEngine3/include/matter/river_network.h`, `MatterEngine3/src/hydrology/river_network_builder.{h,cpp}`, and `MatterEngine3/src/script/world_definition_loader.cpp`: water appearance, wave, foam, and local-override builder contracts.
- `MatterEngine3/src/render/water_field_vk.{h,cpp}`: eight-slot immutable image upload, descriptor ownership, generation swaps, and diagnostics.
- `MatterEngine3/src/render/gpu_meshing/water_scene_part.{h,cpp}`: resolved water material/domain and explicit field-slot metadata.
- `MatterEngine3/shaders_vk/water_surface.glsl`: shared field sampling, bounded RK2 backtrace, wave, foam, optical modulation, and temporal reactivity.
- `MatterEngine3/shaders_vk/{raster.vert,gbuffer.frag,rt_surface_common.glsl,rt_lighting.rgen}`: transport water identity into raster/RT and consume the shared evaluator.
- `projects/world_demo/scenes/RiverFloatLab/`: deterministic acceptance scene, raft part, crate/raft recipes, and water appearance.
- `MatterEngine3/tools/river_float_lab.timeline` and `tools/run-river-float-lab-acceptance.ps1`: fixed-camera stills, timed sequences, traces, performance gates, and evidence validation.

---

### Task 1: Derive the Immutable Runtime and Presentation Fields

**Files:**
- Create: `MatterEngine3/src/hydrology/river_presentation_field.h`
- Create: `MatterEngine3/src/hydrology/river_presentation_field.cpp`
- Create: `MatterEngine3/tests/river_presentation_field_tests.cpp`
- Modify: `MatterEngine3/src/hydrology/water_visual_products.h`
- Modify: `MatterEngine3/src/hydrology/water_visual_products.cpp`
- Modify: `MatterEngine3/src/hydrology/fluid_gameplay_field.h`
- Modify: `MatterEngine3/src/hydrology/fluid_gameplay_field.cpp`
- Modify: `MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: accepted `FluidParticle` values, `GameplayFieldLayout`, terrain heights, collider/wake distance samples, and authored waterfall/pool/spillway markers.
- Produces: `PresentationSample`, `RiverFeature`, `PresentationDerivationSettings`, `build_river_presentation_field(...)`, `sample_river_presentation_field(...)`, and a new `ProductKeys::presentation` derived independently of simulation and shader identity.

- [ ] **Step 1: Add failing deterministic derivation and strict sampling tests**

Register `river_presentation_field_tests` with `matter_add_engine_cpu_test`. Build a 5 x 5 analytic field whose centre row has increasing speed, a vertical waterfall sample, a shallow cell, a synthetic boulder-wake distance, and dry neighbours. Assert exact repeatability, finite normalized positive-hemisphere normals, feature classification, bounded `[0,1]` presentation channels, and strict dry rejection:

```cpp
std::vector<PresentationSample> first;
std::vector<PresentationSample> second;
CHECK(build_river_presentation_field(input, settings, first, error));
CHECK(build_river_presentation_field(input, settings, second, error));
CHECK(first == second);
CHECK(first[rapid].feature == RiverFeature::Rapid);
CHECK(first[fall].feature == RiverFeature::Waterfall);
CHECK(first[pool].feature == RiverFeature::Pool);
CHECK(first[wake].foam_potential > first[calm].foam_potential);
CHECK(!sample_river_presentation_field(layout, first, dry_x, dry_z, sample));
```

Extend gameplay-field tests so bilinear interpolation succeeds only when all contributing cells are wet and valid, clamps neither out-of-bounds coordinates nor dry borders, preserves 3D velocity, and returns no partially blended surface at a wet/dry edge.

- [ ] **Step 2: Run the focused tests and verify RED**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_presentation_field_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
```

Expected: compilation fails because the presentation types, product key, and builders do not exist; the new interpolation assertions fail against nearest-cell sampling.

- [ ] **Step 3: Define the renderer-independent field contracts**

Keep the existing `GameplaySample` authoritative and add these canonical product types to `water_visual_products.h`:

```cpp
enum class RiverFeature : std::uint8_t {
    Calm = 0, Current = 1, Rapid = 2, Waterfall = 3,
    Impact = 4, Spillway = 5, Pool = 6
};

struct PresentationSample {
    float normal_x = 0.0f;
    float normal_z = 0.0f;
    float turbulence = 0.0f;
    float aeration = 0.0f;
    float foam_potential = 0.0f;
    RiverFeature feature = RiverFeature::Calm;
    bool wet_valid = false;
};

struct ProductKeys {
    std::uint64_t visual = 0;
    std::uint64_t coarse_cpu = 0;
    std::uint64_t gameplay = 0;
    std::uint64_t presentation = 0;
};
```

Define `PresentationDerivationSettings` with a `contract_version`, finite nonnegative weights for velocity variance, divergence, vorticity, vertical speed, surface slope, shallows, wake distance, and each authored feature, plus normalization scales. Its canonical hash must include every field and local override but exclude material color, shader digest, and RT quality.

- [ ] **Step 4: Implement deterministic gameplay interpolation and presentation derivation**

Use cell-centre bilinear sampling for continuous gameplay channels. Require each nonzero-weight source cell to be wet; derive the returned wet flag only after all contributors validate. Presentation sampling bilinearly filters normal/turbulence/aeration/foam but chooses `RiverFeature` from the nearest validated cell.

The presentation builder must:

1. Reconstruct positive-hemisphere base normals from central height gradients, using one-sided gradients only when the required wet cells exist.
2. Compute velocity variance from the accepted per-cell particle accumulators retained during gameplay-field extraction.
3. Compute bounded horizontal divergence and vorticity from valid neighbours.
4. Combine normalized variance, vorticity, divergence, vertical speed, slope, and shallowness into turbulence.
5. Combine vertical speed, variance, waterfall/impact markers into aeration.
6. Combine turbulence, aeration, shallows, wake/spillway markers, and canonical local override multipliers into foam potential.
7. Select features by explicit marker precedence `Waterfall > Impact > Spillway > Pool > Rapid > Current > Calm`.

Every intermediate is finite-checked and every stored scalar is clamped to `[0,1]`. No adaptive iteration, random number, wall-clock input, or unordered traversal may enter the output.

- [ ] **Step 5: Pass the field and key tests**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_presentation_field_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'river_presentation_field_tests|gpu_visual_mesher_cpu_tests' --output-on-failure
```

Expected: both tests pass; two builds from identical input are byte-identical and presentation-only setting changes alter only `ProductKeys::presentation`.

- [ ] **Step 6: Commit the field derivation slice**

```powershell
git add MatterEngine3/src/hydrology/river_presentation_field.h MatterEngine3/src/hydrology/river_presentation_field.cpp MatterEngine3/tests/river_presentation_field_tests.cpp MatterEngine3/src/hydrology/water_visual_products.h MatterEngine3/src/hydrology/water_visual_products.cpp MatterEngine3/src/hydrology/fluid_gameplay_field.h MatterEngine3/src/hydrology/fluid_gameplay_field.cpp MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: derive immutable river presentation field"
```

### Task 2: Serialize, Assemble, Package, and Publish One Field Generation

**Files:**
- Create: `MatterEngine3/include/matter/river_runtime.h`
- Create: `MatterEngine3/src/hydrology/river_runtime.cpp`
- Create: `MatterEngine3/tests/river_runtime_tests.cpp`
- Modify: `MatterEngine3/src/hydrology/hydrology_artifact.h`
- Modify: `MatterEngine3/src/hydrology/hydrology_artifact.cpp`
- Modify: `MatterEngine3/src/hydrology/hydrology_network_artifact.h`
- Modify: `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- Modify: `MatterEngine3/src/hydrology/hydrology_handoff_products.h`
- Modify: `MatterEngine3/src/hydrology/hydrology_handoff_products.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/tests/hydrology_artifact_tests.cpp`
- Modify: `MatterEngine3/tests/hydrology_network_artifact_tests.cpp`
- Modify: `MatterEngine3/tests/hydrology_handoff_products_tests.cpp`
- Modify: `MatterEngine3/tests/async_bake_tests.cpp`
- Modify: `tools/tests/physx_dependency_contract_tests.ps1`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: section-local gameplay/presentation fields and the assembled network generation inside `HydrologyNetworkBakeResult`.
- Produces: artifact format v5, network-manifest format v2, `runtime_field_digest`, `presentation_field_digest`, immutable `matter::RiverRuntimeBinding`, and `WorldSession::river_runtime_binding()`.

- [ ] **Step 1: Add failing persistence, handoff, publication, and cancellation tests**

Add artifact round-trip fixtures containing every gameplay and presentation channel. Flip one byte in each payload and assert stable digest failure; truncate each section and reject it. Add handoff fixtures whose two sides differ in height, velocity, normal, turbulence, and foam; assert bounded interpolation across the ownership band and strict dry-edge rejection.

Add `async_bake_tests` coverage that parks generation A before publication, requests generation B, releases A, and proves no A CPU binding or render binding becomes visible. After B publishes, assert the returned binding's `generation()`, `runtime_digest()`, and `presentation_digest()` match the accepted manifest.

- [ ] **Step 2: Run the focused tests and verify RED**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target async_bake_tests
```

Expected: new fields and public binding APIs are absent; v4/v1 persistence has no presentation payload or manifest digests.

- [ ] **Step 3: Define the public immutable binding**

`river_runtime.h` must remain renderer/provider-free:

```cpp
namespace matter {
enum class RiverFeature : std::uint8_t {
    Calm = 0, Current = 1, Rapid = 2, Waterfall = 3,
    Impact = 4, Spillway = 5, Pool = 6
};

struct RiverFieldSample {
    Float3 surface_position_m{};
    Float3 surface_normal{0.0f, 1.0f, 0.0f};
    Float3 velocity_mps{};
    float depth_m = 0.0f;
    float turbulence = 0.0f;
    float aeration = 0.0f;
    float foam_potential = 0.0f;
    RiverFeature feature = RiverFeature::Calm;
    bool wet_valid = false;
};

class RiverRuntimeBinding {
public:
    std::uint64_t generation() const noexcept;
    std::uint64_t runtime_digest() const noexcept;
    std::uint64_t presentation_digest() const noexcept;
    bool sample(Float3 world_position_m, RiverFieldSample& out) const noexcept;
    std::size_t sample_batch(const Float3* positions, RiverFieldSample* samples,
                             std::size_t count) const noexcept;
private:
    struct Storage;
    std::shared_ptr<const Storage> storage_;
    friend class WorldSession;
};
}
```

Expose `std::shared_ptr<const RiverRuntimeBinding> WorldSession::river_runtime_binding() const noexcept;`. Sampling combines the authoritative gameplay and presentation fields and rejects dry, non-finite, out-of-bounds, mismatched-layout, or stale storage.

- [ ] **Step 4: Upgrade artifacts and network manifests transactionally**

Bump `hydrology_artifact.cpp` from v4 to v5 and serialize the presentation array after the 21-byte gameplay array using an explicitly sized record: normal X/Z, turbulence, aeration, foam as five little-endian floats plus one feature byte and one wet byte. Validate dimensions, enum range, positive normal-Y reconstruction, bounded normalized channels, exact remaining bytes, and payload digest before assignment.

Bump `hydrology_network_artifact.cpp` from v1 to v2. Add nonzero `runtime_field_digest` and `presentation_field_digest` to `HydrologyNetworkArtifact`; include both in the payload digest and Ready validation. The package closure must name both field payloads and reject a missing, stale, or extra product for the Ready manifest.

Extend `HydrologyNetworkProducts` with the presentation field. Handoff aggregation uses the same ownership weights for continuous channels, renormalizes the blended X/Z normal, applies explicit nearest feature ownership, and never wets a cell for which neither side is valid.

- [ ] **Step 5: Publish CPU and render bindings under one generation lock**

In `matter_engine.cpp`, build `RiverRuntimeBinding::Storage` and the renderer candidate before taking `hydrology_generation_mutex`. Under the existing token check, commit the provider result, CPU binding, renderer binding, accepted flag, status, and event as one generation. Store a single internal publication record containing both CPU and renderer members so acquire readers cannot mix generations. A failed candidate leaves the prior record installed; cancellation publishes neither half.

Keep failed-debug water visual-only: it receives no runtime field and can never drive float forces.

- [ ] **Step 6: Pass persistence, publication, and dependency gates**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_runtime_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target async_bake_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_dependency_contract_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'hydrology_artifact_tests|hydrology_network_artifact_tests|hydrology_handoff_products_tests|river_runtime_tests|async_bake_tests|physx_dependency_contract_tests' --output-on-failure
```

Expected: all selected tests pass, artifact bytes repeat exactly, handoff bounds hold, and a cancelled generation exposes neither CPU nor render state.

- [ ] **Step 7: Commit the immutable publication slice**

```powershell
git add MatterEngine3/include/matter/river_runtime.h MatterEngine3/src/hydrology/river_runtime.cpp MatterEngine3/tests/river_runtime_tests.cpp MatterEngine3/src/hydrology/hydrology_artifact.h MatterEngine3/src/hydrology/hydrology_artifact.cpp MatterEngine3/src/hydrology/hydrology_network_artifact.h MatterEngine3/src/hydrology/hydrology_network_artifact.cpp MatterEngine3/src/hydrology/hydrology_handoff_products.h MatterEngine3/src/hydrology/hydrology_handoff_products.cpp MatterEngine3/src/provider/local_provider.cpp MatterEngine3/src/matter_engine.cpp MatterEngine3/include/matter/world_session.h MatterEngine3/tests/hydrology_artifact_tests.cpp MatterEngine3/tests/hydrology_network_artifact_tests.cpp MatterEngine3/tests/hydrology_handoff_products_tests.cpp MatterEngine3/tests/async_bake_tests.cpp tools/tests/physx_dependency_contract_tests.ps1 cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: publish immutable river runtime bindings"
```

### Task 3: Add Queued Force-at-World-Point to Box3D

**Files:**
- Modify: `MatterEngine3/include/matter/physics.h`
- Modify: `MatterEngine3/src/ecs/physics_context.h`
- Modify: `MatterEngine3/src/ecs/physics_context.cpp`
- Modify: `MatterEngine3/tests/physics_tests.cpp`
- Modify: `cmake/MatterEngine.cmake`

**Interfaces:**
- Consumes: an entity-owned dynamic Box3D body, finite world-space force, and finite world-space point.
- Produces: `physics_apply_force_at_world_point(flecs::entity, Float3 force, Float3 point)`, `PhysicsCommandKind::ForceAtPoint`, trace rows, and deferred `b3Body_ApplyForce(...)` mutation during `PhysicsPush`.

- [ ] **Step 1: Register `physics_tests` and add failing command-contract tests**

Register the existing `MatterEngine3/tests/physics_tests.cpp` with `matter_add_engine_cpu_test`. Add tests proving:

- force-at-point is queued rather than immediately mutating Box3D;
- it appears in deterministic command order between velocity and impulse commands;
- the trace stores force in `primary` and world point in `secondary`;
- an off-centre upward force produces angular velocity after the fixed step;
- foreign-world, destroyed-body, static-body, NaN force, and NaN point calls fail without queue mutation; and
- queue overflow increments the existing failed-command diagnostic without an out-of-bounds write.

- [ ] **Step 2: Run `physics_tests` and verify RED**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target physics_tests
```

Expected: the target/API/enum do not yet exist.

- [ ] **Step 3: Implement the queued command without direct gameplay Box3D access**

Add:

```cpp
enum class PhysicsCommandKind : uint8_t {
    Teleport, Velocity, Force, ForceAtPoint, Impulse, Wake
};

bool PhysicsContext::enqueue_force_at_world_point(
    const flecs::world_t* originating_world, flecs::entity_t entity,
    Float3 force, Float3 world_point) noexcept;

bool physics_apply_force_at_world_point(
    flecs::entity entity, Float3 force, Float3 world_point);
```

Reuse the existing queue ownership, capacity, world, entity, and finiteness checks. In `PhysicsContext::push`, resolve the body and call:

```cpp
b3Body_ApplyForce(body, box_vector(force), box_position(world_point), true);
```

Do not call Box3D from the public function or the future float system. Preserve existing command order and trace semantics.

- [ ] **Step 4: Pass the physics command gate**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target physics_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R '^physics_tests$' --output-on-failure
```

Expected: all existing and new physics tests pass and the off-centre force produces torque only after the fixed pipeline runs.

- [ ] **Step 5: Commit the physics API slice**

```powershell
git add MatterEngine3/include/matter/physics.h MatterEngine3/src/ecs/physics_context.h MatterEngine3/src/ecs/physics_context.cpp MatterEngine3/tests/physics_tests.cpp cmake/MatterEngine.cmake
git commit -m "feat: queue Box3D forces at world points"
```

### Task 4: Implement the Allocation-Free River Float System

**Files:**
- Create: `MatterEngine3/src/ecs/river_float_system.h`
- Create: `MatterEngine3/src/ecs/river_float_system.cpp`
- Create: `MatterEngine3/tests/river_float_system_tests.cpp`
- Modify: `MatterEngine3/include/matter/river_runtime.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/src/ecs/physics_systems.cpp`
- Modify: `MatterEngine3/src/ecs/scene_registry.h`
- Modify: `MatterEngine3/src/ecs/scene_registry.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/tests/scene_registry_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: `RiverRuntimeBinding`, `ecs::LocalTransform`, `physics::PhysicsVelocity`, `physics::BoxCollider`, `physics::RigidBody`, and `RiverFloatBody`.
- Produces: `RiverFloatForces` fixed phase, bounded `RiverFloatProbeForce` rows, queued force-at-point commands, `RiverFloatDiagnostics`, and deterministic per-body trace checksums.

- [ ] **Step 1: Add failing pure-force, system-order, reset, and allocation tests**

Use an injected analytic sampler rather than a PhysX artifact. Cover equilibrium draft for a 3 m crate and a 4.8 x 0.7 x 3 m raft, convergence toward a uniform 5 m/s current, torque from a cross-body velocity gradient, anisotropic longitudinal/lateral/vertical drag, angular damping, dry exit, waterfall loss of support, pool re-entry, and capped impact forces.

Add fixed-pipeline assertions that the trace is exactly:

```text
PhysicsReconcile -> RiverFloatForces -> PhysicsPush -> Physics -> PhysicsPull
```

Warm the system for 16 ticks, reset the allocation counter, run 1,000 fixed ticks with 24 bodies, and assert zero steady-state allocations. Snapshot initial transforms/velocities, run 300 ticks, restore, rerun, and compare per-tick field-sample, force, and transform checksums.

- [ ] **Step 2: Run focused tests and verify RED**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_float_system_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target scene_registry_tests
```

Expected: the component, phase, system, registry entry, and binding injection seam do not exist.

- [ ] **Step 3: Define bounded authoring and diagnostic components**

Add the public component to `river_runtime.h`:

```cpp
struct RiverFloatBody {
    float effective_density_kg_m3 = 650.0f;
    float displaced_volume_scale = 1.0f;
    std::uint8_t probes_x = 2;
    std::uint8_t probes_y = 2;
    std::uint8_t probes_z = 2;
    float probe_inset = 0.15f;
    float buoyancy_response = 1.0f;
    float longitudinal_drag = 0.8f;
    float lateral_drag = 1.4f;
    float vertical_drag = 1.8f;
    float angular_damping = 0.4f;
    float max_force_per_probe_n = 30000.0f;
    float max_total_force_n = 120000.0f;
    Float3 diagnostic_color{0.2f, 0.8f, 1.0f};
};

struct RiverFloatForces {};
```

Validate densities in `(0, 2000]`, volume scale in `(0, 4]`, probe axes in `[1,4]` with at most 64 total probes, inset in `[0,0.49]`, finite nonnegative responses, and positive force caps. Add a private fixed-size `RiverFloatState` with generation, consecutive invalid count, disabled flag, sample checksum, and force checksum; do not put runtime counters in the authored component.

- [ ] **Step 4: Implement the pure force kernel**

Expose a testable function accepting a fixed-capacity output span:

```cpp
bool compute_river_float_forces(
    const RiverFloatBody& settings, const physics::BoxCollider& box,
    const ecs::LocalTransform& transform,
    const physics::PhysicsVelocity& velocity,
    const RiverSampleFunction& sample,
    float gravity_mps2, RiverFloatForceBuffer& output,
    RiverFloatDiagnostics& diagnostics) noexcept;
```

For each deterministic X-major/Y/Z probe:

- transform the inset box-lattice point to world space;
- reject dry, non-finite, stale, or waterfall support samples;
- compute represented probe volume and `submerged_fraction = clamp((surface_y - probe_bottom_y) / probe_height, 0, 1)`;
- apply `rho_water * displaced_probe_volume * gravity * submerged_fraction * buoyancy_response` upward;
- compute point velocity `linear + cross(angular, point - centre)`;
- form flow, lateral, and up axes from sampled velocity and apply the authored quadratic drag coefficient per axis;
- include bounded angular damping through the point-relative velocity term;
- clamp per-probe force, then proportionally scale all rows if total magnitude exceeds the body cap.

The waterfall feature contributes bounded drag but no upward buoyancy. A dry sample contributes no force. After eight consecutive invalid/non-finite inputs, disable that body's float state and emit one stable diagnostic; ordinary dry traversal does not count as an invalid input.

- [ ] **Step 5: Insert the system between reconcile and push**

Register `RiverFloatForces` as a Flecs phase depending on `PhysicsReconcile`; change `PhysicsPush` to depend on `RiverFloatForces`. The system queries the five required components, reads the current immutable binding from an internal singleton, uses stack/fixed storage for up to 64 probes, and calls only `physics_apply_force_at_world_point`.

On water-generation replacement, update the ECS singleton at the same WorldSession publication boundary and clear per-body sample history. Play/Stop restoration must restore the component, transform, velocity, and private state through the existing world snapshot path.

- [ ] **Step 6: Add the component to scene recipes and pass all gates**

Add `ComponentKind::RiverFloatBody`, explicit field descriptors, validation/copy/instantiation branches, size/alignment assertions, and Flecs registration. Then run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_float_system_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target scene_registry_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target physics_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'river_float_system_tests|scene_registry_tests|physics_tests' --output-on-failure
```

Expected: all tests pass, the fixed phase ordering is exact, replay checksums match, invalid input never reaches Box3D, and the warmed 24-body loop allocates zero times.

- [ ] **Step 7: Commit the float-system slice**

```powershell
git add MatterEngine3/src/ecs/river_float_system.h MatterEngine3/src/ecs/river_float_system.cpp MatterEngine3/tests/river_float_system_tests.cpp MatterEngine3/include/matter/river_runtime.h MatterEngine3/src/ecs/ecs_runtime.cpp MatterEngine3/src/ecs/physics_systems.cpp MatterEngine3/src/ecs/scene_registry.h MatterEngine3/src/ecs/scene_registry.cpp MatterEngine3/src/matter_engine.cpp MatterEngine3/tests/scene_registry_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: float Box3D bodies on accepted river fields"
```

### Task 5: Author RiverFloatLab and Prove Real-River Body Traversal

**Files:**
- Create: `projects/world_demo/shared-lib/river_hydrology_definition.js`
- Create: `projects/world_demo/scenes/RiverFloatLab/RiverFloatLab.js`
- Create: `projects/world_demo/scenes/RiverFloatLab/objects/RiverRaft.js`
- Create: `projects/world_demo/tests/river_float_lab_scene_tests.mjs`
- Create: `MatterEngine3/tools/river_float_physics.timeline`
- Create: `tools/run-river-float-physics-proof.ps1`
- Modify: `projects/world_demo/scenes/RiverHydrology/RiverHydrology.js`
- Modify: `projects/world_demo/tests/river_hydrology_scene_tests.mjs`
- Modify: `projects/world_demo/objects/Crate.js`
- Modify: `docs/agent/qa-cookbook.md`

**Interfaces:**
- Consumes: the accepted two-section ravine definition, boulder transforms/colliders, `RiverFloatBody`, normal scene recipes, and Box3D Play/Stop.
- Produces: the `RiverFloatLab` world, 24+ named crates/rafts, two reference traversal bodies, static boulder collision entities, a fixed physics-proof timeline, body traces, and review screenshots.

- [ ] **Step 1: Add failing scene-structure tests**

Move the pure `buildRiverHydrologyDefinition(seed)` implementation into `projects/world_demo/shared-lib/river_hydrology_definition.js` and make both worlds import it. In the new test, assert:

- exactly the same curve, channel profile, sections, waterfall, pools, spillways, and boulder roots are used by both scenes;
- at least 24 dynamic bodies carry `LocalTransform`, `PartInstance`, `RigidBody`, `BoxCollider`, and `RiverFloatBody`;
- `reference-crate` and `reference-raft` start in a clear deterministic upstream lane;
- the raft collider and visual part are 4.8 x 0.7 x 3.0 m;
- every visual boulder has a matching static `RigidBody` plus transformed `SphereCollider`; and
- the river generator creates no body implicitly.

- [ ] **Step 2: Run the Node tests and verify RED**

```powershell
node projects/world_demo/tests/river_hydrology_scene_tests.mjs
node projects/world_demo/tests/river_float_lab_scene_tests.mjs
```

Expected: the shared module, new world, raft part, and float-body recipes do not exist.

- [ ] **Step 3: Build explicit crate, raft, and boulder recipes**

Keep the current `Crate` visual at 3 x 3 x 3 m. Build `RiverRaft` as a centred 4.8 x 0.7 x 3.0 m rounded/flattened box using the existing wood-like material chosen by the project; its `BoxCollider.halfExtents` must be `[2.4, 0.35, 1.5]`.

`RiverFloatLab.buildEntities()` authors:

- `reference-crate` at the upstream centre lane with density 620 kg/m3 and a 2 x 2 x 2 probe lattice;
- `reference-raft` 10 m behind it with density 420 kg/m3 and a 3 x 2 x 3 lattice;
- 22 additional bodies distributed across upper rapids, boulder wakes, waterfall approach, and first spillway with stable ids and bounded parameter variations; and
- one static sphere-collider entity for each authored boulder root, using the exact root transform and fluid-collider sphere.

Set continuous collision on reference bodies, retain ordinary gravity, and disable sleep only for the two reference traversers. Do not script translations, velocities, waterfall teleports, or respawns.

- [ ] **Step 4: Add deterministic body tracing and the physics-proof runner**

`tools/run-river-float-physics-proof.ps1` must build `matter_editor` with PhysX, clear only `projects/world_demo/.cache/RiverFloatLab` after validating the resolved path, launch `RiverFloatLab` through `drive.py`, wait for Ready, enter Play, and write:

```text
trace/body-reference-crate.csv
trace/body-reference-raft.csv
trace/river-float-diagnostics.json
screenshots/upper-start.png(.done)
screenshots/waterfall-fall.png(.done)
screenshots/plunge-recovery.png(.done)
screenshots/spillway-entry.png(.done)
screenshots/lower-entry.png(.done)
```

CSV rows contain fixed tick, binding generation, position, rotation, linear/angular velocity, wet-probe count, sample checksum, force checksum, and float-disabled flag. The runner fails unless both reference bodies cross authored distance markers in order, neither disables float behavior, and no non-finite value appears.

- [ ] **Step 5: Pass scene tests and capture the real-river proof**

```powershell
node projects/world_demo/tests/river_hydrology_scene_tests.mjs
node projects/world_demo/tests/river_float_lab_scene_tests.mjs
tools/run-river-float-physics-proof.ps1 -PhysxRoot $env:MATTER_PHYSX_ROOT -CudaRoot $env:CUDA_PATH -RunId 20260824-float-physics-01
```

Expected: both Node tests pass; the two reference bodies traverse upper, fall, pool, spillway, and lower markers under sampled forces; 24 bodies remain finite; all five PNGs and `.done` sidecars exist.

- [ ] **Step 6: Commit the playable physics proof**

```powershell
git add projects/world_demo/shared-lib/river_hydrology_definition.js projects/world_demo/scenes/RiverFloatLab/RiverFloatLab.js projects/world_demo/scenes/RiverFloatLab/objects/RiverRaft.js projects/world_demo/tests/river_float_lab_scene_tests.mjs MatterEngine3/tools/river_float_physics.timeline tools/run-river-float-physics-proof.ps1 projects/world_demo/scenes/RiverHydrology/RiverHydrology.js projects/world_demo/tests/river_hydrology_scene_tests.mjs projects/world_demo/objects/Crate.js docs/agent/qa-cookbook.md
git commit -m "feat: add RiverFloatLab physics proof"
```

### Task 6: Replace Hardcoded Glass with a Dedicated Water Domain and DSL

**Files:**
- Modify: `libs/MatterSurfaceLib/include/material_registry.h`
- Modify: `libs/MatterSurfaceLib/src/material_registry.c`
- Modify: `libs/MatterSurfaceLib/tests/material_registry_tests.cpp`
- Modify: `MatterEngine3/include/matter/river_network.h`
- Modify: `MatterEngine3/src/hydrology/river_network_builder.h`
- Modify: `MatterEngine3/src/hydrology/river_network_builder.cpp`
- Modify: `MatterEngine3/src/script/world_definition_loader.cpp`
- Modify: `MatterEngine3/src/render/gpu_meshing/water_scene_part.h`
- Modify: `MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/tests/world_definition_tests.cpp`
- Modify: `MatterEngine3/tests/river_network_tests.cpp`
- Modify: `MatterEngine3/tests/gpu_water_render_tests.cpp`
- Modify: `projects/world_demo/scenes/RiverFloatLab/RiverFloatLab.js`
- Modify: `cmake/manifests/engine-core.sources`

**Interfaces:**
- Consumes: a dynamic PBR material flagged as a water surface and imperative `waterSurface(...).optics(...).waveBand(...).foam(...).localOverride(...)` calls.
- Produces: `MATERIAL_WATER_SURFACE`, `WaterSurfaceDefinition`, deterministic canonical appearance text/hash, resolved water material identity, and water-only bounded parameter records separate from `MaterialDef`.

- [x] **Step 1: Add failing DSL, canonicalization, material-domain, and mesh tests**

Add a world fixture using:

```js
const RIVER_WATER = defineMaterial("riverWater", {
  albedo: [0.05, 0.14, 0.18], roughness: 0.06,
  transmission: 0.98, ior: 1.333, volumeBoundary: true,
  waterSurface: true,
});

network.waterSurface(RIVER_WATER)
  .optics({
    shallowAbsorption: [0.03, 0.015, 0.008], shallowDistance: 8,
    deepAbsorption: [0.18, 0.055, 0.025], deepDistance: 2.5,
    scatteringColor: [0.08, 0.22, 0.24], scatteringDistance: 7,
    anisotropy: 0.35, ior: 1.333,
  })
  .waveBand({ wavelength: 7.5, amplitude: 0.16, speed: 0.8, response: 0.35 })
  .waveBand({ wavelength: 1.6, amplitude: 0.24, speed: 1.4, response: 0.75 })
  .waveBand({ wavelength: 0.28, amplitude: 0.08, speed: 2.1, response: 0.20 })
  .foam({ threshold: 0.42, gain: 1.8, persistence: 2.5,
          breakupScale: 0.7, roughnessGain: 0.55,
          scatteringGain: 1.4, transmissionLoss: 0.72,
          normalSoftening: 0.6 })
  .localOverride({ shape: "sphere", center: [111, 46, 5], radius: 14,
                   foamMultiplier: 1.25, waveMultiplier: 1.1,
                   thresholdOffset: -0.08 });
```

Assert reordered object keys canonicalize identically; call order of the three wave bands remains significant; invalid IOR, distances, wavelengths, amplitudes, thresholds, anisotropy, shapes, or extents fail with the exact field path. Assert water mesh construction rejects an unflagged glass material and accepts a flagged dynamic material without requiring id 4 or 7.

- [x] **Step 2: Run focused tests and verify RED**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target material_registry_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_definition_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_network_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
```

Expected: `waterSurface` and the builder do not exist; water mesh tests still require material 4.

- [x] **Step 3: Add the generic water-domain flag and separate settings**

Add `MATERIAL_WATER_SURFACE = 1u << 4` to `MaterialSurfaceFlags`, expose strict `waterSurface` parsing in `defineMaterial`, and bump `MATERIAL_SCHEMA_VERSION` from 4 to 5. Do not add wave/foam fields to `MaterialDef` or `MaterialGpuRecord`.

Define in `river_network.h`:

```cpp
struct WaterWaveBandDefinition {
    float wavelength_m, normal_amplitude, speed_multiplier, response;
};
struct WaterOpticalDefinition {
    Float3 shallow_absorption, deep_absorption, scattering_color;
    float shallow_distance_m, deep_distance_m, scattering_distance_m;
    float anisotropy, ior;
};
struct WaterFoamDefinition {
    float threshold, gain, persistence_s, breakup_scale_m;
    float roughness_gain, scattering_gain, transmission_loss, normal_softening;
};
struct WaterLocalOverrideDefinition {
    enum class Shape : std::uint8_t { Sphere, Box } shape;
    Float3 center_m, half_extents_m;
    float radius_m, foam_multiplier, wave_multiplier, threshold_offset;
};
struct WaterSurfaceDefinition {
    std::uint32_t material_id;
    WaterOpticalDefinition optics;
    std::vector<WaterWaveBandDefinition> wave_bands;
    WaterFoamDefinition foam;
    std::vector<WaterLocalOverrideDefinition> local_overrides;
    std::uint64_t appearance_hash;
};
```

Require exactly three wave bands for Ready publication. Validate physical IOR in `[1,2.5]`, positive distances/wavelengths, amplitudes in `[0,1]`, anisotropy in `[-0.95,0.95]`, finite bounded foam controls, and positive override volumes.

- [x] **Step 4: Implement the imperative builder and resolved mesh identity**

`network.waterSurface(materialId)` returns a dedicated QuickJS builder object with chainable `.optics`, `.waveBand`, `.foam`, and `.localOverride` methods. Native canonical text uses fixed field order and float formatting, includes appearance controls in the appearance/presentation product key, and excludes them from PhysX semantic identity.

Change `build_water_scene_part` to accept `material_id`, validate `MATERIAL_WATER_SURFACE`, and write that id to every `VkRasterVertex`. Remove every `mesh.material == 4` and `material_index = 4` check. Failed-debug water uses the same flagged water material when available and otherwise uses a clearly logged non-water static debug material without a field binding.

- [x] **Step 5: Pass material, DSL, and water-mesh gates**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target material_registry_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_definition_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_network_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'material_registry_tests|world_definition_tests|river_network_tests|gpu_water_render_tests' --output-on-failure
```

Expected: all tests pass; the RiverFloatLab material is dynamically resolved, id-independent, and a water appearance change does not invalidate PhysX section artifacts.

- [x] **Step 6: Commit the dedicated water-domain slice**

```powershell
git add libs/MatterSurfaceLib/include/material_registry.h libs/MatterSurfaceLib/src/material_registry.c libs/MatterSurfaceLib/tests/material_registry_tests.cpp MatterEngine3/include/matter/river_network.h MatterEngine3/src/hydrology/river_network_builder.h MatterEngine3/src/hydrology/river_network_builder.cpp MatterEngine3/src/script/world_definition_loader.cpp MatterEngine3/src/render/gpu_meshing/water_scene_part.h MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp MatterEngine3/src/matter_engine.cpp MatterEngine3/tests/world_definition_tests.cpp MatterEngine3/tests/river_network_tests.cpp MatterEngine3/tests/gpu_water_render_tests.cpp projects/world_demo/scenes/RiverFloatLab/RiverFloatLab.js cmake/manifests/engine-core.sources
git commit -m "feat: add dedicated authored water surface domain"
```

### Task 7: Upload Eight Immutable Vulkan Water-Field Bindings

**Files:**
- Create: `MatterEngine3/src/render/water_field_vk.h`
- Create: `MatterEngine3/src/render/water_field_vk.cpp`
- Create: `MatterEngine3/tests/water_field_vk_tests.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/render/gpu_meshing/water_scene_part.h`
- Modify: `MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp`
- Modify: `MatterEngine3/shaders_vk/cull.comp`
- Modify: `MatterEngine3/shaders_vk/raster.vert`
- Modify: `MatterEngine3/shaders_vk/visibility_id.vert`
- Modify: `MatterEngine3/shaders_vk/rt_surface_common.glsl`
- Modify: `MatterEngine3/tests/gpu_water_render_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: one validated immutable assembled field, its generation/digests, and `WaterSurfaceDefinition`.
- Produces: `WaterFieldVk::publish/release`, eight stable slots, two `VK_FORMAT_R16G16B16A16_SFLOAT` images, one `VK_FORMAT_R8G8B8A8_UNORM` image per slot, raster descriptor arrays at set 1 bindings 20-22 with parameters at 23, RT mirrors at set 0 bindings 21-23 with parameters at 24, and explicit raster/RT slot+generation metadata.

- [ ] **Step 1: Add failing format packing, slot lifetime, and transport tests**

Unit-test CPU packing for all channels, half-float saturation, normal-Y reconstruction inputs, feature UNORM encoding/nearest decoding, dimension overflow, and dry borders. Test eight successful allocations, ninth-allocation failure, last-valid retention, generation replacement, deferred GPU lifetime release, and slot reuse only after completion.

Extend Vulkan smoke tests to draw two water parts using the same material but different slots and prove raster and RT observe distinct field generations. Assert a field/material generation mismatch fails closed to static water and increments a bounded diagnostic.

- [ ] **Step 2: Run focused tests and verify RED**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_field_vk_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
```

Expected: upload service, descriptors, slot metadata, and shader records do not exist.

- [ ] **Step 3: Implement immutable slot resources and descriptors**

Each occupied slot owns three sampled images/views, staging lifetime, layout, appearance record, runtime/presentation digests, and generation. Pack exactly:

```text
A RGBA16F = surface height, depth, velocity X, velocity Z
B RGBA16F = velocity Y, normal X, normal Z, turbulence
C RGBA8   = aeration, foam potential, wet validity, feature / 6
```

Use linear samplers for A/B continuous channels and a nearest sampler for C. Bind arrays of eight descriptors at raster set 1 bindings 20, 21, and 22; binding 23 is a storage buffer of eight records containing origin X/Z, cell size, width/depth, generation, runtime digest, presentation digest, and valid flag. Mirror the three arrays at RT set 0 bindings 21, 22, and 23 and the same parameter buffer at binding 24 because RT binding 20 is already `raw_transmission_aux_image`. Empty slots bind the renderer's existing safe dummy images and a zero valid record.

Construct a replacement fully before swapping the slot record. Upload or capacity failure returns an explicit category and preserves the last valid record. Release uses frame-completion lifetimes, never immediate destruction of in-flight resources.

- [ ] **Step 4: Transport explicit slot and generation through raster and RT**

Append `water_binding_slot` and `water_generation` to the CPU part record. Grow `GpuInstance` to 176 bytes and `GpuDrawTransform` to 160 bytes with explicit padding and updated static assertions. `cull.comp`, the visibility tail, direct draws, and skin-tail writers must initialize both values; `raster.vert` forwards them flat at locations 15 and 16.

Reuse `GpuRtPartRecord`'s current `pad1` and `pad2` words for slot and generation, preserving its byte size. `emit_ray_instances` fills them for the selected traced rung, and `RtSurface` carries them from `gl_InstanceCustomIndexEXT` without changing the TLAS custom-index meaning.

Use `UINT32_MAX`/zero as the fail-closed no-water values. Do not look up a field from material identity.

- [ ] **Step 5: Pass CPU and Vulkan binding gates**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_field_vk_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'water_field_vk_tests|gpu_water_render_tests|vulkan_smoke_tests' --output-on-failure
```

Expected: all tests pass with zero Vulkan validation errors; two same-material rivers sample different slots; a ninth network does not evict a valid binding.

- [ ] **Step 6: Commit the Vulkan field-binding slice**

```powershell
git add MatterEngine3/src/render/water_field_vk.h MatterEngine3/src/render/water_field_vk.cpp MatterEngine3/tests/water_field_vk_tests.cpp MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/render/gpu_meshing/water_scene_part.h MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp MatterEngine3/shaders_vk/cull.comp MatterEngine3/shaders_vk/raster.vert MatterEngine3/shaders_vk/visibility_id.vert MatterEngine3/shaders_vk/rt_surface_common.glsl MatterEngine3/tests/gpu_water_render_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: bind immutable river fields in Vulkan"
```

### Task 8: Share Flow Backtracing and Three-Band Waves Across Raster and RT

**Files:**
- Create: `MatterEngine3/shaders_vk/water_surface.glsl`
- Create: `MatterEngine3/src/render/water_surface_reference.h`
- Create: `MatterEngine3/src/render/water_surface_reference.cpp`
- Create: `MatterEngine3/tests/water_surface_reference_tests.cpp`
- Modify: `MatterEngine3/shaders_vk/gbuffer.frag`
- Modify: `MatterEngine3/shaders_vk/rt_surface_common.glsl`
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/tests/shader_source_tests.cpp`
- Modify: `MatterEngine3/tests/gpu_water_render_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: world position, geometric/base normal, explicit field slot and generation, `WaterSurfaceDefinition`, and one renderer animation time.
- Produces: `water_sample_field`, `water_backtrace_rk2`, `water_wrapped_phase`, `water_evaluate_surface`, three-band animated shading normals, and identical CPU reference vectors.

- [ ] **Step 1: Add failing CPU vectors and shader-sharing tests**

Create fixed reference vectors for:

- world-to-field mapping at cell centres, fractional cells, borders, and dry rejection;
- three-substep RK2 backtrace through constant flow and a curved analytic velocity field;
- phase values immediately before/after both reset boundaries;
- calm/current/rapid amplitude responses;
- a grazing base normal and maximum wave amplitudes remaining in the geometric hemisphere; and
- generation/slot mismatch returning static fallback rather than reading another field.

`shader_source_tests` must assert both `gbuffer.frag` and `rt_lighting.rgen` include `water_surface.glsl`, neither defines a private second `water_backtrace_rk2`, and all loops use named compile-time bounds.

- [ ] **Step 2: Run focused tests and verify RED**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_surface_reference_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
```

Expected: the reference and shared GLSL functions are absent.

- [ ] **Step 3: Implement field sampling and bounded curved-flow backtrace**

`water_surface.glsl` declares descriptor bindings through macros so raster maps them to set 1 bindings 20-23 and RT maps them to set 0 bindings 21-24 if binding 20 is already the RT transmission auxiliary image. The include validates slot, generation, dimensions, bounds, and nearest wet classification before any continuous fetch.

Use exactly three RK2 substeps for a bounded age `age_s`:

```glsl
vec2 water_backtrace_rk2(uint slot, vec2 xz, float age_s) {
    const int WATER_BACKTRACE_STEPS = 3;
    float dt = age_s / float(WATER_BACKTRACE_STEPS);
    for (int i = 0; i < WATER_BACKTRACE_STEPS; ++i) {
        vec2 v0 = water_velocity_xz(slot, xz);
        vec2 mid = xz - 0.5 * dt * clamp_length(v0, water.max_shading_speed);
        vec2 vm = water_velocity_xz(slot, mid);
        vec2 next = xz - dt * clamp_length(vm, water.max_shading_speed);
        if (!water_valid(slot, next)) break;
        xz = next;
    }
    return xz;
}
```

Invalid steps retain the last valid coordinate and increment one per-frame diagnostic only after a persistent invalid count; they never sample across a dry bank.

- [ ] **Step 4: Implement reset-free dual phases and three wave bands**

For loop period `P`, compute:

```text
phase0 = mod(time, P)             weight0 = sin(pi * phase0 / P)^2
phase1 = mod(time + P/2, P)       weight1 = sin(pi * phase1 / P)^2
```

The weights sum to one and each phase has zero weight exactly where its own age resets, eliminating the wrap jump. Backtrace both phase ages and blend their analytic/noise wave gradients.

Evaluate exactly three authored bands: broad, chop, and capillary. Align their tangent frame to local horizontal velocity; fall back to a deterministic world axis in calm water. Scale broad waves with depth/current, chop with speed/slope/turbulence, and capillary detail down under foam/calm response. Sum gradients with authored amplitudes, cap total slope, construct the shading normal around the base normal, and project it back into the geometric positive hemisphere with a `dot >= 0.05` safety floor.

- [ ] **Step 5: Integrate the same evaluator into raster and traced water hits**

In `gbuffer.frag`, detect the generic water-surface flag, validate explicit slot/generation, and replace only the shading normal/roughness inputs with `water_evaluate_surface`. Keep positions, depth, and motion vectors unchanged.

Extend `RtSurface` with slot/generation and call the same evaluator when a traced hit material has the water flag. Use its animated normal for optical lobes but retain `RtSurface.normal` as the geometric normal for face orientation and offsets. Pass one frame animation time through both pipelines; never read wall-clock time independently in a shader.

- [ ] **Step 6: Pass reference, compilation, and Vulkan agreement gates**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_surface_reference_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'water_surface_reference_tests|shader_source_tests|gpu_water_render_tests|vulkan_smoke_tests' --output-on-failure
```

Expected: all tests pass; boundary vectors are continuous; raster and RT read the same generation/time; Vulkan reports zero validation errors and zero runtime water BLAS rebuilds.

- [ ] **Step 7: Commit the shared flow-animation slice**

```powershell
git add MatterEngine3/shaders_vk/water_surface.glsl MatterEngine3/src/render/water_surface_reference.h MatterEngine3/src/render/water_surface_reference.cpp MatterEngine3/tests/water_surface_reference_tests.cpp MatterEngine3/shaders_vk/gbuffer.frag MatterEngine3/shaders_vk/rt_surface_common.glsl MatterEngine3/shaders_vk/rt_lighting.rgen MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/tests/shader_source_tests.cpp MatterEngine3/tests/gpu_water_render_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: animate water normals along baked flow"
```

### Task 9: Add Automatic Whitewater and Temporal Reactivity

**Files:**
- Modify: `MatterEngine3/shaders_vk/water_surface.glsl`
- Modify: `MatterEngine3/shaders_vk/gbuffer.frag`
- Modify: `MatterEngine3/shaders_vk/gi_temporal.comp`
- Modify: `MatterEngine3/shaders_vk/gi_atrous.comp`
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/tests/water_surface_reference_tests.cpp`
- Modify: `MatterEngine3/tests/gpu_water_render_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `MatterEngine3/tests/shader_source_tests.cpp`

**Interfaces:**
- Consumes: baked foam potential/aeration/feature, local canonical overrides, flow-advected breakup detail, current and previous water surface evaluations, and binding generation.
- Produces: coherent foam optical modulation, `R8_UNORM` G-buffer reactivity, calm-water confidence, water-aware temporal weights, and generation invalidation.

- [ ] **Step 1: Add failing foam and temporal tests**

Extend CPU vectors to cover threshold/gain, macro attachment under moving detail, local multiply/suppress/threshold overrides, optical energy bounds, calm versus rapid reactivity, and generation changes forcing full rejection.

Add a Vulkan temporal sequence with a stationary camera: calm pool history must converge, moving foam must not leave a sustained trail after the macro mask clears, and swapping field generation must reject old history on the affected water pixels while leaving terrain history valid.

- [ ] **Step 2: Run focused tests and verify RED**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_surface_reference_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
```

Expected: the evaluator has no foam optical state, reactivity attachment, or water-aware temporal rejection.

- [ ] **Step 3: Implement physically grouped automatic foam**

Compute:

```text
macro = saturate((foam_potential + aeration_bias + feature_bias
                  - threshold - local_threshold_offset) * gain)
detail = advected_breakup_noise(backtraced_xz, time, breakup_scale, persistence)
foam = saturate(macro * local_foam_multiplier * detail)
```

Feature bias is nonzero only for baked Rapid, Waterfall, Impact, and Spillway classes. Runtime detail may cut holes and streaks inside the macro cause but may not translate the macro footprint away from it.

Apply foam as one energy-bounded group: blend toward bright diffuse scattering, increase roughness, scale coherent transmission down, soften wave-normal energy, and increase reactive confidence. Clamp the sum of reflective, transmitted, and diffuse/scattered weights to one. A zero macro mask must produce zero foam regardless of breakup noise.

- [ ] **Step 4: Add and wire a dedicated reactivity attachment**

Add `VkRasterAttachments::reactivity` and one `VK_FORMAT_R8_UNORM` image cleared to zero. When a slot generation is replaced, `WaterFieldVk` sets that bit in a per-frame `changed_water_slot_mask`; the mask is cleared only after the first successfully submitted frame using the replacement. `gbuffer.frag` compares its explicit slot bit and writes:

```text
reactivity = max(normal_change, foam_motion, turbulence_response,
                 generation_changed ? 1 : 0)
```

Non-water pixels write zero. Feed this attachment into `gi_temporal.comp` for diffuse/specular/transmission signal modes and lower history weight monotonically with reactivity. `gi_atrous.comp` preserves filtering but prevents highly reactive water from borrowing strong coherent transmission across foam boundaries. On water generation change, invalidate only matching water tokens rather than resetting unrelated scene history.

Expose attachment allocation and water temporal-reset counters in `FrameStats` and renderer diagnostics.

- [ ] **Step 5: Pass foam and temporal stability gates**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_surface_reference_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'water_surface_reference_tests|shader_source_tests|gpu_water_render_tests|vulkan_smoke_tests' --output-on-failure
```

Expected: all tests pass, calm history remains stable, moving whitewater rejects stale history, and no terrain pixel is invalidated by a water-only generation change.

- [ ] **Step 6: Commit the whitewater/temporal slice**

```powershell
git add MatterEngine3/shaders_vk/water_surface.glsl MatterEngine3/shaders_vk/gbuffer.frag MatterEngine3/shaders_vk/gi_temporal.comp MatterEngine3/shaders_vk/gi_atrous.comp MatterEngine3/shaders_vk/rt_lighting.rgen MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/include/matter/world_session.h MatterEngine3/tests/water_surface_reference_tests.cpp MatterEngine3/tests/gpu_water_render_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/tests/shader_source_tests.cpp
git commit -m "feat: add automatic river whitewater and reactivity"
```

### Task 10: Specialize Existing RT Transport for Water High and Ultra

**Files:**
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/shaders_vk/water_surface.glsl`
- Modify: `MatterEngine3/shaders_vk/rt_surface_common.glsl`
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Modify: `MatterEngine3/shaders_vk/composite.frag`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEditor/src/property_editor.cpp`
- Modify: `MatterEngine3/tests/water_surface_reference_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `MatterEngine3/tests/shader_source_tests.cpp`

**Interfaces:**
- Consumes: existing GGX reflection/refraction, entry/exit walk, TIR, path length, transmission denoiser, animated water state, authored optics, and baked depth fallback.
- Produces: `VulkanWaterQuality::{Static,High,Ultra}`, bounded water RT specialization, Beer-Lambert absorption, analytic single scattering, foam transport, diagnostic thickness fallback, optional Ultra rough-lobe/internal-reflection/caustic features, and per-feature GPU counters/timings.

- [ ] **Step 1: Add failing optical, bound, fallback, and quality-tier tests**

CPU reference tests cover Beer-Lambert transmittance at 0/1/10 m, shallow/deep coefficient blending, single-scattering limits, whitewater energy conservation, TIR, actual exit thickness, depth-field fallback, and invalid fallback rejection.

Shader tests assert:

- High has one reflection/refraction path and one analytic scattering evaluation;
- Ultra adds exactly one rough-lobe sample and at most one additional internal reflection;
- caustics use no ray recursion and no unbounded loop;
- geometric normals own offsets/orientation while animated normals own optical lobes; and
- quality changes do not alter field or artifact keys.

- [ ] **Step 2: Run focused tests and verify RED**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_surface_reference_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
```

Expected: no water quality settings or specialized scattering/fallback counters exist.

- [ ] **Step 3: Add explicit renderer quality controls and bounds**

Define:

```cpp
enum class VulkanWaterQuality : std::uint8_t { Static, High, Ultra };
struct VulkanWaterSettings {
    VulkanWaterQuality quality = VulkanWaterQuality::High;
    bool ultra_rough_lobe = true;
    bool ultra_internal_reflection = true;
    bool ultra_shallow_caustics = true;
};
```

Add it to `RenderOptions` and the property system. Pipeline specialization constants resolve to fixed maxima:

```text
Static: 0 water reflection/refraction samples, static generic transmission
High:   1 coherent reflection/refraction path, 1 interior exit walk
Ultra:  High + 1 rough-lobe sample + at most 1 extra internal reflection
```

Toggles must remove their work and measured cost without changing water state, field generation, or authored appearance.

- [ ] **Step 4: Extend the existing bounded refraction path**

At a primary or secondary water hit:

- use the animated shading normal for GGX/Fresnel direction selection;
- use the geometric normal for ray-origin offset, front/back orientation, and entry/exit safety;
- trace the existing bounded interior exit walk and use its path length when valid;
- when an exit is unavailable only because of clipped/open bounded geometry, use the validated baked depth along the refracted direction as a conservative fallback and increment `water_depth_fallbacks`;
- reject a dry, non-finite, zero, or generation-mismatched depth fallback; and
- never hide repeated fallback: expose count/rate and fail acceptance above 0.1% of water transmission rays.

For distance `d`, calculate wavelength-dependent extinction and:

```glsl
vec3 T = exp(-extinction * d);
vec3 L = refracted_radiance * T + in_scattered_sun_sky * (vec3(1.0) - T);
```

Use authored scattering color/distance and bounded anisotropy for the analytic sun/sky term. Foam increases scattering and roughness and reduces coherent transmission while retaining the energy bound.

- [ ] **Step 5: Implement separately switchable Ultra additions**

The rough-lobe sample uses the same water state and one additional bounded GGX direction. The optional internal-reflection allowance handles one additional TIR/inside bounce only. Shallow caustics use the animated-normal focusing determinant, flow-advected phase, direct sun, and validated depth; fade to zero in deep, foam-covered, backlit, or invalid water. It traces no photons and spawns no additional caustic ray.

Add GPU timestamp zones/counters for base water transport, Ultra rough lobe, and shallow caustics. Add a `rt-water` Vulkan smoke mode covering reflection, refraction, actual/fallback thickness, absorption, scattering, foam, TIR, generation replacement, and all three quality modes.

- [ ] **Step 6: Pass RT correctness and validation gates**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_surface_reference_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'water_surface_reference_tests|shader_source_tests|vulkan_smoke_tests' --output-on-failure
```

Expected: all tests pass, every loop/sample bound is asserted, depth fallback is diagnostics-visible, and the `rt-water` smoke reports zero validation errors.

- [ ] **Step 7: Commit the RT water slice**

```powershell
git add MatterEngine3/include/matter/world_session.h MatterEngine3/shaders_vk/water_surface.glsl MatterEngine3/shaders_vk/rt_surface_common.glsl MatterEngine3/shaders_vk/rt_lighting.rgen MatterEngine3/shaders_vk/composite.frag MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEditor/src/property_editor.cpp MatterEngine3/tests/water_surface_reference_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/tests/shader_source_tests.cpp
git commit -m "feat: add bounded ray-traced river optics"
```

### Task 11: Build the Complete RiverFloatLab Evidence and Performance Gate

**Files:**
- Create: `MatterEngine3/tools/river_float_lab.timeline`
- Create: `tools/run-river-float-lab-acceptance.ps1`
- Create: `tools/tests/river_float_package_tests.ps1`
- Modify: `MatterEditor/src/main.cpp`
- Modify: `MatterEditor/src/ui.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `projects/world_demo/scenes/RiverFloatLab/RiverFloatLab.js`
- Modify: `docs/agent/control-surface.md`
- Modify: `docs/agent/qa-cookbook.md`
- Modify: `docs/README.md`
- Modify: `cmake/MatterEngine.cmake`

**Interfaces:**
- Consumes: completed RiverFloatLab, High/Ultra/static quality controls, body tracing, normal performance output, FIFO cameras/steps/screenshots, and package dependency closure.
- Produces: 11 required still views, four 8-frame timed sequences at fixed 30-tick intervals, four contact sheets, body/field/renderer diagnostics, matched static/High/Ultra timing JSON, package/WSL gates, and live-editor acceptance instructions.

- [ ] **Step 1: Add failing package and performance-schema tests**

`river_float_package_tests.ps1` must reject a Ready package missing either field payload, carrying a stale digest, or containing a manifest-unreferenced stale field. It must verify the editor package contains required shaders/material schema while the Linux/WSL-native headless build remains PhysX/CUDA/Vulkan-water optional.

Extend the performance JSON test to require `p99_frame_ms`, water field/shading/RT/Ultra/caustic GPU timings, float median/p99 CPU timings, steady-state allocation count, fallback count/rate, slot count, and water BLAS rebuild count.

- [ ] **Step 2: Run package/schema tests and verify RED**

```powershell
tools/tests/river_float_package_tests.ps1
tools/build-windows.ps1 -Config RelWithDebInfo -Target compiler_portability_tests
```

Expected: the final package contract, p99, and water-specific measurement fields are absent.

- [ ] **Step 3: Implement the fixed evidence timeline**

The timeline captures these named stills from authored matched cameras:

```text
calm-pool.png
upper-rapids.png
crate-raft-draft.png
boulder-wake.png
waterfall-approach.png
waterfall-free-fall.png
plunge-recovery.png
first-pool-settling.png
spillway-traversal.png
lower-rapids.png
player-low-rt.png
```

It then captures `upper-rapid-00..07`, `waterfall-00..07`, `plunge-00..07`, and `spillway-00..07`. Between frames it issues exactly 30 fixed `step` commands while paused, so screenshots are deterministic and body/wave progression is comparable. Every PNG uses the blocking `shot` command and therefore requires its `.done` sidecar.

The PowerShell runner creates four 4 x 2 contact sheets with `System.Drawing`, preserving source resolution/aspect ratio and labelling frame index/tick. It validates all 43 source PNGs, 43 sidecars, and four nonempty contact sheets before success.

- [ ] **Step 4: Extend normal performance output and matched runs**

Add p99 calculation to the existing sorted performance samples. Record explicit GPU zones for water field sampling/shading, base RT transport, Ultra rough lobe, and caustics; record the fixed-tick river-float phase separately on CPU. The acceptance runner executes the same settled 2560 x 1440 camera/timeline three times:

1. `Static` water control;
2. `High` with all required animation/foam/scattering/reactivity;
3. `Ultra` with all three optional additions.

Run at least 10 seconds warmup and 30 seconds sampling through `MATTER_PERF_OUTPUT`, `MATTER_PERF_WARMUP_SECONDS`, and `MATTER_PERF_SAMPLE_SECONDS`. Report total and incremental median/p95/p99, and separately repeat Ultra with rough lobe off and caustics off to prove each measured cost disappears.

- [ ] **Step 5: Enforce the approved target-machine gates**

Fail the runner unless:

```text
High total:  median <= 16.7 ms, p95 <= 22.0 ms, p99 <= 33.3 ms
High delta:  animated water median <= 3.0 ms over matched Static
Ultra total: median <= 33.3 ms, p95 <= 41.7 ms, p99 <= 50.0 ms
Physics:     24-body float median <= 0.5 ms, p99 <= 1.0 ms
```

Also require zero steady allocations, zero non-finite submissions, zero water BLAS rebuilds after Ready, zero validation errors, correct slot/generation, and fallback rate <= 0.1%. These are RTX 4090 acceptance gates, not general minimum hardware claims.

- [ ] **Step 6: Run the complete MSVC, Vulkan, package, and WSL gates**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_engine_cpu_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor -EnablePhysx -PhysxRoot $env:MATTER_PHYSX_ROOT -CudaRoot $env:CUDA_PATH
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -L cpu --output-on-failure
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'vulkan_smoke_tests|physx_fluid_integration_tests' --output-on-failure
tools/tests/river_float_package_tests.ps1
wsl.exe -- bash -lc 'cd /mnt/c/Users/webde/.codex/worktrees/af80/matter-engine-cpp && ./tools/build-windows-from-wsl.sh -Config RelWithDebInfo -Target matter_editor -EnablePhysx'
tools/run-river-float-lab-acceptance.ps1 -PhysxRoot $env:MATTER_PHYSX_ROOT -CudaRoot $env:CUDA_PATH -RunId 20260824-river-float-final
```

Expected: all gates pass; the evidence directory contains stills, sequences, contact sheets, exact settings, traces, diagnostics, package report, and matched timing JSON.

- [ ] **Step 7: Perform the final live-editor acceptance**

Launch the worktree MSVC editor in `RiverFloatLab`, enter Play, and let the user watch the reference crate and raft traverse the river. Keep the live window open after the scripted evidence is complete; scripted screenshots are review evidence, not a replacement for the final user-visible run.

- [ ] **Step 8: Update documentation and commit the acceptance slice**

Document the new binding API, float-body fields, water DSL, renderer quality controls, performance JSON, evidence directory, and fallback counters. Then commit:

```powershell
git add MatterEngine3/tools/river_float_lab.timeline tools/run-river-float-lab-acceptance.ps1 tools/tests/river_float_package_tests.ps1 MatterEditor/src/main.cpp MatterEditor/src/ui.cpp MatterEngine3/include/matter/world_session.h MatterEngine3/src/render/vk_scene_renderer.cpp projects/world_demo/scenes/RiverFloatLab/RiverFloatLab.js docs/agent/control-surface.md docs/agent/qa-cookbook.md docs/README.md cmake/MatterEngine.cmake
git commit -m "test: prove real-time RiverFloatLab acceptance"
```

---

## Final Verification Checklist

- [ ] `git status --short` contains only intentionally retained unrelated pre-existing files.
- [ ] All CPU-labelled MSVC tests pass.
- [ ] Vulkan and `rt-water` smoke tests pass with zero validation errors.
- [ ] PhysX integration and package dependency tests pass.
- [ ] WSL can invoke the Windows MSVC/PhysX build path without manual user work.
- [ ] RiverFloatLab produces all 11 stills, 32 timed frames, four contact sheets, and `.done` sidecars.
- [ ] Reference crate and raft traverse upper rapids, waterfall, pool, spillway, and lower reach in order.
- [ ] Twenty-four bodies remain finite and the float phase allocates nothing at steady state.
- [ ] Static/High/Ultra matched timing reports satisfy every approved RTX 4090 threshold.
- [ ] Water field, physics, raster, and RT report the same generation/digests.
- [ ] Runtime water vertex/index uploads, TLAS geometry changes, and BLAS rebuilds remain zero after Ready.
- [ ] The final live worktree editor run is visible to the user.
