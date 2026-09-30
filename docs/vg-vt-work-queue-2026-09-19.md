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
| 1.3 | **POM march in physical page space (P0-1).** Resolve the VT address once per page crossing; hoist the envelope/snapshot/mapping validation out of the 128+12 step loop. Prerequisite for turning POM back on in the world props. Since `3bafe049` (clear-ridge task 1) POM is off by default: `TilesetPomSettings::enabled=false`, one flag for terrain and world props. Turning it back on means flipping that default. | POM on vs off at 300 s: +221.0 ms `gbuffer`, +56.4 ms `rt_gi`, +277.0 ms `total` (medians). At 45 s: +23.1 ms `gbuffer` (1.6× the 14.4 ms run spread) and +4.1 ms `rt_gi`; `chart_only` +16.4 ms, within noise. The 10–100 ms estimate is confirmed, and exceeded once the stream fills. | §2.1, §2.2, §5 | M | none: the §2.2 on/off pair is the before number; re-run it after |
| 1.12 | **Done: POM-off G-buffer split and vertex-cache ordering (`clear-ridge.2`, `clear-ridge.7`).** The original differential split found geometry/raster dominant (geometry diagnostic 146/313 ms at 45/300 s). GPU part prebuild now reorders triangles within each existing cluster/rung mesh for vertex reuse; corner order, attributes, draw spans and LOD choices stay unchanged. Optional fenced pipeline queries expose the actual workload. | Matched early captures submit exactly 11,548,963 triangles: vertex invocations fall 19,640,358 → 13,718,366 (−30.2%), fragments +0.09%, G-buffer median/p99 18.75/21.35 → 18.11/20.49 ms. After GPU median/p99/max 36.52/40.53/48.54 ms; intervals >100 ms 1/542, >1 s 0. Final 300 s GPU median/p99/max 60.68/74.79/89.84 ms; G-buffer 30.77 ms versus 32.78 ms before, with ~2% fewer scanned instances; intervals >100 ms 5/325, >1 s 0. Most total-frame improvement versus the early reference is independent VT variation, not this fix. | [Split](findings/streammountain-gbuffer-split-2026-09-28.md); [cache-order findings and verification](findings/streammountain-gbuffer-cache-order-2026-09-29.md) | S–M | Next: attribute triangles per part/rung, then conservative meshlet frustum/backface-cone culling. Vertex reuse alone leaves the microtriangle/raster burden. Separate `vt-normal-frame` readiness failure recorded in findings. |
| 1.7 | **Static-buffer hitch stopgap measured below 100 ms (`clear-ridge.5`, `5c4f2d46`); full single-owner CPU residency remains open.** Growth preserves existing triangles by GPU copy; raster and RT share vertex/index buffers. Dirty triangle transfers now share a 32 MiB per-frame budget. Pending parts stay hidden from raster, RT and page readiness until both streams are resident; eviction cancels queued writes. CPU mirrors and impostor registration remain. | Rebuilt POM-off before → after: GPU median/p99/max 42.31/321.77/428.84 → 37.61/281.39/368.62 ms; intervals >100 ms 55/283 → 49/320, >1 s 0 → 0. Task 1 had 161/172 >100 ms and 6/172 >1 s, but its much larger G-buffer cost makes this an unmatched GPU comparison. Initial growth publication 312.76/96.17/14.07 → 28.29/40.65/10.71 ms. All 75 after upload frames obeyed 32 MiB; max 48.63 ms, none >100 ms, final pending queue empty. Separate part registrations still reach 454.06 ms (impostors 285.11 ms; CPU vertex staging 150.56 ms), so the whole publication path is not bounded. MSVC editor and serial cull/RT/animation-skin smoke checks pass, validation errors 0. | [Static growth findings](findings/streammountain-static-growth-2026-09-29.md); §3.2, §4.2, §5 | L (M stopgap) | Bound CPU part staging and impostor registration separately; full single-owner layout waits for 1.5's runtime needs |
| 1.13 | **CPU terrain root payload streaming implemented (clear-ridge.3); full paged load remains open.** Terrain assets admit manifest/root descriptors without pinning all root payloads in the shared `RootCache` bank; the bounded runtime worker reads roots and published nodes release CPU payloads. The two known `geometry vertex outside bounds` assets were also repaired by validating bounds only on referenced vertices. | Before: cold `prepare` had 243/381 root-payload failures; warm had 100/227, and both ended in device-memory exhaustion (§4.2). The first after run had 207 cold outcomes, zero root-payload failures, and no GPU OOM; its 1 GiB GPU bank saturated before cache misses. The rebuilt-editor retry prepared 350 assets with zero paging failures and all 335 desired visible sectors ready. At a 4 GiB geometry GPU budget, cache-only load admitted up to 369 paged assets, used at most 2.31 GiB of its reservation, and had zero root-bank rejects, admission rejects, reservation stalls, or GPU OOM. Continued background streaming found 154 uncooked manifests, leaving 216 global source fallbacks; neither bounded audit reached idle. The root-bank defect is fixed, while end-to-end paged coverage remains unproven. Four paged POM-off captures show GPU medians/p99/max of 20.59/78.32/128.28 → 58.86/85.09/90.74 ms at 45 s and 161.65/180.79/180.79 → 235.08/272.75/272.75 ms at 300 s (cache-only); no speed improvement is demonstrated because scene/cache populations differ. Rebuilt MSVC geometry, PartStore and Vulkan smoke suites pass. | [Root paging findings](findings/streammountain-root-paging-2026-09-29.md) | unscoped | Finish the background terrain cache, then establish GPU root coverage at a bounded budget and measure drawn geometry by distance before claiming full load |
| 1.9 | **Done: bounded VT fill/AO slices (`clear-ridge.6`).** Retains tape-register removal, one-evaluation cached-height normals and reused gradients; adds resumable private composition/AO factors, independent adaptive 4 ms budgets priced per 32-texel tile, expensive-pair pacing, and exact all-triangle pruning. Partial outputs publish only at completion; superseded work is cancelled/retired. | Final POM-off captures meet VT p99 <16 ms: **1.91 ms (45 s warmup), 8.25 ms (300 s)**; maxima 2.22/8.69 ms. GPU median/p99/max: 38.97/52.61/65.85 ms early, 65.03/74.89/86.29 ms late. Intervals >100 ms: 8/486 early, 1/303 late; >1 s zero. Matched early retry-before: GPU 50.29/251.09/330.16 ms, VT p99 218.23 ms, >100 ms 81/230. Task-1 baseline: early GPU median 227.5–248.3 ms, p99=max 394.1–594.3 ms, >100 ms 161/172, >1 s 6/172; late 402.1–454.2 ms, 433.5–811.4 ms, 138/141 and 1/141. Geometry/residency populations differ, so baseline totals are context. Early fills 1→2; late AO factors 6→7 while fills stay at 9 and tile work continues. Editor/CPU/Vulkan checks pass serially; validation errors zero. | [VT fill findings](findings/streammountain-vt-fill-2026-09-29.md#final-32-texel-slice-captures) | M+ | Longer per-page latency and persistent backlog; time targets are estimates; legacy ORM-in-place AO stays whole-page |
| 1.14 | **GI profiling split implemented; cost reduction blocked (`clear-ridge.9`).** `LIGHTING_DETAIL=1` now enables the child timers and separates diffuse from reflection/transmission even at equal resolutions, preserving ray counts and random streams. Default rendering retains the combined dispatch. | Historical POM-off `rt_gi` medians 50.4/92.7 ms at 45/300 s; no new before/after result. Attribution could not launch while Qwen occupied the shared GPU (16,733–21,687 MiB before test runs). No dominant lane, speedup or visual acceptance established. | [GI attribution availability](findings/streammountain-gi-attribution-2026-09-29.md); §2.1, §2.2, §6 item 6 | unscoped | Obtain an exclusive GPU window; capture `LIGHTING_DETAIL=1 VARIANTS=pom_off` at 45/300 s, then fix the measured dominant lane and verify visual continuity. |
| 1.15 | **New: cook error measurement (1.2 re-scoped).** `mesh_error::measure` (`verify_ms`, `geometry_compiler.cpp:363-366`) dominates cold-cook compile time; the writer that 1.2 reduced is a 10 % share. | 22,424 of 28,177 compile-plus-write worker-s (79.6 %; 88.5 % of compile alone); median 48.5 s per asset of a 54.7 s median compile. The writer is 2,831 worker-s (10 %). | §4.2, §5, §6 item 7 | unscoped | none |
| 1.5 | **Persistent per-instance cut implemented (`clear-ridge.8`, `e51271af`, `a621fe4a`); end-to-end performance remains open.** Retains indexed frontiers, applies complete-group refine/coarsen deltas, replaces `traced.assign` with constant-time membership, and reuses unchanged RT scenes. Immutable asset snapshots keep `NodeView`/owner/resource copies outside the hot cut walk. Partial roots of unrelated assets no longer invalidate the scene; displayed/pending dependencies and first complete coverage still do. | Final paged POM-off early GPU median/p99/max **77.71/103.65/106.31 ms**, intervals >100 ms **90/222**, >1 s zero; late **266.96/294.00/294.00 ms**, >100 ms **63/63**, >1 s zero. Early/late scene reuse **157/222 (70.7%) / 60/63 (95.2%)**. Late geometry update mean/p95 **81.29/108.57 → 29.51/40.13 ms**, assembly mean **38.74 → 3.26 ms**. Loaded/detail populations differ: the raw GPU numbers are higher than the current paged reference, so no overall GPU speedup is established. Task-1 early/late GPU medians were 227.5–248.3 / 402.1–454.2 ms, with 6/172 / 1/141 intervals >1 s; intervening changes and source-versus-paged geometry make that historical comparison unmatched. | [Persistent cut, native checks and all before/after tables](findings/streammountain-persistent-cut-2026-09-29.md) | L | Cut/hierarchy/PartStore/geometry-pages MSVC suites pass serially, validation errors zero. Next: bound changed-scene metadata/instance publication and compare a fixed geometry/detail population; GPU cost and the full frame-time endpoint remain open. |
| 1.6 | **Done: VT and geometry VRAM cap first part (`clear-ridge.4`); pool packing remains open.** Default VT budget is 2,048 MiB (12,800 pages, 2,032 MiB allocated); terrain geometry reservation is capped at 3,072 MiB. Paging pauses offscreen reads above 75% of that cap and optional fine-page reads above 90%, preserving in-view root coverage. Per-consumer allocation and VT LRU-scan logging are in place. The O(pool) `pick_lru` scan had zero calls in the measured capture, so it was not replaced. | Matched POM-off 20 s samples after 300 s warmup: GPU median/p99/max 162.74/206.08/206.08 ms before and 115.01/128.01/128.32 ms after; frame intervals over 100 ms 73/73 and 128/151, over 1 s zero in both. A final-binary 300 s POM-off sample had GPU median/p99/max 256.34/299.85/308.13 ms, 1,166/1,166 intervals over 100 ms, zero over 1 s, and no OOM; it was still loading, so these times do not demonstrate a speedup. A bounded cache-only load passed after 5,591 s: all 816 assets hit cache, all 2,441 sectors loaded, 153 in-view assets and 16,523 roots ready, zero page work or VT queue, geometry peak 2,764.81/3,072 MiB, 83 optional deferrals, zero stalls/evictions/OOM. The 641 offscreen assets kept source fallback. | [VRAM cap findings](findings/streammountain-vram-cap-2026-09-29.md); §4.2, §6 item 9 | L | sub-page chart packing and compact tails remain; continue with the 1.7 single-owner layout |
| 1.4 | **One shared visibility/detail pass (P0-4).** SoA `{position, radius, part}` → `{visible, rung, distance}` once per frame, consumed by draw, VT demand, RT rung selection, geometry refine and streaming; replace the per-frame TLAS byte hash with a generation counter; make `lod_select` respect the activation radius. Only an `rt.rung_select` early-out is worth doing now. | The six named traversals sum to 2.51–2.94 ms per frame across the six runs (2.66 ms at 45 s and 2.94 ms at 300 s with POM off), under 1 % of the frame, at 159–276 LOD-scanned and 1,633–2,196 RT-scanned instances. `rt.rung_select` is 1.46–1.88 ms of it and `rt.tlas_hash` 0.19–0.23 ms. The 8–15 ms estimate is refuted at this scene's instance count: it assumed about 88 k instances. The ~60 MiB/frame streaming figure was not measured. | §3.1, §5, §6 item 10 | L | none (the 1.1 traversal zones exist) |
| 1.8 | **Compact feedback target (P1-3).** Quarter-res or tile-binned instead of full-res 16 B/pixel; keep the depth-tested pair semantics. | `vt_feedback_readback` 0.11 ms GPU median (p99 ≤ 0.14 ms). `vt.fb_scan` 0.63–0.69 ms CPU mean, max ≤ 2.5 ms: the "up to 10 ms CPU" worst case was not observed. The attachment write (`gbuffer.frag:139`, `uvec4`) happens inside `gbuffer` and is not split out, so its bandwidth share is not confirmed. | §2.1, §3.1, §5, §6 item 11 | M | the 1.12 split, to size the attachment's share of `gbuffer` |
| 1.10 | **Cheap independent wins (P2).** Done: the dirty flag on the 432-sampler RT descriptor write (`35186eaa`, Task 21; `732269b1` also skips unchanged water-field descriptor writes) and the restored `cached()` early-out, now one authored-LOD plan install per hash per session (`ba5f8d82`, Task 19). Open: stop rebuilding `queued_keys_` per frame and the two full `variants_` scans; dirty-set instead of min..max span for page metadata; validate streamed pages once; shared lock or lock-free read in `MaterialMergeGroup`; hoist `getenv` off the decode worker. | Not timed by the attribution session. `35186eaa` cut settled `vt-input-snapshot` frames from 766 to 332 descriptors written. In the 45 s `pom_off` window the VT CPU zones are `draw.vt_requests` 1.98, `vt.enrich` 1.34, `vt.fill_select` 0.95, `vt.residency_begin` 0.80 and `vt.drain_feedback` 0.72 ms mean (nested; do not sum). | §3.1 (VT CPU zones) | S each | none |
| 1.11 | **Bank commit policy.** Done: `prepared_sector::Cache` allocates its 128 MiB bank on first use, and `PartStore` creates the cache only when `MATTER_PREPARED_SECTOR_CACHE` or geometry pages enable it (`bbbfb769`, Task 20); the per-call 16 MiB `RootCache` temporaries went with `load_asset` in `03536326` (Task 18). Open: the 512 MiB encoded-VT bank (`vk_scene_renderer.cpp:6760-6761`, when `MATTER_VT_ENCODED_CACHE` is set), the geometry runtime bank (`geometry_world_runtime.cpp:184`) and the 16 MiB `PartStore` root bank (`geometry_asset.cpp:74`) are still created eagerly with their owners. | Not measured: the attribution session captured no RSS. | — | S–M | none |

Measure-first list (do not fix blind), updated through 2026-09-29: (1) GPU zone breakdown: done per zone (attribution §2); differential `gbuffer` probe and submitted/vertex/fragment workload queries in 1.12. (2) Hardware-counter profile of the render thread for LLC misses and bandwidth: not taken. (3) `vt_composite.comp` and `gbuffer.frag`: static register counts and theoretical occupancy ceiling taken in 1.12; no achieved occupancy or reliable spill-byte/traffic counter, so 1.9 still needs a shader profiler before a tape-spill claim. (4) A true cold load: not obtained, because both paged `prepare` runs ran out of device memory (§4.2). (5) Paged geometry runtime now measured by 1.5: early/late update means 7.47/81.29 ms before and 19.17/29.51 ms after; residency/detail populations differ, so a fixed-population split remains necessary. (6) Render-thread allocation counters: not taken; only `descriptors_written` exists (`35186eaa`).

## clear-ridge POM-off baseline — 2026-09-28

The number every clear-ridge task beats. Full tables, hitch attribution and
protocol: `docs/findings/streammountain-pom-off-baseline-2026-09-28.md`. SHA
`3bafe049`, MSVC RelWithDebInfo, RTX 4090, StreamMountain as shipped (POM
off by default, verified by perf.json `pom_enabled=false` with no FIFO `set`).
Capture with `VARIANTS=pom_off RUNS=3 WARMUP=45|300
tools/streammountain_attribution.sh C:/tmp/<dir>`, then compare `hitches.md`.

| | 45 s warmup, 3 runs | 300 s warmup, 3 runs |
|---|---|---|
| GPU total median (per run) | 227.5–248.3 ms | 402.1–454.2 ms |
| GPU total p99 = max (per run) | 394.1–594.3 ms | 433.5–811.4 ms |
| Frame interval median / p99 / max (pooled) | 233.2 / 3807.1 / 3939.9 ms | 409.5 / 922.2 / 1061.8 ms |
| Frames over 100 ms (pooled) | 161 of 172 | 138 of 141 |
| Frames over 1 s (pooled; per run) | 6; 3 / 1 / 2 | 1; 0 / 0 / 1 |
| `gbuffer` / `rt_gi` median | 159.4–170.2 / 54.3–55.8 ms | 307.0–330.0 / 80.3–102.2 ms |
| `vt` p99 | 162.6–354.6 ms | 0.02–371.1 ms |
| Peak whole-GPU VRAM | 12,545–12,688 MiB | 12,927–13,071 MiB |

Five of the six frames over 1 s at 45 s are single render-thread stalls of
2.65–3.36 s, in `pf.static` or `publish.vulkan` (row 1.7). The captures
shared the host with other agent sessions (WSL load average 8.7–16.5). Next
to the attribution's single POM-off runs, the CPU-side numbers are higher:
root bake 198–225 s against 146–149 s, and stalls about 0.5 s longer. So
compare later work with this table, not with the attribution.

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
