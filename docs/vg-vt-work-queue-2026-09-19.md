# VG+VT work queue — 2026-09-19

Branch: `vg-vt-improvements` (13 commits grouping the 2026-09-13..19 push). Companion findings: `docs/findings/vg-vt-performance-architecture-2026-09-19.md`. Source review reports live in the reviewer's session scratchpad; every item below carries its own `file:line`.

Ordering rule: Tier 0 first because it breaks default paths. Tier 1 next because it is where the frame time and load time actually are, and because several Tier 2 items become moot once Tier 1 lands. Tier 2 defects by subsystem. Tier 3 gating, docs, hygiene. Within a tier, top to bottom.

## Tier 0 — correctness blockers on default paths (each < 1 day)

| # | Item | Where | Why first |
|---|---|---|---|
| 0.1 | Clear `input_update_pending_` on every early return of `push_vt_compositor_inputs()`, or bound retries and transition to `vt_unavailable_` | `vk_scene_renderer.cpp:6939, 6952` | One setter failure freezes VT refinement for the session with only a log line. |
| 0.2 | Make `project_layout::scene_scripts()` / `object_roots()` report duplicates instead of throwing, or guard the four call sites | `project_layout.h:39,79`; `ui.cpp:109`; `asset_browser.cpp:271`; `main.cpp:5877`; `local_provider.h:388` | Duplicate scene stem is `std::terminate` at editor launch. |
| 0.3 | Restore the deadline check and `pending_.empty()` guard in `CommandQueue::pop_wait`; clear `idle_wake_` on every exit | `async_bake.cpp:243-260` | Bake worker can hot-spin holding its mutex. |
| 0.4 | Replace `queued_keys_.at()` with `find()` + guard | `vt_residency.cpp:2314, 3241` | Unhandled `std::out_of_range` inside `record_frame`. |
| 0.5 | Add the missing `retire_slot_*` calls to the tail-eviction path of `register_variant_impl` | `vt_residency.cpp:1708-1718` | Stale content revision binds material mappings and queues AO for never-written pages. |
| 0.6 | Guard zero-size allocations in `advance_mesh_entry` (geometry-reuse path skips the only guard) | `vt_compositor.cpp:1303, 1337-1339, 1352` | VUID-VkBufferCreateInfo-size-00912. |
| 0.7 | Log and fall back when terrain page compilation fails after the ladder was collapsed; better, decide the ladder after compilation | `part_store.cpp:1578-1600` | Sector silently ships one full-detail rung at every distance. |
| 0.8 | Guard failure callbacks and worker-lane bodies in `AsyncStagePipeline`; make hierarchy/scene worker errors degrade instead of aborting `update()` | `async_stage_pipeline.h:68-70, 88-90`; `geometry_world_runtime.cpp:390-393, 659-661` | Exceptions terminate the process or fail rendering. |
| 0.9 | Release the old renderer part when `d.gpu_pages[page]` is overwritten; stop eviction erasing `d.locations`, or clear `Failed` on re-request | `geometry_world_runtime.cpp:628, 840`; `geometry_residency.cpp:295, 367` | Renderer-part leak and permanent page poisoning under memory pressure. |
| 0.10 | Fix `readback_timings()` query count on shadow-only sparse snapshots | `vk_sparse_voxel.cpp:1357-1364` | Out-of-range query read. |
| 0.11 | Bounds-check `bank[p.source]` in `part_surface::prepare`; capture `Prepared` lifetime in `bind()` | `part_surface.cpp:93, 139, 259-263` | OOB and null deref; dangling tape pointer. |
| 0.12 | Add the missing Make shader dependency edges for `vt_parallax.glsl`, `vt_surface_walk.glsl`, `vt_seed_walk.glsl` | `MatterEngine3/Makefile` | Contradicts CLAUDE.md's stale-SPIR-V guarantee on the rollback path. |
| 0.13 | Delete `projects/world_demo/scenes/geometry/DenseTerrainDiagnostic/` (blocks its own tool); fix or rewrite `mountain_terrain_only_tests.mjs`; fix `castle_part_profile.py:63` to `rglob` | | Test regression and a broken tool. |

## Tier 1 — performance architecture (the goal that was not met)

Read the findings first: `docs/findings/vg-vt-performance-architecture-2026-09-19.md` for the analysis, `docs/findings/streammountain-frame-attribution-2026-09-27.md` ("attribution" below) for the measurements. Re-ranked 2026-09-28 (plan Task 23) from the attribution session, which measured StreamMountain as it now ships, with the 2026-09-19 geometry stress profile: GPU frame median 228–262 ms after 45 s of warmup and 417–694 ms after 300 s, render-thread `loop_render_ms` 30–59 ms. The 2026-09-19 figures (41 ms CPU p50 / 74 ms GPU p95; VT 0.8 / 0.25 ms) predate that profile, so the two are not a regression comparison (attribution §0, §5).

Done rows come first. Open rows follow in the order of attribution §6; 1.10 and 1.11 stay last as small independent work. Each Impact cell gives the measured number, or says why the estimate is not confirmed. The Measured column cites attribution sections. Rows 1.12–1.15 are new: the measurements found them.

| # | Item | Impact | Measured | Effort | Gate |
|---|---|---|---|---|---|
| 1.1 | **Done: instrument the whole frame.** `528636f6` (Task 15: ProfileLib zones and scanned-instance counters for the five traversals), `78a5a994` (Task 16: p99/max per GPU zone, `frame_times_ms`, `tools/frame_attribution.py`), `a67034ab` (Task 17: capture script and findings). `35186eaa` added the per-frame `descriptors_written` counter. Not shipped: the `gbuffer` split (now 1.12), heap-allocation and memcpy-byte counters, and one frame serial across ProfileLib, `paging.json` and VT stats. | Prerequisite; produced every number below | §1–§4 | S–M | none |
| 1.2 | **Done: cook writer (P0-3).** `03536326` (Task 18): one persistent writer `BlobStore` in `RootCache`, batched index/ref commits, assets returned from memory before commit. It also removed the per-call 16 MiB `RootCache` temporaries (`load_asset`, `cache_asset`). Error measurement (1.15) dominated the cook before this change and still does. | Same 140 assets: recorded write/commit subtotal 570.3 → 464.2 worker-s (−18.6 %); 120 commits for 140 assets. No end-to-end load speedup shown; median compile 61.5 s before and after. | §4.2, "Cook after batched commits" | M | none |
| 1.3 | **POM march in physical page space (P0-1).** Resolve the VT address once per page crossing; hoist the envelope/snapshot/mapping validation out of the 128+12 step loop. Prerequisite for turning POM back on in the world props. | POM on vs off at 300 s: +221.0 ms `gbuffer`, +56.4 ms `rt_gi`, +277.0 ms `total` (medians). At 45 s: +23.1 ms `gbuffer` (1.6× the 14.4 ms run spread) and +4.1 ms `rt_gi`; `chart_only` +16.4 ms, within noise. The 10–100 ms estimate is confirmed, and exceeded once the stream fills. | §2.1, §2.2, §5 | M | none: the §2.2 on/off pair is the before number; re-run it after |
| 1.12 | **New: split `gbuffer` with POM off.** Split it into geometry, VT sampling and shading (the 1.1 split Tasks 15–17 did not ship), and take the occupancy/spill profile (measure-first #3). The split decides between a geometry fix (1.5, 1.7) and a shading fix (1.9). | POM-off `gbuffer` median 145.5 ms at 45 s and 302.2 ms at 300 s, the largest GPU zone. The Windows `GPU Engine` counter reads 99.9 % busy while `nvidia-smi` reads 128.8 W of 450 W, which suggests latency-bound shaders (an inference, not an occupancy measurement). | §1, §2.1, §2.2, §6 item 2 | S–M | none |
| 1.7 | **One canonical triangle residency (P1-2), plus a bounded static-buffer policy.** Six resident owners of every triangle → one, with the others referencing it; reserve `vertex_staging_` as the stopgap; stop a static-capacity overflow from forcing a full O(world) rewrite on the render thread. After the session, `18d8a4b5` capped the paged-path terrain static fallback at 4,096 triangles per sector; not re-measured. | Render-thread stalls of 2.29–2.73 s in `pf.static` and 2.28–2.49 s in `publish.vulkan`, about two per 20 s while the stream fills (7 of the 8 frames over 1 s). Three `STATIC CAPACITY OVERFLOW` rewrites per launch, the last at 1.50 GB of vertices and 269 MB of indices. Paged profile: `VK_ERROR_OUT_OF_DEVICE_MEMORY` at 2.58–4.30 GB of static vertices. RSS was not captured. | §3.2, §4.2, §5 | L (M stopgap) | none for the bounded-buffer stopgap; the single-owner layout still waits for 1.5 to settle the runtime's needs |
| 1.13 | **New: terrain assets refused by the root bank.** Assets whose root pages the bank refuses (`geometry root payload budget exceeded`, `geometry_asset.cpp:137-139`; bank sized by `MATTER_GEOMETRY_ROOT_MB`) fail paging and fall back to static geometry. Until they page, the paged path cannot load this world and 1.5 cannot be measured. `18d8a4b5` bounds the fallback (see 1.7) but does not make those assets page. Attribution §4.2 words this as assets that exceed `MATTER_GEOMETRY_ROOT_MB`; the bank is shared across assets, so whether single assets exceed it or it fills across assets is not split. | Cold `prepare`: 243 of 381 cooked terrain assets failed with `root payload budget exceeded` at `MATTER_GEOMETRY_ROOT_MB=1024`; warm: 100 of 227 terrain cache outcomes. Both runs ended in `VK_ERROR_OUT_OF_DEVICE_MEMORY` (4,412 s with 1,538 resident sectors; 1,503 s with 1,089). | §4.2, §6 item 4 | unscoped | none |
| 1.9 | **Tape register pressure and page-fill throughput (P1-4).** Stop zero-filling 96 registers per call; derive the direct-source normal from one evaluation plus analytic or cached gradients instead of five; hoist the duplicated gradient in the parallax loop. | `vt` GPU zone median 0–9.2 ms, p99 158–525 ms in all eight runs; fill frames cost 100–525 ms and are ≥ 1 % of frames in all eight runs, ≥ 5 % in seven. Not confirmed as tape cost: the zone brackets page fills, table uploads and tier-2 AO together. The 4 s refinement latency was not measured. | §2.1, §2.2, §5 | M | occupancy/spill measurement (#3); split the `vt` zone |
| 1.14 | **New: RT GI cost.** No Tier 1 row covered it. Split `rt_gi` before choosing a fix: its `rt_gi_diffuse` and `rt_gi_reflection_transmission` sub-zones recorded no samples in any run. | POM-off `rt_gi` median 50.4 ms at 45 s and 92.7 ms at 300 s. POM adds 56.4 ms more at 300 s (counted under 1.3). | §2.1, §2.2, §6 item 6 | unscoped | a sub-zone split of `rt_gi` |
| 1.15 | **New: cook error measurement (1.2 re-scoped).** `mesh_error::measure` (`verify_ms`, `geometry_compiler.cpp:363-366`) dominates cold-cook compile time; the writer that 1.2 reduced is a 10 % share. | 22,424 of 28,177 compile-plus-write worker-s (79.6 %; 88.5 % of compile alone); median 48.5 s per asset of a 54.7 s median compile. The writer is 2,831 worker-s (10 %). | §4.2, §5, §6 item 7 | unscoped | none |
| 1.5 | **Persistent geometry cut (P0-2).** Keep the cut across frames and apply refine/coarsen deltas; drop the per-node `NodeView` copy, `owners.insert`, `shared_ptr` copy and per-instance `traced.assign`. | Not confirmed: the geometry runtime does not run on the default path (`geometry.*` zones absent from all six default traces), and the paged profile runs out of device memory before steady state. The only sample, `geometry.update` 0.91 ms mean / 1.49 ms p95 during `prepare`, skips runtime page registration. The 24–26 ms figure is neither reproduced nor refuted. | §3.3, §4.2, §5 | L | 1.13; measure-first #5 (was 1.4, which now ranks below it) |
| 1.6 | **VT pool packing (P1-1).** Sub-page chart packing and compact tails; replace the O(pool) `pick_lru` scan. | Not confirmed: VRAM per consumer and eviction counts were not captured. The pool is `25600 page pool (4064 MiB)` and competes with static geometry for the same VRAM; both paged runs died of device-memory exhaustion. | §4.2, §6 item 9 | L | per-consumer VRAM capture; still sequenced before the 1.7 single-owner layout |
| 1.4 | **One shared visibility/detail pass (P0-4).** SoA `{position, radius, part}` → `{visible, rung, distance}` once per frame, consumed by draw, VT demand, RT rung selection, geometry refine and streaming; replace the per-frame TLAS byte hash with a generation counter; make `lod_select` respect the activation radius. Only an `rt.rung_select` early-out is worth doing now. | The six named traversals sum to 2.51–2.94 ms per frame across the six runs (2.66 ms at 45 s and 2.94 ms at 300 s with POM off), under 1 % of the frame, at 159–276 LOD-scanned and 1,633–2,196 RT-scanned instances. `rt.rung_select` is 1.46–1.88 ms of it and `rt.tlas_hash` 0.19–0.23 ms. The 8–15 ms estimate is refuted at this scene's instance count: it assumed about 88 k instances. The ~60 MiB/frame streaming figure was not measured. | §3.1, §5, §6 item 10 | L | none (the 1.1 traversal zones exist) |
| 1.8 | **Compact feedback target (P1-3).** Quarter-res or tile-binned instead of full-res 16 B/pixel; keep the depth-tested pair semantics. | `vt_feedback_readback` 0.11 ms GPU median (p99 ≤ 0.14 ms). `vt.fb_scan` 0.63–0.69 ms CPU mean, max ≤ 2.5 ms: the "up to 10 ms CPU" worst case was not observed. The attachment write (`gbuffer.frag:139`, `uvec4`) happens inside `gbuffer` and is not split out, so its bandwidth share is not confirmed. | §2.1, §3.1, §5, §6 item 11 | M | the 1.12 split, to size the attachment's share of `gbuffer` |
| 1.10 | **Cheap independent wins (P2).** Done: the dirty flag on the 432-sampler RT descriptor write (`35186eaa`, Task 21; `732269b1` also skips unchanged water-field descriptor writes) and the restored `cached()` early-out, now one authored-LOD plan install per hash per session (`ba5f8d82`, Task 19). Open: stop rebuilding `queued_keys_` per frame and the two full `variants_` scans; dirty-set instead of min..max span for page metadata; validate streamed pages once; shared lock or lock-free read in `MaterialMergeGroup`; hoist `getenv` off the decode worker. | Not timed by the attribution session. `35186eaa` cut settled `vt-input-snapshot` frames from 766 to 332 descriptors written. In the 45 s `pom_off` window the VT CPU zones are `draw.vt_requests` 1.98, `vt.enrich` 1.34, `vt.fill_select` 0.95, `vt.residency_begin` 0.80 and `vt.drain_feedback` 0.72 ms mean (nested; do not sum). | §3.1 (VT CPU zones) | S each | none |
| 1.11 | **Bank commit policy.** Done: `prepared_sector::Cache` allocates its 128 MiB bank on first use, and `PartStore` creates the cache only when `MATTER_PREPARED_SECTOR_CACHE` or geometry pages enable it (`bbbfb769`, Task 20); the per-call 16 MiB `RootCache` temporaries went with `load_asset` in `03536326` (Task 18). Open: the 512 MiB encoded-VT bank (`vk_scene_renderer.cpp:6760-6761`, when `MATTER_VT_ENCODED_CACHE` is set), the geometry runtime bank (`geometry_world_runtime.cpp:184`) and the 16 MiB `PartStore` root bank (`geometry_asset.cpp:74`) are still created eagerly with their owners. | Not measured: the attribution session captured no RSS. | — | S–M | none |

Measure-first list (do not fix blind), with its 2026-09-28 status: (1) GPU zone breakdown: done per zone (attribution §2); the `gbuffer` split is 1.12. (2) Hardware-counter profile of the render thread for LLC misses and bandwidth: not taken. (3) Occupancy/register spill for `vt_composite.comp` and `gbuffer.frag`: not taken; gates 1.9 and 1.12. (4) A true cold load: not obtained, because both paged `prepare` runs ran out of device memory (§4.2). (5) The 24–26 ms geometry runtime split: not measurable on this world until 1.13 lands (§3.3). (6) Render-thread allocation counters: not taken; only `descriptors_written` exists (`35186eaa`).

## Tier 2 — remaining defects by subsystem

**Silent-failure paths (log + count every one; needed before any warm/cold measurement is trustworthy)**
- `blas_disk_cache.h:205-209` write failures dropped; `captures` counted at enqueue not durable write (`vk_blas_cache.h:92`).
- `prepared_sector_cache.h:169` stale error on write failure.
- `part_store.cpp:2172` `geometry_pages_for` bypasses all flat ladders, impostors and shared-surface compile when `MATTER_GEOMETRY_MODULE` is unset.
- `part_store.cpp:1363-1364` paged props publish an empty chart entry and are excluded from VT permanently.
- `asset_export.cpp:203-205` portable publish returns false with empty error; `face_material_bake.cpp:33-35` empty diagnostic.
- `local_provider.cpp:1569/1578/1584` "renderer service unavailable" fails headless bake of any world with a `static surface` part.

**VT residency**
- Separate-occlusion enrichment retries forever, allocating a page per frame (`vt_residency.cpp:2455-2461, 2518-2547`).
- Feedback readback buffers and descriptor sets torn down without a retirement horizon on resize (`:2779-2825`).
- `set_slot_geometry` / `retire_slot_*` mutate GPU metadata without dirtying the upload range (`:1117-1157`).
- mip 8 excluded after `kVtMaxMips` = 9 (`vt_export.cpp:87`, `vt_canonical_page.h:24`).
- `vt_chart_resolve.glsl:164` division by zero texels-per-metre.
- Encoded store orphans blobs on ref-update failure (`vt_encoded_store.h:57-77`).

**Compositor / texturing**
- `tris.length()` as the triangle bound on a buffer that also holds seed-BVH nodes (`vt_composite.comp:145, 183`); use `geometry_counts.y`.
- Late `continue`s leak candidate slots (`vt_compositor.cpp:2095, 2115`).
- `gpu_face_material_vk.cpp:87-88` division by zero on an empty tape.
- Unbounded GPU loops over buffer-supplied counts (`vt_composite.comp:653-670`; `finite_surface_stamp.glsl:32-40`).
- `vt_apply_coating` OOB register index (`vt_surface_tape.glsl:359-365`).
- `mesh_error` unbounded memo cache (`mesh_error.cpp:50, 84-87`); fixed-point non-convergence is a hard bake failure (`finite_surface_stamp.cpp:212-217`).
- `terrain_field.cpp:1653-1655` receiver-material code 13 escapes the continuous-height guard.
- `MeshEntry` needs a destructor (`vt_compositor.cpp:1447`).
- Reduce the `material_registry` mutex to mutators and packers; make production callers check the pack return value (`material_registry.c:424-436`; `cell.cpp:151, 164`). Surface as an MSL scope decision.
- The plan's shared layer evaluator (L3) was skipped; `vt_composite.comp::main()` has six evaluation paths. Decide whether to build L3 before splats and contact blending.

**Sparse voxel**
- `cull.comp` pass-0 stats dropped and uninitialised shared atomics (`cull.comp:508-593`).
- `record_shadows()` rewrites a per-slot descriptor set during recording without a documented fence requirement (`vk_sparse_voxel.cpp:1319-1331`).
- Subgroup collectives in divergent flow in `sparse_hierarchy.comp` (`:32, 50, 91, 116, 120, 201-204`); needs a second vendor or `VK_KHR_shader_maximal_reconvergence`.
- `sparse_voxel.frag:155-156` whole-fragment discard on near-plane clip.
- `bake_clusters` O(n²) and `raster_texture` 537 MB at config maxima (`surface_proxy.cpp:170-196, 152-154`).
- `conifer.js:321` raw `p.seed`; `mountain_forest.js:14` undefined `barkMaterial` changes the param hash.
- The voxel bake still resolves no real materials, the gating item for the approach.

**Geometry / streaming**
- `Residency::snapshot()` / `ready()` mutate under `const` (`geometry_residency.cpp:417-442`).
- `attributes()` single-material-group invariant unasserted (`geometry_compiler.cpp:204`).
- Displacement: constant footprint (`surface_displacement.h:84`) and cracks at hard-normal seams (`:69-80`) before any production use.
- `prepared_identity_cache.h:297-313` orphans a page every 64 remembers; never compacts.
- `VkBlasCache` capture backlog unbudgeted (`vk_blas_cache.h:60`).
- `source_by_instance` rebuilt for the new selection while last frame's draws are returned (`geometry_world_runtime.cpp:709, 878-880`).
- `shared_surface_meshes_` never erased (`part_store.h:638`); `align_material_grid` keyed off `terrain_tile` while neighbours use `terrain_charts` (`part_store.cpp:1543`).
- `Node::error` is documented as a conservative bound but populated from a sampled measurement (`geometry_hierarchy.h:20` vs `geometry_compiler.cpp:367`); settle before any acceptance gate uses it.
- Binary-children format constraint is undocumented and forecloses parent reclustering without a version bump.

**Renderer / facade / export**
- GPU traversal overflow drops the frame (`vk_scene_renderer.cpp:7850`); degrade like `words[7]`.
- `MATTER_VT_MAX_VARIANTS` bypassed for promotions with no counter (`:7286`).
- 432 sampled-image descriptors with no capability gate (`vk_scene_renderer.h:2680-2682`); storage floor 6→14 (`:1878`).
- Asset export: GLB payload duplicated and `uint32_t` truncation past 4 GiB (`asset_export.cpp:249-257, 459-468`); manifest omits itself (`:502-509`); `refresh_default_params` empty-path guard (`part_workbench.cpp:300-301`); `.exporting-*` orphan sweep; fsync before publish.
- `set_sparse_voxels` / `set_sparse_hierarchy` skip `note_sparse_snapshot_changed()` (`:2003-2018, 2122-2132`).
- The four removed `wait_idle()` calls read as correct; they need a tileset slot load/unload-in-flight run under the validation layer.

## Tier 3 — test gating, docs, hygiene

**Tests that exist but never run**
- Done (plan Task 22): `add_test` for `static_surface_vt_tests` and `solid_face_projection_gpu_tests` (`cmake/MatterViewer.cmake:285-322`); the latter is the only executor of the GPU face-material bake. It runs from the repo root and passes on `solid_face_projection_gpu_tests: PASS`.
- Done (plan Task 22, `smoke_<mode>` ctests, all pass): mode ctests for `geometry-pages`, `vt-material-domain`, `vt-pom-work`, `vt-receiver-material`, `vt-module-residency`, `vt-queue`, `vt-export`, `vt-surface-connections`, `vt-feedback-pair`, `rt-empty-tlas`, `water-field-nort` (15 of 20 new smoke modes are ungated).
- Supply `MATTER_VT_CLAY_FIXTURE` / `MATTER_VT_PERIODIC_CLAY_FIXTURE` via `ENVIRONMENT` or split those blocks into opt-in tests (`vt_compositor_tests.cpp:1658, 2048`). Left opt-in by plan Task 22: both variables name a directory of eight `clay-projected-<seed>.fst` files at about 2 MB each (16.7 MB total), over the 1 MB check-in limit. `solid_face_projection_gpu_tests` writes them when `MATTER_CLAY_FACE_DUMP=<dir>` is set, so a follow-up can generate them in the build directory (a CTest `FIXTURES_SETUP` on that test) and add an opt-in `vt_compositor_clay_tests` that requires the fixture, with nothing checked in.
- Done (plan Task 22: `vt_density_tests`, `vt_acceptance_tests`, `geometry_paging_report_tests`): register `tools/tests/test_vt_density.py`, `test_vt_acceptance.py`, `test_geometry_paging_report.py` in CTest (pattern at `cmake/MatterPackaging.cmake:119`).
- Port `bank_noalloc_tests` to the Windows toolchain so the zero-allocation gate is measurable on the canonical build.
- Restore or re-cover `test_reload_sees_a_same_size_commit` in `asset_store_tests.cpp`.
- Either add the 23 new suites to `MatterEngine3/tests/Makefile` or state that the Make test path is retired.
- `MatterEditor/tools/smoke_vulkan_faults.ps1` fails on timing, not on a test failure. On 2026-09-28 (tip `35186eaa`, MSVC RelWithDebInfo, RTX 4090) `animation-skin` took 65 s run alone against the gate's 30 s default, and `rt` took 78 s against its 90 s limit (the script's comment says ~35 s). Both print `ALL PASS` and `validation errors: 0`. Raise the two limits or find what doubled the runtime.
- Fix `vt_acceptance.py` (passes on absent evidence, `:169-173`; `REQUIRED` set mismatches `vt_trace.h`) and `vt_churn_report.py` (stdout/stderr correlation); document both.

**Docs**
- Document `MATTER_GBUFFER_POM_PATH` (non-default values are deliberately wrong output) and `MATTER_GEOMETRY_VISIBLE_PROFILE` in `control-surface.md`; add `asset.export` there; update the STATSVT column description.
- `docs/agent/asset-export.md`: 8192 → 16384.
- Done (plan Task 22): `.claude/skills/qa-smoke/SKILL.md`: 12-of-57 framing, the CMake ctest path, MSVC build commands.
- `ROADMAP.md`: add the geometry-paging / sector-bundle workstream.
- CLAUDE.md "JS world-script tests": `--experimental-vm-modules` is required by `castle_dsl_harness.mjs`.
- `projects/world_demo/README.md` endorses copying `objects/templates/WorldSector.js`; six byte-identical 471-line copies exist. Replace with a shared-lib sector module plus per-scene parameters, or at minimum a test asserting the copies stay identical.
- Add a `lod_distance.glsl` shared by `cull.comp`, `sparse_voxel_select.comp`, `sparse_hierarchy.comp`; extend `lod_distance_tests.cpp` for the two new helpers.

**Hygiene**
- Evidence retention: 2.3 GB of PNG/log/jsonl/zip under `docs/agent/evidence/` is untracked on disk; text files under 64 KB were committed. Decide external storage plus ignore rules, or a per-directory budget.
- Normalize the dense one-liner formatting in the new renderer, compositor and part_surface code before it becomes the baseline of a 20k-line file.
- Split `vk_scene_renderer.cpp` (20,482 lines) along the visible seams: sparse binding, geometry cut, export marshaller, tileset source-bank state machine.
- Move the ~3,600 lines of header-only implementation in `src/render/` (`vt_prepare.h` worker thread, `blas_disk_cache.h`, prepared caches, ten `vt_*.h`) into `.cpp` files in the manifests.

## Baseline 2026-09-27

Tip `d006f20e`, MSVC RelWithDebInfo via `./tools/build-windows-from-wsl.sh`. CPU suites ran from `MatterEngine3/tests`, one at a time; smoke modes ran from `MatterEditor/build/cmake/windows-msvc/relwithdebinfo`, one at a time.

- Build `matter_editor`: rc=0.
- Build `all`: rc=1. Four test objects fail with C2027/C2338: `matter_engine_headless_consumer_tests`, `authored_world_provider_cache_tests`, `conifer_lod_provider_tests`, `viewer_logic_tests`. All four instantiate the implicit `viewer::LocalProvider::~LocalProvider`, which deletes `std::unique_ptr<part_surface::SourceCache>` (`MatterEngine3/src/provider/local_provider.h:878`) while that type is only forward-declared (`:96`). `matter_engine_cpu_tests` depends on the first of these targets, so it fails too. The eight targets this plan names each build rc=0.
- `vt_residency_tests`: rc=0, `ALL PASS`.
- `geometry_hierarchy_tests`: rc=0, `ALL PASS`.
- `async_queue_tests`: rc=0, `ALL PASS`.
- `async_stage_pipeline_tests`: rc=0, `ALL PASS: asynchronous overlap, bounded admission, cancellation, failures and shutdown pins`.
- `world_definition_tests`: rc=1, `22 FAILURE(S)`. All 22 come from `test_shared_lib_only_names_shared_objects`: `shared-lib/kreuzenstein.js` names `KreuzensteinBrick` (moved to `objects/` on main in `135ac7cc`, which this branch lacks), `shared-lib/villa_doric_pilot.js` names `VillaDoricColumnPilot`, and `shared-lib/villa_website_kit.js` names 20 `Villa*` modules that each live only in a scene `objects/` folder.
- `partstore_tests`: rc=0, `partstore_tests: ALL PASS`.
- smoke `vt-feedback`: rc=0, `ALL PASS`, `validation errors: 0`.
- smoke `vt-input-snapshot`: rc=0, `ALL PASS`, `validation errors: 0`.
- smoke `vt-direct-source`: rc=0, `ALL PASS`, `validation errors: 0`.
- smoke `vt-surfaces`: rc=0, `ALL PASS`, `validation errors: 0`.
- smoke `sparse-voxel`: rc=0, `ALL PASS`, `validation errors: 0`.
