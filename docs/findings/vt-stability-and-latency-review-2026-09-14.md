# VT stability and latency: foundation for layered surfaces

Date: 2026-09-14. Scope: terrain and buildings in the current working tree.

Implementation direction: [VT design](../superpowers/specs/2026-09-14-vt-reliability-and-throughput-design.md)
and [implementation plan](../superpowers/plans/2026-09-14-vt-reliability-and-throughput.md).
This audit remains source evidence; the plans do not establish implementation acceptance.

## Decision

**Make VT stability and latency the first milestone, before adding richer material layering.** The proposed splats would be material inputs composited into cached surface pages. That only scales well if unchanged pages stay valid, local changes update locally, and the renderer retains usable content while work is pending.

The current implementation has useful foundations: GPU page composition, compressed physical pages, pinned coarse fallback pages, feedback deduplication, eviction protection, deferred resource reuse, and activation gated on a successful coarse-page fill. Preserve these. The investigation found gaps in invalidation scope, mandatory-request retention, and preparation/scheduling that deserve attention before increasing composition cost.

This is a **source audit**, not a new native reproduction of the reported flashing. It identifies mechanisms and failure conditions; it does not establish how frequently they occur in the user's session. No renderer code was changed. Source hashes are recorded in [vt-source-provenance.json](../agent/evidence/2026-09-14-texturing-review/vt-source-provenance.json).

The earlier [texturing review](texturing-system-review-2026-09-14.md) measured CPU preparation of source GTEX textures. Those 0.335–1.72 second median times exclude live VT page filling and GPU upload. Improving that cache helps source loading, but does not by itself fix page churn during camera movement.

## 1. Local or limited input changes can reset the entire page cache

In `VkSceneRenderer::push_vt_compositor_inputs`, a dirty compositor-input push waits for the device to become idle, binds the tilesets/material table, and, after the initial push, calls `invalidate_all_content`. See [vk_scene_renderer.cpp](../../MatterEngine3/src/render/vk_scene_renderer.cpp), lines 6796–6883.

Accepted material-table updates mark these inputs dirty. The no-op test considers both revisions and record bytes: even byte-identical records with a changed accepted revision can reach this path. Tileset loads/unloads also mark inputs dirty. This does **not** mean an unchanged update invalidates every frame; proving repeated pushes during streaming requires a capture. See `update_materials`, same file, line 10885.

There is a more direct scope mismatch in the surface-edit path. `update_vt_part_surface` updates the affected part's registered rungs, but `end_vt_surface_update` then invalidates content for **all** registered variants. A successfully applied edit to one part can therefore discard unrelated detail pages. The edit also drops that part's compositor and AO-enricher mesh caches. See the same file, lines 7252–7319.

`VtResidency::invalidate_all_content` immediately unmaps and releases every non-pinned resident page and queues all pinned tails for refilling. Previously activated tails retain their mapping and activation serial, so existing surfaces can fall back to old coarse content while replacements arrive. The reset does not preserve their old fine pages. See [vt_residency.cpp](../../MatterEngine3/src/render/vt_residency.cpp), line 1234.

**Consequence:** a real input change can make a large area lose sharpness and then reconstruct progressively. This is a plausible explanation for widespread visible refreshes, although black flashes, lighting flicker, and geometry replacement need separate attribution.

**Change:** track dependencies from material/tileset/surface revisions to affected variants, then to affected pages where spatial bounds are available. Appending an unused material should invalidate zero existing pages. A moved splat should update pages touching its old or new bounds, including affected borders and coarse mips. Keep current pages readable while replacements are built in spare slots; switch mappings only after replacement work is ordered safely. Bound replacement memory and retain coarse coverage under pressure.

## 2. The shared queue cap can discard mandatory coarse-page work

The renderer waits for a new variant's pinned coarse page, or *tail*, to be filled before routing it through VT. Registration queues this fill with `force=true` and a preassigned slot because the tail already has an indirection mapping. Ordinary feedback requests fast-return for an already mapped page. See [vt_residency.cpp](../../MatterEngine3/src/render/vt_residency.cpp), lines 1120–1139 and 1461–1501, and the activation rule in [vt_residency.h](../../MatterEngine3/src/render/vt_residency.h), line 130.

`record_frame` correctly uses separate tail and detail-fill budgets, but afterward it truncates the **combined** pending queue to `max_queue_`. That truncation does not preserve tail requests. Its rationale—visible pages return through feedback—applies to ordinary missing detail pages, but not to the forced, preassigned tail work. See [vt_residency.cpp](../../MatterEngine3/src/render/vt_residency.cpp), lines 2085–2205.

For example, a global invalidation of 300 live variants queues 300 forced tails. With the default 16 tail fills per frame and queue cap of 256, a frame with successful fills takes 16, retains 256, and discards 28. Discarded old-tail refreshes can remain stale until another explicit refresh. If discarded work belongs to a newly registered variant, its never-filled tail can leave that variant VT-inactive. This is a source-derived scenario, not a measured occurrence in the current scene. Failed dispatched tails do have a retry path; truncation is a separate gap.

**Change:** keep mandatory initial/refresh tails in a protected queue, coalesced by live variant identity. Apply the expendable-request cap only to feedback work. Preserve retry state and remove mandatory work only on success or explicit owner cancellation. Test overflow, sustained detail demand, failed fills, release/reuse, and eventual activation through the production scheduler. Existing header-only activation tests do not exercise this queue truncation.

## 3. Current budgets permit seconds of refinement

Defaults in [vt_budgets.h](../../MatterEngine3/include/matter/vt_budgets.h), line 49:

| Resource/work | Default | Meaning |
|---|---:|---|
| Detail fills | 8/frame | Limits visible sharpening throughput |
| Tail fills | 16/frame | Limits initial VT readiness and coarse refresh |
| AO enrichments | 2/frame | Adds a later page-content update |
| Pending queue cap | 256 | Currently shared with mandatory tails |
| Registration requests | 16/frame | Limits how much demand is surfaced |
| Registration service budget | 4 ms | Stops starting further jobs after the budget; a single job can exceed it |
| Variant linger | 240 frames | Retains unwanted variants for a bounded interval |
| Page eviction protection | 16 frames | Protects recently requested coverage |
| Physical page pool | 4096 MiB | Takes precedence over the legacy `pool_pages=8192` setting |

These are defaults, not verified effective settings in the user's running editor. The registration service checks elapsed time between jobs and always permits at least one: [matter_engine.cpp](../../MatterEngine3/src/matter_engine.cpp), lines 6309–6342. It is not a hard upper bound on a frame's registration cost.

As an illustrative lower bound, 2,000 detail-page fills at eight per frame require 250 frames: **4.17 seconds at 60 FPS**, excluding feedback delay, failed fills, and other stalls. A queue can therefore drain slowly even when individual page shaders are reasonably fast.

**Change:** first eliminate unnecessary work, then measure useful completed pages per CPU/GPU millisecond. Use measured time and bounded work units to control registration, preparation, fills, and enrichment. Increasing only the fill count can exchange slow refinement for frame hitches, especially when a fill triggers cold mesh preparation. The default pool is already substantial; inspect actual pool occupancy and total GPU memory pressure before increasing it.

## 4. Page residency and preparation-cache residency are different

The compositor builds and caches per-variant/rung triangle streams and GPU buffers. Its cache has a 512-entry limit, independent of the page-pool capacity and the residency layer's mesh-copy budget. A resident or newly demanded surface can therefore require expensive preparation after its compositor entry has been evicted. See `kMaxMeshEntries` and `Impl::mesh_entry` in [vt_compositor.cpp](../../MatterEngine3/src/render/vt_compositor.cpp), lines 120 and 996.

Releasing a VT rung schedules a part-wide compositor/enricher invalidation. Those caches invalidate all entries for the part, potentially including another rung that remains useful. Surface edits similarly rebuild geometry-associated cache entries even when geometry itself did not change. See `evict_vt_rung` in [vk_scene_renderer.cpp](../../MatterEngine3/src/render/vk_scene_renderer.cpp), line 7026, and `VtCompositor::invalidate_part` in [vt_compositor.cpp](../../MatterEngine3/src/render/vt_compositor.cpp), line 1384.

**Change:** split reusable geometry preparation from changing material inputs. Prefer byte-aware cache policy informed by reuse and preparation cost; do not simply expand the entry limit. Prepare CPU data outside the critical render-thread path and amortize GPU allocations/uploads. Track cache hits, misses, rebuild reasons, bytes, and cold-preparation latency separately from page shader cost.

Texture parameterization should also remain stable across geometry LOD changes whenever the surface permits it. Parameterization aliases already exist, and `MATTER_VT_UNIFY=1` enables the experimental bake-side sharing path; the default is off. Verify coverage, seams, and fallback rates before making that path the standard. A camera crossing a geometry LOD threshold should not automatically give an unchanged material a new cache identity.

## 5. Measure the refresh the player actually sees

Feedback already has run-length reduction and sort/unique deduplication. Its readback is approximately two to three frames old. Priority is based on how much finer the requested page is than resident coverage; the deduplication does not preserve pixel-hit counts for screen-coverage weighting. See [vt_residency.cpp](../../MatterEngine3/src/render/vt_residency.cpp), lines 1461 and 1673–1772.

Add request-age and visible-coverage measures before tuning prioritization. Reserve service for old visible requests so high-priority new demand cannot continually postpone them. Repeated feedback for a resident page is normal bookkeeping; a repeated fill for unchanged resident content needs an explained eviction or invalidation reason.

Existing useful instrumentation includes `vt.input_pushes`, `vt.wait_idle`, `vt.pages_invalidated`, `vt.queue_depth`, `vt.requests_dropped`, `vt.fill_batch`, `vt.mesh_alloc`, and `vt.mesh_cache_wipes`. Residency statistics also count invalidations, dropped pages, and failed fills. Record these with material/tileset revisions, variant/rung changes, and actual effective budgets.

CPU scopes such as `vt.fill` measure CPU work and command recording; they do not establish GPU execution time. Capture GPU timestamps separately. Add first-request-to-visible latency, mandatory-tail backlog, oldest pending age, coarse-fallback screen coverage, and reason-coded invalidation/eviction events.

Run the same camera sequence in StreamMountain and CastleUpgraded:

1. Load and hold a fixed camera until source loading, page refinement, and optional enrichment finish.
2. Turn away and back while the working set fits the configured budgets.
3. Move repeatedly across an LOD boundary, then fly into new terrain/buildings and return.
4. Edit one local surface; separately change an unused material and a widely used material.
5. Repeat under deliberately constrained cache budgets and a burst exceeding the mandatory-tail queue capacity.

Capture stable-lighting material/debug views alongside final lighting. Correlate geometry/sector publication, lighting-history resets, and page changes. The historical [August 9 investigation](../vt-mesh-entry-allocation-2026-08-09.md) initially studied VT but later traced the worst flight freezes to sector eviction. Its timing numbers are historical, and its documented fixes must not be reported as current regressions.

## Acceptance and implementation order

1. **Establish the native baseline and fix mandatory-request loss.** No undefined page becomes visible; every live pending initial tail eventually activates when resources and fill success permit it.
2. **Make invalidation local and replacement continuous.** A local edit leaves unrelated pages resident. Old valid content remains usable until affected replacement pages are ready. Ordinary edits/streaming should not require a device-wide idle wait; retire replaced resources using the existing frame/fence discipline.
3. **Preserve prepared work and LOD identity.** A settled, unchanged view performs no repeated fills or enrichment. Looking away and back within the retained working set does not reconstruct its surfaces.
4. **Tune throughput against frame time and request latency.** Measure CPU/GPU p50/p95/p99 and first-visible-page latency on the target machine. As an initial interaction goal, aim for visible local edits/refinement to begin within roughly 100 ms under the reference workload; this is a proposed target, not a measured capability or a promise that an entire newly exposed world resolves in 100 ms. Establish explicit CPU/GPU budgets from the target frame time.
5. **Add prepared source-texture caching, then cached material splats and blending.** Measure source preparation separately. Layer composition should run when a relevant page needs new content, with a bounded set of spatially overlapping layers. Distant surfaces read filtered composed pages. Near POM must consume a consistent composed height field; the current VT page format needs extension for that contract.

The architectural goal is simple: **unchanged surfaces reuse work, changed surfaces update locally, and missing detail refines over valid coverage.** That is the foundation for both faster texturing and convincing layered materials.
