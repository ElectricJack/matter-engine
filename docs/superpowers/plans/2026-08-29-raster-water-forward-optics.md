# Raster Water Forward Optics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace G-buffer-only water shading with one parity-quality forward
water pass that preserves the opaque scene for screen-space optics, renders
both active animated water and its accepted static fallback, and retains the
existing HDR/depth/temporal presentation contract.

**Architecture:** First complete and verify
`docs/superpowers/plans/2026-08-29-render-eligibility-and-raster-only-water.md`;
this plan assumes animated water no longer performs RT decode, BLAS, cache, or
TLAS work. The opaque G-buffer, RT lighting, volumetrics, and composite remain
in their current order. After the opaque composite, copy HDR and depth into
sample-only preservation images, then issue static-water indirect ranges and
active-water direct draws through two vertex-input variants of one forward
fragment shader, loading and updating the existing HDR, depth, velocity,
reactivity, and identity targets.

**Tech Stack:** C++20, Vulkan 1.3 dynamic rendering, GLSL 460, embedded SPIR-V,
Python 3 acceptance tooling, CMake/Ninja/MSVC tests.

**Spec:**
`docs/superpowers/specs/2026-08-28-render-eligibility-and-document-lifecycle-design.md`

## Global Constraints

- This plan starts only after
  `docs/superpowers/plans/2026-08-29-render-eligibility-and-raster-only-water.md`
  passes its CPU and `water-animation` Vulkan gates.
- Animated water remains raster-only. The forward pass must not restore any
  water RT decode, BLAS, cache, TLAS entry, or water proxy shadow.
- Active animation draws the packed direct-water frame. If the animation frame
  is unavailable or rejected, the accepted immutable proxy draws through the
  same forward optics shader.
- Opaque G-buffer, RT lighting, volumetrics, and composite results are complete
  before water. Water must preserve them in sampled HDR/depth images before it
  writes the main HDR/depth targets.
- The final main `hdr_`, `depth_`, `velocity_`, `reactivity_`, and
  `material_instance_` images remain the inputs consumed by DLSS, display, and
  picking. Do not add a second presentation funnel.
- Water owns no second primary-shadow evaluation. Its copied scene color is
  already shadowed, and first-plan RT eligibility keeps water out of the RT
  caster set.
- Foam is driven primarily by baked `foam_potential` (the hydrology product
  derived from turbulence/whitewater). Aeration and feature class may support
  that signal but may not create full foam by themselves.
- Reflection uses bounded screen-space ray marching first and the existing
  physical environment/sky on a miss. Refraction uses copied opaque color and
  copied screen depth, with baked field depth as the invalid-screen fallback.
- Preserve unrelated dirty-worktree hydrology, transmitted-shadow, and
  receiver fixes. Apply narrow hunks; never replace renderer or shader files
  wholesale from `HEAD`.
- Checkpoint commit steps are conditional. Run them only from a clean isolated
  implementation worktree. When executing in the current shared dirty
  worktree, skip every commit step and do not stage files that already contain
  unrelated hunks.
- Use red-green-refactor. Each task ends with a focused passing gate before the
  next task starts.

---

### Task 1: Establish the prerequisite and capture the raster-only baseline

**Files:**

- Read: `docs/superpowers/plans/2026-08-29-render-eligibility-and-raster-only-water.md`
- Read: `docs/findings/render-eligibility-acceptance-2026-08-29.md`
- Create: `MatterEngine3/tools/raster_water_forward_perf.timeline`
- Create during execution: `build/qa/raster-water-forward-2026-08-29/baseline/`

**Interfaces:**

- Consumes: a renderer in which active and fallback water are raster-visible,
  `water_animation_decode_dispatch_count() == 0`, no water BLAS is built or
  cached, and no water instance enters TLAS.
- Produces: three before-change JSON measurements named
  `shadow-01.json`, `shadow-10.json`, and `shadow-16.json`.

- [ ] **Step 1: Verify the first plan's implementation, not merely its file.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_animation_render_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  $env:MATTER_VK_SMOKE_MODE='water-animation'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

  Expected: all four targets build, the smoke process exits zero with zero
  validation errors, active direct raster and static fallback both pass, and
  decode/BLAS-cache/build/TLAS water assertions are zero.

- [ ] **Step 2: Write the fixed performance timeline.**

  Create `MatterEngine3/tools/raster_water_forward_perf.timeline` with exactly:

  ```text
  wait_event bake.finished 3600
  play
  wait_frames 10
  cam 45 72 2 78 60 -3
  wait_idle 5 3600
  ```

  It intentionally has no `quit`: the editor's existing
  `MATTER_PERF_WARMUP_SECONDS`/`MATTER_PERF_SAMPLE_SECONDS` harness owns the
  sampling interval and exits after writing JSON.

- [ ] **Step 3: Build the editor used by both baseline and candidate runs.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor
  ```

  Expected: exit code zero and
  `MatterEditor/build/windows-msvc/editor.exe`
  exists.

- [ ] **Step 4: Capture the three matched raster-only baselines.**

  Run these from the repository root, changing only the shadow sample count
  and output paths:

  ```powershell
  $editor = (Resolve-Path 'MatterEditor/build/windows-msvc/editor.exe').Path
  $base = (New-Item -ItemType Directory -Force 'build/qa/raster-water-forward-2026-08-29/baseline').FullName
  foreach ($samples in 1, 10, 16) {
      $tag = '{0:D2}' -f $samples
      py -3 MatterEngine3/tools/drive.py --world RiverFloatLab `
          --timeline MatterEngine3/tools/raster_water_forward_perf.timeline `
          --out-dir "$base/run-$tag" --timeout 4200 --editor $editor --hide-ui `
          --env "MATTER_SUN_SHADOW_SAMPLES=$samples" `
          --env "MATTER_PERF_OUTPUT=$base/shadow-$tag.json" `
          --env MATTER_PERF_WARMUP_SECONDS=20 `
          --env MATTER_PERF_SAMPLE_SECONDS=30
      if ($LASTEXITCODE -ne 0) { throw "baseline shadow-$tag failed" }
  }
  ```

  Expected: each JSON has `validation_errors: 0`,
  `water_animation_decode_dispatch_delta: 0`, and
  `water_animation_steady_state_allocation_delta: 0`.

- [ ] **Step 5: Record a checkpoint without changing implementation files.**

  Save the three JSON files as local QA evidence. They remain under `build/`
  and are not added to source control.

---

### Task 2: Define deterministic optics and turbulence-primary foam contracts

**Files:**

- Modify: `MatterEngine3/src/render/water_surface_reference.h`
- Modify: `MatterEngine3/src/render/water_surface_reference.cpp`
- Modify: `MatterEngine3/shaders_vk/water_surface.glsl`
- Test: `MatterEngine3/tests/water_surface_reference_tests.cpp`

**Interfaces:**

- Produces:
  `WaterOpticalState water_optical_state_reference(const matter::WaterSurfaceDefinition&, float optical_distance_m, float foam_coverage) noexcept`.
- Produces:
  `float water_foam_driver_reference(float foam_potential, float turbulence, float aeration, hydrology::RiverFeature feature) noexcept`.
- Produces GLSL:
  `WaterOpticalState water_evaluate_optics(WaterFieldGpuRecord record, float optical_distance_m, float foam_coverage)`.
- Produces GLSL:
  `float water_foam_driver(WaterFieldSample field)`.
- Preserves: `water_evaluate_surface(...)` and
  `water_evaluate_surface_reference(...)` signatures for existing callers.

- [ ] **Step 1: Write failing optical-depth tests.**

  Add cases that call `water_optical_state_reference` with identical authored
  optics and distances `0.25f`, `0.75f`, `3.0f`, and `8.0f`. Assert every
  transmittance component is finite and in `[0, 1]`, each component is
  monotonically non-increasing with distance, the `0.75f` result retains more
  luminance than the `3.0f` result, and foam coverage `1.0f` reduces coherent
  transmission relative to foam coverage `0.0f`.

- [ ] **Step 2: Write failing foam-dominance tests.**

  Use the same feature and assert these exact relationships:

  ```cpp
  const float turbulent = water_foam_driver_reference(
      0.85f, 0.90f, 0.10f, hydrology::RiverFeature::Calm);
  const float aerated = water_foam_driver_reference(
      0.05f, 0.05f, 1.00f, hydrology::RiverFeature::Calm);
  const float feature_only = water_foam_driver_reference(
      0.00f, 0.00f, 0.00f, hydrology::RiverFeature::Waterfall);
  CHECK(turbulent > 0.80f);
  CHECK(aerated <= 0.25f);
  CHECK(feature_only <= 0.15f);
  CHECK(turbulent > aerated && aerated >= feature_only);
  ```

  Add a matching surface-evaluation case proving that high baked
  `foam_potential` crosses the authored foam threshold without a waterfall
  feature, while feature-only support does not reach full coverage.

- [ ] **Step 3: Run the focused target and verify the red state.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_surface_reference_tests
  ```

  Expected: compilation fails because the two new reference functions do not
  exist.

- [ ] **Step 4: Extract the optical-distance function in C++ and GLSL.**

  Move the existing shallow/deep coefficient blend, Beer-Lambert
  transmittance, luminance-based bottom visibility, IOR/Fresnel base term,
  foam transmission suppression, and scattering calculation into the two
  new optical functions. `water_evaluate_surface` passes
  `max(state.field.depth, 0.0)` so all existing raster and reference behavior
  stays unchanged until the forward shader supplies screen-derived distance.

- [ ] **Step 5: Make baked whitewater the explicit primary foam signal.**

  Implement the same bounded expression in C++ and GLSL:

  ```cpp
  const float primary = std::clamp(foam_potential, 0.0f, 1.0f);
  const float turbulence_support = 0.10f * std::clamp(turbulence, 0.0f, 1.0f);
  const float aeration_support = 0.20f * std::clamp(aeration, 0.0f, 1.0f);
  const float feature_support = std::min(water_feature_foam_bias(feature), 0.15f);
  return std::clamp(primary + turbulence_support + aeration_support +
                        feature_support,
                    0.0f, 1.0f);
  ```

  Retain flow-advected breakup, local multiplier, local threshold offset, wave
  multiplier, foam roughening, and foam normal softening. Only the macro driver
  changes.

- [ ] **Step 6: Run the reference and shader-source gates.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_surface_reference_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
  ```

  Expected: both exit zero; the old surface-reference suite remains green and
  the new monotonicity and foam-dominance assertions pass.

- [ ] **Step 7: Create an implementation checkpoint commit.**

  Only in a clean isolated implementation worktree; skip in the shared dirty
  worktree:

  ```powershell
  git add MatterEngine3/src/render/water_surface_reference.h MatterEngine3/src/render/water_surface_reference.cpp MatterEngine3/shaders_vk/water_surface.glsl MatterEngine3/tests/water_surface_reference_tests.cpp
  git commit -m "refactor(water): expose forward optics contracts"
  ```

---

### Task 3: Classify static water and separate opaque from water draw ranges

**Files:**

- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp`
- Test: `MatterEngine3/tests/gpu_water_render_tests.cpp`
- Test: `MatterEngine3/tests/gpu_water_animation_render_tests.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**

- Extends `VkScenePart` with append-only
  `bool raster_water_surface = false;` after `water_field_binding`.
- Extends private `PartRecord` and public `PartCommandRange` with
  `bool raster_water_surface = false;`.
- Produces:
  `bool VkSceneRenderer::part_is_raster_water(uint64_t part_hash) const noexcept`
  under `MATTER_VK_TEST_FAULT_INJECTION` for focused assertions.
- Consumes first-plan meaning of `VkSceneInstance::rt_proxy_only`: suppress the
  accepted immutable proxy only while a direct animated frame owns visibility.

- [ ] **Step 1: Write failing part-classification tests.**

  Assert `build_water_scene_part(...)` sets both a valid
  `water_field_binding` and `raster_water_surface == true`. Assert a generic
  `VkScenePart{}` remains false. In the Vulkan smoke fixture, register each and
  assert `test_recorded_draw_ranges()` carries the matching classification.

- [ ] **Step 2: Write failing active/fallback ownership tests.**

  Extend the animation fixture with these states:

  ```text
  accepted proxy + no direct frame   -> one static forward-eligible range
  accepted proxy + active direct     -> zero proxy instances, one direct draw
  rejected direct frame              -> one static forward-eligible range
  ordinary opaque part               -> one opaque range, zero water ranges
  ```

  Assert `ray_traced == false` in all three water states.

- [ ] **Step 3: Run the CPU tests and verify the red state.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_animation_render_tests
  ```

  Expected: compilation or assertions fail because the water classification
  fields are absent.

- [ ] **Step 4: Carry immutable water identity through staging.**

  Set `candidate->raster_water_surface = true` only in
  `gpu_meshing/water_scene_part.cpp`. Copy the flag in `ensure_part`, retain it
  in `PartRecord`, and stamp every generated `PartCommandRange` from its owning
  part record. Append fields so positional `VkScenePart` fixtures keep their
  established bindings.

- [ ] **Step 5: Partition recording without changing cull command ABI.**

  In the opaque G-buffer range loop, skip ranges whose
  `raster_water_surface` is true. Do not delete or compact their indirect
  commands: the forward pass in Task 5 will bind the same indirect buffer and
  issue only those ranges. Keep `rt_proxy_only` instance-count suppression in
  cull output, so active direct animation cannot double-draw its static proxy.

- [ ] **Step 6: Keep water out of the opaque visibility-ID occluder pass.**

  Apply the same range classification in `record_visibility_id_pass`: issue
  only `raster_water_surface == false` ranges. The forward pass later restores
  visible water identity into `material_instance_`; water must not become an
  opaque depth-pyramid occluder that hides its own riverbed.

- [ ] **Step 7: Run the focused tests.**

  Run all three targets from Step 3 and:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  $env:MATTER_VK_SMOKE_MODE='water-animation'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

  Expected: CPU tests pass; active direct and accepted fallback ownership pass;
  validation stays at zero.

- [ ] **Step 8: Create an implementation checkpoint commit.**

  Only in a clean isolated implementation worktree; skip in the shared dirty
  worktree:

  ```powershell
  git add MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp MatterEngine3/tests/gpu_water_render_tests.cpp MatterEngine3/tests/gpu_water_animation_render_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
  git commit -m "refactor(render): route water outside the opaque pass"
  ```

---

### Task 4: Add preserved opaque resources and the forward pipeline ABI

**Files:**

- Create: `MatterEngine3/shaders_vk/water_screen_space.glsl`
- Create: `MatterEngine3/shaders_vk/water_forward.frag`
- Modify: `MatterEngine3/shaders_vk/environment_common.glsl`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Test: `MatterEngine3/tests/shader_source_tests.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**

- Produces `struct alignas(16) WaterForwardConstants` with exact ABI:

  ```cpp
  struct alignas(16) WaterForwardConstants {
      GpuMat4 clip_to_world;
      matter::Float4 to_sun;
      matter::Float4 viewport_refraction;
      matter::Float4 reflection_controls;
  };
  static_assert(sizeof(WaterForwardConstants) == 112);
  ```

  `viewport_refraction = {width, height, 24.0f, 0.75f}` and
  `reflection_controls = {24.0f, 2.0f, 0.25f, 80.0f}` mean maximum refraction
  offset in pixels, edge/depth rejection in metres, SSR step count, pixel
  stride, hit thickness in metres, and maximum reflection distance.
- Produces descriptor layout:
  set 0 = existing frame constants, set 1 = existing scene set, set 2 binding 0
  = copied opaque HDR combined sampler, set 2 binding 1 = copied opaque depth
  combined sampler, set 2 binding 2 = `WaterForwardConstants` uniform buffer,
  set 3 = existing environment set.
- Produces two pipelines sharing `water_forward.frag`: static uses
  `raster.vert.spv`; direct animation uses `raster_water.vert.spv`.
- Produces `opaque_hdr_` (`VK_FORMAT_R16G16B16A16_SFLOAT`) and `opaque_depth_`
  (`VK_FORMAT_D32_SFLOAT`) at internal raster extent.

- [ ] **Step 1: Write failing build-inventory tests.**

  Extend `shader_source_tests.cpp` to require
  `build/shaders_vk/water_forward.frag.spv` in `VK_SPV`, and require explicit
  dependencies on `water_surface.glsl`, `water_screen_space.glsl`,
  `material_common.glsl`, and `environment_common.glsl`. Assert
  `environment_common.glsl` uses an `ENVIRONMENT_SET` macro whose default is
  `1`, so existing shaders retain their descriptor ABI.

- [ ] **Step 2: Write failing Vulkan resource/ABI tests.**

  In `vulkan_smoke_tests.cpp`, assert:

  ```cpp
  CHECK(sizeof(viewer::WaterForwardConstants) == 112u,
        "water forward constants ABI");
  CHECK(renderer.test_opaque_hdr_format() == VK_FORMAT_R16G16B16A16_SFLOAT,
        "preserved opaque HDR format");
  CHECK(renderer.test_opaque_depth_format() == VK_FORMAT_D32_SFLOAT,
        "preserved opaque depth format");
  CHECK(renderer.test_opaque_extent().width == width &&
            renderer.test_opaque_extent().height == height,
        "preserved opaque resources match raster extent");
  ```

  Also assert main HDR/depth usage includes `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`
  and both preservation images include `VK_IMAGE_USAGE_TRANSFER_DST_BIT |
  VK_IMAGE_USAGE_SAMPLED_BIT`.

- [ ] **Step 3: Run tests and verify the red state.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  ```

  Expected: source assertions and compilation fail because the shader,
  constants, images, and test accessors do not exist.

- [ ] **Step 4: Make the environment descriptor set relocatable.**

  Change only the set qualifier in `environment_common.glsl`, retaining the
  sampling-test specialization at set 0:

  ```glsl
  #ifndef ENVIRONMENT_SET
  #ifdef MATTER_ENVIRONMENT_SAMPLING_TEST
  #define ENVIRONMENT_SET 0
  #else
  #define ENVIRONMENT_SET 1
  #endif
  #endif
  layout(set = ENVIRONMENT_SET, binding = 0) uniform sampler2D atmosphere_sky_view;
  ```

  Apply the macro to every environment binding in that include. Existing
  shaders compile without a define and remain at set 1. `water_forward.frag`
  defines `ENVIRONMENT_SET 3` before including it.

- [ ] **Step 5: Add the shader files and complete inventory.**

  Add `water_forward.frag` to `VK_SPV` and add this dependency rule:

  ```make
  build/shaders_vk/water_forward.frag.spv: \
      shaders_vk/material_common.glsl shaders_vk/water_surface.glsl \
      shaders_vk/water_screen_space.glsl shaders_vk/environment_common.glsl
  ```

  `water_forward.frag` must consume the existing `raster.vert` varyings at
  locations 0 (`normal`), 3 (`velocity_valid`), 4 (`material_index`), 5
  (`instance_token`), 6 (`material_valid`), 7 (`world_pos`), 15
  (`water_binding_slot`), and 16 (`water_generation`). It writes HDR at
  location 0, velocity at location 1, reactivity at location 2, and
  `uvec2(material, instance)` at location 3. Declare the same set-0/binding-0
  `FrameConstants` block as `gbuffer.frag` so the shader reads camera eye and
  `water_animation.x`; use `WATER_SET 1` for field records and
  `ENVIRONMENT_SET 3` for the physical environment.

- [ ] **Step 6: Allocate preservation resources and per-frame descriptors.**

  Add `opaque_hdr_`, `opaque_depth_`, `water_forward_sampler_`, the set-2
  descriptor layout/pool/sets, and one host-visible aligned
  `WaterForwardConstants` buffer per `FrameResources`. Recreate the two images
  only when internal raster extent changes. Destroy them through existing RAII
  reset paths and destroy Vulkan descriptor/pipeline handles beside their
  neighboring raster handles.

- [ ] **Step 7: Create both forward pipeline variants.**

  Add `create_water_forward_pipelines(std::string& error)` and call it from
  renderer pipeline creation after `create_raster_pipelines`. Use dynamic
  rendering formats:

  ```text
  location 0: R16G16B16A16_SFLOAT  (hdr_)
  location 1: R16G16_SFLOAT        (velocity_)
  location 2: R8_UNORM             (reactivity_)
  location 3: R32G32_UINT          (material_instance_)
  depth:      D32_SFLOAT            (depth_)
  ```

  Set `depthTestEnable = true`, `depthWriteEnable = true`, reversed-Z
  `compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL`, and disable color blending.
  Create the static vertex-input variant from `VkRasterVertex`, then replace
  its vertex module and input description with the existing packed 12-byte
  water specialization for the direct-animation variant.

- [ ] **Step 8: Run build and Vulkan creation gates.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  $env:MATTER_VK_SMOKE_MODE='default'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

  Expected: SPIR-V inventory matches CMake discovery, both pipelines create,
  resource assertions pass, and validation is zero.

- [ ] **Step 9: Create an implementation checkpoint commit.**

  Only in a clean isolated implementation worktree; skip in the shared dirty
  worktree:

  ```powershell
  git add MatterEngine3/shaders_vk/water_screen_space.glsl MatterEngine3/shaders_vk/water_forward.frag MatterEngine3/shaders_vk/environment_common.glsl MatterEngine3/Makefile MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/tests/shader_source_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
  git commit -m "feat(render): add forward water pipeline resources"
  ```

---

### Task 5: Implement screen-space refraction, absorption, and reflection

**Files:**

- Modify: `MatterEngine3/shaders_vk/water_screen_space.glsl`
- Modify: `MatterEngine3/shaders_vk/water_forward.frag`
- Modify: `MatterEngine3/shaders_vk/water_surface.glsl`
- Create: `MatterEngine3/src/render/water_forward_reference.h`
- Create: `MatterEngine3/src/render/water_forward_reference.cpp`
- Create: `MatterEngine3/tests/water_forward_reference_tests.cpp`
- Modify: `cmake/MatterEngine.cmake`
- Test: `MatterEngine3/tests/shader_source_tests.cpp`

**Interfaces:**

- Produces C++:
  `WaterScreenDepth water_screen_depth_reference(float baked_depth_m, float water_ray_distance_m, float center_opaque_ray_distance_m, float refracted_opaque_ray_distance_m, float discontinuity_limit_m) noexcept`.
- Produces C++:
  `matter::Float2 water_refraction_uv_reference(matter::Float2 source_uv, matter::Float2 normal_xz, float optical_distance_m, matter::Float2 viewport_px, float max_offset_px) noexcept`.
- Produces GLSL:
  `WaterScreenSample water_refract_scene(vec3 water_world, vec3 normal, vec3 view_dir, float baked_depth_m)`.
- Produces GLSL:
  `WaterScreenSample water_reflect_scene(vec3 water_world, vec3 reflection_dir)`.
- `WaterScreenSample` contains `vec2 uv`, `vec3 color`, `float distance_m`, and
  `bool valid`.

- [ ] **Step 1: Write failing screen-depth reference tests.**

  Cover these exact cases:

  ```text
  center/refracted depth behind water, delta <= 0.75 m -> valid screen distance
  copied depth is reversed-Z sky value 0                     -> baked fallback
  reconstructed opaque point is in front of water            -> baked fallback
  refracted-vs-center discontinuity > 0.75 m                 -> baked fallback
  refraction UV leaves half-texel viewport inset             -> rejected sample
  requested offset exceeds 24 px                             -> clamped to 24 px
  ```

  Assert every returned distance is finite, non-negative, and no larger than
  `max(baked_depth_m * 2.0f, baked_depth_m + 0.5f)` when a screen sample is
  accepted.

- [ ] **Step 2: Register and run the new red target.**

  Add:

  ```cmake
  matter_add_engine_cpu_test(water_forward_reference_tests
      MatterEngine3/tests/water_forward_reference_tests.cpp
      MatterEngine3/src/render/water_forward_reference.cpp)
  ```

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_forward_reference_tests
  ```

  Expected: compilation fails because the reference header and functions are
  not implemented.

- [ ] **Step 3: Implement bounded refraction and screen-depth validation.**

  In GLSL, derive source UV from `gl_FragCoord.xy / textureSize(opaque_hdr, 0)`.
  Reconstruct copied opaque world position with `clip_to_world`; reject copied
  depth `<= 0.0` (reversed-Z sky), points not behind the water along the view
  ray, UV outside a half-texel inset, and refracted depth discontinuities over
  `viewport_refraction.w` (`0.75 m`). Offset along the projected surface normal
  and clamp its length to `viewport_refraction.z` (`24 px`). On rejection,
  sample undisplaced opaque color and pass baked depth to optics.

- [ ] **Step 4: Apply screen/baked depth absorption.**

  For a valid refracted point, compute the ray distance between water and the
  reconstructed opaque point, clamp it to
  `max(baked_depth * 2.0, baked_depth + 0.5)`, and pass it to
  `water_evaluate_optics`. For invalid screen depth, pass baked field depth.
  Compose transmitted copied HDR through `optics.transmittance`; add
  `optics.scattering_color * optics.diffuse_scattering_weight`; suppress
  transmission with `coherent_transmission_weight`. This keeps shallow bottom
  color visible rather than replacing it with water albedo.

- [ ] **Step 5: Implement bounded SSR with environment fallback.**

  March the reflected world ray for at most 24 projected steps, beginning two
  pixels away from the source. Reject off-screen UV and reversed-Z sky. Detect
  a hit when reconstructed scene distance crosses the ray point within
  `0.25 m`; binary-refine four iterations and sample copied HDR at the refined
  UV. Stop beyond `80 m`. On miss, call the existing physical sky/environment
  sampler with `reflection_dir` and `to_sun`.

  Blend SSR toward environment as roughness rises. Weight final reflection by
  Schlick Fresnel using authored water IOR and `dot(normal, view_dir)`. Increase
  roughness and reduce coherent reflection under foam using the existing
  `foam_response` channels.

- [ ] **Step 6: Enforce single shadow ownership in shader source.**

  `water_forward.frag` must not declare or sample `visibility_texture`,
  `raw_diffuse`, `raw_specular`, `raw_transmission`, or a TLAS. Copied opaque
  HDR already contains primary shadows, GI, and volumetrics. Environment
  reflection may use physical sky/sun radiance but must not multiply a second
  receiver-visibility term.

- [ ] **Step 7: Add source assertions for bounded work and ownership.**

  Assert shader text contains the 24-step ceiling, four refinement iterations,
  `water_refract_scene`, `water_reflect_scene`, `water_evaluate_optics`, and
  environment miss sampling. Assert it contains none of the forbidden shadow
  resources from Step 6 and no ray-query or ray-tracing extension.

- [ ] **Step 8: Run the CPU and shader gates.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_forward_reference_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_surface_reference_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
  ```

  Expected: all targets pass; screen-depth fallbacks are deterministic;
  refraction offsets and reflection work are bounded.

- [ ] **Step 9: Create an implementation checkpoint commit.**

  Only in a clean isolated implementation worktree; skip in the shared dirty
  worktree:

  ```powershell
  git add MatterEngine3/shaders_vk/water_screen_space.glsl MatterEngine3/shaders_vk/water_forward.frag MatterEngine3/shaders_vk/water_surface.glsl MatterEngine3/src/render/water_forward_reference.h MatterEngine3/src/render/water_forward_reference.cpp MatterEngine3/tests/water_forward_reference_tests.cpp cmake/MatterEngine.cmake MatterEngine3/tests/shader_source_tests.cpp
  git commit -m "feat(water): add screen-space forward optics"
  ```

---

### Task 6: Record the parity forward pass and preserve temporal outputs

**Files:**

- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Test: `MatterEngine3/tests/gpu_water_animation_render_tests.cpp`

**Interfaces:**

- Produces private
  `void record_water_forward(VkCommandBuffer, const WaterForwardRecord&)`.
- `WaterForwardRecord` consumes frame/scene/forward/environment descriptor
  sets, both water pipelines, opaque preservation images, the existing
  indirect buffer and classified ranges, optional active direct-water draws,
  and the five final attachments.
- Produces test observation
  `WaterForwardObservation { uint32_t static_draws; uint32_t direct_draws;
  bool copied_opaque_hdr; bool copied_opaque_depth; bool wrote_depth;
  bool wrote_velocity; bool wrote_reactivity; bool wrote_identity; }`.

- [ ] **Step 1: Write a failing `water-forward` Vulkan smoke scenario.**

  Add `run_water_forward_path(VulkanDevice&)` and dispatch it from
  `MATTER_VK_SMOKE_MODE=water-forward`. Render an opaque background triangle,
  one accepted static-water triangle, and one active packed direct-water
  triangle in separate frames. Assert preservation copies occur once per
  water frame, opaque ranges do not draw in the forward pass, static fallback
  produces `static_draws == 1`, active animation produces
  `direct_draws == 1 && static_draws == 0`, and all four temporal/identity/depth
  booleans are true.

- [ ] **Step 2: Add pixel assertions for composition and temporal state.**

  Read back a center water pixel and neighboring opaque control. Assert the
  water HDR is finite/non-black, differs from both raw water base color and
  opaque control, water depth wins reversed-Z testing, velocity is finite,
  reactivity is nonzero for turbulent input, and identity equals the water
  material/instance. Assert the opaque control pixel remains byte-identical
  with and without the water pass.

- [ ] **Step 3: Run the smoke target and verify the red state.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  $env:MATTER_VK_SMOKE_MODE='water-forward'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

  Expected: compilation or assertions fail because forward recording and
  observations do not exist.

- [ ] **Step 4: Copy opaque HDR and depth after composite.**

  At the end of `record_raster`, after `vkCmdEndRendering` for the opaque
  composite, transition `hdr_` and `depth_` to transfer source, transition
  `opaque_hdr_` and `opaque_depth_` to transfer destination, and issue one
  full-extent `vkCmdCopyImage` per image. Transition both copies to
  shader-read. Transition the five final targets to color/depth attachment
  layouts with source masks matching their preceding fragment/depth/transfer
  accesses.

- [ ] **Step 5: Begin a load-preserving dynamic-rendering scope.**

  Bind `hdr_`, `velocity_`, `reactivity_`, and `material_instance_` as the four
  color attachments and `depth_` as depth. Use `LOAD`/`STORE` for every
  attachment. This preserves opaque pixels and permits water pixels to replace
  temporal and identity state where their depth wins.

- [ ] **Step 6: Draw accepted static water ranges.**

  Bind the static forward pipeline, existing raster vertex/index/indirect
  buffers, and descriptor sets 0-3. Iterate all `PartCommandRange` values and
  issue only `raster_water_surface == true` ranges. Their existing indirect
  instance counts decide ownership: accepted fallback draws; an active
  `rt_proxy_only` proxy contributes zero instances.

- [ ] **Step 7: Draw active packed direct water.**

  Bind the direct-animation forward pipeline and the current frame's packed
  water vertex/index buffers. Reissue the current `WaterAnimationRasterDraw`
  list using `proxy_transform_slot`; the referenced `GpuDrawTransform` supplies
  `water_binding_slot` and `water_generation`. Do not decode vertices for RT
  and do not create a second copy of direct-water geometry.

- [ ] **Step 8: Write final temporal and identity values.**

  In `water_forward.frag`, write `in_velocity_valid.xy` when its validity
  component is nonzero, otherwise zero; write the evaluated water reactivity;
  write unmasked material and instance token; and preserve
  `gl_FragDepth = gl_FragCoord.z`. The static fallback therefore uses the
  existing current/previous draw transform history, and active packed water
  uses the existing specialized vertex shader's current/previous transform
  contract. Deforming packed vertices do not have a previous-position stream;
  their baked turbulence/foam reactivity is therefore the temporal rejection
  signal for surface deformation while velocity continues to represent camera
  and proxy-transform motion.

- [ ] **Step 9: Return final targets to downstream sampled layouts.**

  End rendering, then transition HDR/depth/velocity/reactivity/identity to the
  layouts and destination stages already expected by DLSS, native temporal
  evaluation, display, and readback. Do not change `composite_source`: it
  remains `hdr_` or the existing DLSS output selected from `hdr_`.

  Add `opaque_hdr_`, `opaque_depth_`, every per-frame forward constants buffer,
  and both static/direct water vertex/index buffers to the existing
  `retain_for_frame` or immediate-submit dependency lists. A successful command
  recording must keep every copied, sampled, and drawn resource alive until
  the frame slot retires.

- [ ] **Step 10: Run forward, animation, and default smoke modes.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_animation_render_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  foreach ($mode in 'water-forward', 'water-animation', 'default') {
      $env:MATTER_VK_SMOKE_MODE=$mode
      & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
      if ($LASTEXITCODE -ne 0) { throw "$mode failed" }
  }
  ```

  Expected: all modes exit zero; validation errors are zero; active and static
  fallback use the same forward optics; no water RT work returns.

- [ ] **Step 11: Create an implementation checkpoint commit.**

  Only in a clean isolated implementation worktree; skip in the shared dirty
  worktree:

  ```powershell
  git add MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/shaders_vk/water_forward.frag MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/tests/gpu_water_animation_render_tests.cpp
  git commit -m "feat(render): composite raster water after opaque lighting"
  ```

---

### Task 7: Expose forward timing and memory evidence

**Files:**

- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEditor/src/main.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**

- Reinterprets append-only `kGpuZoneWaterDraw` as the complete forward-water
  span; `kGpuZoneWaterDecode` remains present for capture compatibility and
  retires as zero after the first plan.
- Produces `FrameStats::gpu_water_forward_ms`.
- Produces `FrameStats::water_forward_image_bytes` equal to logical copied
  image storage: `raster_width * raster_height * (8 + 4)`.
- Adds perf JSON keys `gpu_water_forward_ms`, `water_forward_width`,
  `water_forward_height`, and `water_forward_image_bytes`.

- [ ] **Step 1: Write failing stats and JSON assertions.**

  Extend the Vulkan smoke fixture to assert a recorded forward draw writes the
  `kGpuZoneWaterDraw` timestamp pair and that logical image bytes equal
  `width * height * 12`. Extend the existing perf-output unit seam in
  `vulkan_smoke_tests.cpp` to require all four JSON keys and exact dimensions.

- [ ] **Step 2: Run the smoke target and verify the red state.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  ```

  Expected: compilation fails because the new stats fields and JSON keys do
  not exist.

- [ ] **Step 3: Time only the preservation and forward-water span.**

  Begin `kGpuZoneWaterDraw` immediately before the first opaque-image
  transition/copy and end it after final target transitions. When no static or
  direct water is eligible, skip both copies and the rendering scope and leave
  the zone unwritten/zero.

- [ ] **Step 4: Publish stable memory accounting.**

  Add `uint64_t water_forward_image_bytes() const noexcept`; return zero when
  preservation images are absent, otherwise compute with checked 64-bit
  multiplication from the allocated raster extent and the fixed 8-byte HDR +
  4-byte depth texels. This is logical persistent image payload, not a claim
  about driver heap padding.

- [ ] **Step 5: Thread timing and memory through `FrameStats` and perf JSON.**

  Assign `gpu_water_forward_ms` from the smoothed water-draw zone and assign
  the byte/dimension fields after raster resources are ready. Keep existing
  `gpu_water_animation_ms` as the raw compatibility sum of decode plus draw;
  with decode retired, it equals the raw forward span. Emit the four new JSON
  keys beside existing water-animation counters.

- [ ] **Step 6: Run the stats and smoke gates.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  $env:MATTER_VK_SMOKE_MODE='water-forward'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

  Expected: stats/JSON assertions pass and validation stays zero.

- [ ] **Step 7: Create an implementation checkpoint commit.**

  Only in a clean isolated implementation worktree; skip in the shared dirty
  worktree:

  ```powershell
  git add MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/include/matter/world_session.h MatterEngine3/src/matter_engine.cpp MatterEditor/src/main.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
  git commit -m "perf(water): report forward pass timing and memory"
  ```

---

### Task 8: Automate RiverFloatLab visual, performance, and memory acceptance

**Files:**

- Create: `MatterEngine3/tools/raster_water_forward_acceptance.timeline`
- Create: `MatterEngine3/tools/raster_water_forward_acceptance.py`
- Create: `MatterEngine3/tools/tests/test_raster_water_forward_acceptance.py`
- Create: `MatterEngine3/tools/run_raster_water_forward_acceptance.ps1`
- Modify: `docs/agent/qa-cookbook.md`
- Create: `docs/findings/raster-water-forward-optics-acceptance-2026-08-29.md`
- Modify: `ROADMAP.md`

**Interfaces:**

- `raster_water_forward_acceptance.py compare --baseline <dir> --candidate <dir> --screenshots <dir>` exits nonzero on an acceptance failure and writes one
  JSON summary to stdout.
- `run_raster_water_forward_acceptance.ps1 -BaselineDir <dir> -OutputDir <dir>`
  performs one screenshot run and candidate performance runs at shadow samples
  1, 10, and 16, then invokes the comparator.
- Acceptance thresholds at each shadow setting:
  `median_frame_ms <= max(baseline * 1.08, baseline + 1.0)`,
  `p95_frame_ms <= max(baseline * 1.10, baseline + 2.0)`, validation errors
  zero, water decode dispatch delta zero, steady-state allocation delta zero,
  and `water_forward_image_bytes == width * height * 12`.

- [ ] **Step 1: Write failing comparator unit tests.**

  Use `tempfile.TemporaryDirectory` and synthetic baseline/candidate JSON.
  Cover: an exact pass; median regression; p95 regression; nonzero validation;
  nonzero decode; nonzero steady allocation; incorrect byte formula; missing
  screenshot; zero-byte screenshot; and all five required PNG names present.

- [ ] **Step 2: Run the Python tests and verify the red state.**

  Run:

  ```powershell
  py -3 -m unittest MatterEngine3.tools.tests.test_raster_water_forward_acceptance -v
  ```

  Expected: import fails because the comparator does not exist.

- [ ] **Step 3: Write the representative screenshot timeline.**

  Create this exact four-view sequence, using `{{OUTPUT_DIR}}` for the runner's
  absolute-path substitution:

  ```text
  wait_event bake.finished 3600
  play
  wait_frames 10
  cam 60 70 -2 88 57 0
  wait_idle 4 900
  shot {{OUTPUT_DIR}}/shallow-player-low.png
  cam 45 72 2 78 60 -3
  wait_idle 4 900
  shot {{OUTPUT_DIR}}/upper-rapids.png
  cam 119 72 2 110 49 5
  wait_idle 4 900
  shot {{OUTPUT_DIR}}/waterfall-side.png
  cam 126 75 -3 111 45 5
  wait_idle 4 900
  shot {{OUTPUT_DIR}}/plunge-pool.png
  cam 140 70 -1 151 43 0
  wait_idle 4 900
  shot {{OUTPUT_DIR}}/section-handoff.png
  quit
  ```

  Five files cover the four required situations because waterfall and plunge
  pool are separated for shadow/reflection review.

- [ ] **Step 4: Implement the strict comparator.**

  Parse `shadow-01.json`, `shadow-10.json`, and `shadow-16.json` from both
  directories. Reject missing/non-finite metrics before applying the thresholds
  above. Require all five PNGs and their `.done` sidecars to exist and be
  nonempty. Include per-setting baseline/candidate/delta/limit values and exact
  failure strings in the stdout summary.

- [ ] **Step 5: Implement the native PowerShell runner.**

  Resolve repository, editor, baseline, and output paths. Replace
  `{{OUTPUT_DIR}}` in a temporary timeline with forward-slash absolute paths;
  call `drive.py` once visibly for screenshots and three times with `--hide-ui`
  for performance. Use the exact `MATTER_SUN_SHADOW_SAMPLES`, 20-second warmup,
  and 30-second sample settings from Task 1. Stop on every nonzero exit code and
  finally invoke the comparator.

- [ ] **Step 6: Run comparator tests.**

  Run:

  ```powershell
  py -3 -m unittest MatterEngine3.tools.tests.test_raster_water_forward_acceptance -v
  ```

  Expected: all pass/fail fixtures report the intended result.

- [ ] **Step 7: Run complete native build and CPU gates.**

  Run:

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo
  & 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -L cpu --output-on-failure
  ```

  Expected: build exits zero and all CPU-labeled tests pass.

- [ ] **Step 8: Run the RiverFloatLab acceptance suite.**

  Run:

  ```powershell
  $baseline = (Resolve-Path 'build/qa/raster-water-forward-2026-08-29/baseline').Path
  $candidate = (New-Item -ItemType Directory -Force 'build/qa/raster-water-forward-2026-08-29/candidate').FullName
  & MatterEngine3/tools/run_raster_water_forward_acceptance.ps1 -BaselineDir $baseline -OutputDir $candidate
  ```

  Expected: comparator exit code zero; every performance/memory/validation gate
  passes; all PNG and `.done` pairs exist.

- [ ] **Step 9: Perform the visual review against explicit criteria.**

  Inspect the candidate PNGs at native size and record pass/fail for:

  ```text
  shallow-player-low: riverbed remains legible; absorption increases with depth
  upper-rapids: foam follows baked turbulent lanes and visibly advects with flow
  waterfall-side/plunge-pool: whitewater is continuous without a static proxy seam
  section-handoff: no optics, normal, foam, or animation discontinuity at the join
  every view: reflected scene/sky is plausible and screen-edge misses fall back cleanly
  every view: one coherent shadow set, with no duplicated water/proxy shadow
  ```

  Comparable composition and material quality are required; pixel identity with
  the removed RT-water path is not.

- [ ] **Step 10: Record evidence and close only the completed roadmap item.**

  In the finding, record commit hash, device/driver, internal/output resolution,
  build/test totals, all three timing rows, forward image bytes, zero RT-water
  counters, screenshot paths, and the six visual verdicts from Step 9. Update
  `docs/agent/qa-cookbook.md` with the runner command. In `ROADMAP.md`, remove
  completed `Now` items 3 and 4 only when every automated and visual gate above
  passes. If the first plan has already removed completed items 1 and 2, remove
  the now-empty `Now` heading and its authority paragraph as well; otherwise
  retain items 1 and 2 verbatim. Leave `Next`, `Scale before expansion`, and
  `Deferred decisions` unchanged.

- [ ] **Step 11: Create the final implementation checkpoint commit.**

  Only in a clean isolated implementation worktree; skip in the shared dirty
  worktree:

  ```powershell
  git add MatterEngine3/tools/raster_water_forward_acceptance.timeline MatterEngine3/tools/raster_water_forward_acceptance.py MatterEngine3/tools/tests/test_raster_water_forward_acceptance.py MatterEngine3/tools/run_raster_water_forward_acceptance.ps1 docs/agent/qa-cookbook.md docs/findings/raster-water-forward-optics-acceptance-2026-08-29.md ROADMAP.md
  git commit -m "test(water): accept forward optics in RiverFloatLab"
  ```

---

## Final verification checklist

- [ ] The first plan's full CPU suite and `water-animation` smoke gate pass.
- [ ] `water-forward`, `water-animation`, and `default` Vulkan smoke modes exit
  zero with zero validation errors.
- [ ] Active direct water and accepted static fallback both use
  `water_forward.frag`; neither is rendered in the opaque G-buffer pass.
- [ ] Opaque HDR/depth are copied before water and main HDR/depth/velocity/
  reactivity/identity remain the downstream presentation resources.
- [ ] Refraction is bounded to 24 pixels and uses valid screen depth or baked
  field depth; SSR is bounded to 24 steps plus four refinement steps and falls
  back to the physical environment.
- [ ] Baked `foam_potential` dominates aeration and feature-only support in CPU
  reference tests and RiverFloatLab rapids/waterfall screenshots.
- [ ] Water forward owns no visibility texture, TLAS, ray query, RT decode,
  BLAS, cache, TLAS insertion, or second receiver-shadow multiplication.
- [ ] Candidate timings pass matched 1/10/16-shadow-sample thresholds and
  forward image bytes equal exactly 12 logical bytes per internal pixel.
- [ ] Shallow, rapids, waterfall/plunge, and section-handoff images pass the
  explicit visual criteria and are linked from the acceptance finding.
