# Baked Water Mesh Animation Implementation Plan

> **For Codex:** Execute this plan in order using `superpowers:executing-plans` and the repository's MSVC build only. Every production change begins with a focused failing test. Commit after each task once its focused tests pass.

**Goal:** Bake one second of PhysX particle motion per river section at 30 Hz, convert the two half-cycle particle phases into 30 seamless GPU-generated water meshes, store them in bounded `.mhwa` artifacts, and play them through a raster-only Vulkan path while retaining the accepted static water mesh as the ray-tracing and failure fallback proxy.

**Architecture:** The DSL opts a network into one fixed animation profile: 30 frames over one second with a half-second phase offset. PhysX keeps a rolling device-side capture ring and performs one host readback after the accepted fill state. Each output frame is meshed from two captured particle sets using per-phase smooth-min weights. Baking writes independently validated section and handoff animation artifacts. Runtime keeps the compressed frames on the CPU, uploads and decodes only a changed 30 Hz frame into the active Vulkan frame slot, draws it directly through a water-specialized raster pipeline, and suppresses only the static proxy's raster submission. Any load, decode, upload, or draw preparation failure restores the static mesh without affecting physics, collision, or ray tracing.

**Toolchain:** C++20, MSVC v143, CMake/CTest through `tools/build-windows.ps1`, PhysX 5 GPU/CUDA interop, packed Vulkan raster decoding, GLSL/SPIR-V, QuickJS world DSL.

**Design authority:** `docs/superpowers/specs/2026-08-26-baked-water-mesh-animation-design.md`.

---

## Invariants to preserve throughout

- Static water bake output, collision meshes, gameplay fields, buoyancy, and ray-tracing geometry remain unchanged.
- Animation authoring accepts exactly 30 fps, 1.0 second, and 0.5 second phase offset in v1.
- A 1/120-second PhysX step yields a capture stride of four steps, 30 retained frames, and a 15-frame phase offset.
- Quarantined or non-finite particles never enter animation capture or animation artifacts.
- Legacy `ParticleJob` callers produce bit-identical field values and mesh digests.
- Every animation frame is an independent indexed mesh; no vertex correspondence is assumed.
- Animation artifacts are immutable, content-digested, size-limited, and published only with a ready network manifest.
- Dynamic animation geometry is raster-only. The accepted static mesh remains the RT proxy and fallback.
- No steady-state allocations and no frame payload upload when the selected 30 Hz frame has not changed.

## Task 1: Add the fixed animation profile to the river DSL

**Files:**

- Modify: `MatterEngine3/include/matter/hydrology.h`
- Modify: `MatterEngine3/src/hydrology/river_network_builder.h`
- Modify: `MatterEngine3/src/hydrology/river_network_builder.cpp`
- Modify: `MatterEngine3/src/script/world_definition_loader.cpp`
- Modify: `projects/world_demo/shared-lib/river_hydrology_definition.js`
- Test: `MatterEngine3/tests/river_network_tests.cpp`
- Test: `MatterEngine3/tests/world_definition_tests.cpp`
- Test: `projects/world_demo/tests/river_hydrology_scene_tests.mjs`

### Step 1: Write failing authoring tests

Add tests which prove:

1. `network.meshAnimation({framesPerSecond: 30, duration: 1.0, phaseOffset: 0.5})` produces an enabled profile with `frame_count == 30`, `sample_step_stride == 4`, and `phase_offset_frames == 15` when `fixed_step_seconds == 1/120`.
2. The profile participates in canonical text and semantic hashing.
3. Missing `meshAnimation` leaves animation disabled and preserves the prior hash fixture.
4. Values other than 30, 1.0, and 0.5 fail with a DSL error naming the rejected property.
5. An incompatible fixed step, including 1/100 second, fails because the 30 Hz samples cannot land on integer simulation steps.
6. The RiverFloat definition opts in with the accepted values.

Build and run the tests, recording the expected failure caused by the missing DSL method:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_network_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_definition_tests
& build/windows/RelWithDebInfo/river_network_tests.exe
& build/windows/RelWithDebInfo/world_definition_tests.exe
node projects/world_demo/tests/river_hydrology_scene_tests.mjs
```

### Step 2: Implement the authoring model

Add `HydrologyMeshAnimationProfile` to `hydrology.h` with authored values and derived integer scheduling fields. Store it on `HydrologyFluidRequest`. Add `RiverNetworkBuilder::set_mesh_animation(...)`, validate the fixed profile and integral step ratio, and include all profile fields in canonical text/hash generation.

Expose the imperative builder method as `network.meshAnimation(object)` in `world_definition_loader.cpp`. Require all three properties, reject unknown/non-finite values through the existing DSL error conventions, and derive schedule fields only after PBD settings are known.

Call the method in `river_hydrology_definition.js` after `network.pbd(...)` and before quality/product settings.

### Step 3: Run focused tests and commit

Repeat the three commands above. Commit:

```text
feat(hydrology): author fixed water mesh animation profile
```

## Task 2: Implement deterministic rolling particle capture in the PhysX adapter

**Files:**

- Create: `MatterEngine3/src/hydrology/water_mesh_animation_capture.h`
- Create: `MatterEngine3/src/hydrology/water_mesh_animation_capture.cpp`
- Modify: `MatterEngine3/src/hydrology/physx_fluid_types.h`
- Modify: `MatterEngine3/src/hydrology/physx_runtime.h`
- Modify: `integrations/physx_adapter/physx_runtime.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Test: `MatterEngine3/tests/water_mesh_animation_capture_tests.cpp`
- Test: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- Modify: `cmake/MatterEngine.cmake`

### Step 1: Write failing capture-schedule tests

Test the pure scheduling and compaction layer before touching CUDA:

- Sampling steps 4 through 120 into a 30-slot ring yields chronological frame steps 4, 8, ..., 120.
- More than 30 samples overwrites only the oldest slot and chronological extraction starts at the correct wrap point.
- A bake that accepts before 30 samples returns `InsufficientAnimationHistory` and does not publish a partial capture.
- Stable particle IDs preserve deterministic order within every frame.
- Quarantined IDs and non-finite positions are excluded from all frames.
- A frame exceeding the configured particle limit fails before allocation growth.
- Disabled animation creates no capture storage and preserves the old callback/event sequence.

Add the target with `matter_add_engine_cpu_test(water_mesh_animation_capture_tests ...)`, build it and run it once to witness the missing API failure.

### Step 2: Add capture output types and pure ring logic

Define:

```cpp
struct FluidParticleAnimationFrame {
    std::uint32_t simulation_step = 0;
    std::vector<gpu_meshing::ParticleSample> particles;
};

struct FluidParticleAnimationCapture {
    std::uint32_t frames_per_second = 0;
    std::uint32_t phase_offset_frames = 0;
    std::vector<FluidParticleAnimationFrame> frames;
};
```

Add `std::optional<FluidParticleAnimationCapture> animation_capture` to `FluidBakeOutput`. Keep scheduling arithmetic and chronological ring indexing in the new engine-core helper so it is testable without PhysX/CUDA.

### Step 3: Add the device-side capture ring

In `physx_runtime.cpp`, allocate 30 slots of `PxVec4` position storage plus per-slot active counts only when animation is enabled. Slots are exact-size for the current particle capacity and grow transactionally with the PhysX particle buffer. Every fourth completed simulation step, enqueue device-to-device copies from the active PhysX position buffer into the next ring slot on the adapter's ordered CUDA stream. Do not synchronize or read back per frame.

At accepted fill completion:

1. Synchronize the stream once.
2. Read the 30 retained slot counts and positions to host.
3. Apply the same finite/quarantine mask and stable ID ordering used by the final snapshot.
4. Rotate the ring into chronological order.
5. Populate `animation_capture` only when all 30 frames validate.

Release all CUDA allocations through the existing adapter cleanup path on success, cancellation, device loss, and validation failure. Add an `AnimationFrameCaptured` diagnostic event carrying only step, slot, and active count; never export private device pointers.

### Step 4: Run focused tests and commit

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_capture_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
& build/windows/RelWithDebInfo/water_mesh_animation_capture_tests.exe
& build/windows/RelWithDebInfo/physx_adapter_contract_tests.exe
```

Commit:

```text
feat(physx): capture rolling water animation frames
```

## Task 3: Add phase-weighted smooth-min evaluation to CPU and GPU meshing

**Files:**

- Modify: `MatterEngine3/include/matter/gpu_visual_meshing.h`
- Modify: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp`
- Modify: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp`
- Modify: `MatterEngine3/shaders_vk/gpu_mesh_common.glsl`
- Modify: `MatterEngine3/shaders_vk/gpu_mesh_field.comp`
- Modify: `MatterEngine3/shaders_vk/gpu_mesh_emit.comp`
- Test: `MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp`
- Test: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp`

### Step 1: Write failing weighted-field tests

Add tests for:

- A default/static job has the exact existing field samples and mesh content digest.
- Primary weight one and secondary weight zero ignores the second phase.
- Primary weight zero and secondary weight one ignores the first phase.
- Two coincident particles with 0.5/0.5 weights equal one full-weight particle within `1e-6`.
- A zero-weight phase contributes nothing and cannot introduce NaN.
- A weighted job rejects split indices beyond particle count, negative/non-finite weights, weights whose sum differs from one beyond tolerance, or zero blend width.
- Vulkan and CPU reference fields agree at selected grid points within existing GPU tolerance.

Build both targets and record the compile failure for the absent phase descriptor.

### Step 2: Extend `ParticleJob` without changing static semantics

Add:

```cpp
struct ParticlePhaseBlend {
    std::uint32_t split_index = 0;
    float primary_weight = 1.0f;
    float secondary_weight = 0.0f;
};
```

Store it at the end of `ParticleJob`. Normalize a default static job to `split_index == particle_count` during validation when `secondary_weight == 0`; animated callers must supply the actual first-phase count. Update the reference evaluator with an overload accepting the blend descriptor. Apply weight inside the smooth-min accumulation and skip zero-weight particles before logarithm/exponential work.

### Step 3: Mirror the descriptor in the Vulkan ABI

Reuse `FieldParams.counts.z` for the resolved split index and `queryRadiusAndPadding.yz` for primary/secondary weights so the uniform block size and descriptor layouts do not change. Add identical particle-weight logic to the shared GLSL and use it in both field and emit/normal evaluation. Add static assertions/comments tying the C++ packing to `gpu_mesh_common.glsl`.

### Step 4: Run focused tests and commit

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_vk_tests
& build/windows/RelWithDebInfo/gpu_visual_mesher_cpu_tests.exe
& build/windows/RelWithDebInfo/gpu_visual_mesher_vk_tests.exe
```

Commit:

```text
feat(meshing): blend dual particle phases on CPU and GPU
```

## Task 4: Generate, pack, and validate `.mhwa` animation artifacts

**Files:**

- Create: `MatterEngine3/src/hydrology/water_mesh_animation.h`
- Create: `MatterEngine3/src/hydrology/water_mesh_animation.cpp`
- Create: `MatterEngine3/src/hydrology/water_mesh_animation_artifact.h`
- Create: `MatterEngine3/src/hydrology/water_mesh_animation_artifact.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Test: `MatterEngine3/tests/water_mesh_animation_tests.cpp`
- Test: `MatterEngine3/tests/water_mesh_animation_artifact_tests.cpp`
- Modify: `cmake/MatterEngine.cmake`

### Step 1: Write failing product and artifact tests

Use synthetic two-phase captures to prove:

- Frame `i` pairs capture `i` with `(i + 15) % 30`.
- Weights are `0.5 - 0.5*cos(2*pi*i/30)` and the complement, and sum to one.
- Exactly 30 independent `MeshResult` frames are requested from the supplied visual mesher.
- Frame failure cancels publication and reports the failing frame index.
- Position quantization/dequantization stays within half a quantization step of source bounds.
- Octahedral normal packing round-trips within an angular tolerance of one degree.
- Serialize/deserialize preserves frame counts, offsets, index data, bounds, and digests.
- Bad magic/version, truncated tables, overflowed offsets, digest mismatch, non-finite bounds, invalid indices, and files above 1 GiB are rejected.
- Save is immutable: identical content at an existing path succeeds; differing bytes do not replace it.

Build and run both targets to record missing-symbol failures.

### Step 2: Implement frame construction

Create a pure phase schedule function and `build_water_mesh_animation(...)`. Concatenate the two `ParticleSample` arrays into one temporary array, set `split_index` to the first array size, assign the analytic weights, and invoke the existing visual mesher once per output frame. Reuse vector capacity across frames. Validate finite geometry and index bounds after every mesh.

### Step 3: Implement the v1 artifact

Use magic `MHYDWAN1`, a fixed little-endian versioned header, a 30-entry frame table, per-frame quantization bounds, and content digests. Pack each vertex into 12 bytes:

- `uint16 x, y, z` in frame-local bounds;
- two signed normalized 16-bit octahedral normal components;
- one 16-bit material index.

Store indices as `uint32`. Validate all additions and multiplications before allocating. Keep serialized bytes below 1 GiB and report `LimitExceeded` before write. Add helpers that expose compressed frame spans without decoding the whole artifact.

### Step 4: Run focused tests and commit

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_artifact_tests
& build/windows/RelWithDebInfo/water_mesh_animation_tests.exe
& build/windows/RelWithDebInfo/water_mesh_animation_artifact_tests.exe
```

Commit:

```text
feat(hydrology): build packed water mesh animation artifacts
```

## Task 5: Publish section and handoff animations with the network manifest

**Files:**

- Modify: `MatterEngine3/src/hydrology/hydrology_network_artifact.h`
- Modify: `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- Modify: `MatterEngine3/src/hydrology/hydrology_handoff_products.h`
- Modify: `MatterEngine3/src/hydrology/hydrology_handoff_products.cpp`
- Modify: `MatterEngine3/src/hydrology/river_section_coordinator.h`
- Modify: `MatterEngine3/src/hydrology/river_section_coordinator.cpp`
- Modify: `MatterEngine3/src/hydrology/local_provider.cpp`
- Test: `MatterEngine3/tests/hydrology_network_artifact_tests.cpp`
- Test: `MatterEngine3/tests/hydrology_handoff_products_tests.cpp`
- Test: `MatterEngine3/tests/river_section_coordinator_tests.cpp`

### Step 1: Write failing publication tests

Add tests proving:

- A ready animation-enabled manifest has one `.mhwa` reference per section and handoff, each with semantic key and payload digest.
- Animation references follow topological order and participate in manifest digesting.
- A section cache hit requires both accepted `.mhyd` and matching `.mhwa`; a missing or corrupt animation artifact rebuilds that section's animation.
- The final manifest remains incomplete when any animation frame or handoff animation fails.
- A failed rebuild never overwrites the last valid immutable artifact or ready manifest.
- Handoff frame meshes are clipped/welded against both adjacent section frames at the same frame index.
- Networks without animation serialize and validate byte-for-byte under the existing version path.

Build and run the three targets to witness the missing manifest fields.

### Step 2: Thread animation products through section execution

Add `HydrologyWaterAnimationReference`, separate section/handoff animation vectors on `HydrologyNetworkArtifact`, optional animation output on `SectionBakeResult`, and animation products/timings on `HydrologyNetworkBakeResult`. Include the animation semantic profile and static upstream/downstream payload digests in cache keys.

After a section reaches accepted fill, build its 30 frames from `FluidBakeOutput::animation_capture`, serialize to `animations/<section-id>-<semantic-key>.mhwa`, then publish the reference. For each handoff, build frame-aligned clipped/welded transition meshes using the same seam planes and tolerance as the static handoff. Only mark the network ready after all static, field, and animation references validate.

### Step 3: Run focused tests and commit

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_section_coordinator_tests
& build/windows/RelWithDebInfo/hydrology_network_artifact_tests.exe
& build/windows/RelWithDebInfo/hydrology_handoff_products_tests.exe
& build/windows/RelWithDebInfo/river_section_coordinator_tests.exe
```

Commit:

```text
feat(hydrology): publish section and handoff water animations
```

## Task 6: Add deterministic runtime playback and CPU decode oracle

**Files:**

- Create: `MatterEngine3/src/render/water_mesh_animation_playback.h`
- Create: `MatterEngine3/src/render/water_mesh_animation_playback.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Test: `MatterEngine3/tests/water_mesh_animation_playback_tests.cpp`
- Modify: `cmake/MatterEngine.cmake`

### Step 1: Write failing playback tests

Test:

- `floor(network_seconds * 30) % 30` chooses the frame, including wrap at one second.
- All sections and handoffs receive the same selected frame.
- Pausing time does not request an upload; advancing within the same 1/30-second interval does not request an upload.
- A newly acquired Vulkan frame slot uploads its current selected frame once even if another slot already has it.
- Packed vertex CPU decode reproduces positions/normals/materials within artifact tolerances.
- Missing, corrupt, over-budget, or mismatched animation references produce a typed fallback result without partial activation.
- Two loaded RiverFloat sections stay below the 700 MiB compressed CPU budget.

Build and run the new target once to witness missing APIs.

### Step 2: Implement immutable runtime state

Load and validate every referenced `.mhwa` transactionally into shared immutable compressed byte storage. Create one common `WaterAnimationClock` per network, one `last_uploaded_frame` per Vulkan frame slot, and a pure frame-selection result containing compressed vertex/index spans for every visible section and handoff. Reserve all per-slot metadata at activation and perform no steady-state heap allocation.

Implement the CPU decode oracle for tests and debug validation; production raster playback remains GPU-decoded.

### Step 3: Run focused tests and commit

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_playback_tests
& build/windows/RelWithDebInfo/water_mesh_animation_playback_tests.exe
```

Commit:

```text
feat(render): load and schedule baked water animation
```

## Task 7: Add packed GPU decode and raster-only dynamic water drawing

> Acceptance optimization (2026-08-26): the initial compute expansion was
> visually correct but measured about 6 ms for RiverFloatLab. The final path
> keeps the same GPU decode contract but performs it in `raster_water.vert`
> directly from the packed 12-byte vertex buffer. A second measurement showed
> the replacement staging-to-device copies still dominated at about 5.5 ms,
> so the final frame-slot buffers are persistently mapped and GPU-visible:
> changed frames write them once, then raster reads them directly. This
> eliminates the 28-byte expanded buffer, compute descriptors/pipeline, and
> both redundant full-frame device copies. The focused tests below were
> updated to gate this lower-bandwidth path.

**Files:**

- Modify: `MatterEngine3/shaders_vk/raster.vert`
- Modify: `MatterEngine3/shaders_vk/gbuffer.frag`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `CMakeLists.txt`
- Modify: `MatterEngine3/Makefile`
- Test: `MatterEngine3/tests/gpu_water_animation_render_tests.cpp`
- Test: `MatterEngine3/tests/shader_source_tests.cpp`
- Modify: `cmake/MatterEngine.cmake`

### Step 1: Write failing shader and Vulkan contract tests

Add tests for:

- The packed 12-byte vertex ABI and 28-byte CPU decode-oracle ABI.
- The shader inventory contains the `raster_water.vert.spv` specialization and no obsolete water compute decoder.
- Packed vertex-shader decode matches the CPU oracle on a known triangle.
- Upload occurs once per selected frame per Vulkan frame slot, not once per render frame.
- Host writes precede vertex/index reads with explicit host-write barriers.
- Direct indexed draws use the static proxy instance transform and the existing water material/field descriptors.
- Dynamic water vertices are never submitted to BLAS build/update paths.
- Resource replacement is generation-checked and old resources survive until their owning frame fences retire.

Build the test and shader targets and record the expected missing shader/pipeline failures.

### Step 2: Implement packed upload and vertex-shader decode

Compile a `MATTER_WATER_ANIMATION_VERTEX_INPUT` specialization of `raster.vert`. The vertex shader reads a 12-byte `uvec3`, decodes frame-local uint16 positions and oct normals from per-draw push bounds, and forwards the material index. Keep the following expanded record only as the CPU oracle used by tests:

```cpp
struct VkWaterAnimationVertex {
    matter::Float3 position;
    matter::Float3 normal;
    std::uint32_t material_index;
};
```

Allocate persistently mapped, host-visible, preferably device-local packed vertex and index buffers per Vulkan frame slot at activation using the maximum loaded frame sizes. On a changed selected frame, write packed vertices and indices and issue explicit host-write-to-raster-read barriers. If the frame is unchanged in that slot, reuse the buffers with no upload. There is no compute expansion or decoded GPU buffer.

### Step 3: Implement the direct raster draw

Extend renderer frame preparation with `VkWaterAnimationRasterDraw` records. Bind the water-specialized vertex pipeline after existing static/skinned draws in the G-buffer pass, bind the packed vertex and current index buffers, push decode bounds plus the proxy instance transform/material indices, and issue `vkCmdDrawIndexed`. Feed existing `gbuffer.frag`/`water_surface.glsl` descriptors so transparency, depth fog, animated normals, and foam fields stay on the accepted material path.

Add a runtime per-part `rt_proxy_only` raster-suppression flag. The culling/raster path skips proxy geometry while animation is healthy; ray tracing selection ignores this flag. Do not add dynamic BLAS resources or updates.

Update both CMake discovery and the Makefile inventory so their equality guard continues to pass.

### Step 4: Run focused tests and commit

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_animation_render_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
& build/windows/RelWithDebInfo/gpu_water_animation_render_tests.exe
& build/windows/RelWithDebInfo/shader_source_tests.exe
```

Commit:

```text
feat(vulkan): draw GPU-decoded animated water meshes
```

## Task 8: Integrate animation activation, time, and static fallback in the engine

**Files:**

- Modify: `MatterEngine3/src/render/gpu_meshing/water_scene_part.h`
- Modify: `MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Test: `MatterEngine3/tests/gpu_water_render_tests.cpp`
- Test: `MatterEngine3/tests/river_runtime_tests.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

### Step 1: Write failing integration/failure tests

Add tests proving:

- A ready animation manifest activates compressed playback after the static water proxy and water field are published.
- The engine passes real network time to the common 30 Hz clock.
- Healthy animation marks only the visual static water part raster-suppressed.
- Buoyancy, collision, gameplay field bindings, and static proxy hashes are identical with animation enabled or disabled.
- Artifact load failure, GPU allocation failure, decode failure, stale generation, and device loss all disable animation atomically and restore static raster visibility.
- A later valid bake generation can reactivate animation after fallback.
- Shutdown/reload retires animation resources without use-after-free.

Build and run the three targets to witness the missing activation path.

### Step 2: Publish transactionally from `matter_engine.cpp`

Extend `AuthoredFluidRenderBinding` with immutable animation runtime state, generation, and proxy part/instance identifiers. Publish in this order:

1. Accepted static water part and instance.
2. Existing packed water field.
3. Validated animation artifact set.
4. Vulkan per-slot resources.
5. Raster suppression of the static proxy.

Any failure before step five leaves the existing static path visible. Any failure after activation clears dynamic draw records first and then restores proxy raster visibility. Advance only the common playback time during normal frames; do not touch physics or bake state.

### Step 3: Run focused tests and commit

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_runtime_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
& build/windows/RelWithDebInfo/gpu_water_render_tests.exe
& build/windows/RelWithDebInfo/river_runtime_tests.exe
& build/windows/RelWithDebInfo/vulkan_smoke_tests.exe
```

Commit:

```text
feat(engine): activate baked water animation with static fallback
```

## Task 9: Bake RiverFloat, measure budgets, and capture visual proof

**Files:**

- Modify only if thresholds need codification: `projects/world_demo/shared-lib/river_hydrology_definition.js`
- Add generated evidence under the repository's existing QA/findings convention; do not commit binary cache artifacts unless that convention explicitly tracks them.
- Document measurements in: `docs/findings/2026-08-26-baked-water-mesh-animation-acceptance.md`

### Step 1: Run the full MSVC verification gate

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build/windows -C RelWithDebInfo -L cpu --output-on-failure
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor -EnablePhysX ON -PhysXGpu ON
```

Run the PhysX integration and Vulkan smoke labels available in this checkout using `ctest -N` to discover their exact label names, then run them with `--output-on-failure`. Do not substitute GCC/Make binaries.

### Step 2: Bake and validate RiverFloat animation

Use the documented FIFO/QA recipe to load RiverFloat and request a fresh animation-enabled bake. Record per section:

- accepted PhysX step and retained capture step range;
- particle counts per capture frame;
- each mesh frame's vertices/triangles;
- capture, meshing, serialization, and total bake time;
- `.mhwa` size and digest;
- compressed CPU bytes and per-visible-section Vulkan bytes.

Fail acceptance if any section artifact exceeds 300 MiB, two loaded sections exceed 700 MiB compressed CPU storage, or a visible section exceeds 96 MiB extra GPU storage excluding the static BLAS proxy.

### Step 3: Measure playback performance

After warm-up, collect at least 600 rendered frames on the RTX 4090. Report median and p95 for upload + decode + direct water draw. Require median at or below 1.5 ms and p95 at or below 3.0 ms. Confirm with counters that unchanged 30 Hz frames issue no upload/dispatch and that there are no steady-state allocations.

### Step 4: Capture visual proof

Capture matched camera screenshots and a one-second-or-longer sequence at:

- shallow upstream channel with the riverbed visible;
- boulder rapid with visible moving/foaming water;
- curved ravine view showing spatial continuity;
- waterfall into the first pool;
- first-pool spillway and second-section transition;
- floating crates/rafts over the animated surface.

Also force one artifact-load failure and capture the static fallback, proving the scene remains usable. Inspect frame 29 to frame 0 and the half-cycle overlap for popping, density pulsing, cracks, or exposed terrain.

### Step 5: Document, verify git state, and commit

Write exact commands, hardware, artifact sizes, timings, counters, and screenshot paths into the acceptance finding. Run `git diff --check` and confirm no unrelated user files were added. Commit:

```text
test(hydrology): verify baked water mesh animation
```

## Completion gate

Before claiming completion:

1. Use `superpowers:verification-before-completion` and rerun all commands needed to support the final claims.
2. Request a code review using `superpowers:requesting-code-review`; address verified findings before proceeding.
3. Use `superpowers:finishing-a-development-branch` to present integration options without modifying unrelated user work.
4. Report the actual artifact sizes, bake time, median/p95 playback cost, test counts, screenshots, commit list, and any remaining limitations. Do not describe the feature as complete if RiverFloat has not produced a valid 30-frame artifact and visible loop.
