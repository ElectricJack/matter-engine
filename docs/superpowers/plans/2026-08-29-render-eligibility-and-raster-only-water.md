# Render Eligibility and Raster-Only Water Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver the authored `rayTraced` inheritance contract and guarantee
that accepted, animated, fallback, debug, and acceptance water surfaces remain
raster-visible while doing no animated-water RT decode, BLAS, cache, or TLAS
work.

**Architecture:** A public boolean authoring surface resolves through an
internal tri-state instance override. Part defaults and child-placement
overrides live in a small deterministic `RNDR` PartBundle section, leaving the
stable geometry/child record layouts unchanged. Static and dynamic publication
resolve to the renderer's existing per-instance `VkSceneInstance::ray_traced`
boolean; the Vulkan renderer remains policy-free and continues to filter RT
instances after raster staging.

**Tech Stack:** C++20, QuickJS DSL, PartBundle artifacts, Flecs scene recipes,
Vulkan raster/RT renderer, CMake/Ninja/MSVC tests.

**Spec:**
`docs/superpowers/specs/2026-08-28-render-eligibility-and-document-lifecycle-design.md`

## Global Constraints

- The authored API is a boolean named `rayTraced`; only the internal instance
  representation may be tri-state.
- Resolution order is explicit instance override, referenced part default,
  then engine default `true`.
- `rayTraced: false` does not change visibility, collision, or shadow-casting
  settings. It removes only RT geometry/TLAS participation.
- A raster-only instance remains in raster staging and receives the normal
  raster lighting and shadow composition.
- Mixed instances of one part are supported: one eligible instance may create
  and share the part BLAS while another stays out of TLAS.
- Animated water is pinned false in active animation and static fallback. No
  animated-water RT vertex decode, BLAS construction/cache, or TLAS entry may
  remain.
- Preserve the dirty worktree's unrelated hydrology, transmitted-shadow, and
  water-receiver fixes. Do not reset or replace whole renderer files from
  `HEAD`.
- New behavior follows red-green-refactor and is verified with the native
  Windows MSVC CMake graph.

---

### Task 1: Persist part defaults and child-placement overrides

**Files:**

- Create: `MatterEngine3/include/matter/render_eligibility.h`
- Create: `MatterEngine3/src/part_render_policy.h`
- Modify: `MatterEngine3/src/part_bundle.h`
- Modify: `MatterEngine3/src/part_base.js.h`
- Modify: `MatterEngine3/src/dsl_state.h`
- Modify: `MatterEngine3/src/dsl_state.cpp`
- Modify: `MatterEngine3/src/dsl_bindings.cpp`
- Modify: `MatterEngine3/src/script_host.cpp`
- Modify: `MatterEngine3/src/render/part_store.h`
- Modify: `MatterEngine3/src/render/part_store.cpp`
- Test: `MatterEngine3/tests/script_host_tests.cpp`
- Test: `MatterEngine3/tests/partstore_tests.cpp`

**Interfaces:**

- Produces:
  `enum class RayTracingOverride : uint8_t { Inherit, Disabled, Enabled };`
- Produces:
  `bool resolve_ray_traced(RayTracingOverride override_value,
  bool part_default = true) noexcept`.
- Produces: `PartRenderPolicy { bool ray_traced = true;
  std::vector<RayTracingOverride> child_overrides; }`.
- Produces: `save_part_render_policy(path, resolved_hash, policy)` and
  `load_part_render_policy(path, resolved_hash, expected_child_count, policy)`.
- Consumes: QuickJS calls `this.rayTraced(bool)` and
  `this.placeChild(module, params, { rayTraced: bool })`.

- [ ] **Step 1: Write the failing artifact and DSL tests.**

  Add tests that assert all of the following state, not only return codes:

  ```cpp
  CHECK(resolve_ray_traced(RayTracingOverride::Inherit, true));
  CHECK(!resolve_ray_traced(RayTracingOverride::Inherit, false));
  CHECK(!resolve_ray_traced(RayTracingOverride::Disabled, true));
  CHECK(resolve_ray_traced(RayTracingOverride::Enabled, false));
  ```

  Bake a parent that calls `this.rayTraced(false)`, places three children with
  omitted/false/true options, load its `RNDR` policy, and assert the exact
  sequence `{Inherit, Disabled, Enabled}`. Bake a legacy fixture with no `RNDR`
  section and assert the compatibility result is `ray_traced == true` with all
  child overrides inherited. Corrupt an existing `RNDR` byte and assert the
  coherent PartStore snapshot rejects it instead of silently enabling RT.

- [ ] **Step 2: Run the focused tests and verify the red state.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target script_host_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target partstore_tests
  ```

  Expected: compilation or assertions fail because the enum, DSL methods, and
  policy section do not exist.

- [ ] **Step 3: Implement the minimal deterministic policy format.**

  Add `kSectionRenderPolicy = 0x52444E52u` (`"RNDR"`) to PartBundle. Encode the
  payload manually, without native struct padding, as:

  ```text
  u32 magic 'MRTP'
  u32 version 1
  u64 resolved_hash
  u8  part_ray_traced (0 or 1)
  u32 child_count
  u8[child_count] override (0 inherit, 1 disabled, 2 enabled)
  ```

  A missing section is the backward-compatible true/inherit policy. A present
  but malformed section is an invalid artifact. Write `RNDR` before `REP0` so
  an interrupted first bake leaves no geometry-only false-policy artifact that
  can later be accepted as default true.

- [ ] **Step 4: Bind and retain the authored values.**

  Add this exact JS method:

  ```js
  rayTraced(value) { __dsl_rayTraced(value); }
  ```

  Reject non-boolean values. Store a default-true bool on `DslState` and a
  `RayTracingOverride` on each `ChildPlacement`; omitted `rayTraced` remains
  `Inherit`. Carry `PartRenderPolicy` through `script_host::BakedGeometry`,
  `PartStore::CoherentSnapshot`, the retained-geometry fast path, and
  `LoadedPart`.

- [ ] **Step 5: Resolve static expansion eligibility.**

  Append `bool ray_traced = true` to `viewer::ExpandedNode`. Change the private
  part-tree walk so each visited node resolves its incoming edge override
  against that node's `LoadedPart::render_policy.ray_traced`. A child's
  override applies to that placement only; its descendants use their own edge
  overrides and referenced defaults.

- [ ] **Step 6: Run focused tests and commit.**

  Run both targets from Step 2 and their executables. Expected: all pass.

  ```powershell
  git add MatterEngine3/include/matter/render_eligibility.h MatterEngine3/src/part_render_policy.h MatterEngine3/src/part_bundle.h MatterEngine3/src/part_base.js.h MatterEngine3/src/dsl_state.h MatterEngine3/src/dsl_state.cpp MatterEngine3/src/dsl_bindings.cpp MatterEngine3/src/script_host.cpp MatterEngine3/src/render/part_store.h MatterEngine3/src/render/part_store.cpp MatterEngine3/tests/script_host_tests.cpp MatterEngine3/tests/partstore_tests.cpp
  git commit -m "feat(render): persist authored ray tracing eligibility"
  ```

---

### Task 2: Resolve authored scene and dynamic-instance eligibility

**Files:**

- Modify: `MatterEngine3/include/matter/scene.h`
- Modify: `MatterEngine3/src/ecs/scene_registry.cpp`
- Modify: `MatterEngine3/src/ecs/dynamic_scene_bridge.cpp`
- Modify: `MatterEngine3/src/render/dynamic_instance_slots.h`
- Modify: `MatterEngine3/src/render/dynamic_instance_slots.cpp`
- Modify: `MatterEngine3/src/render/animation_rigid_bridge.h`
- Modify: `MatterEngine3/src/render/animation_rigid_bridge.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `cmake/MatterEngine.cmake`
- Test: `MatterEngine3/tests/entity_recipe_tests.cpp`
- Test: `MatterEngine3/tests/scene_registry_tests.cpp`
- Test: `MatterEngine3/tests/dynamic_instance_slots_tests.cpp`
- Test: `MatterEngine3/tests/dynamic_scene_bridge_tests.cpp`
- Test: `MatterEngine3/tests/animation_rigid_bridge_tests.cpp`

**Interfaces:**

- Consumes: `RayTracingOverride`, `resolve_ray_traced`, and
  `LoadedPart::render_policy` from Task 1.
- Produces: `PartInstance::ray_traced`, defaulting to `Inherit`.
- Produces: final `bool ray_traced` on each renderer-facing dynamic slot
  change; renderer policy remains a resolved bool.

- [ ] **Step 1: Write failing scene and slot tests.**

  Parse recipes with absent, false, and true `rayTraced` and assert
  `Inherit`, `Disabled`, and `Enabled`. Add a slot test where eligibility alone
  changes and assert exactly one update is emitted. Add an articulated binding
  test proving an entity override is copied to every generated rigid segment
  and attachment.

- [ ] **Step 2: Verify the focused targets fail before implementation.**

  Add native MSVC targets for the currently Make-only focused suites, then run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target entity_recipe_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target dynamic_instance_slots_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target dynamic_scene_bridge_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target animation_rigid_bridge_tests
  ```

- [ ] **Step 3: Parse the boolean into the internal tri-state.**

  Append the enum field so existing `{hash, visible, casts_shadow}` aggregate
  initializers retain inheritance. The descriptor presents an internal
  three-option enum (`Inherit`, `Raster only`, `Ray traced`), while authored
  recipe JSON accepts only the boolean `rayTraced` key. Register the enum with
  Flecs and preserve it through edit/play snapshots.

- [ ] **Step 4: Carry unresolved policy through the dynamic bridge.**

  Extend `DynamicInstanceInput`, `DynamicSlotChange`, and slot state with the
  tri-state plus a `policy_part_hash`. Root and articulated records use the
  source entity's `PartInstance::part_hash` as `policy_part_hash`; this makes a
  false/default policy apply to every moving subpart rather than accidentally
  inheriting a generated segment part's engine-true default.

- [ ] **Step 5: Resolve before calling the renderer.**

  In `matter_engine.cpp`, load `policy_part_hash` from `PartStore` while
  validating Bind/Transform changes, resolve the tri-state against that
  `LoadedPart` default, and hand the renderer a final bool. Add a parallel
  `dynamic_instance_ray_traced_` vector. Bind/Transform update it; Remove resets
  it. Dynamic instances always enter raster staging, and enter the appended
  `RtInstance` tail only when the final bool is true.

- [ ] **Step 6: Publish static expansion values.**

  When converting `ExpandedNode` or a leaf `LoadedPart` to
  `VkSceneInstance`, assign the resolved `ray_traced` value. Include it in any
  instance-cache fingerprint/snapshot that can otherwise reuse stale policy.

- [ ] **Step 7: Run tests and commit.**

  Run the four targets from Step 2 plus `scene_registry_tests`. Expected: all
  pass.

  ```powershell
  git add MatterEngine3/include/matter/scene.h MatterEngine3/src/ecs/scene_registry.cpp MatterEngine3/src/ecs/dynamic_scene_bridge.cpp MatterEngine3/src/render/dynamic_instance_slots.h MatterEngine3/src/render/dynamic_instance_slots.cpp MatterEngine3/src/render/animation_rigid_bridge.h MatterEngine3/src/render/animation_rigid_bridge.cpp MatterEngine3/src/matter_engine.cpp MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp cmake/MatterEngine.cmake MatterEngine3/tests/entity_recipe_tests.cpp MatterEngine3/tests/scene_registry_tests.cpp MatterEngine3/tests/dynamic_instance_slots_tests.cpp MatterEngine3/tests/dynamic_scene_bridge_tests.cpp MatterEngine3/tests/animation_rigid_bridge_tests.cpp
  git commit -m "feat(render): resolve instance ray tracing eligibility"
  ```

---

### Task 3: Remove animated-water RT work and pin every water path raster-only

**Files:**

- Delete: `MatterEngine3/shaders_vk/water_animation_rt_decode.comp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEngine3/src/render/water_animation_gpu.h`
- Modify: `MatterEngine3/src/render/water_animation_gpu.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp`
- Modify: `MatterEngine3/src/render/gpu_meshing/water_scene_part.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/shaders_vk/rt_surface_common.glsl`
- Test: `MatterEngine3/tests/gpu_water_render_tests.cpp`
- Test: `MatterEngine3/tests/gpu_water_animation_render_tests.cpp`
- Test: `MatterEngine3/tests/shader_source_tests.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**

- Consumes: final renderer-facing `VkSceneInstance::ray_traced` from Tasks 1-2.
- Produces: `set_water_scene_animation_active(proxy, active)` where
  `proxy.ray_traced` remains false and only raster proxy suppression toggles.

- [ ] **Step 1: Invert the current positive RT-water tests.**

  With global RT enabled, assert active animated water has a raster direct draw
  but all of these stay zero/absent:

  ```cpp
  CHECK(renderer.water_animation_decode_dispatch_count() == 0u);
  CHECK(renderer.test_last_rt_blas_build_count() == 0u);
  CHECK(renderer.rt_tlas_build_count() == tlas_builds_before);
  CHECK(std::none_of(records.begin(), records.end(),
      [hash](const auto& record) { return record.part_hash == hash; }));
  ```

  On fallback, assert the accepted static water raster returns while the
  instance remains `ray_traced == false`.

- [ ] **Step 2: Verify the new tests fail against the dirty RT-water path.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_animation_render_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  ```

- [ ] **Step 3: Remove only animated-water RT additions.**

  Delete the RT decode shader and remove its build entry. Remove RT decode
  structs/indices/barriers/buffer usages from `water_animation_gpu`; remove RT
  buffers, compute pipeline, per-frame BLAS cache, counters, special water RT
  proxy, decode/build/emit functions, lifetime retention, and TLAS insertion
  from `VkSceneRenderer`. Restore the generic single
  `if (source.ray_traced)` RT candidate branch and the RT gate based only on
  `rt_instances_`.

  Preserve unrelated dirty edits in `rt_shadow.rgen`, `rt_lighting.rgen`, and
  later Vulkan transmission/visibility smoke scenarios.

- [ ] **Step 4: Pin all engine-generated water instances false.**

  Set `ray_traced = false` for accepted authored water, active animation,
  static fallback, failed-bake debug water, and GPU-mesher acceptance water at
  their creation/publication seams. `rt_proxy_only` means only “hide the
  immutable raster proxy while direct animated raster owns the surface.”

- [ ] **Step 5: Strengthen the source-level negative contract.**

  Assert source/build text contains none of:

  ```text
  water_animation_rt_decode.comp.spv
  prepare_water_animation_blas
  water_animation_current_blas_
  WaterAnimationRtDecodeHeader
  ```

- [ ] **Step 6: Run focused tests and commit.**

  Run all targets from Step 2. Run Vulkan smoke mode `water-animation` on the
  available RT device. Expected: raster animation and fallback pass; validation
  is clean; animated-water decode, BLAS-cache bytes, builds, and TLAS entries
  are zero.

  ```powershell
  git add MatterEngine3/Makefile MatterEngine3/src/render/water_animation_gpu.h MatterEngine3/src/render/water_animation_gpu.cpp MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp MatterEngine3/src/render/gpu_meshing/water_scene_part.h MatterEngine3/src/matter_engine.cpp MatterEngine3/shaders_vk/rt_surface_common.glsl MatterEngine3/tests/gpu_water_render_tests.cpp MatterEngine3/tests/gpu_water_animation_render_tests.cpp MatterEngine3/tests/shader_source_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
  git commit -m "perf(water): keep animated water out of ray tracing"
  ```

---

### Task 4: Prove false-only and mixed-instance behavior

**Files:**

- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `docs/findings/spec-implementation-gap-audit-2026-08-28.md`
- Create: `docs/findings/render-eligibility-acceptance-2026-08-29.md`

**Interfaces:**

- Consumes: authored/static/dynamic eligibility and raster-only water from
  Tasks 1-3.
- Produces: the measured acceptance evidence required by the approved design.

- [ ] **Step 1: Add a false-only synthetic Vulkan scenario.**

  Register one triangle part and one false instance. Assert the raster command
  count is one, RT geometry records are empty, BLAS build count is zero, and
  TLAS build count does not advance.

- [ ] **Step 2: Add a mixed shared-part scenario.**

  Register two placements of the same part, one false and one true. Assert two
  raster placements, exactly one RT instance/TLAS record, and exactly one
  shared per-part BLAS build.

- [ ] **Step 3: Run MSVC CPU and Vulkan gates.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo
  & 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -L cpu --output-on-failure
  $env:MATTER_VK_SMOKE_MODE='water-animation'; & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

- [ ] **Step 4: Record evidence and commit.**

  Record exact commands, device/driver, pass/fail totals, false-only and mixed
  counts, active/fallback water counters, validation status, and remaining
  scope. Update the gap audit so animated-water BLAS caching is marked removed,
  not merely planned.

  ```powershell
  git add MatterEngine3/tests/vulkan_smoke_tests.cpp docs/findings/spec-implementation-gap-audit-2026-08-28.md docs/findings/render-eligibility-acceptance-2026-08-29.md
  git commit -m "test(render): prove raster-only RT eligibility"
  ```

