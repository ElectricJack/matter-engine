# Vulkan Hardware-RT `.gtex` Bake — Porting the Ground-Tileset Atlas Bake and Retiring the GL Path

> Design and implementation spec for moving the `.gtex` ground-tileset atlas bake into the Vulkan renderer using its hardware ray tracing, and deleting the legacy GL compute-shader/software-BVH bake. Today the Vulkan-only viewer can *load* a cached `.gtex` but cannot *bake* one — the deferred tileset phase falls into a headless load-only branch, and a cache miss leaves the ground untextured. This spec replaces the GL bake's three passes (primary ortho rays, AO hemisphere rays, horizon scan) with Vulkan compute passes that trace against a bake-only `VkAccelerationStructureKHR` via `GL_EXT_ray_query`, read back through the existing staging machinery, and write a **byte-identical `.gtex` format** through the untouched `save_gtex`. The bake changes; the format, the cache-key scheme, the Wang-seam invariant, and the entire consumer stack (slicer, texture arrays, POM, horizon shadows) do not. Once the Vulkan bake is proven, the GL bake TUs, their compute shaders, and the `MATTER_VULKAN_ONLY` bake gate are deleted — `.gtex` baking becomes Vulkan-only.

- **Target:** new `MatterEngine3/src/render/tileset_bake_vk.{h,cpp}` + two `shaders_vk/tileset_bake_*.comp` SPIR-V shaders + `VkSceneRenderer` bake entry point + `LocalProvider` re-wiring (`run_tileset_deferred`), followed by deletion of `tileset_bake_gpu.cpp`, `tileset_bake_primary/ao/horizon.{h,cpp}`, `render/tileset_gl_ctx.{h,cpp}`, `shaders_gpu/tileset_bake_*.comp`, and the `cfg_.gl_available` / `#ifndef MATTER_VULKAN_ONLY` branching in `provider/local_provider.cpp`
- **Baseline:** `c9a41297` (feature/bake-lab)
- **Status:** Spec — Part I design settled per user decision (hardware RT, not a software-BVH port); Part II milestones ready to sequence; implementation not started
- **Relation:** bake-side sequel to the Vulkan tileset **consumer** port ([2026-07-21-tileset-vulkan-parallax-macro-design.md](../../docs/superpowers/specs/2026-07-21-tileset-vulkan-parallax-macro-design.md)), which ported `.gtex` consumption (slicing, per-tile mips, POM, macro layer) and explicitly declared "any change to the bake pipeline … or `.gtex` format" a non-goal. This spec is that deferred bake half. The settle stage and its optimizer tooling ([settle-tick-optimizer.md](settle-tick-optimizer.md)) are untouched — settle is renderer-agnostic and stays on the bake worker.

---

## Part I — Design Spec

### I.1 Problem statement

The `.gtex` atlas bake is the last piece of the tileset pipeline that still requires OpenGL. The current bake (`bake_tileset_gpu()`, `src/tileset_bake_gpu.cpp:102`) is **compute-shader ray tracing against a CPU-built software BVH**:

1. **Cache check** — `gtex_content_hash(pose_hash, script_source_hash, kEngineBakeVersion, kBox3dVersion)` probed against the on-disk header (`tileset_bake_gpu.cpp:115-121`, `gtex_cache_hit` in `tileset_gtex.h:134`).
2. **GL ≥ 4.6 gate** — `tileset_gl_init(err)` (`tileset_bake_gpu.cpp:126`, `render/tileset_gl_ctx.h:14`).
3. **CPU BVH assembly** — `assemble_torus_bvh(settled, …)` folds the settled torus (periodically tessellated base heightfield + every `SettledInstance`'s baked part) into a MatterSurfaceLib `BLASManager`/`TLASManager` (`tileset_bake_gpu.cpp:132-140`, `tileset_torus_bvh.h:27-33`).
4. **Three GL compute programs** compiled from `shaders_gpu/tileset_bake_primary.comp` / `tileset_bake_ao.comp` / `tileset_bake_horizon.comp` (`tileset_bake_gpu.cpp:145-157`), the first two textually including `materials.glsl` (81 lines) and the 832-line software traversal library `bvh_tlas_common.glsl` (`tileset_bake_primary.comp:28,37`; both live in `MatterSurfaceLib/shaders/`).
5. **PRIMARY pass** — one straight-down ortho ray per texel (`O=(wx, rayY, wz)`, `D=(0,-1,0)`, `tileset_bake_primary.comp:56-63`) through `intersectScene`, writing four channels: RGBA8 albedo (material color), RG8 normal packed `0.5*N.xz+0.5` with a `N.y<0` flip (`:80-83`), RGBA8 ORM with `.r=1.0` AO placeholder (`:91`), R16 height normalized over `[heightMin, heightMax]` (`:86-87`). C++ side: `GL_RGBA8/GL_RG8/GL_RGBA8/GL_R16` images read back as 3/2/3-byte and u16 buffers (`tileset_bake_primary.cpp:58-61`, `:146-152`).
6. **AO pass** — re-casts the primary ray, then 64 cosine-hemisphere shadow rays per texel via `shadowQuery`, distance-capped at `maxRayDist = cfg.edge_strip_width` (0.15 m default; `tileset_bake_ao.cpp:111`, `tileset_spec.h:13`). The Monte Carlo sequence is seeded from **tile-local coordinates** (`local_x = xy.x % tile_texels`, `tileset_bake_ao.comp:80-97`) so texels at the same phase in different tiles get *identical* ray sets — the load-bearing mechanism behind Wang-boundary seam invariance. AO is packed into ORM `.r` on the CPU (`pack_orm_ao`, `tileset_bake_gpu.cpp:72-75,212`).
7. **HORIZON pass** — no BVH at all: re-uploads the full-res R16 height buffer and scans it (8 azimuths × 24 radial samples, 0.30 m radius, toroidal wrap addressing) into two quarter-res RGBA8 images (`tileset_bake_horizon.comp:38-51`, `tileset_bake_horizon.cpp:41-56`).
8. **`save_gtex()`** — pure `fwrite` of the v2 container (PNG-compressed channels, LE header); completely renderer-agnostic (`tileset_gtex.h:10-29,95-105`).

The Windows viewer is built `-DMATTER_VULKAN_ONLY` (`MatterViewer/Makefile:243`) and does **not** compile any of the GL bake TUs — `WIN_ME3_CPP` (`MatterViewer/Makefile:128-199`) omits `tileset_bake_gpu.cpp`, `tileset_bake_primary/ao/horizon.cpp`, `tileset_gl_ctx.cpp`, and `tileset_provider.cpp`, while it *does* compile `tileset_torus_bvh.cpp`, `tileset_gtex.cpp`, and the Vulkan consumer (`render/tileset_slicer.cpp`, `:140-141,155`). At runtime `cfg.gl_available` is hard-false under `MATTER_VULKAN_ONLY` (`matter_engine.cpp:1088-1091`, `:3465-3467`), so `run_tileset_deferred` takes the headless branch (`provider/local_provider.cpp:805-882`): settle (needed for the `pose_hash` half of the cache key), probe the cache, load a hit via `cfg_.vk_tileset_load` → `VkSceneRenderer::load_tileset_slot` (`vk_scene_renderer.cpp:2555`) — and on a miss, print a diagnostic and leave the ground untextured. The GL bake block is `#ifndef MATTER_VULKAN_ONLY` (`local_provider.cpp:884-955`) with an `#else` hard error (`:956-959`).

Consequences: the flagship viewer cannot produce its own ground textures. Any tileset source edit, seed change, or `kEngineBakeVersion` bump strands it until someone runs the GL viewer (WSLg + `GALLIUM_DRIVER=d3d12`) to repopulate the cache. Meanwhile the Vulkan renderer already owns everything a bake needs: KHR acceleration-structure builds (`vkCmdBuildAccelerationStructuresKHR`, `vk_scene_renderer.cpp:5571`), ray dispatch (`vkCmdTraceRaysKHR`, `:3549,5574`), ray queries in compute (`vol_scatter.comp:3,13,77-85`), storage images, staging upload, image→buffer readback (`vkCmdCopyImageToBuffer` `:3917,:4141`; `matter::readback_buffer` `:3961`), and an off-frame submission path (`matter::submit_immediate` + `ImmediateSubmitPhase`, `render/vk_resources.h:137-146`) already used by the GI test harness for exactly this record-submit-readback shape.

### I.2 Goals and non-goals

**Goals:**

- The Vulkan-only viewer **bakes `.gtex` itself**, on a cache miss, using hardware ray tracing — end-to-end (settle → AS build → primary → AO → horizon → `save_gtex` → slot load) with no GL anywhere.
- **Byte-identical `.gtex` output format.** `save_gtex`/`load_gtex`, the `GTexHeader`, the channel set and encodings are untouched; the existing consumer (`load_tileset_slot` → `tileset_slicer.cpp` → `tileset_common.glsl` sampling/POM/horizon shadows) works unchanged on a Vulkan-baked atlas.
- **Seam invariance preserved as a hard correctness requirement:** same-color Wang boundary strips remain byte-identical within one baked atlas, via the same tile-local-seed mechanism and distance-capped AO.
- **Deterministic bakes:** the same settled pose + source + versions produces the same atlas run-to-run on the same device/driver (fixed seeds, no temporal or frame-order dependence).
- **Clean cache-identity cut:** `kEngineBakeVersion` bumps 2 → 3, so every GL-baked cache entry honestly invalidates and Vulkan-baked atlases carry a distinct identity.
- **Delete the GL bake** — TUs, compute shaders, the `gl_available` gate, and the `MATTER_VULKAN_ONLY` bake branching — once the Vulkan bake is validated.
- Bake runs where the Vulkan device lives (the app/render thread), marshaled through the existing `cfg_.gpu_run` → `GpuJobQueue::run_blocking` seam (`matter_engine.cpp:1056-1064`); settle stays on the bake worker, unchanged.

**Non-goals:**

- **No change to settle** — physics, the settle cache (`tileset_phase.cpp:180-194`), `SettledTorus`, or `pose_hash` computation.
- **No `.gtex` format change** — no new channels, no header fields, no version bump of the *container* (`kGTexVersion` stays 2; only `kEngineBakeVersion` moves).
- **No consumer changes** — the slicer, texture arrays, descriptor layout, `tileset_common.glsl`, POM, and horizon shading from the consumer port are out of scope.
- **No GL-consumer retirement in this spec.** `render/tileset_provider.cpp` (the GL viewer's atlas upload) survives for the frozen Linux GL viewer; it simply never gets fresh bakes after deletion (see §I.8). Full GL-path retirement is the existing separate track.
- **No async/amortized baking.** The GL bake blocked its thread for the bake duration; the Vulkan bake may too (marshal + block, same as today). Splitting the bake across frames is a recorded future refinement, not this spec.
- **No macro-tileset-specific work** — a macro `.gtex` is just another bake through the same path (consumer spec, Phase 3).

### I.3 Approach comparison

#### A — Port the GL software-BVH compute shaders to Vulkan SPIR-V

Compile `tileset_bake_primary/ao.comp` (plus `bvh_tlas_common.glsl`'s 832 lines of stack-based BLAS/TLAS traversal and its four data textures) to Vulkan compute; keep `assemble_torus_bvh` and the CPU BVH build exactly as-is.

- **Pro:** maximally faithful — identical traversal, identical results, the seam/determinism argument is inherited rather than re-made.
- **Pro:** no RT feature dependency; would run on any Vulkan 1.3 compute device.
- **Con — disqualifying in context:** it *permanently enshrines* ~900 lines of hand-rolled traversal GLSL (plus the texture-encoded BVH upload path in `BLASManager`/`TLASManager`, currently raylib-GL-specific — `blas_manager.hpp:242-255`) in the renderer we are consolidating on, while the same renderer already ships hardware AS builds and three ray-tracing pipelines. Two BVH systems forever.
- **Con:** the GL BVH texture plumbing (`bind_bvh_samplers`, `ensure_gpu_textures_ready`) has no Vulkan equivalent; porting it means new SSBO encodings and re-validating traversal — most of the port cost with none of the hardware win.
- **Con:** software traversal of a few hundred thousand rays × 65 rays/texel is exactly what RT cores are for; the GL bake is seconds-scale on this workload where HW RT is trivially faster.

#### B — Hardware-RT bake **(chosen)**

Build a **bake-only** `VkAccelerationStructureKHR` (BLASes from the torus geometry, one TLAS over the settled instances), and re-express the primary and AO passes as **Vulkan compute shaders using `GL_EXT_ray_query`** against that TLAS — the exact pattern already shipping in `vol_scatter.comp` (`:3,13,77-85`, gated on `vulkan_->ray_tracing_available()`, `vk_volumetrics.cpp:117-121`). Storage-image outputs, `vkCmdCopyImageToBuffer` readback, `save_gtex` unchanged. Horizon stays a (trivially ported) compute pass over the height image — it casts no rays.

- **Pro:** ~200 lines of bake shader replace ~900 lines of traversal library; the AS build, dispatch, barrier, and readback code all reuse proven `VkSceneRenderer`/`vk_resources` machinery.
- **Pro:** faster bakes (hardware traversal), and the AS build cost for a torus scene (tens of parts, hundreds of instances) is milliseconds-scale next to the per-frame AS builds the renderer already does (`build_ray_geometry`/`emit_ray_instances`, `vk_scene_renderer.cpp:5580-5604`).
- **Pro:** one BVH technology in the engine going forward; deleting the GL bake also deletes the last consumer of the GL BVH texture path in this pipeline.
- **Con:** requires `rayQuery` support — the bake inherits the RT-capable-device requirement. Acceptable: the Vulkan viewer's headline features (RT lighting/shadows/GI) already require it; a non-RT device degrades to today's behavior (cache-load or untextured), fail-closed.
- **Con:** results are not bit-identical to the GL bake (different traversal, different float paths). Handled as a *cache-identity* problem, not a compat problem: `kEngineBakeVersion` bump (§I.6).
- **Con:** needs an offscreen submission distinct from the scene frame, and a determinism argument re-made for HW traversal (§I.6). Both bounded — see below.

**Why ray queries in compute rather than a raygen pipeline:** the bake is a regular 2-D grid of independent rays with image-store outputs — compute-shaped, not SBT-shaped. Ray queries avoid adding bake entries to the scene's SBT (`rt_sbt_*`, built in `create_ray_tracing_pipeline`, `vk_scene_renderer.cpp:1255`), avoid payload/hit-group plumbing, keep the bake pipeline-independent of the scene RT pipeline's descriptor layout, and match the GL bake's structure one-to-one (three dispatches, same uniforms, same images) — which keeps the diff reviewable and the seam argument transferable. The rgen route (`rt_surface_test.rgen` + `record_test_surface_ray`, `:3476-3598`) remains the fallback if a driver's ray-query path misbehaves, but is not the plan.

#### C — CPU bake (embree-style or hand traversal on the worker)

Trace on the CPU against `assemble_torus_bvh`'s existing CPU BVH; no GPU at all.

- **Pro:** renderer-independent, runs headless, trivially deterministic.
- **Con:** 64+1 rays × (4096²) texels ≈ 1.1 B ray casts through a scalar CPU BVH — minutes per atlas where the GPU takes seconds; would add a *third* traversal implementation (the CPU BVH's `intersect` path is currently only GPU-consumed in this pipeline); rejected on performance and consolidation grounds.

> **Decision:** Approach **B** — hardware-RT bake via `GL_EXT_ray_query` compute passes against a bake-only acceleration structure, `kEngineBakeVersion` 2 → 3, then delete the GL bake path. (User decision; A and C recorded for the archaeology.)

### I.4 The bake as ray tracing

#### Geometry: settled torus → bake-only BLAS/TLAS

`assemble_torus_bvh` already does the *assembly* half renderer-agnostically: it tessellates the base heightfield periodically with wrap-extension and real per-vertex normals (`tileset_torus_bvh.cpp:46-95`), loads each referenced part via `part_asset::load_v2`, and registers everything with `BLASManager`, whose `BLASEntry` retains the plain CPU triangle arrays (`blas_manager.hpp:54-69` — `std::vector<Tri> triangles` + `TriEx` per-vertex normals), plus a `TLASManager` holding per-instance transforms. The split this spec makes:

- **Keep the assembly** (part loading, torus tessellation, instance transform composition, material-id per triangle) — it is the input contract for any backend.
- **Feed Vulkan from the CPU-side entries:** a new `tileset_bake_vk.cpp` walks `BLASManager::get_entries()` and the TLAS instance list, uploads per-entry vertex/index/material-id buffers (staging upload, `vk_resources` helpers), builds one BLAS per entry and one TLAS of `VkAccelerationStructureInstanceKHR` (transform = the settled instance's TRS), using the same `vkGetAccelerationStructureBuildSizesKHR`/`vkCmdBuildAccelerationStructuresKHR` sequence as `emit_ray_instances` (`vk_scene_renderer.cpp:5568-5593`) but into **bake-owned, single-use allocations** freed when the bake returns. The CPU BVH that `assemble_torus_bvh` also builds is wasted work in V1 (correctness-neutral, seconds at most) and is stripped in V5 when its only remaining consumer dies (§I.8, open question Q2).
- **Why not the scene TLAS:** the settled torus is a *synthetic periodic arrangement* — the 4×4 wrapped strip-and-interior layout with the toroidal base — that never exists as scene geometry. A bake-only AS is not an optimization choice; it is the only correct input. (Sharing already-resident part BLASes with the scene renderer is a possible future optimization, recorded in Q2.)

Hit-attribute note: ray queries return `(instance, primitive, barycentrics)`, not interpolated attributes. The bake shaders therefore read the uploaded vertex/index/material-id SSBOs directly to reconstruct the hit normal (smooth `TriEx` normals where present, face normal otherwise — matching `bvh_tlas_common.glsl`'s decode) and the material id. We own these buffers, so the encoding is whatever the shader wants.

#### Pass 1 — primary (`shaders_vk/tileset_bake_primary.comp`)

A near-transliteration of the GL shader minus the traversal library: per texel, texel-center → world XZ (`(x+0.5)/texelsPerMeter`), ortho ray from `rayY` straight down, `maxT = rayY - (heightMin - 1)`; `rayQueryEXT` closest-hit (opaque, no cull); on hit decode material albedo/roughness/metallic from the material SSBO (packed exactly as today's 12-float `MaterialRegistryPackForGPU` layout, `tileset_bake_primary.cpp:75-88`) and write:

| image | format | encoding (identical to GL, `tileset_bake_primary.comp:66-92`) |
|---|---|---|
| albedo | `VK_FORMAT_R8G8B8A8_UNORM` | material rgb, a=1; miss → (0,0,0,1) |
| normal | `VK_FORMAT_R8G8_UNORM` | `0.5*N.xz+0.5` after `N.y<0` flip; miss → (0.5,0.5) |
| ORM | `VK_FORMAT_R8G8B8A8_UNORM` | (1.0, rough, metal, 1); miss → (1,1,0,1) |
| height | `VK_FORMAT_R16_UNORM` | `clamp((hit.y-heightMin)/(heightMax-heightMin),0,1)`; miss → 0 |

`ray_y`, `heightMin/Max` come from the same `compute_height_range` logic (`tileset_bake_gpu.cpp:41-67,176-181`), which moves verbatim into the new orchestrator. Readback converts RGBA→RGB / RG / RGBA→RGB / R16 into the exact buffer shapes `save_gtex` expects (3/2/3 bytes + u16 per texel) — same post-processing the GL readback's `GL_RGB`/`GL_RG`/`GL_RED` formats performed implicitly.

#### Pass 2 — AO (`shaders_vk/tileset_bake_ao.comp`)

Re-cast the primary ray for position+normal (as the GL pass does — cheaper than persisting hit data, and keeps the two passes independent), offset `P + N*1e-3`, then 64 cosine-hemisphere occlusion rays with `tMax = edge_strip_width`, each a `rayQueryEXT` initialized with `gl_RayFlagsTerminateOnFirstHitEXT` — the exact shape of `vol_scatter.comp:77-85`'s shadow query. **The RNG moves over unchanged**: same `splitmix32`, same `u01`, same `cosine_hemi`, same basis construction, and critically the same **tile-local seed coordinates** (`local_x = xy.x % tile_texels` with `tile_texels = atlasW/4`, `tileset_bake_ao.comp:80-97`) and the same `seed = (uint32_t)settled.cfg.seed` (`tileset_bake_gpu.cpp:202`). Output R8 storage image → readback → `pack_orm_ao` into ORM `.r` on the CPU, as today.

This is the pass where seam invariance lives, so the invariant is restated as the implementation contract: *two texels at the same tile-local phase must trace the identical ray set, and each ray contributes only a boolean "hit within 0.15 m"* — which makes the per-texel result independent of traversal order (§I.6).

#### Pass 3 — horizon

No rays, no BVH — the GL pass re-uploads the primary's R16 height output and scans it with toroidal integer wrap (`tileset_bake_horizon.comp:42-79`). Two equally valid ports:

- **(preferred)** a Vulkan compute pass: upload height R16, two quarter-res RGBA8 storage images, same 8×24 scan constants — a mechanical shader port with zero traversal content; keeps all three passes on one code path and one readback idiom.
- a CPU loop over the height buffer already sitting in host memory (~1M quarter-res texels × 192 samples — sub-second, deterministic by construction).

Default is the compute port; the CPU fallback is recorded as Q3 in case the shader port surfaces any friction (it is also a useful cross-check in tests either way).

#### Orchestration

A new `tileset::bake_tileset_vk(settled, script_source_hash, out_gtex_path, inputs, force, dump_png, err)` mirrors `bake_tileset_gpu`'s step list exactly (cache check → assemble → AS build → primary → AO → pack → horizon → header fill → `save_gtex` → optional PNG dump), asserting the render thread instead of `assert_gl_thread` (`tileset_bake_gpu.cpp:110`). GPU work records into an off-frame submission via `matter::submit_immediate` (`vk_resources.h:144`) — the pattern the GI debug readbacks already use (`vk_scene_renderer.cpp:3922-3926`) — with AS-build → ray-query barriers between build and dispatch, compute → transfer barriers before the copies, and a host barrier before `readback_buffer`. One submission for the whole bake is acceptable at these sizes; splitting per-pass is an implementation freedom, not a contract.

### I.5 Format and consumer invariance

The `.gtex` contract is frozen at every layer:

- **Writer:** `save_gtex` (`tileset_gtex.h:95-105`) is pure `fwrite` + stb PNG encode, called with the same six buffers, same dims, same header fields. Not one line changes.
- **Header:** `GTexHeader` (`tileset_gtex.h:53-68`) fields are filled exactly as `tileset_bake_gpu.cpp:231-240` fills them; only the *value* of `engine_bake_version` changes (2 → 3), which is data, not format. `kGTexVersion` stays 2.
- **Channels:** `CHAN_ALBEDO_RGB8 / NORMAL_RG8 / ORM_RGB8 / HEIGHT_R16 / HORIZON_A / HORIZON_B` (`tileset_gtex.h:42-51`) with the encodings tabulated in §I.4 — the consumer's decode assumptions (normal-Z reconstruction via `sqrt(1-nx²-nz²)`, height denorm via `height_min/max`, horizon `sin(elevation)` unorm8 packing) all hold because the producer encodings are transliterated, not redesigned.
- **Consumer:** `VkSceneRenderer::load_tileset_slot` (`vk_scene_renderer.cpp:2555`) → `tileset_slicer.cpp` → per-layer mips → `tileset_common.glsl` sampling/POM/horizon shadows operates on `load_gtex` output only; it cannot observe which renderer baked the file. The acceptance test for this section is literal: a Vulkan-baked ForestFloor `.gtex` loads through the unchanged consumer and renders.

### I.6 Determinism and the cache key

**Cache identity.** The content-hash inputs stay `(pose_hash, script_source_hash, kEngineBakeVersion, kBox3dVersion)` (`gtex_content_hash`, `tileset_gtex.h:79-82`) — the scheme is untouched. Because HW-RT output is not bit-identical to GL output, the two bakes must not share a cache identity: **`kEngineBakeVersion` bumps 2 → 3** (`tileset_gtex.h:39`). Every GL-baked `.gtex` then fails `gtex_cache_hit` honestly (the headless-miss diagnostic even names the stale half, `local_provider.cpp:855-877`) and is rebaked by Vulkan on next load. No dual-accept, no migration shims.

**Run-to-run determinism (same device + driver).** Required, and achieved by construction:

- All ray sets are closed-form functions of `(texel, cfg.seed, pass constants)` — no time, no frame counter, no atomics-ordered accumulation. The AO RNG is the ported `splitmix32` chain.
- The AS is built from identically ordered input (BLAS entry order and instance order are deterministic outputs of `assemble_torus_bvh`), with fixed build flags — no per-frame LOD selection, no compaction, no update builds.
- Each pass is one dispatch writing disjoint texels via `imageStore` — no inter-invocation communication.

What is *not* claimed: bit-identity across driver versions or GPU vendors. That is the same (previously implicit) situation the GL bake was in, now made explicit; the cache key's `kEngineBakeVersion` is the knob if a cross-device-authoritative bake is ever needed (Q5).

**Seam invariance (byte-identical Wang boundary strips) — the hard requirement.** The GL bake's invariant rests on three legs, and each survives the HW-RT move:

1. *Identical inputs:* same-color boundary strips contain identical settled geometry relative to the strip (settle-side property, untouched), and the primary/AO ray sets for same-phase texels are identical (tile-local seeding, ported verbatim).
2. *Order-independent math:* the AO result per texel is `1 − count(hit)/64` where each term is a boolean "any hit within 0.15 m" — invariant under traversal order, so HW traversal cannot perturb it *except* through leg 3. The primary pass is a closest-hit query, well-defined regardless of traversal order except at exact-tie hit distances.
3. *Float-path identity:* two same-phase texels tracing congruent rays against congruent geometry must take bit-identical float paths. Under the GL bake this held because ray *origins* differ by exact tile offsets and the traversal math is shader-defined. Under HW RT the intersection itself is fixed-function; congruent-but-translated ray/triangle pairs are *expected* to resolve identically, but the hardware makes no formal promise, and world-space coordinates differing in magnitude across the atlas can shift rounding.

Leg 3 is therefore the one open risk, and it gets a test, not an assumption: the existing seam assertion (byte-equality of boundary strips per color pair, from `tileset_seam_tests`) is re-pointed at the Vulkan bake as a **release gate** (Part II, V2/V5). Mitigation ladder if it trips: (a) express bake geometry and rays in *tile-local* coordinates per strip region (translating the whole trace frame so congruent texels are bit-congruent, eliminating magnitude drift); (b) post-bake strip canonicalization — copy one representative strip over its color-mates before `save_gtex` (byte-equality by construction; legitimate because the strips are geometrically identical by the settle contract). Option (b) is crude but bounded, guaranteed, and invisible to the consumer; it is the floor under this risk.

### I.7 Where and when the bake runs

The bake slots into the **deferred tileset phase** exactly where the GL bake sat, replacing the headless load-only branch:

- **Trigger:** `run_tileset_deferred` (`local_provider.cpp:753`), called from `publish_pipeline`'s tail after `BakeFinished` (non-fatal; `matter_engine.cpp:1948-1967`) and eagerly from `connect()` (fatal, sync contract).
- **Worker half (unchanged):** settle via `run_tileset_phase_from_objects` (settle-cache-wired, `tileset_phase.cpp:180-194`), script-source hash, cache-path computation — all stays on the bake worker (`local_provider.cpp:781-788`, `:888-910`).
- **Render-thread half:** the Vulkan device is owned by the app thread; GL bake work already marshaled there via `cfg_.gpu_run` → `GpuJobQueue::run_blocking` (`matter_engine.cpp:1056-1064`), pumped by the app loop (`:4788`). The Vulkan bake uses the *same seam*: the worker calls `run_gl(...)` (renamed `run_gpu` in passing) with a closure that now invokes the Vulkan bake + slot load.
- **Wiring:** `LocalProvider::Config` drops `gl_available` (`local_provider.h:85`) and gains `vk_tileset_bake` — a `std::function<bool(const tileset::SettledTorus&, uint64_t script_hash, const std::string& gtex_path, std::string&)>` sibling of `vk_tileset_load` (`local_provider.h:77`). `matter_engine.cpp` binds it to `vk_scene->bake_tileset(...)` under `MATTER_VULKAN_VIEWER` (next to the existing `vk_tileset_load` binding, `:1066-1078`); a null callback (no Vulkan renderer, headless test builds) degrades to exactly today's headless behavior: settle → cache probe → load or untextured-with-diagnostic. The branch structure in `run_tileset_deferred` collapses from three arms (headless / GL / VULKAN_ONLY-error) to two (bake-capable / load-only) with no preprocessor gate.
- **Failure contract:** unchanged — deferred-path bake failure emits `BakeError{phase="tileset"}` and leaves the world rendering untextured (`matter_engine.cpp:1958-1966`); `connect()` stays fail-fast.
- **Slot load:** on bake success the closure calls `cfg_.vk_tileset_load(slot, gtex_path)` — the identical call the cache-hit path makes today (`local_provider.cpp:840-853`) — so post-bake loading shares one code path with cache-hit loading.

**Frame interaction:** the bake records via `submit_immediate` between frames on the same queue the frame loop uses. The frame in flight completes (submission-order serialization); the next frame waits behind the bake. That is a deliberate stall, identical in character to the GL bake occupying the GL thread. The `on_tileset_part` progress events (`local_provider.cpp:800-801,954-955`) still bracket each tileset so the UI can show "baking ground…". Per-pass submission granularity (three smaller stalls instead of one) is an implementation freedom; true async is future work.

### I.8 Deleting the GL bake

Executed only after V1–V4 validate (Part II). Verified reference graph as of baseline:

**Deleted outright:**

| artifact | references to unwind |
|---|---|
| `src/tileset_bake_gpu.{h,cpp}` | `MatterEngine3/Makefile:103,170`; tests `GPU_PIPELINE_CPP` (`tests/Makefile:796`), `VIEWER_LOGIC_CPP` (`:725`) |
| `src/tileset_bake_primary.{h,cpp}`, `tileset_bake_ao.{h,cpp}`, `tileset_bake_horizon.{h,cpp}` | `Makefile:103,169-170`; `tests/Makefile:723,804` |
| `src/render/tileset_gl_ctx.{h,cpp}` | `Makefile:109,173`; `tests/Makefile:721,771` |
| `shaders_gpu/tileset_bake_primary/ao/horizon.comp` | `SHADER_LOGICAL` embed list (`Makefile:202-207`) |

**Gate removal:** the `#ifndef MATTER_VULKAN_ONLY` block + `#else` error (`local_provider.cpp:884-959`) and the `cfg_.gl_available` branch (`:805`) collapse per §I.7; `Config::gl_available` (`local_provider.h:85`) and its two assignment sites (`matter_engine.cpp:1088-1091`, `:3465-3467`) go; `viewer::gl_loaded()` (`render/gl46.h:14`) loses its last bake-path caller (it may have others — verify at deletion time before touching it).

**Stays (with consequences noted):**

- `assemble_torus_bvh` / `tileset_torus_bvh.cpp` — now feeding the Vulkan AS build; its CPU-BVH-build tail becomes dead weight → slimmed in V5 (Q2).
- `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` + `materials.glsl` — shared with the GL viewer's other RT shaders (`SHADER_LOGICAL`, `Makefile:202-204`); losing the bake `.comp`s does not orphan them.
- `render/tileset_provider.cpp` (GL atlas upload) and the `MatterViewer/shaders_gpu` symlink/junction plumbing (`MatterViewer/Makefile:337-341,605-615`, `setup-worktree.sh`) — the frozen Linux GL viewer keeps *loading* cached `.gtex`… but note honestly: after the `kEngineBakeVersion` bump its caches are stale and it has no bake, so the **frozen GL viewer's ground goes untextured** unless pointed at Vulkan-baked caches (which it can load — the format is unchanged and the loader checks only the stored hash it is given). This is accepted fallout of "the GL raster path is frozen" (consumer spec) — flagged for the user rather than silently discovered.
- `BLASManager`/`TLASManager` compile into the Windows viewer today (`MatterViewer/Makefile:203,210`) and continue to, as `assemble_torus_bvh`'s containers.

**GPU test targets** (`tests/Makefile`): `tileset-gpu-tests`/`run-tilesetgpu` (`:1540-1546`) and `tileset-seam-tests`/`run-tilesetseam` (`:1548-1554`) build the GL stack (`GPU_RENDER_CPP:771`, `GPU_PIPELINE_CPP:796-804`) and run under `GALLIUM_DRIVER=d3d12`. Their *assertions* (atlas well-formedness; boundary-strip byte-equality) are exactly the Vulkan bake's release gates, so the assertions migrate to a Vulkan-device harness (the `vk-scene-renderer-tests` / `vulkan_smoke_tests` precedent) and the GL targets retire with the code they test. Whether the migrated tests can run in CI headless (WSLg lavapipe ray-query support) vs. Windows-manual is Q4. `tileset-provider-tests`/`tileset-load-tests` (GL consumer + `load_gtex`) are consumer-side and keep building until GL-consumer retirement; they must not link the deleted TUs (their shared-source lists get pruned).

### I.9 Risks and mitigations

| risk | assessment | mitigation |
|---|---|---|
| **Seam invariance breaks under HW traversal** (float-path drift between congruent texels, §I.6 leg 3) | The one genuinely novel correctness risk in this spec | Seam byte-equality test as a release gate before the GL path is deleted; mitigation ladder: tile-local trace frames → post-bake strip canonicalization (guaranteed floor) |
| **Run-to-run nondeterminism** on one device | Low — no ordering-sensitive math survives into the output (§I.6) | Double-bake byte-compare test in V2; any diff is a bug, not a tolerance |
| **Channel-encoding mismatch breaks the consumer** (e.g. RGBA→RGB repack, normal flip, height normalization subtly off) | Moderate — transliteration errors are the classic port failure | V1 validates against a GL-baked reference `.gtex` channel-by-channel with per-channel tolerances *and* renders through the unchanged consumer; PNG dump path (`MATTER_TILESET_DUMP_PNG`) ported for eyeballing |
| **AS-build cost / bake-time regression** | Low — torus scenes are small vs. per-frame scene AS builds; HW trace ≫ software trace | `BAKE_SPAN(kSpanTileset)` already wraps the phase (`local_provider.cpp:772`); Timeline shows bake vs settle split; compare against GL-bake wall time on the same content |
| **Frame-loop interaction** (bake stall mid-session, resource collisions) | Moderate | Bake-owned single-use allocations only; `submit_immediate` with lifetime pinning (the GI-test pattern); stall is bounded and progress-evented; per-pass submission if a single stall proves obnoxious |
| **Readback stalls** | Accepted by design — the bake is synchronous on its thread, as the GL one was | Host barrier + `readback_buffer`; no frame-pipelined readback needed |
| **The ~45 s ForestFloor "settle" the user observed** | Unattributed — could be genuine first-run box3d settle (settle cache cold), a settle-cache miss bug, or repeated headless cache-probe settles per launch (`local_provider.cpp:806-819` settles even when only a cache probe is possible, because `pose_hash` feeds the key) | **Perf investigation item, not blocked on this spec:** read the `kSpanTileset` span on the Timeline for a warm launch; if warm launches re-settle, the settle cache key or save path is the suspect — file separately. The VK bake neither worsens nor fixes it (settle is untouched) |
| **Device without RT support** | Behavior identical to today's headless branch: cache-load or untextured, fail-closed with a clear diagnostic naming the reason | Gate on `vulkan_->ray_tracing_available()` exactly as volumetrics does (`vk_volumetrics.cpp:117-121`) |
| **`kEngineBakeVersion` bump strands existing caches** | Intended (clean cut), but first post-upgrade load of every tileset world rebakes | Cheap on HW RT; release note; the stale-hash diagnostic already names the moved version half |

---

## Part II — Implementation Spec

### II.1 Milestones

**V1 — Primary pass end-to-end.** `tileset_bake_vk.{h,cpp}`: cache check (shared helpers), geometry lift from `BLASManager::get_entries()`/TLAS instances → staging upload → bake-only BLAS/TLAS build (`submit_immediate`), `shaders_vk/tileset_bake_primary.comp` (ray query, four storage images), readback → repack → `save_gtex` (AO=1.0 placeholder in ORM `.r`, empty horizon → writer emits v2 with dims 0 or v1 form — match what the GL bake produced *for these buffers*, i.e. keep passing horizon buffers from V3 on; before V3, gate the milestone on the four core channels only and do not ship). `VkSceneRenderer::bake_tileset(...)` entry + `vulkan-spirv` embed list additions (`Makefile:222-247`, `MatterViewer/Makefile:420-467`). **Validation:** bake a tileset that has a GL-baked reference `.gtex` (same source, same seed, pre-bump build); decode both; compare albedo/ORM.gb exactly, normal/height within ±1 lsb tolerance; load the VK atlas through `load_tileset_slot` and render. *Exit: textured ground from a Vulkan-baked (partial) atlas in a dev world.*

**V2 — AO with seam invariance.** `shaders_vk/tileset_bake_ao.comp` (ported RNG + tile-local seeding + terminate-on-first-hit queries), `pack_orm_ao` reuse. **Validation:** double-bake byte-compare (determinism); boundary-strip byte-equality assertion per color pair (the `tileset_seam_tests` predicate, re-hosted); AO channel vs GL reference within Monte-Carlo-free tolerance (same ray sets + boolean occlusion → expect near-exact; investigate any texel diff > 1 lsb). *Exit: seam gate green on the VK bake.*

**V3 — Horizon.** Port `tileset_bake_horizon.comp` to `shaders_vk/` (or CPU per Q3); full six-channel v2 `.gtex`. **Validation:** horizon channels vs GL reference (tolerance ±1 lsb — pure image-space math, expect exact); horizon shadows render via the consumer's existing path. *Exit: byte-complete v2 atlas from Vulkan.*

**V4 — Wire into the deferred phase + version bump.** `Config::vk_tileset_bake` + `matter_engine.cpp` binding; `run_tileset_deferred` branch collapse (bake-capable / load-only); `kEngineBakeVersion` 2 → 3; progress events verified; `connect()` sync path exercised. **Validation:** cold-cache Vulkan viewer load of a ForestFloor world bakes, saves, loads, renders; second launch is a cache hit (no bake span on the Timeline); non-RT / headless build degrades to load-only with diagnostic. *Exit: the Vulkan-only viewer is self-sufficient for `.gtex`.*

**V5 — Delete the GL bake.** Remove the four bake TU pairs + `tileset_gl_ctx` + the three `shaders_gpu/*.comp` + embed-list entries; collapse the `MATTER_VULKAN_ONLY` gate and `Config::gl_available`; slim `assemble_torus_bvh` (Q2); retire/migrate `run-tilesetgpu`/`run-tilesetseam` per Q4; prune `tests/Makefile` shared-source lists (`GPU_PIPELINE_CPP`, `VIEWER_LOGIC_CPP`) so surviving targets link clean; grep-gate that no reference to the deleted symbols survives (`tools/grep_gate.sh` precedent). **Validation:** full `build-all.sh` + headless test suites green; Windows viewer `make windows` green; Linux GL viewer still builds (load-only fallout documented). *Exit: `.gtex` bake is Vulkan-only; GL software-BVH bake is gone.*

### II.2 Touched files

| file | milestone | change |
|---|---|---|
| `MatterEngine3/src/render/tileset_bake_vk.{h,cpp}` *(new)* | V1–V3 | orchestrator: cache check, geometry lift, AS build, three dispatches, readback, `save_gtex`, PNG dump |
| `MatterEngine3/shaders_vk/tileset_bake_primary.comp` *(new)* | V1 | ray-query primary pass |
| `MatterEngine3/shaders_vk/tileset_bake_ao.comp` *(new)* | V2 | ray-query AO pass, ported RNG/seeding |
| `MatterEngine3/shaders_vk/tileset_bake_horizon.comp` *(new, or CPU in bake_vk.cpp)* | V3 | horizon scan port |
| `MatterEngine3/Makefile` (`VK_SPV:222`, `SHADER_LOGICAL:202`), `MatterViewer/Makefile` (`VK_SPV:420`, `WIN_ME3_CPP:128`) | V1, V5 | add bake SPIR-V; add `tileset_bake_vk.cpp` to Windows viewer; later remove GL bake entries |
| `MatterEngine3/src/render/vk_scene_renderer.{h,cpp}` | V1 | `bake_tileset(...)` entry (device/queue/`submit_immediate` access for the bake; per Q1 possibly a thin passthrough to a free function) |
| `MatterEngine3/src/provider/local_provider.{h,cpp}` | V4, V5 | `Config::vk_tileset_bake`; `run_tileset_deferred` branch collapse (`:805-959`); drop `gl_available` |
| `MatterEngine3/src/matter_engine.cpp` | V4, V5 | bind `vk_tileset_bake` (`~:1066`, `~:3455`); remove `gl_available` sites (`:1088`, `:3465`) |
| `MatterEngine3/src/tileset_gtex.h` | V4 | `kEngineBakeVersion` 2 → 3 (`:39`) |
| `MatterEngine3/src/tileset_torus_bvh.{h,cpp}` | V5 | strip CPU-BVH build tail; expose assembly output (per Q2) |
| `MatterEngine3/src/tileset_bake_gpu.{h,cpp}`, `tileset_bake_primary/ao/horizon.{h,cpp}`, `src/render/tileset_gl_ctx.{h,cpp}`, `shaders_gpu/tileset_bake_*.comp` | V5 | **deleted** |
| `MatterEngine3/tests/Makefile` (`:721-725,771,796-804,1540-1554`) + `tileset_gpu_tests.cpp`/`tileset_seam_tests.cpp` | V2, V5 | seam/atlas assertions re-hosted on a VK harness; GL targets retired; shared lists pruned |
| `MatterEngine3/tests/` *(new VK bake test)* | V1–V3 | reference-compare + determinism + seam gate |

### II.3 Open questions (for the user)

1. **Validation reference strategy.** V1–V3 compare against a GL-baked reference `.gtex`. Preferred source: bake references *now* (pre-deletion, on WSLg `GALLIUM_DRIVER=d3d12` via `run-tilesetgpu` fixtures) and commit them as test fixtures? Or keep a GL-capable branch alive for on-demand reference generation until V5? Committed fixtures are simpler and survive the deletion; they bloat the repo by a few MB per tileset.
2. **`assemble_torus_bvh` refactor depth.** V1 consumes `BLASManager::get_entries()` as-is (CPU BVH built and ignored — wasted seconds, zero risk). V5 options: (a) split out a `assemble_torus_geometry()` returning plain vertex/index/material/instance arrays and delete the BLAS/TLASManager dependency from the tileset path entirely; (b) keep the managers as containers and only skip `tlas.build()`. (a) is cleaner and also unblocks ever sharing scene-resident part BLASes; (b) is smaller. Recommend (a); confirm.
3. **Horizon on compute vs CPU.** Spec defaults to the compute port (one idiom, GPU-fast). The CPU version is trivially deterministic and removes one shader; it also gives tests a free cross-check. Either is a day of work — preference?
4. **Fate of the GPU test targets / CI reality.** Do WSLg's Vulkan drivers (lavapipe/dozen) expose usable `rayQuery` for a headless CI run of the migrated bake tests, or do the VK bake gates become Windows-manual (like the viewer shot suites) with only `load_gtex`-level checks in headless CI? Needs a 30-minute empirical probe before V2 test hosting is decided.
5. **Cross-device cache authority.** With HW RT, atlases baked on different GPUs/drivers may differ bitwise while sharing a content hash. For a single-user local cache this is invisible; if `.gtex` artifacts are ever shipped or shared, the hash should probably fold a device/driver identifier or the bake must be declared "authoring-machine output". Accept "local cache only" for now?
6. **Frozen GL viewer fallout.** After the version bump the Linux GL viewer can no longer bake *or* cache-hit old atlases; it can load Vulkan-baked caches (format unchanged). Accept untextured-ground-by-default on the frozen path, or is pointing it at a Vulkan-populated cache directory part of anyone's workflow?
