# Animated Water Section Continuity Implementation Plan

**Execution status — 2026-08-30:** Stage 1 continuity is accepted and the
committed Task10/10a measurement harness remains. Task11 scalar-lock and
conforming-fairing architectures were rejected by real frame-0 evidence and
all experimental implementation/wiring was removed. Waterfall refinement is
open pending a new design; the Task11 steps below are historical, not an
instruction to resume tuning. Controller integration proceeds against the
accepted coarse river. Overall raster-water appearance is not yet accepted.
See [rejection evidence](../../findings/waterfall-refinement-rejection-2026-08-30.md).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the static-collar/zipper animation handoff with a per-frame, common-lattice replacement strip, then refine only the waterfall region enough to pass the blocked raster-water visual gates without changing the `0.20 m` simulation particle spacing or exceeding the animation memory contracts.

**Architecture:** Every visual particle job in one river network uses a versioned, world-anchored lattice with exact voxel spacing. Section bakes persist small, immutable, cropped particle-frame sidecars at handoff endpoints; the handoff builder consumes the two sidecars, reconstructs the existing 30-frame/15-frame-offset phase blend, and meshes upstream band, collar, and downstream band as one field with disjoint cell ownership. After the handoff passes a visual gate, the waterfall uses a bounded higher-quality replacement region with scalar-field boundary locking only where a coarse/fine boundary measurement proves it is required; contour welding remains an assertion within quantization tolerance, never a multi-metre zipper.

**Tech Stack:** C++20, MSVC 2022, CMake/Ninja, Vulkan 1.4 compute marching cubes, GLSL 460, PhysX PBD capture data, immutable binary hydrology artifacts, Python 3 acceptance tooling, PowerShell native runners.

**Spec:** `.superpowers/sdd/2026-08-29-raster-water-forward-optics/next-water-mesh-continuity-analysis.md`, implementing `ROADMAP.md` “Next — playable river proof” item 1 and using `docs/findings/raster-water-forward-optics-acceptance-2026-08-29.md` as the required failed baseline.

## Global Constraints

- `docs/findings/raster-water-forward-optics-acceptance-2026-08-29.md` is prerequisite evidence, not a gate to weaken: its waterfall/plunge and section-handoff failures must reproduce in Stage 0 and must pass before this plan can close.
- Keep simulation `particleSpacing = 0.20 m`, capture/playback at 30 fps for one second, and phase offset at 15 frames. Do not change the shared playback clock or interpolate unrelated marching-cubes vertices.
- Keep animated water raster-only. Water animation RT decode, BLAS build/cache, TLAS insertion, and RT records must remain zero in every GPU acceptance run.
- A canonical lattice uses one network/world anchor, exact `voxel_m` on all axes, and integer cell ranges. Tight bounds may expand to lattice faces but may not change the anchor or replace exact spacing with `extent / ceil(extent / voxel)`.
- Handoff replacement input includes the complete particle influence halo returned by the mesher. No handoff code may duplicate the current `radius * 2.5 + blend * 4` formula.
- The owned bulk animation and replacement strip must consume the same decoded sidecar positions for every particle capable of influencing an outer ownership face. Sharing only a lattice while remeshing raw float positions on one side and quantized positions on the other is insufficient.
- Rendered water ownership is disjoint. Raw particle/field support may overlap; rendered triangles may not overlap, alpha-blend at a seam, or create co-planar duplicate optics.
- Smooth union is attempted first for the handoff interior. The scalar-field source blend in Task 8 runs only if the recorded cross-section/wet-dry gate fails; it is never a shader seam mask or a vertex-space blend.
- In every new animated strip/refinement path, a weld is permitted only after both contours are already within `visual_voxel_m / 16`. A larger symmetric Hausdorff distance is a product failure with metrics, never permission to call the legacy zipper. The legacy static fallback is left byte-compatible but cannot contribute triangles while accepted animation is active.
- Preserve the existing limits exactly: each complete `.mhwa` file, including its 32-byte fixed header, must remain at or below 1 GiB, so its serialized payload may be at most `1 GiB - 32 bytes`. The sum of complete section and handoff animation file sizes admitted for one network must remain at or below `700 * 1024 * 1024` bytes, matching playback's current `file_size` accounting.
- Boundary sidecars are transient build inputs, not runtime assets. Unload them after handoff assembly and report their peak CPU residency separately from the 700 MiB runtime animation budget.
- The waterfall quality region is bounded to approximately 2 m before the authored lip through 4 m after the authored landing, expanded only by snapped ownership cells and the complete field halo. Keep river coverage outside that region unchanged.
- Static fallback, CPU query/collision, gameplay field, presentation field, and raster publication remain valid. This plan does not redesign collision resolution, the shared runtime clock, forward optics, or character control.
- Canonical MSVC commands run through `tools/build-windows.ps1`; GPU acceptance requires a Vulkan-capable NVIDIA adapter and zero validation errors.

## Producer/consumer order

The implementation must preserve this order; it resolves the current cache-hit contradiction:

1. Resolve the network lattice and handoff cell ownership before any section animation is built.
2. Bake or cache-load each static section.
3. For a fresh section capture, build every required endpoint sidecar first. Build the owned bulk animation from the raw frames with each sidecar's decoded positions substituted inside its snapped crop; this makes the complete influence set at every outer ownership face byte-identical to the later strip input. Validate all candidates before publishing any of them.
4. Publish the static section, section animation, and endpoint sidecars immutably. Section cache admission is true only when all three classes are present and valid.
5. After both adjacent sections are accepted, cache-load or build the static handoff and per-frame animated replacement strip from the two endpoint sidecars and the two owned bulk animation artifacts.
6. Publish the ready network manifest only after all section/handoff products pass continuity, memory, and digest checks. A failed or partial continuation leaves the previously ready network untouched.
7. Release decoded sidecar frames before runtime water playback activation.

An old cache has no lattice metadata or boundary sidecars and therefore misses once by design. After one successful v2 bake, an unchanged upstream section must be reusable without PhysX when only the downstream section changes.

## Dirty-worktree safeguards

The implementation branch already contains accepted renderer/hydrology work and may be dirty. Every task must:

1. Run `git status --short` and `git diff -- <each touched file>` before editing.
2. Use `apply_patch`; never replace a whole dirty file and never run `git checkout --`, `git restore`, or `git reset --hard`.
3. Build and test before staging.
4. Stage new files explicitly and use `git add -p -- <dirty existing file>` for overlapping files.
5. Run `git diff --cached --check` and inspect `git diff --cached --stat` plus `git diff --cached` before every commit.
6. Leave `build/`, `.cache/`, screenshots, generated SPIR-V, and `.superpowers/sdd/` out of commits unless a task explicitly names a retained artifact.

## File responsibility map

- `MatterEngine3/include/matter/gpu_visual_meshing.h`: canonical lattice, exact cell-range layout, public field-support query, and the optional bounded scalar override ABI.
- `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp`: validated integer lattice math and CPU scalar reference.
- `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp`, `MatterEngine3/shaders_vk/gpu_mesh_field.comp`: Vulkan parity for canonical samples and, only for bounded refinement, scalar override buffers.
- `MatterEngine3/src/hydrology/water_mesh_continuity.{h,cpp}`: cut-contour, normal, open-edge, duplicate-triangle, and coverage metrics shared by tests and acceptance reporting.
- `MatterEngine3/src/hydrology/water_boundary_animation_source.{h,cpp}`: immutable cropped/quantized endpoint capture sidecar and frame decode.
- `MatterEngine3/src/hydrology/water_mesh_animation.{h,cpp}`: unchanged temporal phase math plus per-frame substitution of canonical sidecar positions into common-lattice section construction.
- `MatterEngine3/src/hydrology/water_mesh_animation_artifact.{h,cpp}`: v2 lattice metadata and the 1 GiB artifact contract.
- `MatterEngine3/src/hydrology/hydrology_handoff_products.{h,cpp}`: cell ownership, per-frame joint replacement strip, zero-distance boundary assertion, diagnostics, and network timing JSON.
- `MatterEngine3/src/provider/local_provider.cpp`: producer/consumer order, sidecar paths, cache invalidation, transactional admission, peak memory accounting, and final network cap.
- `MatterEngine3/src/hydrology/river_section_coordinator.{h,cpp}`: carries endpoint sources from section execution to handoff assembly without publishing them to runtime.
- `MatterEngine3/src/hydrology/waterfall_visual_refinement.{h,cpp}`: bounded feature-region derivation, coarse/fine replacement, and boundary-lock construction.
- `MatterEngine3/shaders_vk/water_forward.frag`, `MatterEngine3/shaders_vk/water_screen_space.glsl`, `MatterEngine3/src/render/vk_scene_renderer.{h,cpp}`: QA-only identity/geometry-normal/foam diagnostic modes; normal rendering is unchanged.
- `MatterEngine3/tools/run_water_mesh_continuity_acceptance.ps1`, timelines, and comparator: cold/cache-hit, GPU/perf/memory, deterministic phase screenshots, and final Task 8 rerun.

---

## Stage 0 — diagnostic contracts and failed baseline

### Task 1: Land reusable cut-contour and ownership diagnostics

**Files:**

- Create: `MatterEngine3/src/hydrology/water_mesh_continuity.h`
- Create: `MatterEngine3/src/hydrology/water_mesh_continuity.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Test: `MatterEngine3/tests/hydrology_handoff_products_tests.cpp`

**Interfaces:**

- Produces:

```cpp
struct WaterCutContourMetrics {
    std::uint32_t first_points = 0;
    std::uint32_t second_points = 0;
    std::uint32_t unmatched_open_edges = 0;
    std::uint32_t duplicate_coplanar_triangles = 0;
    float symmetric_hausdorff_m = 0.0f;
    float rms_distance_m = 0.0f;
    std::array<float, 3> first_height_quantiles_m{};   // p10,p50,p90
    std::array<float, 3> second_height_quantiles_m{};
    float minimum_normal_dot = 1.0f;
    float p95_normal_angle_degrees = 0.0f;
};

bool measure_water_cut_continuity(
    const gpu_meshing::MeshResult& first,
    const gpu_meshing::MeshResult& second,
    const SpillwayHandoffRecord& handoff,
    float signed_cut_m,
    float edge_match_tolerance_m,
    WaterCutContourMetrics& metrics,
    FluidBakeError& error);

bool water_cut_is_assertion_weldable(
    const WaterCutContourMetrics& metrics,
    float quantization_tolerance_m) noexcept;
```

- `measure_water_cut_continuity` intersects indexed triangles with the handoff plane, joins segments by quantized endpoint key, carries linearly interpolated normals, computes nearest-segment symmetric Hausdorff/RMS metrics, counts combined unmatched boundary edges, and detects equal-position triangle triples with the same ownership plane.
- `water_cut_is_assertion_weldable` is true only when Hausdorff and RMS are within tolerance, `minimum_normal_dot >= 0.995`, and both open-edge and duplicate-triangle counts are zero.

- [ ] **Step 1: Write the failing 3.9 m mismatch fixture.**

  Add two deterministic ribbon meshes that cross one handoff cut with identical topology but median heights `44.9 m` and `48.8 m`. Assert point counts are nonzero, symmetric Hausdorff is at least `3.8 m`, median delta is at least `3.8 m`, and assertion welding rejects `0.15 / 16` tolerance.

  ```cpp
  WaterCutContourMetrics metrics{};
  FluidBakeError error{};
  CHECK(measure_water_cut_continuity(
            static_ribbon(44.9f), animated_ribbon(48.8f),
            handoff_fixture(), -2.5f, 1.0e-4f, metrics, error),
        error.message.c_str());
  CHECK(metrics.symmetric_hausdorff_m >= 3.8f);
  CHECK(std::fabs(metrics.first_height_quantiles_m[1] -
                  metrics.second_height_quantiles_m[1]) >= 3.8f);
  CHECK(!water_cut_is_assertion_weldable(metrics, 0.15f / 16.0f));
  ```

- [ ] **Step 2: Add exact, normal-discontinuous, open-edge, and duplicate fixtures.**

  Prove an exact shared contour passes; a `normal dot < 0.995` contour fails and reports its p95 normal-angle delta; one missing segment increments `unmatched_open_edges`; and copied co-planar triangles increment `duplicate_coplanar_triangles`.

- [ ] **Step 3: Run the test and verify it is red.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
  ```

  Expected: compile failure because `water_mesh_continuity.h` and the metric functions do not exist.

- [ ] **Step 4: Implement the deterministic metric library.**

  Use double precision for distance accumulation, sort contour components and endpoint keys before reduction, use a fixed p10/p50/p90 order statistic, and fail on non-finite mesh data or an absent contour. Do not move vertices or emit triangles.

- [ ] **Step 5: Run the focused test.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/hydrology_handoff_products_tests.exe
  ```

  Expected: PASS; the current multi-metre synthetic mismatch is measurable and cannot be accepted as a weld.

- [ ] **Step 6: Commit the diagnostic contract.**

  ```powershell
  git add MatterEngine3/src/hydrology/water_mesh_continuity.h MatterEngine3/src/hydrology/water_mesh_continuity.cpp cmake/manifests/engine-core.sources MatterEngine3/tests/hydrology_handoff_products_tests.cpp
  git commit -m "test(water): quantify section boundary continuity"
  ```

---

### Task 2: Record cold/cache-hit build, frame, payload, and runtime upload evidence

**Files:**

- Modify: `MatterEngine3/src/hydrology/hydrology_handoff_products.h`
- Modify: `MatterEngine3/src/hydrology/hydrology_handoff_products.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEditor/src/main.cpp`
- Test: `MatterEngine3/tests/hydrology_handoff_products_tests.cpp`
- Test: `MatterEngine3/tests/water_mesh_animation_playback_tests.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**

```cpp
struct HandoffAnimationBuildDiagnostics {
    std::array<double, 30> frame_mesh_ms{};
    std::array<WaterCutContourMetrics, 30> upstream_cut{};
    std::array<WaterCutContourMetrics, 30> downstream_cut{};
    std::uint64_t artifact_file_bytes = 0;
    std::uint64_t peak_build_cpu_payload_bytes = 0;
    bool source_blend_required = false;
};

struct HydrologyHandoffTimings {
    std::string id;
    bool static_cache_hit = false;
    bool animation_cache_hit = false;
    std::array<double, 30> animation_frame_ms{};
    std::uint64_t animation_file_bytes = 0;
    std::uint64_t boundary_source_bytes = 0;
    std::uint64_t peak_build_cpu_payload_bytes = 0;
};
```

- Extends `HydrologyNetworkTimings` with `std::vector<HydrologyHandoffTimings> handoffs` and aggregate `std::uint64_t peak_build_cpu_payload_bytes`. JSON is keyed by handoff id; it does not collapse a longer network into one ambiguous cache flag.
- Extends `FrameStats` and perf JSON with `float water_animation_publish_ms`, `std::uint64_t water_animation_compressed_cpu_bytes`, `std::uint64_t water_animation_peak_activation_cpu_bytes`, `std::uint64_t water_animation_gpu_bytes_per_slot`, and `float gpu_water_direct_draw_ms`.
- Append `kGpuZoneWaterDirectDraw` without renumbering existing query zones. When direct animated draws exist, it begins inside the load-preserving forward-water render pass after static water ranges and immediately before the direct pipeline bind, then ends after the direct draw loop and before `vkCmdEndRendering`; when none exist, report zero without writing the pair. `gpu_water_forward_ms` continues to measure copies + static/direct draws + final transitions.

- [ ] **Step 1: Write failing timing JSON assertions.**

  Extend `test_formats_animation_acceptance_timing_trace()` to require all new keys, exactly 30 finite nonnegative frame times, and exact integer byte values. Add playback fixtures that distinguish retained compressed bytes from peak activation bytes.

- [ ] **Step 2: Run focused tests and verify red.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_playback_tests
  ```

  Expected: compile/assertion failure for missing timing and memory fields.

- [ ] **Step 3: Time each current handoff frame without changing its geometry.**

  Add an optional diagnostics output to the current handoff animation builder. Measure each frame around decode + static-collar append + current stitches, compute both cut metrics, and retain the failed metrics rather than enforcing them in Stage 0.

- [ ] **Step 4: Measure activation and retained memory.**

  Around `activate_water_mesh_animation_playback` and `publish_water_animation`, report wall time and checked byte totals. Track the activation high-water after each load as `retained artifact heap bytes + current serialized file buffer bytes + current decoded candidate artifact heap bytes`; do not assume the file buffer disappears before deserialization copies its frame payload. Final compressed CPU bytes remain the complete-file-size sum used by the existing budget. GPU bytes per slot come from `WaterAnimationGpuCapacity::gpu_bytes_per_slot()`.

- [ ] **Step 5: Isolate the GPU direct-draw span.**

  Add a timestamp pair around only the animated direct-draw loop, excluding opaque HDR/depth copies, static water ranges, and final image transitions. Extend `water-forward` smoke to require a nonzero direct-draw measurement when animated water is eligible and zero when only static water or no water is eligible.

- [ ] **Step 6: Emit stable JSON.**

  Append the hydrology fields to `MATTER_HYDROLOGY_TRACE_DIR/timings.json` and the runtime fields to the existing perf JSON. Use integer bytes and finite milliseconds; missing animation reports zero rather than omitting keys.

- [ ] **Step 7: Run focused tests.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_playback_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/hydrology_handoff_products_tests.exe
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/water_mesh_animation_playback_tests.exe
  $env:MATTER_VK_SMOKE_MODE='water-forward'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

- [ ] **Step 8: Commit observability.**

  ```powershell
  git add -p -- MatterEngine3/src/hydrology/hydrology_handoff_products.h MatterEngine3/src/hydrology/hydrology_handoff_products.cpp MatterEngine3/src/provider/local_provider.cpp MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/include/matter/world_session.h MatterEngine3/src/matter_engine.cpp MatterEditor/src/main.cpp MatterEngine3/tests/hydrology_handoff_products_tests.cpp MatterEngine3/tests/water_mesh_animation_playback_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
  git commit -m "perf(water): expose mesh continuity build costs"
  ```

---

### Task 3: Preserve deterministic phase and diagnostic screenshots

**Files:**

- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/render/water_animation_gpu.h`
- Modify: `MatterEngine3/src/render/water_animation_gpu.cpp`
- Modify: `MatterEngine3/shaders_vk/raster.vert`
- Modify: `MatterEngine3/shaders_vk/gbuffer.frag`
- Modify: `MatterEngine3/shaders_vk/water_screen_space.glsl`
- Modify: `MatterEngine3/shaders_vk/water_forward.frag`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/tests/gpu_water_animation_render_tests.cpp`
- Modify: `MatterEngine3/tests/shader_source_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Create: `MatterEngine3/tools/water_mesh_continuity_diagnostics.timeline`
- Create: `MatterEngine3/tools/run_water_mesh_continuity_diagnostics.ps1`
- Modify: `docs/agent/control-surface.md`

**Interfaces:**

- `MATTER_WATER_CAPTURE_FRAME=<0..29>` freezes only the cosmetic water clock at `(frame + 0.5) / 30.0`, so floating-point rounding cannot select the preceding frame; absent means the existing unscaled presentation clock.
- `MATTER_WATER_DIAGNOSTIC_VIEW=identity|geometry-normal|foam-driver`; absent means normal forward optics.
- Append `uvec4 diagnostics` to the CPU/GLSL `WaterForwardConstants`, making the explicit ABI 128 bytes. `.x` carries the diagnostic enum; the other words remain zero and are asserted.
- Replace the first trailing water padding word in the existing 80-byte CPU/GLSL `RasterDebugPushConstants` with `water_diagnostic_identity`; do not grow the push range. `WaterAnimationFrameDraw::identity` is hashed deterministically into `VkWaterAnimationRasterDraw::diagnostic_identity`, copied into that push word per direct draw, and emitted from `raster.vert` as flat location 17. Static water writes zero and the fragment falls back to `in_instance_token`.
- Diagnostic outputs are:
  - identity: deterministic hash color of the nonzero direct-draw diagnostic identity, otherwise `in_instance_token`; section and handoff artifact identities must map to distinct stable colors across all 30 frames;
  - geometry-normal: `normalize(in_normal) * 0.5 + 0.5` before animated shading normals;
  - foam-driver: heat map of `surface.foam.coverage`, black when the field is invalid.

- [ ] **Step 1: Write failing ABI, parser, and shader assertions.**

  Assert the 128-byte CPU/GLSL forward layout, unchanged 80-byte raster push layout, exact enum mapping, stable nonzero identity hashing, propagation into every valid direct draw, invalid frame/view fail-fast startup messages, and that each diagnostic branch bypasses optics only when selected.

- [ ] **Step 2: Run source and Vulkan tests to verify red.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_animation_render_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  ```

- [ ] **Step 3: Implement normal-off-by-default diagnostics.**

  Parse both environment variables once at session/renderer initialization. A fixed capture frame replaces the accumulated animation time before `select`; it does not change simulation, bake phase, or the network clock contract stored in artifacts. Hash the complete playback identity string rather than vector order, so cache/load order cannot recolor an ownership band.

- [ ] **Step 4: Add the five-phase capture runner.**

  The PowerShell runner launches the editor for frames `0,7,15,22,29`, normal rendering plus each of the three diagnostic views, and both `waterfall-side` and `section-handoff` cameras. It writes 40 named PNG/`.done` pairs under `build/qa/water-mesh-continuity-2026-08-29/stage0/diagnostics/` and rejects a missing or zero-byte pair. The normal run removes `MATTER_WATER_DIAGNOSTIC_VIEW` from the child environment rather than inventing a fourth enum value.

  ```text
  wait_event bake.finished 3600
  cam 119 72 2 110 49 5
  wait_idle 2 900
  shot {{OUTPUT_DIR}}/waterfall-side.png
  cam 140 70 -1 151 43 0
  wait_idle 2 900
  shot {{OUTPUT_DIR}}/section-handoff.png
  quit
  ```

- [ ] **Step 5: Run the tests and diagnostics on Vulkan.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target shader_source_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_animation_render_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/shader_source_tests.exe
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/gpu_water_animation_render_tests.exe
  $env:MATTER_VK_SMOKE_MODE='water-forward'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor
  & MatterEngine3/tools/run_water_mesh_continuity_diagnostics.ps1 -OutputDir build/qa/water-mesh-continuity-2026-08-29/stage0
  ```

  Expected: all 40 captures exist; the identity view reproduces one waterfall section draw and three non-overlapping handoff ownership bands; normal, geometry-normal, and foam closeups reproduce the blocked discontinuity.

- [ ] **Step 6: Commit the QA-only capture surface.**

  ```powershell
  git add -p -- MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/render/water_animation_gpu.h MatterEngine3/src/render/water_animation_gpu.cpp MatterEngine3/shaders_vk/raster.vert MatterEngine3/shaders_vk/gbuffer.frag MatterEngine3/shaders_vk/water_screen_space.glsl MatterEngine3/shaders_vk/water_forward.frag MatterEngine3/src/matter_engine.cpp MatterEngine3/tests/gpu_water_animation_render_tests.cpp MatterEngine3/tests/shader_source_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp docs/agent/control-surface.md
  git add MatterEngine3/tools/water_mesh_continuity_diagnostics.timeline MatterEngine3/tools/run_water_mesh_continuity_diagnostics.ps1
  git commit -m "test(water): retain phased continuity diagnostics"
  ```

---

## Stage 1 — canonical lattice and animated handoff replacement

### Task 4: Add the versioned world-anchored exact-voxel lattice

**Files:**

- Modify: `MatterEngine3/include/matter/gpu_visual_meshing.h`
- Modify: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp`
- Modify: `MatterEngine3/src/hydrology/physx_fluid_bake.cpp`
- Modify: `MatterEngine3/src/hydrology/water_visual_products.cpp`
- Modify: `MatterEngine3/src/hydrology/water_mesh_animation_artifact.h`
- Modify: `MatterEngine3/src/hydrology/water_mesh_animation_artifact.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Test: `MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp`
- Test: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp`
- Test: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- Test: `MatterEngine3/tests/water_mesh_animation_artifact_tests.cpp`

**Interfaces:**

```cpp
struct ParticleSamplingLattice {
    matter::Float3 origin_m{};
    float voxel_m = 0.0f;
    std::uint32_t version = 0; // 0 legacy/tight; 1 canonical
};

bool make_particle_sampling_lattice(
    const matter::Float3& world_anchor_m, float voxel_m,
    ParticleSamplingLattice& lattice, Error& error);

bool particle_field_support_radius_m(
    float particle_radius_m, float blend_width_m,
    float& support_radius_m, Error& error);
```

- Append `ParticleSamplingLattice sampling_lattice{}` to `ParticleJob`.
- Append `std::array<std::int64_t,3> cell_min` to `GridLayout`.
- Production uses the stable world anchor `constexpr matter::Float3 kWaterVisualWorldAnchorM{0.0f, 0.0f, 0.0f}`. Bounds select only integer cell minima/maxima; moving or extending a downstream river cannot move the anchor or invalidate an unchanged upstream section.
- Version 1 requires finite anchor, finite positive lattice voxel, and bit-equal `job.voxel_m == sampling_lattice.voxel_m`. It computes integer cell minima/maxima from bounds, `layout.origin_m = anchor + cell_min * voxel`, and `layout.spacing_m = {voxel,voxel,voxel}` exactly.
- Add `ParticleSamplingLattice lattice` to `WaterMeshAnimationArtifactMetadata` and `WaterMeshAnimationArtifact`; change magic/version to `MHYDWAN2`/2 and semantic string to `water-mesh-animation-v2`.

- [ ] **Step 1: Write failing non-origin-aligned lattice tests.**

  Use anchor `{-31.2, 7.4, 11.8}`, voxel `0.15`, and non-aligned bounds. Assert outward integer cell expansion, exact equal spacing on every axis, stable cell indices after a slightly different tight particle bound, and rejection of a conflicting `job.voxel_m`.

- [ ] **Step 2: Write failing chunk and artifact identity tests.**

  Assert chunked and unchunked jobs inherit byte-identical lattice metadata; changing origin, voxel, or lattice version changes the visual product key and section animation semantic/payload identity while leaving coarse/gameplay keys unchanged; v1 animation bytes fail closed as stale instead of being read as v2.

- [ ] **Step 3: Add the Vulkan parity fixture.**

  In `run_gpu_visual_mesher_vk_tests`, evaluate every sample on the translated lattice and compare GPU field values to `evaluate_particle_field_reference` at `origin + exact_voxel * integer_index` within `2e-5`. Assert the returned CPU and repeated GPU layouts are identical.

- [ ] **Step 4: Run focused CPU/GPU tests and verify red.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_artifact_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  $env:MATTER_VK_SMOKE_MODE='gpu-mesher'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

- [ ] **Step 5: Implement exact lattice validation and support query.**

  Use checked double-precision floor/ceil for index derivation, reject `int64`/`uint32` overflow before allocation, and make the public support helper the only implementation of the current radius/blend support formula.

- [ ] **Step 6: Anchor the network once and propagate it.**

  `assemble_authored_fluid_section_request` constructs the lattice from `kWaterVisualWorldAnchorM` and the authored visual voxel before section particles are known. `resolved_visual_job`, section animation jobs, recursive chunks, static handoff jobs, and failure diagnostics inherit it unchanged. Add a downstream-geometry-edit regression proving the upstream lattice and visual product key stay unchanged.

- [ ] **Step 7: Persist and hash v2 metadata.**

  Serialize origin, voxel, and version before frame records. Add those three fields only to the `ProductKeys::visual` digest returned by `derive_product_keys` and to the animation identity; update validation and equality fixtures so old tight-lattice visual assets miss once without invalidating the unchanged coarse collision/gameplay products.

- [ ] **Step 8: Run focused and GPU gates.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_artifact_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/gpu_visual_mesher_cpu_tests.exe
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/physx_adapter_contract_tests.exe
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/water_mesh_animation_artifact_tests.exe
  $env:MATTER_VK_SMOKE_MODE='gpu-mesher'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

- [ ] **Step 9: Commit the lattice foundation.**

  ```powershell
  git add -p -- MatterEngine3/include/matter/gpu_visual_meshing.h MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp MatterEngine3/src/hydrology/physx_fluid_bake.cpp MatterEngine3/src/hydrology/water_visual_products.cpp MatterEngine3/src/hydrology/water_mesh_animation_artifact.h MatterEngine3/src/hydrology/water_mesh_animation_artifact.cpp MatterEngine3/src/provider/local_provider.cpp MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp MatterEngine3/tests/physx_adapter_contract_tests.cpp MatterEngine3/tests/water_mesh_animation_artifact_tests.cpp
  git commit -m "feat(water): anchor visual meshing to a canonical lattice"
  ```

---

### Task 5: Persist cropped boundary particle-frame sidecars

**Files:**

- Create: `MatterEngine3/src/hydrology/water_boundary_animation_source.h`
- Create: `MatterEngine3/src/hydrology/water_boundary_animation_source.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Create: `MatterEngine3/tests/water_boundary_animation_source_tests.cpp`
- Modify: `cmake/MatterEngine.cmake`

**Interfaces:**

```cpp
struct WaterBoundaryCaptureFrameRecord {
    std::uint32_t simulation_step = 0;
    std::uint64_t position_byte_offset = 0;
    std::uint32_t particle_count = 0;
    std::uint64_t content_digest = 0;
};

struct WaterBoundaryAnimationSource {
    std::string section_id;
    std::uint64_t source_section_payload_digest = 0;
    std::uint64_t handoff_semantic_key = 0;
    gpu_meshing::ParticleSamplingLattice lattice{};
    std::uint32_t frames_per_second = 0;
    std::uint32_t phase_offset_frames = 0;
    float particle_radius_m = 0.0f;
    float blend_width_m = 0.0f;
    gpu_meshing::Aabb crop_bounds_m{};
    std::vector<WaterBoundaryCaptureFrameRecord> frames;
    std::vector<std::uint8_t> quantized_positions;
    std::uint64_t payload_digest = 0;
};

bool build_water_boundary_animation_source(
    const FluidParticleAnimationCapture& capture,
    std::string_view section_id,
    std::uint64_t source_section_payload_digest,
    const SpillwayHandoffRecord& handoff,
    const gpu_meshing::ParticleSamplingLattice& lattice,
    float particle_radius_m,
    float blend_width_m,
    bool upstream_endpoint,
    WaterBoundaryAnimationSource& source,
    gpu_meshing::Error& error);

bool decode_water_boundary_frame(
    const WaterBoundaryAnimationSource& source,
    std::uint32_t frame_index,
    std::vector<matter::Float3>& positions_m,
    gpu_meshing::Error& error);

bool water_boundary_source_contains(
    const WaterBoundaryAnimationSource& source,
    const matter::Float3& position_m) noexcept;
```

- Also produces immutable serialize/deserialize/save/load functions using magic `MHYDWBS1`, version 1, local UNORM16 XYZ positions (6 bytes), per-frame digests, and a checked 1 GiB file ceiling.
- Crop bounds cover signed handoff strip `[-overlap/2,+overlap/2]`, full width/vertical capture bounds, plus the value returned by `particle_field_support_radius_m` on every side, then expand to canonical lattice faces.
- Upstream capture excludes every particle whose support intersects `temporary_dam_exclusion_bounds_m`; downstream capture does not apply that exclusion.

- [ ] **Step 1: Write failing round-trip and quantization tests.**

  Build 30 frames with distinct steps and positions. Require deterministic bytes/digest, exact metadata, per-frame order, maximum decoded position error `<= lattice.voxel_m / 16`, and immutable same-content save idempotence.

- [ ] **Step 2: Write failing crop/halo/dam tests.**

  Place particles just inside/outside the strip, exactly one support radius outside, and inside the expanded temporary dam. Assert full-support contributors survive, unrelated particles are absent, and no excluded upstream dam particle survives any frame.

- [ ] **Step 3: Write corruption and partial-set tests.**

  Truncate the directory, corrupt one frame digest, provide 29 frames, overflow offsets, and change lattice metadata. Every case must fail with an empty output and an `ArtifactFailure`/`LimitExceeded` error.

- [ ] **Step 4: Run and verify red.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_boundary_animation_source_tests
  ```

- [ ] **Step 5: Implement bounded crop and immutable artifact IO.**

  Sort positions lexicographically before quantization for deterministic bytes, retain `simulation_step`, and decode only the requested frame. Use one half-open snapped-crop predicate for both extraction and later raw-frame substitution, so a boundary particle is neither dropped nor duplicated. Do not store velocity, full-section capture data, or runtime mesh data.

- [ ] **Step 6: Run focused tests.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_boundary_animation_source_tests
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/water_boundary_animation_source_tests.exe
  ```

- [ ] **Step 7: Commit the sidecar format.**

  ```powershell
  git add MatterEngine3/src/hydrology/water_boundary_animation_source.h MatterEngine3/src/hydrology/water_boundary_animation_source.cpp MatterEngine3/tests/water_boundary_animation_source_tests.cpp
  git add -p -- cmake/manifests/engine-core.sources cmake/MatterEngine.cmake
  git commit -m "feat(water): persist cropped handoff particle frames"
  ```

---

### Task 6: Integrate sidecar production, cache admission, and invalidation

**Files:**

- Modify: `MatterEngine3/src/hydrology/river_section_coordinator.h`
- Modify: `MatterEngine3/src/hydrology/river_section_coordinator.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Modify: `MatterEngine3/src/hydrology/hydrology_handoff_products.h`
- Modify: `MatterEngine3/src/hydrology/water_mesh_animation.h`
- Modify: `MatterEngine3/src/hydrology/water_mesh_animation.cpp`
- Test: `MatterEngine3/tests/river_section_coordinator_tests.cpp`
- Test: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- Test: `MatterEngine3/tests/hydrology_handoff_products_tests.cpp`

**Interfaces:**

- Append `std::vector<WaterBoundaryAnimationSource> boundary_sources` to `SectionBakeResult`; it is transient and is not serialized into the runtime network manifest.
- Extend `build_water_mesh_animation` with a span of canonical endpoint sources. For each primary/secondary source frame, it filters raw positions with the shared half-open crop predicate and appends that source's decoded positions before constructing the existing `ParticlePhaseBlend`; it materializes only the current frame pair, never a second 30-frame capture.
- Section sidecar path:

  ```text
  <cache>/hydrology/animations/boundaries/<section>-<handoff>-<semantic>.mhwb
  ```

- Boundary semantic key includes `section semantic key`, accepted section payload digest, handoff semantic key, upstream/downstream endpoint role, animation profile, particle radius/blend, crop bounds, and lattice version/origin/voxel.
- `section_result.cache_hit` is true only when static artifact, v2 animation artifact, and every required endpoint sidecar validate.

- [ ] **Step 1: Write failing cache transaction tests.**

  Cover: all products hit; missing sidecar; corrupt sidecar; partial 29-frame sidecar; overlapping endpoint crops; downstream-only edit; upstream handoff edit; and save failure after a previously accepted network. Assert missing/corrupt input never reaches a handoff builder, overlapping substitutions fail rather than silently choosing precedence, and no failure replaces a ready manifest.

- [ ] **Step 2: Write the producer-order test.**

  A fake backend records calls. Require `simulate -> static product -> boundary source -> owned section animation from sidecar-normalized frames -> immutable saves -> handoff build -> ready manifest` on cold bake. Assert the bulk-frame boundary contributor digest equals the sidecar frame digest after substitution. On downstream edit require zero upstream simulation calls and reuse of the upstream sidecar digest.

- [ ] **Step 3: Run focused tests and verify red.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target river_section_coordinator_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
  ```

- [ ] **Step 4: Build sidecars before the owned animation.**

  In the section executor, after the static product yields the accepted section payload digest and before releasing `output.animation_capture`, build one endpoint source for every incoming/outgoing handoff. Pass the validated in-memory sources to `build_water_mesh_animation`, which substitutes their decoded coordinates inside the exact same crop used during extraction. At each outer cut, every particle with nonzero support is therefore the same decoded particle in bulk and strip builds.

- [ ] **Step 5: Make cache admission all-or-nothing.**

  Calculate expected sidecar keys/paths before deciding `cache_hit`. A missing v2 sidecar deliberately reruns the section once; a valid upstream sidecar remains reusable when only downstream inputs change.

- [ ] **Step 6: Save, reopen, and carry sources transiently.**

  Use immutable temporary-file + validate + install behavior. Reopen every saved sidecar before adding it to `SectionBakeResult`. Do not add sidecar references to `HydrologyNetworkArtifact`.

- [ ] **Step 7: Run focused cache gates.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target river_section_coordinator_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/river_section_coordinator_tests.exe
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/physx_adapter_contract_tests.exe
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/hydrology_handoff_products_tests.exe
  ```

- [ ] **Step 8: Commit sidecar lifecycle integration.**

  ```powershell
  git add -p -- MatterEngine3/src/hydrology/river_section_coordinator.h MatterEngine3/src/hydrology/river_section_coordinator.cpp MatterEngine3/src/provider/local_provider.cpp MatterEngine3/src/hydrology/hydrology_handoff_products.h MatterEngine3/src/hydrology/water_mesh_animation.h MatterEngine3/src/hydrology/water_mesh_animation.cpp MatterEngine3/tests/river_section_coordinator_tests.cpp MatterEngine3/tests/physx_adapter_contract_tests.cpp MatterEngine3/tests/hydrology_handoff_products_tests.cpp
  git commit -m "feat(water): cache handoff boundary animation sources"
  ```

---

### Task 7: Replace the static animated collar and zipper with one per-frame strip

**Files:**

- Modify: `MatterEngine3/src/hydrology/hydrology_handoff_products.h`
- Modify: `MatterEngine3/src/hydrology/hydrology_handoff_products.cpp`
- Modify: `MatterEngine3/src/hydrology/water_mesh_continuity.h`
- Modify: `MatterEngine3/src/hydrology/water_mesh_continuity.cpp`
- Modify: `MatterEngine3/src/hydrology/water_mesh_animation.h`
- Modify: `MatterEngine3/src/hydrology/water_mesh_animation.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Test: `MatterEngine3/tests/hydrology_handoff_products_tests.cpp`
- Test: `MatterEngine3/tests/water_mesh_animation_tests.cpp`
- Test: `MatterEngine3/tests/water_mesh_animation_playback_tests.cpp`

**Interfaces:**

```cpp
struct HandoffAnimationBuildInput {
    const WaterBoundaryAnimationSource* upstream = nullptr;
    const WaterBoundaryAnimationSource* downstream = nullptr;
    const WaterMeshAnimationArtifact* upstream_bulk = nullptr;
    const WaterMeshAnimationArtifact* downstream_bulk = nullptr;
    SpillwayHandoffRecord handoff{};
    gpu_meshing::ParticleSamplingLattice lattice{};
    gpu_meshing::ParticleJob visual_template{};
};

struct MeshIndexRange {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
};

struct HandoffFrameProducts {
    gpu_meshing::MeshResult replacement_strip;
    MeshIndexRange upstream_band{};
    MeshIndexRange collar{};
    MeshIndexRange downstream_band{};
};

struct WaterCellOwnershipCut {
    gpu_meshing::ParticleSamplingLattice lattice{};
    SpillwayHandoffRecord handoff{};
    float signed_cut_m = 0.0f;
};

bool build_handoff_animation_frames(
    const HandoffAnimationBuildInput& input,
    const PhysxFluidBake::VisualMesher& mesher,
    WaterMeshAnimation& output,
    HandoffAnimationBuildDiagnostics& diagnostics,
    FluidBakeError& error);

bool clip_section_water_mesh_animation(
    const WaterMeshAnimation& source,
    const std::string& section_id,
    const std::vector<SpillwayHandoffRecord>& handoffs,
    const gpu_meshing::ParticleSamplingLattice& lattice,
    WaterMeshAnimation& owned,
    FluidBakeError& error);

bool build_handoff_water_animation_artifact(
    const HandoffAnimationBuildInput& input,
    const PhysxFluidBake::VisualMesher& mesher,
    WaterMeshAnimationArtifact& artifact,
    HandoffAnimationBuildDiagnostics& diagnostics,
    FluidBakeError& error);

bool measure_water_cell_boundary_continuity(
    const gpu_meshing::MeshResult& first,
    const gpu_meshing::MeshResult& second,
    const WaterCellOwnershipCut& cut,
    float edge_match_tolerance_m,
    WaterCutContourMetrics& metrics,
    FluidBakeError& error);

std::uint64_t derive_handoff_animation_semantic_key(
    const HandoffAnimationBuildInput& input,
    std::uint64_t upstream_animation_payload_digest,
    std::uint64_t downstream_animation_payload_digest);
```

- Replace `build_handoff_water_animation_artifact` with a wrapper around this builder plus v2 packing.
- Section bulk ownership and strip ownership classify triangles by canonical root-cell center against `upstream_visual_cut_m` and `downstream_visual_cut_m`. This represents the oriented cut as a deterministic set of lattice cell faces; no mesh is polygon-clipped at an arbitrary plane.
- For frame `i`, decode raw captures `i` and `(i + 15) % 30` for both sources. Order particles as upstream+downstream primary followed by upstream+downstream secondary, then reuse the existing cosine `ParticlePhaseBlend` weights.
- Before meshing, enforce complete-support source guards: retain upstream particles for the strip only when their support cannot cross the downstream outer cut, and retain downstream particles only when their support cannot cross the upstream outer cut. The remaining middle interval still contains both sources for the smooth-union trial, while each outer sample layer is influenced by exactly the same section source as its adjacent bulk mesh.
- Cache path is `<cache>/hydrology/animations/handoffs/<handoff>-<semantic>.mhwa`. The semantic key includes the handoff record, both boundary-source payload digests, both owned-bulk animation payload digests, lattice identity, visual radius/blend/iso settings, phase profile, and replacement-strip contract version.

- [ ] **Step 1: Write the failing per-frame source test.**

  Give frames 0 and 15 visibly different endpoint positions. Assert output frame 0 uses both with the existing cosine weights and does not contain any vertex/digest from `static_handoff.visual_mesh`. Change each semantic input one at a time and assert the handoff animation key changes; identical inputs must produce the same key and cache path.

- [ ] **Step 2: Write failing ownership and continuity tests.**

  For all 30 frames, including 29 -> 0, require:

  ```cpp
  CHECK(metrics.symmetric_hausdorff_m <= input.lattice.voxel_m / 16.0f);
  CHECK(metrics.minimum_normal_dot >= 0.995f);
  CHECK(metrics.unmatched_open_edges == 0u);
  CHECK(metrics.duplicate_coplanar_triangles == 0u);
  ```

  Also require upstream/collar/downstream index ranges are disjoint and cover every replacement index once.

- [ ] **Step 3: Write failing chunk parity and dam-exclusion tests.**

  Mesh the same strip unchunked and under forced small grid limits. Compare canonicalized positions/topology within the existing weld tolerance. Seed an excluded upstream dam particle and assert no field-support contribution or visible triangle remains. Put downstream particles within one support radius of the upstream outer cut and upstream particles within one support radius of the downstream outer cut; prove those individual cross-cut particles are rejected. Separately seed particles from both sources in the interior overlap and prove both populations remain.

- [ ] **Step 4: Run focused tests and verify red.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_tests
  ```

- [ ] **Step 5: Implement canonical cell ownership.**

  Derive each triangle's root cell from its centroid and the v1 lattice. Section bulk retains cells before/after the outer strip; the strip retains only the band between those cuts. Use the same classification function for static and animated products. Extract the resulting stepped set of shared lattice faces with `WaterCellOwnershipCut`; Stage 1 continuity uses `measure_water_cell_boundary_continuity`, while the Stage 0 planar helper remains available only for the legacy zipper baseline.

- [ ] **Step 6: Mesh one smooth-union strip per frame.**

  Decode only four cropped frames at a time, build a single `ParticleJob` over snapped strip bounds with the full source halo, call `PhysxFluidBake::build_visual_job_chunks`, partition diagnostic index ranges, then release decoded positions before the next frame.

- [ ] **Step 7: Remove legacy animation stitching.**

  Delete the per-frame append of `static_handoff.visual_mesh` and both animated `append_cut_stitches` calls. Do not route animation construction through the legacy static fallback builder. The static fallback remains byte-compatible in this plan; it is not rendered when the accepted animated products are active, and none of its bridge triangles may appear in an animated artifact.

- [ ] **Step 8: Enforce continuity before packing.**

  Decode matching owned bulk frames, measure both outer cuts, and fail the candidate with frame/cut metrics when tolerance, normal, open-edge, or duplicate-triangle gates fail. In `local_provider.cpp`, validate a cache-hit candidate against the same v2 metadata/key before skipping sidecar decode; save a cold candidate immutably only after all 30 frames pass. Do not mutate an already accepted artifact.

- [ ] **Step 9: Preserve playback synchronization.**

  Keep 30 frames, 30 fps, 15-frame offset, shared `water_animation_frame`, and raster direct draws unchanged. Extend playback tests to assert all section/handoff draws select frame 29 and wrap to 0 together.

- [ ] **Step 10: Run focused tests.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_playback_tests
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/hydrology_handoff_products_tests.exe
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/water_mesh_animation_tests.exe
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/water_mesh_animation_playback_tests.exe
  ```

- [ ] **Step 11: Commit the replacement strip.**

  ```powershell
  git add -p -- MatterEngine3/src/hydrology/hydrology_handoff_products.h MatterEngine3/src/hydrology/hydrology_handoff_products.cpp MatterEngine3/src/hydrology/water_mesh_continuity.h MatterEngine3/src/hydrology/water_mesh_continuity.cpp MatterEngine3/src/hydrology/water_mesh_animation.h MatterEngine3/src/hydrology/water_mesh_animation.cpp MatterEngine3/src/provider/local_provider.cpp MatterEngine3/tests/hydrology_handoff_products_tests.cpp MatterEngine3/tests/water_mesh_animation_tests.cpp MatterEngine3/tests/water_mesh_animation_playback_tests.cpp
  git commit -m "fix(water): animate handoffs from a shared field"
  ```

---

### Task 8: Add longitudinal scalar-field blending only if the union gate requires it

**Execution rule:** Run the Stage 1 synthetic cross-section gate and one real RiverFloatLab cold bake after Task 7. Execute this task only if either (a) any interior strip cross-section area exceeds `1.10 * max(upstream_area, downstream_area)`, (b) a dry source erases a valid wet surface, or (c) the strip visual shows a union bulge while the outer cuts already pass. Otherwise record `sourceBlendRequired=false` in the Stage 1 finding and skip this commit.

**Files (only when triggered):**

- Modify: `MatterEngine3/include/matter/gpu_visual_meshing.h`
- Modify: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp`
- Modify: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp`
- Modify: `MatterEngine3/shaders_vk/gpu_mesh_field.comp`
- Modify: `MatterEngine3/src/hydrology/hydrology_handoff_products.cpp`
- Test: `MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp`
- Test: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp`
- Test: `MatterEngine3/tests/hydrology_handoff_products_tests.cpp`

**Interfaces:**

```cpp
struct ParticleSourcePhaseSpan {
    std::uint32_t primary_begin = 0, primary_count = 0;
    std::uint32_t secondary_begin = 0, secondary_count = 0;
};

struct ParticleLongitudinalFieldBlend {
    std::array<ParticleSourcePhaseSpan, 2> source{};
    matter::Float3 origin_m{};
    matter::Float3 direction{};
    float upstream_full_m = 0.0f;
    float downstream_full_m = 0.0f;
    bool enabled = false;
};
```

- Append this descriptor to `ParticleJob`. CPU and GPU evaluate temporal smooth-min independently for each source, then blend scalar values with `smoothstep(upstream_full_m, downstream_full_m, dot(p-origin,direction))`.
- Wet/dry rule: two finite fields interpolate; one finite field wins; neither finite returns infinity. Outer sample bands force exactly upstream-only or downstream-only.

- [ ] **Step 1: Write failing CPU wet/dry, endpoint, and no-bulge tests.**

  Assert endpoint samples are bit-identical to the single source, dry cannot erase wet, two equal fields remain equal, and a doubled coincident source does not thicken the iso-surface.

- [ ] **Step 2: Add GPU parity tests on translated canonical coordinates.**

  Compare every GPU field sample to the CPU source-blend reference within `2e-5`, including zero-weight NaN particles and both wet/dry branches.

- [ ] **Step 3: Implement the bounded field blend.**

  Extend `FieldParams` and shader evaluation without changing the particle `vec4` ABI. Default-disabled jobs remain byte-for-byte equivalent to accepted static GPU mesher fixtures.

- [ ] **Step 4: Use it only inside the handoff strip.**

  Set full-source positions inside the outer cuts by at least one complete support halo. Keep temporal phase math unchanged and rerun the exact boundary assertions.

- [ ] **Step 5: Run CPU/GPU/handoff gates.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/gpu_visual_mesher_cpu_tests.exe
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/hydrology_handoff_products_tests.exe
  $env:MATTER_VK_SMOKE_MODE='gpu-mesher'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

- [ ] **Step 6: Commit only when the execution rule triggered.**

  ```powershell
  git add -p -- MatterEngine3/include/matter/gpu_visual_meshing.h MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp MatterEngine3/shaders_vk/gpu_mesh_field.comp MatterEngine3/src/hydrology/hydrology_handoff_products.cpp MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp MatterEngine3/tests/hydrology_handoff_products_tests.cpp
  git commit -m "fix(water): blend handoff scalar sources longitudinally"
  ```

---

### Task 9: Prove Stage 1 cache locality, fields, GPU ownership, and visual continuity

**Files:**

- Create: `MatterEngine3/tools/water_mesh_continuity_acceptance.py`
- Create: `MatterEngine3/tools/tests/test_water_mesh_continuity_acceptance.py`
- Create: `MatterEngine3/tools/run_water_mesh_continuity_acceptance.ps1`
- Create: `MatterEngine3/tools/water_mesh_continuity_acceptance.timeline`
- Create: `docs/findings/animated-water-section-continuity-acceptance-2026-08-29.md`
- Modify: `docs/agent/qa-cookbook.md`

**Interfaces:**

- `water_mesh_continuity_acceptance.py stage1 --cold <timings.json> --cache <timings.json> --screenshots <dir>` exits nonzero on any metric, cache, memory, or screenshot failure and writes one JSON summary.
- Required phase screenshots at frames `0,7,15,22,29`: normal, geometry-normal, foam-driver, and identity section-handoff views.
- Stage 2 is blocked until the Stage 1 finding says `handoffVisualGate: pass`.

- [ ] **Step 1: Write failing comparator tests.**

  Cover exact pass plus each failure: >quantization Hausdorff, normal dot below 0.995, open edge, duplicate triangle, frame count not 30, frame 29/0 mismatch, upstream resimulation on downstream edit, dam-support survivor, nonzero water RT counter, missing phase image, and invalid JSON.

- [ ] **Step 2: Run Python tests and verify red.**

  ```powershell
  py -3 -m unittest MatterEngine3.tools.tests.test_water_mesh_continuity_acceptance -v
  ```

- [ ] **Step 3: Implement cold/cache-hit runner.**

  Use separate empty cache/output roots. Run one cold bake, one unchanged cache-hit bake, then a fixture-only downstream edit. Set `MATTER_HYDROLOGY_TRACE_DIR` for each. Assert unchanged upstream `simulateMs == 0`, upstream sidecar digest is stable, and only downstream endpoint plus dependent handoff keys change.

- [ ] **Step 4: Add field and feature assertions.**

  In C++ handoff tests and JSON, require height/normal/turbulence/aeration/foam samples bounded and continuous at both cuts, deterministic feature label selection, and no excluded dam contributor.

- [ ] **Step 5: Run native build, CPU suite, and GPU modes.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo
  & 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -L cpu --output-on-failure
  foreach ($mode in 'gpu-mesher','water-forward','water-animation','default') {
      $env:MATTER_VK_SMOKE_MODE=$mode
      & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
      if ($LASTEXITCODE -ne 0) { throw "$mode failed" }
  }
  ```

  Expected: zero Vulkan validation errors; water decode/BLAS/TLAS/RT records remain zero; raster direct draw remains active.

- [ ] **Step 6: Run Stage 1 real acceptance and inspect phase captures.**

  ```powershell
  & MatterEngine3/tools/run_water_mesh_continuity_acceptance.ps1 -Stage Stage1 -OutputDir build/qa/water-mesh-continuity-2026-08-29/stage1
  ```

  Explicit visual gate:

  ```text
  every phase: no coverage step, long bridge triangle, or opening at either cut
  geometry-normal: no normal color jump at either cut
  foam-driver: no seam-aligned foam discontinuity; real turbulence variation remains
  identity: upstream/strip/downstream ownership remains disjoint with no missing band
  animation: frame 29 to 0 is synchronous across every draw
  ```

- [ ] **Step 7: Record Stage 1 evidence.**

  Include the old 3.871 m blocked baseline, new worst-frame metrics, cache behavior, per-frame build times, sidecar bytes/peak residency, zero RT counters, and links to all phase images. Do not start Task 10 until the finding records a pass.

- [ ] **Step 8: Commit Stage 1 acceptance tooling and evidence.**

  ```powershell
  git add MatterEngine3/tools/water_mesh_continuity_acceptance.py MatterEngine3/tools/tests/test_water_mesh_continuity_acceptance.py MatterEngine3/tools/run_water_mesh_continuity_acceptance.ps1 MatterEngine3/tools/water_mesh_continuity_acceptance.timeline docs/findings/animated-water-section-continuity-acceptance-2026-08-29.md
  git add -p -- docs/agent/qa-cookbook.md
  git commit -m "test(water): accept animated section handoffs"
  ```

---

## Stage 2 — bounded waterfall-local visual refinement

### Task 10: Measure the thin-sheet quality matrix before changing RiverFloatLab

**Files:**

- Modify: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.h`
- Modify: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Create: `MatterEngine3/tools/waterfall_visual_quality.py`
- Create: `MatterEngine3/tools/tests/test_waterfall_visual_quality.py`
- Create: `MatterEngine3/tools/run_waterfall_visual_quality.ps1`

**Interfaces:**

- Adds smoke mode `waterfall-mesher` and `run_gpu_visual_mesher_waterfall_quality(VulkanDevice&, const std::filesystem::path& report)`.
- The fixture is an approximately 12 m falling, curving, 0.26 m-diameter particle sheet with current radius `0.13 m`, blend `0.10 m`, and identical captures for every candidate.
- JSON rows for voxel `0.15`, `0.10`, and `0.075` contain vertices, triangles, connected components, open edges/holes, silhouette Hausdorff to the `0.075` oracle, normal variation, projected complete 30-frame `.mhwa` file bytes (header, metadata, directory, and frame payload), and GPU mesh milliseconds.
- Additional A/B rows use `(radius,blend) = (0.14,0.10)` and `(0.13,0.12)` at `0.15` without changing production defaults.

- [ ] **Step 1: Write failing parser/decision tests.**

  Require the oracle row, finite metrics, stable component count, zero new holes, geometry error below `0.15 / 4`, and a deterministic choice of the coarsest passing candidate. Reject any candidate whose projected replacement complete-file bytes would push the measured RiverFloatLab network above 700 MiB or its containing section file above 1 GiB.

- [ ] **Step 2: Add the synthetic GPU fixture and verify red.**

  ```powershell
  py -3 -m unittest MatterEngine3.tools.tests.test_waterfall_visual_quality -v
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  $env:MATTER_VK_SMOKE_MODE='waterfall-mesher'
  $env:MATTER_WATERFALL_QUALITY_REPORT=(Resolve-Path build).Path + '/qa/waterfall-quality.json'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

  Expected: mode/report support is missing.

- [ ] **Step 3: Implement deterministic topology and silhouette metrics.**

  Use the finest `0.075` mesh as the comparison oracle. Project silhouettes into the retained Task 8 waterfall camera as well as measuring world geometry. Parameter-only variants pass only if coverage outside the sheet changes by at most 2% and intentional spray component count is unchanged.

- [ ] **Step 4: Run the quality matrix three times.**

  ```powershell
  py -3 -m unittest MatterEngine3.tools.tests.test_waterfall_visual_quality -v
  & MatterEngine3/tools/run_waterfall_visual_quality.ps1 -OutputDir build/qa/water-mesh-continuity-2026-08-29/waterfall-matrix
  ```

  Require identical topology/digest decisions across runs. The implementation candidate is the coarsest row passing geometry/topology/pixel and projected memory gates; if no row passes, stop Stage 2 without changing RiverFloatLab.

- [ ] **Step 5: Commit the measurement harness.**

  ```powershell
  git add -p -- MatterEngine3/tests/gpu_visual_mesher_vk_tests.h MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
  git add MatterEngine3/tools/waterfall_visual_quality.py MatterEngine3/tools/tests/test_waterfall_visual_quality.py MatterEngine3/tools/run_waterfall_visual_quality.ps1
  git commit -m "test(water): measure bounded waterfall mesh quality"
  ```

---

### Task 11: Build the bounded waterfall replacement with locked scalar boundaries

**Rejected / open:** Do not execute this architecture. Scalar locking and the
later conforming-fairing amendment both failed real frame-0 gates; a new
bounded design is required before waterfall refinement resumes.

**Prerequisite:** Task 10 must select a passing candidate. Use its exact reported voxel/radius/blend row in the retained finding. Do not silently substitute a finer row. `particleSpacing` remains `0.20 m` in every case.

**Files:**

- Create: `MatterEngine3/src/hydrology/waterfall_visual_refinement.h`
- Create: `MatterEngine3/src/hydrology/waterfall_visual_refinement.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `MatterEngine3/include/matter/gpu_visual_meshing.h`
- Modify: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp`
- Modify: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp`
- Modify: `MatterEngine3/shaders_vk/gpu_mesh_field.comp`
- Modify: `MatterEngine3/src/hydrology/water_mesh_animation.h`
- Modify: `MatterEngine3/src/hydrology/water_mesh_animation.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Create: `MatterEngine3/tests/waterfall_visual_refinement_tests.cpp`
- Modify: `cmake/MatterEngine.cmake`
- Test: `MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp`
- Test: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp`

**Interfaces:**

```cpp
struct ParticleScalarBoundaryOverride {
    const float* locked_values = nullptr;
    const float* true_field_weights = nullptr; // 0 locked, 1 particle field
    std::uint32_t sample_count = 0;
};

struct WaterfallVisualQualityRegion {
    std::string id;
    float from_distance_m = 0.0f; // lip - 2 m
    float to_distance_m = 0.0f;   // landing + 4 m
    float visual_voxel_m = 0.0f;
    gpu_meshing::Aabb snapped_bounds_m{};
};

bool build_waterfall_refined_animation(
    const FluidParticleAnimationCapture& capture,
    const matter::RiverSectionDefinition& section,
    const RiverGeometry& geometry,
    float particle_radius_m,
    const gpu_meshing::ParticleJob& coarse_template,
    const WaterMeshAnimationMesher& mesher,
    WaterMeshAnimation& animation,
    WaterfallRefinementDiagnostics& diagnostics,
    gpu_meshing::Error& error);
```

- Append `ParticleScalarBoundaryOverride boundary_override{}` to `ParticleJob`. Default null/count zero is byte-equivalent to current field evaluation.
- Region bounds are derived from existing authored waterfall markers, channel profile, and geometry samples; there is no new DSL setting in this plan. Bounds expand to a face shared by coarse and selected fine lattices and include the complete support halo for input only.
- Coarse section cells outside the quality region and refined replacement cells inside it have disjoint rendered ownership.
- On a failing raw coarse/fine contour metric, build a two-fine-cell scalar transition. For every closed coarse cut loop, compute deterministic 2D parity on the ownership face and set the locked sample to `job.iso_value + signed_distance_m` (negative inside water, positive outside); preserve separate spray loops instead of joining them. Inner samples use the true particle field, and weights increase monotonically from 0 to 1. Marching cubes receives one scalar field; no final vertex interpolation/alpha seam is added.

- [ ] **Step 1: Write failing region and unchanged-outside tests.**

  Require `from = lip - 2`, `to = landing + 4`, snapped finite bounds, input halo present, and byte-identical coarse triangles outside the ownership region before/after refinement.

- [ ] **Step 2: Write failing boundary override tests.**

  Assert null override preserves accepted field digests. For a synthetic coarse/fine cut, require locked face contours within `coarse_voxel / 16`, minimum normal dot `>= 0.995`, zero open combined ownership edges, and monotonically increasing true-field weights through exactly two fine cells.

- [ ] **Step 3: Write failing 30-frame refinement tests.**

  Require every frame preserves intentional spray components, has no new holes, maintains plunge-pool coverage within 2%, and passes both coarse/fine boundaries. Require frame 29/0 metadata remains synchronized.

- [ ] **Step 4: Run focused tests and verify red.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target waterfall_visual_refinement_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
  ```

- [ ] **Step 5: Implement bounded scalar override parity.**

  Validate override count equals `layout.grid_vertices`, all values/weights are finite, and weights lie in `[0,1]`. Upload two small storage buffers for the bounded job; `gpu_mesh_field.comp` mixes locked and particle values before classification. Bind dummy one-element buffers for default jobs so descriptor lifetime is uniform.

- [ ] **Step 6: Derive and mesh the feature region.**

  Build the ordinary coarse frame first, derive outer cut contours, build the selected local job from the same phase-blended captures, apply scalar locks only if the raw boundary metric exceeds tolerance, partition cells, and assemble coarse + replacement without bridge triangles.

- [ ] **Step 7: Enforce artifact and region invariants.**

  Pack the combined frame into the existing section `.mhwa`; runtime draw count and playback remain unchanged. Include selected region settings and lattice identities in the section animation semantic key. Fail before save on boundary, topology, outside-coverage, or payload-limit violation.

- [ ] **Step 8: Run focused CPU/GPU gates.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target waterfall_visual_refinement_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/waterfall_visual_refinement_tests.exe
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/gpu_visual_mesher_cpu_tests.exe
  $env:MATTER_VK_SMOKE_MODE='gpu-mesher'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  $env:MATTER_VK_SMOKE_MODE='waterfall-mesher'
  $env:MATTER_WATERFALL_QUALITY_REPORT=(Resolve-Path build).Path + '/qa/waterfall-refinement.json'
  & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
  ```

- [ ] **Step 9: Commit the bounded refinement.**

  ```powershell
  git add MatterEngine3/src/hydrology/waterfall_visual_refinement.h MatterEngine3/src/hydrology/waterfall_visual_refinement.cpp MatterEngine3/tests/waterfall_visual_refinement_tests.cpp
  git add -p -- cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/include/matter/gpu_visual_meshing.h MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp MatterEngine3/shaders_vk/gpu_mesh_field.comp MatterEngine3/src/hydrology/water_mesh_animation.h MatterEngine3/src/hydrology/water_mesh_animation.cpp MatterEngine3/src/provider/local_provider.cpp MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp
  git commit -m "feat(water): refine waterfall visuals in a bounded region"
  ```

---

### Task 12: Enforce memory caps and rerun the complete Task 8 acceptance

**Files:**

- Modify: `MatterEngine3/src/hydrology/water_mesh_animation_artifact.h`
- Modify: `MatterEngine3/src/hydrology/water_mesh_animation_artifact.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Modify: `MatterEngine3/src/render/water_mesh_animation_playback.cpp`
- Test: `MatterEngine3/tests/water_mesh_animation_artifact_tests.cpp`
- Test: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- Modify: `MatterEngine3/tools/water_mesh_continuity_acceptance.py`
- Modify: `MatterEngine3/tools/tests/test_water_mesh_continuity_acceptance.py`
- Modify: `MatterEngine3/tools/run_water_mesh_continuity_acceptance.ps1`
- Modify: `docs/findings/animated-water-section-continuity-acceptance-2026-08-29.md`
- Modify: `docs/findings/raster-water-forward-optics-acceptance-2026-08-29.md`
- Modify: `ROADMAP.md`

**Interfaces:**

```cpp
inline constexpr std::uint64_t kWaterAnimationArtifactFileLimitBytes =
    1ull * 1024ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kWaterAnimationArtifactHeaderBytes = 32ull;
inline constexpr std::uint64_t kWaterAnimationNetworkBudgetBytes =
    700ull * 1024ull * 1024ull;

bool water_animation_artifact_file_bytes(
    std::uint64_t payload_bytes,
    std::uint64_t& file_bytes,
    gpu_meshing::Error& error);

bool water_animation_network_file_bytes(
    std::span<const std::uint64_t> artifact_file_bytes,
    std::uint64_t& total,
    gpu_meshing::Error& error);
```

- `water_animation_artifact_file_bytes` checked-adds the 32-byte header and rejects a complete file over 1 GiB. The caller supplies one complete file-size count for every section and handoff artifact; `water_animation_network_file_bytes` uses checked addition and rejects `total > 700 MiB` before the ready manifest is published.
- Sidecar bytes and peak build residency are reported but are not added to the runtime complete-file-size animation sum after the sidecars are released.
- Final runner performs a clean cold bake, unchanged cache hit, five standard Task 8 screenshots, phase handoff diagnostics, waterfall diagnostics, shadow-sample 1/10/16 perf, and GPU smoke modes.

- [ ] **Step 1: Write failing exact-boundary memory tests.**

  Exercise the size validators directly without allocating gigabyte fixtures. Accept payload `1 GiB - 32 bytes` as a complete 1 GiB file; reject one more payload byte. Accept aggregate complete-file counts exactly 700 MiB; reject one byte over. Overflowed sums fail closed and preserve the previous manifest.

- [ ] **Step 2: Run focused tests and verify red.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo -Target water_mesh_animation_artifact_tests
  tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
  ```

- [ ] **Step 3: Centralize and enforce both caps.**

  Replace private duplicated literals with the public constants, count complete serialized file bytes before publication, and verify playback's `file_size` accounting reports the same aggregate. Release boundary source vectors before activation and assert their decoded frame scratch is empty.

- [ ] **Step 4: Extend the strict comparator.**

  Require:

  ```text
  aggregate complete animation file bytes <= 734003200 bytes
  every complete serialized animation file <= 1073741824 bytes
  all Stage 1 continuity and ownership metrics pass for 30 frames
  waterfall world silhouette error < 0.0375 m (one quarter of 0.15 m)
  retained Task 8 camera pixel threshold from the Task 10 baseline passes
  no new waterfall holes/components; plunge-pool coverage remains stable
  median frame <= max(baseline * 1.08, baseline + 1.0 ms)
  p95 frame <= max(baseline * 1.10, baseline + 2.0 ms)
  validation/decode/BLAS/TLAS/RT counters == 0
  steady-state allocation delta == 0
  water_forward_image_bytes == width * height * 12
  ```

- [ ] **Step 5: Run the complete native and CPU gates.**

  ```powershell
  tools/build-windows.ps1 -Config RelWithDebInfo
  & 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -L cpu --output-on-failure
  ```

- [ ] **Step 6: Run GPU smoke and the final RiverFloatLab suite.**

  ```powershell
  foreach ($mode in 'gpu-mesher','water-forward','water-animation','default') {
      $env:MATTER_VK_SMOKE_MODE=$mode
      & MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
      if ($LASTEXITCODE -ne 0) { throw "$mode failed" }
  }
  & MatterEngine3/tools/run_water_mesh_continuity_acceptance.ps1 -Stage Final -OutputDir build/qa/water-mesh-continuity-2026-08-29/final
  ```

- [ ] **Step 7: Perform explicit native-size visual acceptance.**

  Inspect and record:

  ```text
  shallow-player-low: bed legible; absorption increases with depth
  upper-rapids: foam follows and advects along baked turbulent lanes
  waterfall-side: smooth continuous falling silhouette; no block curtain or boundary
  plunge-pool: localized impact whitewater, stable pool coverage, intentional spray preserved
  section-handoff at frames 0/7/15/22/29: no coverage, normal, foam, optics, or timing seam
  diagnostic identity: disjoint ownership, no missing or duplicate band
  every view: plausible reflection, clean screen-edge fallback, one coherent shadow set
  ```

  These are hard gates. Do not accept by lowering foam thresholds, hiding the seam with opacity, increasing overlap, or omitting a failed image.

- [ ] **Step 8: Close findings and only the roadmap work actually completed.**

  Update the continuity finding with commit/device/driver, selected local quality row, cold/cache timings, all 30-frame worst metrics, artifact/sidecar/peak memory, GPU/perf rows, and screenshot links. Change the raster-water finding from blocked to accepted only when its two failed visual rows now pass, retaining the old failed evidence as history. In `ROADMAP.md`, remove completed `Now` raster-water items and `Next` item 1; leave character controller, focused playtest, scale work, and deferred decisions unchanged.

- [ ] **Step 9: Commit final acceptance.**

  ```powershell
  git add -p -- MatterEngine3/src/hydrology/water_mesh_animation_artifact.h MatterEngine3/src/hydrology/water_mesh_animation_artifact.cpp MatterEngine3/src/provider/local_provider.cpp MatterEngine3/src/render/water_mesh_animation_playback.cpp MatterEngine3/tests/water_mesh_animation_artifact_tests.cpp MatterEngine3/tests/physx_adapter_contract_tests.cpp MatterEngine3/tools/water_mesh_continuity_acceptance.py MatterEngine3/tools/tests/test_water_mesh_continuity_acceptance.py MatterEngine3/tools/run_water_mesh_continuity_acceptance.ps1 docs/findings/animated-water-section-continuity-acceptance-2026-08-29.md docs/findings/raster-water-forward-optics-acceptance-2026-08-29.md ROADMAP.md
  git commit -m "test(water): accept continuous animated river sections"
  ```

---

## Final verification checklist

- [ ] The Stage 0 synthetic and retained RiverFloatLab evidence reproduces the old multi-metre handoff mismatch and faceted waterfall before behavior changes.
- [ ] Every section, handoff, chunk, CPU oracle, and Vulkan field job uses the same v1 network lattice and exact sample coordinates.
- [ ] Boundary sidecars contain exactly 30 cropped frames, preserve simulation steps, meet `voxel/16` quantization, exclude dam support, and fail closed on corruption.
- [ ] An unchanged upstream section and endpoint sidecar are reused without PhysX after a downstream-only edit.
- [ ] The animated handoff contains no static collar append and no multi-metre zipper; it is one jointly meshed per-frame replacement strip with disjoint ownership.
- [ ] Worst-frame cut Hausdorff/RMS is within quantization tolerance, normal dot is at least 0.995, combined open ownership edges are zero, and duplicate/co-planar rendered triangles are zero.
- [ ] Height, normal, turbulence, aeration, foam, and feature-label samples are bounded and continuous across both handoff cuts.
- [ ] The waterfall keeps `particleSpacing = 0.20 m`, changes only the bounded feature region, preserves spray/pool coverage, and meets world/pixel silhouette gates.
- [ ] Aggregate complete runtime animation file bytes are at most 700 MiB; every complete serialized artifact file, including its 32-byte header, is at most 1 GiB; sidecar peak residency is measured and released before playback.
- [ ] All CPU tests pass; `gpu-mesher`, `water-forward`, `water-animation`, and default Vulkan modes pass with zero validation errors.
- [ ] Water animation decode, BLAS, TLAS, and RT-record counters remain zero; raster direct water remains active and steady-state allocations remain zero.
- [ ] The five standard Task 8 screenshots, five handoff phases, and waterfall/normal/foam/identity diagnostics pass at native size.
- [ ] The raster-water blocked finding and roadmap are updated only after every automated and visual gate passes.
