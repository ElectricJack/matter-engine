# VG+VT performance architecture review — 2026-09-19

Scope: the virtualized geometry + virtual texturing push of 2026-09-13..19 (branch `vg-vt-improvements`). Question asked: did the target-by-target approach miss big-picture hierarchical optimizations around memory layout, cache misses, allocation churn and data flow? Method: an Opus reviewer read the repo's own measurements and the per-frame code paths; Claude Fable 5.1 verified the headline claims against the code and evidence cited below. Nothing here is a new measurement; every number is from the tree.

## 0. Headline

The optimization effort was aimed at the subsystems that had counters, not at the subsystems that cost the frame.

| Measured | Value | Source |
|---|---|---|
| Whole-renderer GPU p95, StreamMountain | 74.43 ms, later 87.68 ms | `docs/superpowers/plans/2026-09-14-vt-reliability-and-throughput.md` V0/V3 |
| Whole-renderer GPU p95, CastleUpgraded | 55.50 ms | same |
| Dedicated VT GPU p95 | 0.2498 ms terrain / 0.0374 ms castle | same |
| VT CPU p95 (demand + hooks + registration) | 0.8118 ms terrain / 0.3376 ms castle | same |
| Moving-window frame interval | p50 41.79 ms, p95 73.24 ms, max 693.84 ms | `docs/agent/evidence/2026-09-18-moving-stream-profile/README.md` |
| G-buffer, POM off → on (Valley) | 3.19 → 26.08 ms | `docs/agent/evidence/2026-09-17-terrain-framerate/README.md` |
| G-buffer, POM off → on (Cliff) | 7.02 → 133.73 ms | same |
| GI alone, one filed issue | 124.07 ms | same |
| Geometry runtime, steady state, nothing publishing | 24–26 ms/frame | `docs/superpowers/plans/2026-09-18-blas-cache-and-terrain-geometry.md:99` |

VT residency is 0.25 ms of a 74 ms GPU frame and 0.81 ms of a 41 ms CPU frame. The VT reliability plan's unmet target is a fight over 0.56 ms. POM costs 23 to 127 ms and was "resolved" by turning it off. The geometry runtime spends 24–26 ms per frame recomputing a cut that did not change.

## 1. What the evidence can and cannot attribute

Can: `gbuffer` and whole-frame GPU zones; three CPU stage names (`resolve` / `build` / `draw`); one warm cached-load stage table (`docs/agent/evidence/2026-09-19-cached-load-audit/paging.json`).

Cannot:
- No p99 anywhere; settled runs report medians only.
- No GPU zone breakdown of a terrain frame although `gpu_timing_sample.h` defines 26 zones. POM was isolated only by A/B-ing the whole pass.
- No allocation counters except `bank.backing_allocations_peak` for one CPU bank.
- No process RSS or VRAM high-water mark.
- No cache-miss, memory-bandwidth or GPU occupancy data at all.
- No cold load of the current path; the cached-load audit is cache-only by construction.
- Three unrelated telemetry systems (ProfileLib zones, `paging.json`, VT stats) with no shared frame correlation.

ProfileLib measures wall time per named scope per lane (`libs/ProfileLib/include/profile.h:70-180`). It structurally cannot answer the cache-miss or allocation question. That question is unanswerable with current instrumentation.

## 2. There is no shared hierarchy

Per frame, at least five independent CPU traversals of the instance set, three of which re-derive the same LOD rung from the same eye. `vk_scene_renderer.cpp:93` names the three mirrors itself.

| # | Traversal | Site | Input |
|---|---|---|---|
| 1 | Sector LOD selection: scans every instance in every sector for the closest, fresh nested `std::map` per frame | `lod_select.cpp:82-135` via `provider/resolvers.cpp:91` | eye + pixel budget |
| 2 | Resolved-instance emit, `std::map` lookup per instance, second nested map deep-compared | `provider/resolvers.cpp:118-200` | eye + #1 |
| 3 | Instance-cache identity, 88 B memcmp per instance every frame | `render/vk_instance_cache.cpp:70-81` | #2 |
| 4 | VT demand, instances × clusters, own `lod::select_rep` | `vk_scene_renderer.cpp:7394-7426` | eye + pixel budget |
| 5 | BLAS/RT rung selection, instances × clusters, measured 5.8 ms before an early-out | `vk_scene_renderer.cpp:16226-16290` | eye + pixel budget |
| 6 | Geometry cut, ~34 assets/frame, own refine predicate | `geometry/geometry_cut.cpp:13`, `geometry_residency.cpp:289` | node error |
| 7 | Geometry visibility, own frustum planes vs root bounds | `docs/agent/evidence/2026-09-19-view-priority/README.md` | unjittered planes |
| 8 | GPU frustum cull + LOD | `shaders_vk/cull.comp` | GPU rule |
| 9 | Sparse voxel selection | `vk_sparse_voxel.h:133` | matrices + budget |

`lod_distance.h` unified the formula but not the traversal. Five "what is visible at what detail" computations run from four different input sets (camera position; unjittered planes; two jittered cameras; GPU feedback 2–3 frames stale per `vt_residency.cpp:2836`). Page, sector and variant residency decisions are therefore made from different views of the world at different times, which is a plausible mechanism for the terrain-churn symptom that has not been reproduced.

Bandwidth consequence: `GpuInstance` is 176 B (`vk_scene_renderer.h:2426-2442`), 128 B of which is two mat4s; `update_vt_demand` reads all 176 B to use about 24 B. At the documented ~88k instances that is ~14 MiB per traversal, ~73 MiB/frame across five passes, before any map lookup. The AoS layout guarantees roughly 7× the bandwidth the passes need.

Also: `vk_scene_renderer.cpp:17140-17150` byte-hashes the whole TLAS instance mirror (64 B × N ≈ 5.4 MiB at 88k) with serial FNV-1a every frame purely as a change detector. A generation counter replaces it.

## 3. Memory added by this work

| Item | Bytes | Evidence |
|---|---|---|
| VT physical page pool | 4064 MiB (25,600 slots × 166,464 B) | `vt_residency.cpp:525-545`; `vt_budgets.h:88`; 9 B/texel `vt_types.h:58` |
| of which useful | 2.93 % (castle) / 12.76 % (terrain) triangle-centre coverage | `docs/findings/vt-memory-density-2026-09-14.md` |
| of which aux channel | 4 of 9 B/texel uncompressed R8G8B8A8 | `vt_types.h:53,58` |
| VT indirection arena | 128 MiB reserved, 79.10 MiB used | `2026-09-19-terrain-churn/handoff-v7-report.json` |
| VT mesh cache | 308.7 / 3512 MB | same |
| Variant table + CPU mirror | 2 × 2.49 MiB | `vt_residency.cpp:659`, `vt_residency.h:1447` |
| Table staging ring | 8.00 MiB | `vt_residency.cpp:676-687` |
| Feedback attachment | 31.6 MiB at 1080p, full res R32G32B32A32_UINT | `vt_feedback_format.h:8`, `vk_scene_renderer.cpp:18664` |
| CPU raster staging vector | 1.07 GiB live, grown to 1.6 GiB; 274 ms copy in one frame | `2026-09-18-moving-stream-profile/README.md`; `vk_scene_renderer.cpp:13821-13847` |
| Eagerly memset-committed banks | ≈720 MiB before any page is read (512 + 128 + 64 + 16) | `vk_scene_renderer.cpp:6733`, `prepared_sector_cache.h:107`, `geometry_world_runtime.cpp:183`, `part_store.cpp:352`; `mem_bank.c:27-31` |
| TLAS CPU mirror | 64 B × N ≈ 5.4 MiB | `vk_scene_renderer.h:2984` |

Roughly 4.2 GiB GPU + ~2.2 GiB CPU added, against a measured useful VT payload of a few percent.

Six concurrent resident owners of every triangle (traced through `part_store.cpp:1329-1372`): encoded geometry pages in `RootCache` (92 B/tri shading, `geometry_pages.cpp:53`); BLASManager `Tri`+`TriEx` 160 B/tri, copied by `adopt_from` (`blas_manager.hpp:123-138`); `IndexedPartGeometry` SoA (`indexed_part_geometry.h:33-52`); the prepared-sector archive (`part_store.cpp:2096-2116`); `VkScenePart` AoS, a second independent weld (`geometry_raster_adapter.h:32-66`); the VT compositor stream (`GpuTri` 160 B + `GpuTriGeometry` 112 B + a third BVH, `vt_seed_bvh.h`, `vt_prepare.h:106`). Plus three transient full copies at staging peak and a 1024 B/tri scratch charge in `vt_prepare.h:341`. Four memory layouts for one mesh, transposed in full at every boundary.

## 4. Prioritized structural items

### P0 — fix now
- **P0-1 POM marches through the VT indirection per step.** `vt_parallax.glsl:91-109`: every march step does `vt_resolve` + footprint validation + envelope check + a dependent `vt_page_inputs[]` load + the height fetch, for up to 128 + 12 steps (`:178`, `:255`). Three of the five operations are loop-invariant. Evidence: 3.19 → 26.08 ms, 7.02 → 133.73 ms. Fix: resolve once per page crossing and march in physical page space. Impact 10–100 ms/frame. Effort M.
- **P0-2 Geometry runtime recomputes 24–26 ms of unchanged work.** `geometry_cut.cpp:13-47` re-walks each cut from the roots every frame through two `std::function` layers; `Residency::select` (`geometry_residency.cpp:289-305`) does a map find, a heap-allocating `NodeView` copy, an `owners.insert()` and a `shared_ptr` copy per node; `geometry_world_runtime.cpp` rebuilds an `owners` map per feedback frame (`:489-490`), walks `admitted` three times (`:659,855,872`) and reassigns `traced` per instance per frame (`:769`). Fix: persistent cut with incremental refine/coarsen deltas. Impact ~20 ms/frame CPU. Effort L. Depends on P0-4 and on measure-first #5.
- **P0-3 Cooking is quadratic.** `geometry_asset.cpp:130-148`: fresh writer `BlobStore` per asset (parsing the 243 MB / 6.08 M-entry index into an `unordered_map`), then `flush_index()` (`asset_pages.cpp:352`) copies, sorts and rewrites the whole index (`blob_store.cpp:630-657`), all under a global mutex. Cold cooks measured 410 s and 166 s (`2026-09-18-terrain-cache-audit/`). Fix: one shared writer, batched commit. Impact order 100 s of cold load. Effort M.
- **P0-4 One shared visibility/detail pass.** Replace traversals 1–5 with one SoA pass over `{position, radius, part}` producing `{visible, rung, distance}` consumed by draw, VT demand, RT, geometry refine and streaming; replace the TLAS byte hash with a generation counter. Impact ~8–15 ms/frame CPU and ~60 MiB/frame of instance streaming. Effort L. Unblocks P0-2.

### P1
- **P1-1 VT pool: 4 GiB holding 3–13 % useful texels, oversubscribed.** Whole 136×136 slot per ≤64×64 tail (789 pinned terrain tails ≈ 97 MiB); 25,536/25,600 slots occupied with 28,278 capacity evictions and an O(pool) `pick_lru` scan per replacement (`2026-09-19-terrain-churn/README.md`). Fix: sub-page chart packing + compact tails, as the density doc already ranks #1. Impact 1–3 GiB VRAM plus the churn. Effort L.
- **P1-2 Six resident copies of every triangle; 1 GiB CPU vector reallocating mid-frame.** Choose one canonical residency layout and have the others reference it; reserve `vertex_staging_` as the stopgap (`vk_scene_renderer.cpp:13830`). Impact 1–2 GiB RSS and the 274 ms hitch. Effort L (M for the reserve).
- **P1-3 Full-resolution 16 B/pixel feedback attachment.** 31.6 MiB written and cleared per frame, extracted by 510 workgroups, then a 64,800-entry CPU scan measured at 11.1 ms when uncached (`vt_residency.cpp:2579-2589, 2788-2794`); replaced a 0.247 MiB image. Fix: quarter-res or tile-binned target with a compact key. Impact several ms GPU bandwidth + up to 10 ms CPU worst case. Effort M.
- **P1-4 GPU tape evaluation spills 96 floats, five times per texel.** `vt_surface_tape.glsl:77,179` zero-fills 96 registers per call; `vt_composite.comp:246,384,499-521` does 1 sample + 4 height evaluations per texel for a central-difference normal; `vt_parallax.glsl:263-264` evaluates the gradient twice per iteration in a 128-step loop. Impact: page-fill throughput (the 4.17 s refinement latency in the stability review). Effort M; measure occupancy first.

### P2 (small, independent, cheap)
- 432 combined-image samplers rewritten every frame on the RT set with no dirty flag (`vk_scene_renderer.cpp:17331-17384`); the raster path at `:6515` shows the guard. S.
- `std::map` on every per-frame VT path: `queued_keys_` cleared and rebuilt each frame (`vt_residency.cpp:2213-2217`); fresh map + vector per `record_frame` (`:3022-3023`); two full `variants_` scans (`:3395-3396`); 2.49 MiB table memcpy when one record changes (`:3487-3491`); page-metadata upload as one min..max span (`:1303-1327`). Collectively why VT never reached 0.25 ms. M.
- Three QuickJS runtimes per already-cached node per publish loop (`local_provider.cpp:3884-3925`; `script_host.cpp:1120,1167,1417`). On the warm-load path. S.
- Every streamed page validated twice (`geometry_residency.cpp:227`, `geometry_raster_adapter.h:17`), three times on cook (`geometry_pages.cpp:71-72`). S.
- Global exclusive SRW lock per particle in the mesher (`cell.cpp:151,164` → `material_registry.c:292-294`). S.
- `std::getenv` + two string constructions per decoded page on the decode worker (`geometry_world_runtime.cpp:382-386`). S.

### P3 (note only)
`vk_instance_cache.cpp:100-104` clears rather than shrinks; `vt_compositor.cpp:1735-1783` scans before the empty-queue guard; duplicate stats in `record_frame`; the dead 16 MiB `RootCache` temporary in `load_asset`.

## 5. Measure first

1. Whole-frame GPU zone breakdown for StreamMountain terrain, using the 26 zones that already exist. The cheapest missing measurement in the repo; P0-1's estimate is an A/B inference until this exists.
2. Is the CPU memory-bound? Needs an external sampling profiler with hardware counters (VTune, AMD uProf, or WPA) on the render thread for one movement window: LLC-miss rate and bandwidth. The 73 MiB/frame figure is arithmetic, not a measurement.
3. GPU occupancy and register spill for `vt_composite.comp` and `gbuffer.frag` (Nsight/RGP, or the static register count from SPIR-V).
4. A true cold load on the current path. Every load number in the repo is warm.
5. The 24–26 ms geometry-runtime figure split into cut / snapshot / pack / submit, which the plan itself asks for at `2026-09-18-blas-cache-and-terrain-geometry.md:100`.
6. Allocation counters on the render thread before claiming any churn win.

Instrumentation to add: a ProfileLib zone for each of the five traversals (only `cull.vt_demand` is named today); per-frame counters for bytes memcpy'd, heap allocations, map nodes, descriptors written; one frame serial correlating ProfileLib, `paging.json` and VT stats; an opt-in split of `gbuffer` into POM / VT-sample / shading.

## 6. Ordering

```
P0-3 cook complexity ──────────────► independent, start now
measure #1 ──► P0-1 POM march ─────► independent
measure #5 ──► P0-2 persistent cut
P0-4 shared visibility pass ───────► unblocks P0-2, subsumes the P2 early-outs
P1-1 chart packing ────────────────► independent; version before P1-2
P1-2 canonical geometry layout ────► after P0-2 settles the runtime's needs
P1-3, P1-4, P2 ────────────────────► any time
```

Two process changes matter more than any single item: instrument the whole frame before optimizing any part of it (VT got 0.25 ms of a 74 ms frame because VT was the subsystem with counters), and treat "recompute from the eye every frame" as the defect rather than capping each recomputation when it gets slow. `vt_budgets.h:104-118` records that pattern reaching 14.3 ms of a 35.3 ms frame before a cap was added; the same pattern is live in the geometry runtime at 24–26 ms.
