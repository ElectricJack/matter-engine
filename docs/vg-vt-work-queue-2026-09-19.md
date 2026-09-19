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

Read the findings doc first. The measured frame is 41 ms CPU p50 / 74 ms GPU p95 on StreamMountain; VT is 0.8 ms and 0.25 ms of that. The items below are ordered by measured or arithmetically grounded impact, with the measurements that must exist before each fix starts.

| # | Item | Impact | Effort | Gate |
|---|---|---|---|---|
| 1.1 | **Instrument the whole frame.** Report all 26 GPU zones for a terrain run; split `gbuffer` into POM / VT-sample / shading; add ProfileLib zones for the five instance traversals; add per-frame counters for heap allocations, bytes memcpy'd, descriptors written; put one frame serial on ProfileLib, `paging.json` and VT stats; capture p95/p99 not medians. | Prerequisite for everything below | S–M | none |
| 1.2 | **Cook complexity (P0-3).** One shared writer `BlobStore` with batched index commit instead of a fresh writer + full 243 MB index sort/rewrite per asset under a global mutex. | order 100 s of cold load | M | none; measure a true cold load first (#4) |
| 1.3 | **POM march in physical page space (P0-1).** Resolve the VT address once per page crossing; hoist the envelope/snapshot/mapping validation out of the 128+12 step loop. | 10–100 ms GPU/frame; lets POM be turned back on | M | 1.1 zone split |
| 1.4 | **One shared visibility/detail pass (P0-4).** SoA `{position, radius, part}` → `{visible, rung, distance}` once per frame, consumed by draw, VT demand, RT rung selection, geometry refine and streaming; replace the per-frame TLAS byte hash with a generation counter; make `lod_select` respect the activation radius. | 8–15 ms CPU/frame, ~60 MiB/frame less streaming; also removes the "different views at different times" source of churn | L | 1.1 traversal zones |
| 1.5 | **Persistent geometry cut (P0-2).** Keep the cut across frames and apply refine/coarsen deltas; drop the per-node `NodeView` copy, `owners.insert`, `shared_ptr` copy and per-instance `traced.assign`. | ~20 ms CPU/frame at steady state | L | 1.4; measure-first #5 (split the 24–26 ms) |
| 1.6 | **VT pool packing (P1-1).** Sub-page chart packing and compact tails; replace the O(pool) `pick_lru` scan. | 1–3 GiB VRAM; ends the 28k-eviction churn | L | none; version before 1.7 |
| 1.7 | **One canonical triangle residency (P1-2).** Six resident owners of every triangle → one, with the others referencing it; reserve `vertex_staging_` as the stopgap. | 1–2 GiB RSS; the 274 ms mid-frame copy | L (M stopgap) | after 1.5 |
| 1.8 | **Compact feedback target (P1-3).** Quarter-res or tile-binned instead of full-res 16 B/pixel; keep the depth-tested pair semantics. | several ms GPU bandwidth, up to 10 ms CPU worst case | M | none |
| 1.9 | **Tape register pressure (P1-4).** Stop zero-filling 96 registers per call; derive the direct-source normal from one evaluation plus analytic or cached gradients instead of five; hoist the duplicated gradient in the parallax loop. | page-fill throughput (4 s refinement latency) | M | occupancy/spill measurement (#3) |
| 1.10 | **Cheap independent wins (P2).** Dirty flag on the 432-sampler RT descriptor write; stop rebuilding `queued_keys_` per frame and the two full `variants_` scans; dirty-set instead of min..max span for page metadata; restore the `cached()` early-out (three QuickJS runtimes per cached node); validate streamed pages once; shared lock or lock-free read in `MaterialMergeGroup`; hoist `getenv` off the decode worker. | each small; together why VT never hit 0.25 ms and part of the warm load | S each | none |
| 1.11 | **Bank commit policy.** Stop eagerly memset-committing ≈720 MiB of banks (512 + 128 + 64 + 16) at startup; make `prepared_sector::Cache` lazy in `PartStore`; remove the 16 MiB `RootCache` temporaries. | ~0.7 GiB RSS and startup time | S–M | none |

Measure-first list (do not fix blind): (1) GPU zone breakdown; (2) hardware-counter profile of the render thread for LLC misses and bandwidth; (3) occupancy/register spill for `vt_composite.comp` and `gbuffer.frag`; (4) a true cold load; (5) the 24–26 ms geometry runtime split; (6) render-thread allocation counters.

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
- `add_test` for `static_surface_vt_tests` and `solid_face_projection_gpu_tests` (`cmake/MatterViewer.cmake:285-322`); the latter is the only executor of the GPU face-material bake.
- Mode ctests for `geometry-pages`, `vt-material-domain`, `vt-pom-work`, `vt-receiver-material`, `vt-module-residency`, `vt-queue`, `vt-export`, `vt-surface-connections`, `vt-feedback-pair`, `rt-empty-tlas`, `water-field-nort` (15 of 20 new smoke modes are ungated).
- Supply `MATTER_VT_CLAY_FIXTURE` / `MATTER_VT_PERIODIC_CLAY_FIXTURE` via `ENVIRONMENT` or split those blocks into opt-in tests (`vt_compositor_tests.cpp:1658, 2022`).
- Register `tools/tests/test_vt_density.py`, `test_vt_acceptance.py`, `test_geometry_paging_report.py` in CTest (pattern at `cmake/MatterPackaging.cmake:119`).
- Port `bank_noalloc_tests` to the Windows toolchain so the zero-allocation gate is measurable on the canonical build.
- Restore or re-cover `test_reload_sees_a_same_size_commit` in `asset_store_tests.cpp`.
- Either add the 23 new suites to `MatterEngine3/tests/Makefile` or state that the Make test path is retired.
- Fix `vt_acceptance.py` (passes on absent evidence, `:169-173`; `REQUIRED` set mismatches `vt_trace.h`) and `vt_churn_report.py` (stdout/stderr correlation); document both.

**Docs**
- Document `MATTER_GBUFFER_POM_PATH` (non-default values are deliberately wrong output) and `MATTER_GEOMETRY_VISIBLE_PROFILE` in `control-surface.md`; add `asset.export` there; update the STATSVT column description.
- `docs/agent/asset-export.md`: 8192 → 16384.
- `.claude/skills/qa-smoke/SKILL.md`: 12-of-57 framing, the CMake ctest path, MSVC build commands.
- `ROADMAP.md`: add the geometry-paging / sector-bundle workstream.
- CLAUDE.md "JS world-script tests": `--experimental-vm-modules` is required by `castle_dsl_harness.mjs`.
- `projects/world_demo/README.md` endorses copying `objects/templates/WorldSector.js`; six byte-identical 471-line copies exist. Replace with a shared-lib sector module plus per-scene parameters, or at minimum a test asserting the copies stay identical.
- Add a `lod_distance.glsl` shared by `cull.comp`, `sparse_voxel_select.comp`, `sparse_hierarchy.comp`; extend `lod_distance_tests.cpp` for the two new helpers.

**Hygiene**
- Evidence retention: 2.3 GB of PNG/log/jsonl/zip under `docs/agent/evidence/` is untracked on disk; text files under 64 KB were committed. Decide external storage plus ignore rules, or a per-directory budget.
- Normalize the dense one-liner formatting in the new renderer, compositor and part_surface code before it becomes the baseline of a 20k-line file.
- Split `vk_scene_renderer.cpp` (20,482 lines) along the visible seams: sparse binding, geometry cut, export marshaller, tileset source-bank state machine.
- Move the ~3,600 lines of header-only implementation in `src/render/` (`vt_prepare.h` worker thread, `blas_disk_cache.h`, prepared caches, ten `vt_*.h`) into `.cpp` files in the manifests.
