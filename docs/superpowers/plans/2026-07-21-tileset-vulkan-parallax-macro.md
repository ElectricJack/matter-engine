# Tileset Vulkan Port, Parallax & Macro Layer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Spec:** `docs/superpowers/specs/2026-07-21-tileset-vulkan-parallax-macro-design.md`

**Goal:** Ground tilesets (`.gtex`) render in the Vulkan hybrid pipeline with no visible seams at any distance, parallax-occlusion-mapped relief with a conservative depth write and sun self-shadowing, and a coarser macro tileset composited as a frequency split. Prerequisite phase: migrate the whole Vulkan pipeline to reversed-Z so the POM depth write (and all future depth work) is authored once against the final convention.

**Architecture:** Phase 0 flips the depth convention at `perspective_rh_zo` (new `perspective_rh_zo_reversed` in `matrix_math`), the renderer's clear/compare state, the depth-consuming shaders (`rt_lighting.rgen`, `vol_*.comp`), and the Streamline bridge flag. Phase 1 adds a CPU `.gtex` slicer (atlas → 16 layers + per-layer mip chains), a `VkSceneRenderer` tileset slot table (4 slots × 4 channels = 16 `sampler2DArray` descriptors + a `TilesetParams` UBO), a `tileset_common.glsl` Wang-sampling port, a GBuffer branch keyed off `MaterialGpu.flags_misc.y`, and flat Wang sampling in the RT hit path. Phase 2 adds the world-space POM march (re-resolving the Wang cell per step), `layout(depth_less)` conservative depth, and a short sun-ward self-shadow march. Phase 3 adds `groundMacroSlot` to the material schema, a `ForestFloorMacro` tileset authored through the unchanged bake pipeline, and frequency-split compositing.

**Tech Stack:** C++17, Vulkan 1.3 (`GLSLC --target-env=vulkan1.3`, SPIR-V embedded via `tools/embed_spirv.py` → `shaders_gen/embedded_spirv.h`), GLSL 460 with `GL_EXT_nonuniform_qualifier`, existing `.gtex` reader (`tileset_gtex.h`), QuickJS DSL (`Tileset` root), MSYS2 UCRT64 GCC on Windows (`TMP`/`TEMP` overrides per CLAUDE.md).

## Global Constraints

- **Always implement the real thing** — no scaffold/demo/preview shortcuts. Any new visual must be reachable through the actual viewer.
- **The GL raster path is frozen.** `tileset_provider.{h,cpp}` (GL), `tileset_sampling.glsl` (GL), `raster.fs`, `raytrace_tlas_blas.fs` are NOT modified by this plan. The GL path continues to work unchanged; all new consumption is Vulkan-side.
- **MatterSurfaceLib is read-only** except spec-mandated feature work. Task 10's `groundMacroSlot` addition is spec-mandated (spec §"Material schema") — flag it in the commit message.
- **The bake pipeline is untouched.** No changes to `tileset_settle`, `tileset_bake_*`, `tileset_gtex` writers, or the `.gtex` format. Phase 3's macro tileset is *authored content* plus loader/material wiring only.
- **Fail-closed everywhere** — every error path sets a structured error naming the file/slot/reason; a `.gtex` failure must degrade to untextured rendering, never crash.
- **Windows build commands** (per CLAUDE.md): all `make` invocations need
  `TMP="C:/Users/webde/AppData/Local/Temp" TEMP="C:/Users/webde/AppData/Local/Temp"` and
  `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`. Abbreviated below as `make -C MatterEngine3 …` — always append the TEMP overrides.
- **After ANY shaders_vk change:** regenerate SPIR-V (`make -C MatterEngine3 vulkan-spirv`) and commit the regenerated `shaders_gen/embedded_spirv.h` together with the GLSL source. New `.glsl` includes need Makefile dependency lines (see the existing `material_common.glsl` pattern at `MatterEngine3/Makefile:236`).
- **After ANY engine code change:** `make -C MatterEngine3` must link `libmatter_engine3.a`, and `make -C MatterViewer windows` must produce `viewer` — both are per-task verification steps, not end-of-plan ones.
- **Keep merged feature branches** — no branch-delete steps.
- **Reversed-Z lands first and alone.** Phase 0 (Tasks 1–4) must be committed, verified, and visually soaked before any Phase 1 task starts; it is the only phase touching code outside the tileset feature.

## File Structure

**New files:**
- `MatterEngine3/src/render/tileset_slicer.h` / `.cpp` — CPU atlas→layers slicer + per-layer mip builder (pure CPU, no GL/VK).
- `MatterEngine3/tests/tileset_slicer_tests.cpp` — headless CPU tests (slice exactness, edge-strip preservation, mip filtering, mean-albedo).
- `MatterEngine3/shaders_vk/tileset_common.glsl` — Wang cell/layer resolution, flat sampling, POM march, self-shadow, macro compositing.
- `MatterEngine3/examples/world_demo/objects/ForestFloorMacro.js` — 16 m macro tileset for Meadow (Phase 3).

**Modified files (by phase):**
- **P0:** `MatterEngine3/src/render/matrix_math.{h,cpp}` (reversed projection + ZO frustum extraction), `frame_matrices.cpp`, `vk_scene_renderer.cpp` (clears, compare ops, near/far derivation, software path), `streamline_bridge.cpp` (depth-inverted flag), `shaders_vk/rt_lighting.rgen`, `shaders_vk/vol_density.comp`, `shaders_vk/vol_scatter.comp`, `shaders_vk/vol_integrate.comp`, `shaders_vk/composite.frag` (if depth-vs-far tests exist — Task 3 Step 1 audits), matrix/frustum unit tests.
- **P1:** `vk_scene_renderer.{h,cpp}` (tileset images, sampler, descriptors, `TilesetParams` UBO, `load_tileset_slot`), `matter_engine.cpp` (facade API + material record packing + world-load wiring), `MatterSurfaceLib/include/material_registry.h` + `src/material_registry.c` (schema v4: `groundMacroSlot` + setter — Phase 3 field added in Phase 1 to avoid two schema bumps), `shaders_vk/gbuffer.frag`, `shaders_vk/raster.vert` (world-pos varying), `shaders_vk/rt_surface_common.glsl`, `shaders_vk/rt_lighting.rgen`, `MatterEngine3/Makefile` (spv deps + new sources + test target), `MatterEngine3/tests/Makefile`.
- **P2:** `shaders_vk/tileset_common.glsl`, `shaders_vk/gbuffer.frag` (march + `depth_less`), `tileset_slicer_tests.cpp` (march-math mirror tests).
- **P3:** `examples/world_demo/WorldData/Meadow/world.manifest`, `matter_engine.cpp` (macro slot binding), `shaders_vk/tileset_common.glsl` + `gbuffer.frag` + `rt_surface_common.glsl` (compositing), Meadow shot scripts.

---

# Phase 0 — Reversed-Z migration

## Task 1: Reversed projection + frustum extraction in `matrix_math`

**Files:**
- Modify: `MatterEngine3/src/render/matrix_math.h` / `.cpp`
- Modify: `MatterEngine3/src/render/frame_matrices.cpp`
- Modify: the existing matrix/frame-matrices unit tests (locate: `grep -rln "perspective_rh_zo\|extract_frustum_planes_zo" MatterEngine3/tests`)

**Interfaces:**
- Produces: `matter::Mat4f perspective_rh_zo_reversed(float fov_y_rad, float aspect, float near_p, float far_p)` — right-handed, zero-to-one depth with **near→1, far→0**. Standard form: `m[10] = near / (far - near)`, `m[11] = far * near / (far - near)` (column-major/row-major per the existing `perspective_rh_zo` layout — mirror it exactly, only the two depth terms change).
- `extract_frustum_planes_zo` must keep returning correct near/far planes for the reversed matrix. Under ZO conventions the near plane comes from clip `z ≥ 0` and far from `w − z ≥ 0`; **reversed-Z swaps which geometric plane each inequality yields**, so either the extractor detects orientation or (simpler, recommended) the near/far plane slots are swapped at the one call site that builds `FrameMatrices` — pick after reading the extractor, and lock the choice with a test.

- [ ] **Step 1: Write failing tests.** In the existing matrix-math test file add: (a) `perspective_rh_zo_reversed` maps a point at `z = -near` to NDC depth 1.0 and `z = -far` to 0.0 (transform, divide by w, assert within 1e-5); (b) a mid-distance point maps to a depth strictly between; (c) frustum planes extracted from `mul(reversed_proj, view)` still cull a point behind the camera and keep a point in front (reuse the existing frustum test fixture with the reversed matrix). Run the tests target (grep the tests Makefile for the matrix test run target) — expect compile failure on the missing function.
- [ ] **Step 2: Implement `perspective_rh_zo_reversed`** in `matrix_math.cpp` by copying `perspective_rh_zo` and replacing the two depth-row terms with the near/far-swapped form. Do NOT delete `perspective_rh_zo` (the GL path and any tools may still use it — `grep -rn "perspective_rh_zo" --include=*.cpp` and leave non-VK callers alone).
- [ ] **Step 3: Switch `frame_matrices.cpp:66`** to call `perspective_rh_zo_reversed`, and update the `depth_scale` validation at `:46-48` to the reversed equivalent (guard `near/(far-near)` finiteness). Apply the near/far frustum-plane resolution chosen in Step 1.
- [ ] **Step 4: Run tests to PASS.** Also run the GPU culler tests if present (`grep -rn "frustum" MatterEngine3/tests/Makefile`) — the culler consumes `frustum_planes` and must still cull correctly.
- [ ] **Step 5: Build gates.** `make -C MatterEngine3` links; `make -C MatterViewer windows` links.
- [ ] **Step 6: Commit** — `rz t1: reversed-Z projection + ZO frustum extraction`.

## Task 2: Renderer depth state + near/far derivation

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`

Known sites (re-grep before editing; the file is large and drifts):
- `:396` `depth_attachment.clearValue.depthStencil = {1.0f, 0}` → `{0.0f, 0}`.
- `:1502` `VK_COMPARE_OP_LESS_OR_EQUAL` → `VK_COMPARE_OP_GREATER_OR_EQUAL`.
- `:3022` `VkClearDepthStencilValue depth_clear{f.depth, 0}` — trace `f.depth`'s producer; if it seeds "cleared/far" it becomes 0.0.
- `:6340` `pixel.depth = 1.0f` (software reference path far-seed) → `0.0f`, and flip the software path's depth comparisons (grep `pixel.depth` for all consumers).
- `:6200-6206` near/far recovered from the projection: `camera_near = m[11]/m[10]` and `camera_far = m[11]/(m[10]+1)` are the **standard-ZO** identities. For the reversed matrix the identities swap roles: with `m[10] = n/(f−n)`, `m[11] = f·n/(f−n)`, we get `m[11]/m[10] = f` and `m[11]/(m[10]+1) = n·f/(f) …` — **derive on paper from the actual matrix layout in Step 2 and assert both constants against `camera.near_plane/far_plane` in a debug check** rather than trusting either identity blind.

- [ ] **Step 1: Exhaustive sweep.** `grep -n "VK_COMPARE_OP\|clearValue.depthStencil\|ClearDepthStencilValue\|\.depth = 1\|depth = 1.0" MatterEngine3/src/render/vk_scene_renderer.cpp` — list every site in the task notes; each must be visited (flip, or record why it is depth-unrelated).
- [ ] **Step 2: Apply the flips** including the near/far derivation fix, with a one-frame debug assert comparing recovered near/far against the camera description (remove or demote to `#ifndef NDEBUG` before commit).
- [ ] **Step 3: Run the existing Vulkan render tests** (grep `MatterEngine3/tests/Makefile` for the vk/gpu suites; on Windows run the ones CLAUDE.md lists as linkable). Expect image-diff tests to fail until Task 3 fixes the shaders — record which, and confirm they are all depth-consumers (anything else failing means a missed Step 1 site).
- [ ] **Step 4: Commit** — `rz t2: reversed-Z clear/compare/near-far in vk_scene_renderer` (note in the body that shader-side fixes land in t3 and which tests are expected-red in between; keep t2+t3 on the same branch, merge only when green).

## Task 3: Shader audit — depth consumers

**Files:**
- Modify: `shaders_vk/rt_lighting.rgen`, `shaders_vk/vol_density.comp`, `shaders_vk/vol_scatter.comp`, `shaders_vk/vol_integrate.comp`; audit `composite.frag`, `rt_shadow.rgen`, `gi_temporal.comp`.

Known sites:
- `rt_lighting.rgen:206` — sky/miss test `depth >= 1.0` → `depth <= 0.0`.
- `rt_lighting.rgen:216` — NDC position rebuild `vec4(…, depth, 1.0)` through `clip_to_world`: **correct as-is** once the matrix is reversed (the inverse matches); verify, don't change.
- `vol_density.comp:39-46`, `vol_scatter.comp:62-63,107-108,118-120` — standard-Z linearize/delinearize (`ndc_z = f(d−n)/((f−n)d)` and inverse). Replace with the reversed-ZO pair: `ndc_z = n·(f−d)/(d·(f−n))` and `depth = n·f/(ndc_z·(f−n)+n)` — derive from the Task 1 matrix and cross-check both directions round-trip in a comment.
- `vol_common.glsl` — froxel slice↔view-depth helpers work in *linear view depth*, not NDC; only the NDC↔linear conversions above change. Verify by reading, note in commit.

- [ ] **Step 1: Audit sweep.** `grep -n "depth" shaders_vk/*.rgen shaders_vk/*.frag shaders_vk/*.comp shaders_vk/*.glsl | grep -v "depth_texture\|depthStencil"` — classify every hit: NDC-consuming (fix), linear-depth (leave), naming-only (leave). Paste the classification into the commit body.
- [ ] **Step 2: Apply fixes; regenerate SPIR-V** (`make -C MatterEngine3 vulkan-spirv`); rebuild; rerun the tests that were expected-red after Task 2 — all must be green now.
- [ ] **Step 3: Visual parity.** Run the viewer on Meadow (GL-independent Vulkan path) and compare against a pre-Phase-0 capture: sky, fog/volumetrics, GI, shadows, DLSS off. Differences beyond far-field z-fighting reduction = missed site.
- [ ] **Step 4: Commit** — `rz t3: reversed-Z shader audit (sky test, volumetrics linearization)` + regenerated `embedded_spirv.h`.

## Task 4: Streamline flag, z-fight regression shot, soak

**Files:**
- Modify: `MatterEngine3/src/render/streamline_bridge.cpp` (~`:412` depth resource setup)
- Modify/Create: shot script alongside the existing viewer shot tooling (`grep -rn "viewer_shots" MatterEngine3/tools MatterViewer` for the current harness)

- [ ] **Step 1:** Set the Streamline depth-inverted constant (`grep -n "Depth\|kBufferType" MatterEngine3/src/render/streamline_bridge.cpp` — Streamline expects `sl::` depth extent/inverted hints where the depth resource is tagged). Verify DLSS visually on/off.
- [ ] **Step 2: Regression shot.** Add a scripted shot with two near-coplanar surfaces at ~2 km (a thin box resting on terrain works). Capture under standard-Z first (pre-flip build or a temporary toggle) to prove it shimmers, then assert the reversed-Z build is stable across 2 frames (image self-diff).
- [ ] **Step 3:** Full builds (engine, viewer windows), full linkable test sweep, commit — `rz t4: streamline inverted-depth flag + distant z-fight regression shot`. **Soak checkpoint:** stop here; do not start Phase 1 in the same session/PR.

---

# Phase 1 — Vulkan tileset consumption

## Task 5: CPU `.gtex` slicer + per-layer mips (`tileset_slicer`)

**Files:**
- Create: `MatterEngine3/src/render/tileset_slicer.h`, `.cpp`
- Create: `MatterEngine3/tests/tileset_slicer_tests.cpp`
- Modify: `MatterEngine3/Makefile` (add source), `MatterEngine3/tests/Makefile` (add `run-tilesetslicer` — CPU-only, no GL/VK, links on Windows)

**Interfaces:**
- Consumes: decoded channel buffers from `tileset::load_gtex` (`tileset_gtex.h`).
- Produces (all pure CPU, deterministic):

```cpp
#pragma once
// tileset_slicer.h — slice a .gtex 4x4 atlas into 16 per-tile layers and build
// per-layer mip chains on the CPU. Pure functions: no GL/VK, fully unit-testable.
// Rationale (spec §Phase 1): per-layer mips make cross-tile mip bleed impossible;
// CPU generation keeps the edge invariant testable byte-for-byte.
#include <cstdint>
#include <string>
#include <vector>

namespace tileset {

struct SlicedChannel {
    // layers[layer][mip] = tightly-packed pixel bytes; layer = row*4 + col.
    // mip 0 is tile_px × tile_px; each level halves (floor), down to 1×1.
    std::vector<std::vector<std::vector<uint8_t>>> layers;
    int tile_px = 0;      // mip-0 edge length
    int mip_count = 0;
    int bytes_per_pixel = 0;
};

// Slice one channel. atlas is W×H tightly packed, W = H = 4*tile_px.
// bytes_per_pixel: albedo/ORM = 3 (packed to 4 with opaque alpha when
// expand_rgb_to_rgba), normal = 2, height = 2 (uint16 LE).
// Box-filter mips; height filters in uint16 space, others per-byte.
// Fails closed (false + err) on dimension mismatches.
bool slice_channel(const uint8_t* atlas, int atlas_w, int atlas_h,
                   int bytes_per_pixel, bool expand_rgb_to_rgba,
                   SlicedChannel& out, std::string& err);

// Mean albedo of the whole atlas in linear-ish 0..1 (spec §Phase 3 compositing).
void mean_rgb(const uint8_t* atlas, int atlas_w, int atlas_h,
              int bytes_per_pixel, float out_rgb[3]);

} // namespace tileset
```

- [ ] **Step 1: Failing tests** in `tileset_slicer_tests.cpp` (plain `REQUIRE` pattern from existing CPU tests): (a) synthetic 64×64 atlas (tile_px=16) with each tile filled by a distinct byte value → 16 layers, each byte-uniform, `layer == row*4+col` order verified; (b) **edge-invariant preservation:** write identical 3-px strips on the shared edges of two color-matched tiles, differing interiors → after slicing, the strip rows of both layers are byte-equal at mip 0 and mip 1 (box filter of equal strips is equal — this is the property the whole design rests on); (c) mip chain: 16→8→4→2→1, `mip_count == 5`, a checkerboard averages to mid-gray at the top mip within ±1; (d) RGB→RGBA expansion sets alpha 255; (e) height uint16 filtering averages in value space (two texels 0 and 65535 → 32767±1); (f) `mean_rgb` of a half-black/half-white atlas ≈ 0.5.
- [ ] **Step 2:** Header (above), stub `.cpp`, Makefile wiring; run `make -C MatterEngine3/tests run-tilesetslicer` → link failure.
- [ ] **Step 3: Implement.** Slicing is row-wise `memcpy`; mips are 2×2 box with odd-size floor handling; keep it boring and exact.
- [ ] **Step 4:** Tests PASS on Windows (CPU-only target). Engine + viewer builds. Commit — `gtexvk t5: CPU .gtex slicer with per-layer mips (seam-invariant tested)`.

## Task 6: Renderer tileset slots — images, sampler, descriptors, params

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h` / `.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp` (facade pass-through)

**Interfaces:**
- Produces on `VkSceneRenderer`:
  - `bool load_tileset_slot(int slot, const std::string& gtex_path, std::string& err)` — load+slice (Task 5), create 4 `VkImage`s (`VK_IMAGE_TYPE_2D`, 16 array layers, full mips; formats `R8G8B8A8_UNORM` / `R8G8_UNORM` / `R8G8B8A8_UNORM` / `R16_UNORM`), upload all layer×mip regions via one staging buffer, transition to `SHADER_READ_ONLY_OPTIMAL`. Follow the existing `VkImageResource` creation/upload pattern (see the GBuffer image creation around `vk_scene_renderer.cpp:296-306` and its staging-upload helpers — grep `create_image\|VkImageResource` and mirror).
  - `void unload_tileset_slot(int slot)`, teardown in the destructor.
  - A single shared sampler: trilinear, `anisotropyEnable`, `maxAnisotropy = min(8, limits)`, repeat, full LOD range.
  - Descriptor plumbing: **raster set 1** gains `binding 6: combined image sampler, descriptorCount 16` (confirm 6 is free: `grep -n "binding = 6" shaders_vk/*.vert shaders_vk/*.frag` and the set-1 layout builder) — index `slot*4 + channel` (channel: 0 albedo, 1 normal, 2 orm, 3 height); empty entries point at a 1×1×16-layer dummy (create once). **RT set 0** gains the mirrored array at `binding 15` (b0–b14 occupied per `vk_scene_renderer.cpp:5170-5259`; confirm) plus `binding 16: TilesetParams` UBO. Raster set 1 gains the same UBO at `binding 7`.
  - `TilesetParams` std140 UBO (16 slots of headroom is unnecessary — 4):

```c
struct TilesetParamsGpu {            // std140 — keep vec4-aligned
    float slot_tile_size_m[4];
    float slot_texels_per_meter[4];
    float slot_height_min[4];
    float slot_height_max[4];
    float slot_mean_albedo[4][4];    // rgb + valid flag in .w
    float pom_steps;                 // Phase 2 tunables, uploaded from day 1
    float pom_refine_steps;
    float pom_max_distance_m;
    float pom_fade_band_m;
    float detail_fade_center_m;      // Phase 3
    float detail_fade_width_m;
    float pad0, pad1;
};
```

- [ ] **Step 1:** Confirm binding availability (greps above) and record final numbers in this plan file (edit it) and in `tileset_common.glsl`'s header comment when created (Task 7).
- [ ] **Step 2:** Implement images/sampler/dummy/UBO + descriptor writes. Descriptor indexing feature: enable `shaderSampledImageArrayNonUniformIndexing` + `runtimeDescriptorArray` off — a fixed `descriptorCount=16` array with `nonuniformEXT` indexing needs only `descriptorIndexing` features already implied by Vulkan 1.2 core + `VK_EXT_descriptor_indexing`-core; verify against the existing device-feature setup (`grep -n "VkPhysicalDeviceVulkan12Features\|descriptorIndexing" vk_scene_renderer.cpp`) and enable the two feature bits if not already on.
- [ ] **Step 3:** Facade: `matter_engine.cpp` exposes `MatterEngineLoadTilesetSlot(slot, path)` → renderer call, mirroring how the GL path is driven (find the GL call site: `grep -rn "tileset_provider::load_slot\|MaterialRegistrySetGroundTilesetSlot" MatterEngine3/src` — the same world-load moment must call the VK load when the VK renderer is active).
- [ ] **Step 4:** Verification without shaders: load Meadow, assert `load_tileset_slot` returns true and validation layers are silent (run with `VK_LAYER_KHRONOS_validation` if available locally; otherwise assert no `VK_ERROR` and log image handles). Engine + viewer-windows builds. Commit — `gtexvk t6: VkSceneRenderer tileset slots (16-layer arrays, aniso sampler, params UBO)`.

## Task 7: `tileset_common.glsl` + GBuffer flat sampling + world-pos varying

**Files:**
- Create: `MatterEngine3/shaders_vk/tileset_common.glsl`
- Modify: `shaders_vk/raster.vert` (add `layout(location = 7) out vec3 out_world_pos;` fed from `world.xyz` at `raster.vert:48`; location 7 is next free per the current interface list at `:12-18`)
- Modify: `shaders_vk/gbuffer.frag` (matching `in`, tileset branch)
- Modify: `MatterEngine3/Makefile` (spv dependency lines: `gbuffer.frag.spv` and later `rt_*.spv` depend on `tileset_common.glsl`)

`tileset_common.glsl` core (complete file to create — bindings per Task 6 Step 1, adjust the two `binding =` lines to the recorded numbers):

```glsl
#ifndef MATTER_VK_TILESET_COMMON_GLSL
#define MATTER_VK_TILESET_COMMON_GLSL
// tileset_common.glsl — Wang-tile ground sampling for the Vulkan pipeline.
// Port of the GL tileset_sampling.glsl with two structural changes:
//   * per-tile texture-array layers (mip bleed between tiles is impossible),
//   * descriptor-array samplers indexed slot*4+channel (no if-chains).
// Included by gbuffer.frag (raster set 1) and rt_* (set 0); the includer
// defines TILESET_SET before including so bindings resolve per pipeline.
#extension GL_EXT_nonuniform_qualifier : require

layout(set = TILESET_SET, binding = TILESET_TEX_BINDING)
    uniform sampler2DArray tilesetTex[16];
layout(set = TILESET_SET, binding = TILESET_PARAMS_BINDING, std140)
    uniform TilesetParams {
    vec4 tile_size_m;            // per slot
    vec4 texels_per_meter;
    vec4 height_min;
    vec4 height_max;
    vec4 mean_albedo[4];         // rgb + valid
    vec4 pom_a;                  // steps, refine_steps, max_distance_m, fade_band_m
    vec4 pom_b;                  // detail_fade_center_m, detail_fade_width_m, pad, pad
} tileset;

#define TILESET_CH_ALBEDO 0
#define TILESET_CH_NORMAL 1
#define TILESET_CH_ORM    2
#define TILESET_CH_HEIGHT 3

// PCG-flavoured integer hash; identical constants to the GL/bake version so the
// runtime arrangement matches the seam tests. Same ivec2 in → same color out.
int wang_edge_color(ivec2 boundaryCoord) {
    uint x = uint(boundaryCoord.x) * 747796405u + 2891336453u;
    uint y = uint(boundaryCoord.y) * 3266489917u + 374761393u;
    uint h = x ^ (y + 0x9e3779b9u + (x << 6) + (x >> 2));
    h = (h ^ (h >> 16)) * 0x85ebca6bu;
    h = (h ^ (h >> 13)) * 0xc2b2ae35u;
    h = h ^ (h >> 16);
    return int(h & 1u);
}

int wang_pair_index(int a, int b) {   // de Bruijn cycle {0,0,1,1}
    if (a == 0 && b == 0) return 0;
    if (a == 0 && b == 1) return 1;
    if (a == 1 && b == 1) return 2;
    if (a == 1 && b == 0) return 3;
    return 0;
}

// world XZ -> (array layer, cell-local UV) for one slot.
void wang_resolve(int slot, vec2 worldXZ, out int layer, out vec2 cellUV) {
    float ts = tileset.tile_size_m[slot];
    vec2 t = worldXZ / ts;
    vec2 tf = floor(t);
    ivec2 cell = ivec2(tf);
    cellUV = t - tf;
    int top = wang_edge_color(ivec2(cell.x * 2 + 0,       cell.y));
    int bot = wang_edge_color(ivec2(cell.x * 2 + 0,       cell.y + 1));
    int lft = wang_edge_color(ivec2(cell.x * 2 + 1,       cell.y));
    int rgt = wang_edge_color(ivec2((cell.x + 1) * 2 + 1, cell.y));
    layer = wang_pair_index(top, bot) * 4 + wang_pair_index(lft, rgt);
}

vec4 tileset_sample(int slot, int channel, vec2 worldXZ,
                    vec2 dWdx, vec2 dWdy) {
    int layer; vec2 uv;
    wang_resolve(slot, worldXZ, layer, uv);
    float inv = 1.0 / tileset.tile_size_m[slot];
    return textureGrad(tilesetTex[nonuniformEXT(slot * 4 + channel)],
                       vec3(uv, float(layer)), dWdx * inv, dWdy * inv);
}

// Flat ground sample: albedo out, tangent normal + ORM via out-params.
vec3 tileset_sample_ground(int slot, vec2 worldXZ, vec2 dWdx, vec2 dWdy,
                           out vec3 normal_ts, out vec3 orm) {
    vec4 alb = tileset_sample(slot, TILESET_CH_ALBEDO, worldXZ, dWdx, dWdy);
    vec4 nrm = tileset_sample(slot, TILESET_CH_NORMAL, worldXZ, dWdx, dWdy);
    vec4 om  = tileset_sample(slot, TILESET_CH_ORM,    worldXZ, dWdx, dWdy);
    vec2 rg = nrm.rg * 2.0 - 1.0;
    normal_ts = vec3(rg, sqrt(max(0.0, 1.0 - dot(rg, rg))));
    orm = om.rgb;
    return alb.rgb;
}

// Material slot decode (MaterialGpu.flags_misc.y): low byte detail+1, next macro+1.
int tileset_detail_slot(uvec4 flags_misc) { return int(flags_misc.y & 0xFFu) - 1; }
int tileset_macro_slot(uvec4 flags_misc)  { return int((flags_misc.y >> 8) & 0xFFu) - 1; }

#endif
```

`gbuffer.frag` branch (after the material fetch at `gbuffer.frag:21-34`): if `tileset_detail_slot(material.flags_misc) >= 0`, compute `dWdx = dFdx(in_world_pos.xz)`, `dWdy = dFdy(...)`, call `tileset_sample_ground`, then: `base_color = tex_albedo * mix(vec3(1), in_tint.rgb, in_tint.a)`; rotate `normal_ts` into the surface frame (T=+X, B=+Z projected onto the plane ⊥ `in_normal`, matching the bake's planar projection); `roughness/metallic` from ORM; `ao = tex_ao * vertex_ao`.

- [ ] **Step 1:** Create the include + varying + branch; add Makefile spv-dependency lines; `make -C MatterEngine3 vulkan-spirv` compiles clean.
- [ ] **Step 2:** Bind material 16 (DIRT) to slot 0 at Meadow load (the Task 6 Step 3 wiring) and run the viewer: ground textured. Screenshot for the ledger.
- [ ] **Step 3: Runtime seam verification (the test v1 lacked):** scripted shots along the v1 seam-heavy camera path at 3 distances chosen to force mips ~2, ~5, ~8 (compute from texel footprint; log chosen distances in the shot script). Visually assert no 2 m grid. Automatable follow-up: self-diff a shot mirrored across a known seam line — defer if the shot harness lacks the crop tooling, but note it.
- [ ] **Step 4:** Builds; commit — `gtexvk t7: Vulkan Wang sampling (texture arrays) + GBuffer ground branch`.

## Task 8: Material schema v4 — `groundMacroSlot` + `flags_misc` packing

**Files:**
- Modify: `MatterSurfaceLib/include/material_registry.h` (schema v4: add `int groundMacroSlot;` after `groundTilesetSlot`; bump `MATERIAL_SCHEMA_VERSION` to 4; add `MaterialRegistrySetGroundMacroSlot(int materialId, int slot)`)
- Modify: `MatterSurfaceLib/src/material_registry.c` (initializers gain `, -1`; parallel override array like the existing tileset-slot one)
- Modify: `MatterEngine3/src/matter_engine.cpp` — the `MaterialGpuRecord` build (~`:4012`; grep `flags_misc` first and confirm `[1]` is unused — if any bit of `flags_misc` is already consumed, pick the free lane and update `tileset_common.glsl`'s decode to match): `records[i].flags_misc[1] = pack(detail, macro)` with `pack(d,m) = uint(d+1) | (uint(m+1) << 8)`.
- Modify: `MatterEngine3/src/render/vk_gi_contract.h` — extend the static-assert block's comment (layout unchanged; semantics of `flags_misc[1]` documented).

- [ ] **Step 1:** Failing CPU test: extend whichever test covers `MaterialGpuRecord` packing (grep `vk_gi_contract\|flags_misc` in tests; if none exists, add a small one to the slicer test binary) asserting a material with detail=0/macro=2 packs `flags_misc[1] == (1 | 3<<8)` and default packs 0.
- [ ] **Step 2:** Implement; GL path untouched (it reads the 12-float table, not `MaterialGpuRecord`). MSL commit-message exception note (spec-mandated).
- [ ] **Step 3:** Tests pass; builds; commit — `gtexvk t8: material schema v4 (groundMacroSlot) + flags_misc slot packing [MSL spec-mandated]`.

## Task 9: RT hit-path flat sampling

**Files:**
- Modify: `shaders_vk/rt_surface_common.glsl` (include `tileset_common.glsl` with `TILESET_SET 0` + RT binding numbers; in the surface-decode path — `rt_surface_common.glsl:97-169` — override albedo/normal/roughness when `tileset_detail_slot >= 0`)
- Modify: `shaders_vk/rt_lighting.rgen` if the material fetch happens there for GBuffer-seeded shading (`:150,292,366`) — same override
- Modify: `MatterEngine3/Makefile` (spv deps)

Gradient proxy: `dW = hit_distance * ray_cone_spread` (constant cone angle from FOV/height is sufficient for ground LOD; document the approximation inline). Grep whether the RT payload already carries a cone/footprint term before inventing one.

- [ ] **Step 1:** Implement + regen SPIR-V.
- [ ] **Step 2:** Visual: a reflective sphere on Meadow ground shows textured (not flat-brown) ground in reflection; GI bounce off ground carries albedo variation (compare raw_diffuse debug view before/after).
- [ ] **Step 3:** Builds; commit — `gtexvk t9: RT hit shaders sample ground tileset (flat) — GI/reflection parity`.

---

# Phase 2 — Parallax occlusion mapping

## Task 10: World-space POM march + conservative depth write

**Files:**
- Modify: `shaders_vk/tileset_common.glsl` (add `tileset_pom_march`), `shaders_vk/gbuffer.frag` (call it; `layout(depth_less) out float gl_FragDepth` — **reversed-Z: pushed-away = smaller depth**), `MatterEngine3/tests/tileset_slicer_tests.cpp` (C++ mirror of the march math)

March (add to `tileset_common.glsl`):

```glsl
// World-space POM: step the view ray below the fragment's triangle plane,
// re-resolving the Wang cell per sample so the march crosses cell boundaries
// onto the true runtime neighbor (seam-transparent by construction).
// plane_point/plane_n: fragment world pos + interpolated normal (the datum).
// Returns the displaced world position; height==datum ⇒ returns entry point.
vec3 tileset_pom_march(int slot, vec3 ray_origin, vec3 ray_dir,
                       vec3 plane_point, vec3 plane_n,
                       vec2 dWdx, vec2 dWdy) {
    float h_range = tileset.height_max[slot] - tileset.height_min[slot];
    if (h_range <= 0.0) return plane_point;
    int   steps  = int(tileset.pom_a.x);
    // March from the point where the ray crosses the relief ceiling (datum, the
    // bake's top surface = height_max) down to the floor (height_min).
    // Bake height is single-valued (top-down ortho) — no overhang cases.
    vec3 p = plane_point;
    vec3 step_v = ray_dir * (h_range / max(abs(dot(ray_dir, plane_n)), 0.08)
                             / float(steps));
    float prev_diff = 0.0;
    vec3 prev_p = p;
    for (int i = 0; i < steps; ++i) {
        p += step_v;
        float ray_h = dot(p - plane_point, plane_n);            // ≤ 0, descending
        float tex_h = (tileset_sample(slot, TILESET_CH_HEIGHT, p.xz,
                                      dWdx, dWdy).r - 1.0) * h_range; // top=0
        float diff = ray_h - tex_h;                              // <0 ⇒ below relief
        if (diff < 0.0) {
            float t = prev_diff / max(prev_diff - diff, 1e-6);   // linear refine
            vec3 hit = mix(prev_p, p, t);
            for (int r = 0; r < int(tileset.pom_a.y); ++r) {     // binary refine
                step_v *= 0.5;
                vec3 mid = hit;  // placeholder; full bisection in implementation
            }
            return hit;
        }
        prev_diff = diff; prev_p = p;
    }
    return p;
}
```

(The bisection placeholder above is intentionally sketched — implement full bisection between `prev_p`/`p`; the C++ mirror test defines the required behavior exactly.)

`gbuffer.frag`: gate on `tileset_detail_slot >= 0 && view_distance < pom_max_distance` with the fade band blending marched vs. flat results; recompute `gl_FragDepth` by projecting the marched world point through `world_to_clip` (add the `FrameConstants` UBO to `gbuffer.frag` — same set 0 binding 0 block as `raster.vert:20-28`); sample albedo/normal/ORM at the marched XZ.

- [ ] **Step 1: C++ mirror tests first.** Port the march (scalar, sampling a synthetic C++ heightfield lambda) into the slicer test binary: (a) flat height → exit at entry point; (b) a step-function trench → hit within one linear step + refined within `2^-refine` of analytic; (c) a boundary-crossing ray over two "cells" with different heightfields but equal edge strips → continuous hit across the boundary; (d) grazing ray (`|dot(dir,n)|` small) terminates (the 0.08 clamp). These tests define the GLSL implementation.
- [ ] **Step 2:** Implement GLSL to match the mirror; regen SPIR-V; wire tunables (already in the UBO from Task 6).
- [ ] **Step 3:** Depth write + `depth_less`; verify early-Z is retained (no validation warnings; frame time on Meadow within noise of Phase-1 build at distance where POM is faded out).
- [ ] **Step 4:** Shots: low-sun grazing view of ForestFloor — pebbles/twigs visibly occlude; seam-crossing close-up unchanged continuity. Builds; commit — `pom t10: world-space POM + reversed-Z depth_less conservative write`.

## Task 11: Height self-shadow

**Files:** `shaders_vk/tileset_common.glsl`, `shaders_vk/gbuffer.frag`

- [ ] **Step 1:** 8-step march from the POM hit toward the sun direction (available via the lighting UBO — grep `sun\|light_dir` in `gbuffer.frag`'s available sets; if the GBuffer pass lacks it, add to `TilesetParams` upload), capped at 0.3 m, soft factor `shadow = saturate(min_clearance / softness)`; multiply into the AO channel output.
- [ ] **Step 2:** Sunset shot demonstrating litter shadows; verify no seam artifacts (cap ≤ 2×edgeStripWidth keeps it arrangement-safe). Builds; regen SPIR-V; commit — `pom t11: height self-shadow toward sun`.

---

# Phase 3 — Macro tileset

## Task 12: `ForestFloorMacro` authoring + loading + binding

**Files:**
- Create: `MatterEngine3/examples/world_demo/objects/ForestFloorMacro.js` (16 m tiles, 32 texels/m, broad mottling `base()` + sparse large features; spec §Phase 3 authoring sketch)
- Modify: `MatterEngine3/examples/world_demo/WorldData/Meadow/world.manifest` (add `ForestFloorMacro [tileset]`)
- Modify: `MatterEngine3/src/matter_engine.cpp` — world-load wiring assigns it slot 1 and calls `MaterialRegistrySetGroundMacroSlot(DIRT, 1)`

- [ ] **Step 1:** Author + bake (existing pipeline; content-hash cache produces `Meadow/ForestFloorMacro.gtex`); confirm bake output PNG dump looks like low-frequency variation, iterate authoring until it does.
- [ ] **Step 2:** Load + bind; verify `flags_misc` packing now carries macro (Task 8 test already covers the pack; add the world-load integration assert where the GL path's equivalent lives).
- [ ] **Step 3:** Builds; commit — `macro t12: ForestFloorMacro tileset authored + bound to slot 1`.

## Task 13: Frequency-split compositing

**Files:** `shaders_vk/tileset_common.glsl` (compositing function per spec §Phase 3 blend math), `shaders_vk/gbuffer.frag`, `shaders_vk/rt_surface_common.glsl` (same function; RT uses it flat)

```glsl
// w = detail weight from distance; 1 near, 0 past the fade.
// macroRatio modulates detail albedo by macro's deviation from its own mean —
// near-field variety AND far-field takeover in one continuous expression.
float tileset_detail_weight(float dist) {
    return 1.0 - smoothstep(tileset.pom_b.x - tileset.pom_b.y,
                            tileset.pom_b.x + tileset.pom_b.y, dist);
}
```

Compositing exactly as the spec's block (albedo ratio clamp `[0.25, 4]`, RNM-style normal add, AO multiply, roughness lerp). POM and self-shadow scale by `w`.

- [ ] **Step 1: Identity tests** (C++ mirror in the slicer test binary, same pattern as Task 10): `w=0` → pure macro; `w=1` with `macro == mean` → pure detail; ratio clamp holds for near-black macro mean.
- [ ] **Step 2:** Implement GLSL both call sites; regen SPIR-V.
- [ ] **Step 3:** Shots: (a) far-field (200 m+) — no visible 2 m or 8 m periodicity (the v1 failure case, now with macro takeover); (b) near-field A/B with macro disabled — repetition visibly broken; (c) the full v1 seam-heavy path re-run one last time across all distances.
- [ ] **Step 4:** Builds; final ledger update (`.superpowers/sdd/progress.md` if in use — grep for it); commit — `macro t13: frequency-split compositing (near variety, far takeover)`.

---

## Task ordering & gates

| Gate | Requirement |
|---|---|
| Phase 0 → 1 | Tasks 1–4 merged AND visually soaked (separate session); z-fight shot green |
| Phase 1 → 2 | Seam shots clean at all three mip-forcing distances; RT reflection parity shot |
| Phase 2 → 3 | POM mirror tests green; frame-time delta acceptable; grazing-light shot approved |
| Done | All shots green; full linkable test sweep on Windows; both builds clean |

**Deliberate deviations from the older GL-era plan style, for the record:** GPU-side unit tests are replaced by C++ mirror tests + scripted shots because the Vulkan renderer has no headless compute-test harness equivalent to `tileset_gpu_tests` yet; if one exists by implementation time (grep `MatterEngine3/tests` for vk headless patterns), prefer porting the mirror tests there.
