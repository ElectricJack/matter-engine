# Reliable, responsive virtual texturing

Date: 2026-09-14. Status: implementation direction; acceptance pending.

Implementation: [VT implementation plan](../plans/2026-09-14-vt-reliability-and-throughput.md).
Next feature set: [layered surface texturing](2026-09-14-layered-surface-texturing-design.md).
Evidence: [VT audit](../../findings/vt-stability-and-latency-review-2026-09-14.md).

## Priority update — 2026-09-15

The user moved procedural material generation, splats and visual development
into the immediate focus. See the [revised layered-material working objective](../plans/2026-09-14-layered-surface-texturing.md)
and its [generation/splat specification](../specs/2026-09-14-layered-surface-texturing-design.md).
Maintain relevant VT correctness checks, valid fallback, bounded work and
lightweight timing during that work. Full performance acceptance is deferred;
the original targets and incomplete tasks below remain unchanged and open.
References below to materials as a follow-on describe scope separation, not a
requirement to finish every VT performance gate before material development.

## 1. Outcome and scope

Terrain and buildings should retain their textures during ordinary camera movement, refine new detail promptly, and update locally when edited. Unchanged surfaces reuse their work. Previously valid content remains visible until replacement content is ready.

This design hardens the existing chart VT implementation. It retains compressed page pools, coarse fallback pages, feedback, POM, the material system, and the existing resource-retirement discipline. It establishes the cache contract required by layered splats. Source GTEX preparation, new material authoring, composed height/POM, and cross-object blending have their own follow-on plan.

The broader [LOD/VT redesign](../../lod-vt-redesign-2026-08-04.md) remains relevant to representation ownership and stable parameterization. This work implements the VT subset; it does not require completion of the proxy world, a new visibility system, or the concurrent sparse-geometry project.

## 2. Findings that drive the design

The audit identifies global invalidation after compositor-input changes and after individual part-surface updates, a shared queue cap that can drop mandatory tail work, count-based refinement limits, device-idle waits, and a compositor preparation cache independent of page residency. These are source-confirmed mechanisms. Their contribution to the user's actual flashing still needs a native capture.

Existing protections are valuable: tails are pinned, new variants wait for a filled tail, unsuccessful fills are not published, requests are deduplicated, and resources are retired after in-flight use. Extend these invariants rather than introducing a second residency system.

## 3. Identities and ownership

Use the existing parameterization identity as the stable owner of shared pages. A geometry LOD alias can refer to that owner when its chart mapping is compatible. A physical slot or a recycled table index is never a content identity.

The logical page identity contains:

| Component | Purpose |
|---|---|
| Parameterization identity/version | Surface coordinates and chart layout |
| Content dependency digest | Only material, source-texture, surface-tape and field inputs actually consumed |
| Compositor/encoding version | Changes in evaluation, filtering, channel layout, and bake quality |
| Virtual mip and page coordinates | Requested region and footprint |
| Placement context, when required | Future world-anchored layers or per-instance variation |

Camera position, requested geometry rung, unrelated table revisions, and physical allocation addresses do not change a compatible surface's content identity.

Maintain a separate owner generation for cancellation and safe reuse. Each preparation/fill completion carries its owner generation and target content revision. Reject stale completions before changing any mapping or readiness state. Coalesce repeated edits to the latest desired revision, preserving the currently displayed revision until replacement succeeds.

## 4. Protected mandatory work and bounded detail demand

Split scheduling into:

1. **Mandatory coverage:** initial tails and explicitly required tail refreshes. One pending latest revision per live parameterization owner; bounded by admitted owners. Retain until success, supersession, or explicit owner cancellation. Queue overflow must never silently remove this work.
2. **Visible refinement/replacement:** deduplicated pages with oldest-request age, current visibility, footprint/coverage, mip deficit, and estimated cost. Bound expendable feedback entries independently. Keep durable dirty state for required replacements if an execution entry is deferred or evicted from this queue.
3. **Optional enrichment:** low-priority AO work tied to the exact page content revision. It can be delayed or cancelled without blocking usable coverage.

Reserve service for mandatory coverage and aged visible requests. A duplicate request updates visibility and coverage without resetting its original waiting time. Priority must not indefinitely starve a still-visible request. Initial variant admission must account for its tail, metadata, preparation and replacement capacity, with explicit backpressure when resources are unavailable.

Keep the existing feedback deduplication. Retain screen-hit counts only where they improve prioritization enough to justify their cost. Readback latency remains asynchronous; never synchronously read the GPU to learn what to request next.

## 5. Dependency-based invalidation

Represent an edit as a dependency change, optional affected surface bounds, and a reason. Maintain reverse links from source/material/tape dependencies to live parameterization owners. Compare effective compositor inputs, not just a global revision counter. New unused materials invalidate no existing pages.

Start with owner-scoped invalidation, then page-scoped invalidation for changes with reliable spatial bounds. A material used across an entire owner legitimately dirties all its affected pages. A splat move later dirties the union of its old/new bounds, expanded for projection/filter support, gutters, and affected coarse mips.

Keep full-cache reset for explicit world/device reconstruction or an incompatible global format/evaluator change. Ordinary streaming publication, material-table growth, and local surface edits must use the dependency path.

Geometry preparation and material content have separate generations. Changing weights, roughness, tint, or a source texture does not automatically destroy unchanged geometry streams or an AO acceleration structure. Releasing one rung must not invalidate a surviving compatible alias's prepared data.

## 6. Continuous replacement and safe publication

For a resident page, use this sequence:

```mermaid
flowchart LR
    A[Resident revision A] --> B[Dirty: keep A visible]
    B --> C[Prepare revision B in reserved capacity]
    C --> D[Record fill and validate generation]
    D --> E[Publish B after ordering dependencies]
    E --> F[Retire A after its last reader]
    C -->|failure or superseded| B
```

Reserve replacement space within the existing memory budget. Do not clear an old mapping to create that space. Under pressure, evict unneeded refinement with valid parent coverage, postpone lower-priority replacements, or reject additional admission. Keep a bounded replacement reserve so a completely occupied pool cannot deadlock all edits. Count old, candidate, pinned and retiring pages in memory accounting.

Fill success means the producer recorded valid work; publication also requires the GPU ordering needed by its consumers. Use submission/frame serials and existing fences/barriers. Delay slot reuse until all old readers retire. Include raster, RT, feedback, indirection uploads and AO enrichment in the lifetime audit.

Compositor inputs must be immutable snapshots while referenced by submitted work. Keep old material buffers, source textures and descriptors alive until their readers retire. The draw path must not combine an old page with an incompatible new height/detail source. Where current shaders cannot address multiple input snapshots, commit the affected owner's page/binding snapshot together after its required replacement coverage is ready. Page-granular publication is allowed only when the resolved page also identifies the compatible input snapshot. This compatibility work is part of removing ordinary `wait_idle` calls.

The selected page-identity transport is a separate uint32 storage-buffer entry per physical page, preserving indirection packing and all pool formats. It references a bounded immutable draw-input table. Residency currently supports eight table identities, captures one on each fill, and commits it with successful page bytes. Clean unaffected dependencies may be retagged without regeneration; already-dirty pages must retain their old identity through later unrelated edits. Table indices cannot be reused while pages or retiring readers own the previous version. Raster/RT must resolve the displayed page before selecting its compatible material, source descriptors and POM parameters.

The current renderer implements per-frame immutable material/source banks and coalesces desired updates while all eight version indices remain occupied. G-buffer and secondary RT surface lookup use captured materials and source handles. Active version capture includes desired inputs, page ownership and retirement, excluding versions held only by older frame captures to avoid perpetual retention. The live source bank plus eight retained banks requires 432 channel descriptors; material payload is 295,040 bytes per frame, plus 3,536 bytes of source parameters and ownership/descriptor bookkeeping. The original held-page detail regression now passes, as does bounded version pressure. Primary screen-space RT now reads the displayed snapshot from spare bits in the existing visibility attachment; held raw-reflection material and source-height POM regressions pass. Ordinary compositor-input/source-load/source-unload device-idle waits are removed, with native event-observer checks and recorded-but-unsubmitted source replacement/unload coverage. Source preparation still synchronously decodes/BC-encodes and waits for its upload fence. Adapter-limit handling, retained source-image byte bounds and filtered-history compatibility at delayed publication remain open. Two subsequent StreamMountain settled captures pass counter stability but still miss the CPU target; no speedup is established. See [current implementation evidence](../../agent/evidence/2026-09-15-vt-feedback/README.md).

AO enrichment records the content revision it modifies and must not compound repeatedly or land on a recycled/replaced page. Preserve usable base content while enrichment is pending. A steady scene eventually stops generating both base and enrichment work.

## 7. Preparation, scheduling and memory

Cache reusable geometry by its actual geometry/parameterization identity, separately from changing surface inputs. Replace unconditional part-wide cache destruction with precise ownership/refcount rules. Measure retained bytes and rebuild cost; an entry count alone is insufficient for a cache containing differently sized meshes.

Move expensive CPU preparation to existing workers using immutable jobs. Split oversized operations into bounded units and use existing upload/allocation facilities where possible. The render thread admits and publishes prepared work; a nominal time budget checked only between unbounded jobs does not satisfy the frame-time contract.

Use delayed GPU timestamps and CPU measurements to estimate costs for coverage, refinement, replacement and enrichment. Schedule within time and byte budgets, retaining count caps as safety limits. Use bounded adaptation and hysteresis so delayed measurements do not cause budget oscillation. Maintain the effective quality/density settings used by the reference workload; meeting timing by silently reducing quality is not acceptance.

Report physical pages, pinned tails, CPU mesh copies, prepared GPU buffers, upload staging, retirement, and temporary replacements separately. Pool sizing must consider the adapter's available memory alongside all other renderer users. Persistent composed-page storage is a later, measured optimization; it is not required to fix in-session churn.

Track packing efficiency separately from capacity. The native density diagnostic measures atlas coverage, chart blocks, gutter-expanded bounds and triangle-center coverage within occupied slots, with tails reported separately. The initial castle result shows page-rounded chart blocks dominate unused payload area; the approximately 4 GiB pool also has substantial unused reserved capacity in the reference views. See the [native evidence](../../agent/evidence/2026-09-14-vt-01/README.md). These measurements motivate evaluating smaller initial allocations, compact coarse-tail storage, and packing several small charts per page. Preserve filtering gutters, material identity and replacement reserve; a low center-coverage fraction is not a promise that all remaining texels can be removed. Changes to chart packing or tail addressing require explicit layout/version and raster/RT/seam acceptance, rather than an undocumented quality reduction during latency tuning.

## 8. Acceptance contract

Establish a native MSVC baseline before behavioral changes. Use StreamMountain, CastleUpgraded, and a small deterministic VT fixture for exact edit/overflow cases. Record source revision/fingerprints, GPU/driver, CPU, output/internal resolution, effective VT settings, material hashes, camera paths, lighting, validation state, and frame cap. The recent local reference reports an RTX 4090; confirm the actual test adapter instead of assuming it.

### Correctness gates

- Every admitted initial tail eventually activates when its owner remains live, resources are available and fills succeed, including bursts larger than the detail queue cap.
- No uninitialized page is sampled, failed fill is published, stale generation is committed, or live slot is reused before its readers retire.
- A fixed view, after completion of pending work, runs for 600 frames with zero unexplained content invalidations, repeated base fills, or repeated enrichments.
- Returning to a view within its retained working set reuses unchanged pages. Geometry LOD changes reuse compatible parameterization; incompatible mappings retain valid fallback until their own coverage is ready.
- A local surface edit leaves unrelated page mappings/content unchanged. Appending or editing an unused material causes zero existing-page invalidations.
- Normal input updates/streaming cause zero device-wide idle waits attributable to VT. World/device teardown is counted separately.
- Constrained-budget and failure tests retain valid coarse coverage, bounded memory/queues, and progress after pressure subsides.
- Existing terrain and finished-surface POM, chart seams/normal frames, raster and RT material behavior remain valid.

The current dedicated CPU trace now includes demand selection in addition to the three VT hooks and registration. Older captures omit demand and cannot establish the combined CPU target. New settled runs measure 0.8118 ms p95 for terrain and 0.3376 ms for castle across those five stages; both miss the target. Other draw setup and sampling remain separately visible in the broader renderer profile. See [current evidence](../../agent/evidence/2026-09-15-vt-feedback/README.md).

### Initial performance targets

These are implementation targets, not measurements. Freeze the reference workload in Task V0. A miss remains an open result; document any justified target revision explicitly with supporting data.

| Metric | Target on the reference machine |
|---|---|
| Settled VT CPU maintenance, p95 | At most 0.25 ms/frame |
| Active VT render-thread work, p95 / p99 | At most 1 / 2 ms/frame, including registration, preparation publication and allocation stalls |
| Active VT GPU work, p95 | At most 1 ms/frame for page work, feedback-related work and enrichment; report added draw sampling separately |
| First visible replacement, p95 | At most 100 ms for a local edit affecting at most 16 resident detail pages with sources/preparation warm |
| Completion of that bounded edit, p95 | At most 250 ms, excluding optional enrichment |
| Camera return within retained working set | Zero regenerated unchanged pages and no VT-induced loss of prior coverage |

Measure frame costs over at least three repeated 600-frame active sequences after initialization, and edit latency over at least 30 controlled edits. Record p50/p95/p99, worst outliers, useful pages per millisecond, total frame time, and request backlog. Cold source loading, cold geometry preparation, large material edits and new-world streaming are separate workloads with their own reported latency; the 100 ms target does not describe them.

Completion requires native artifacts demonstrating both the correctness gates and measured targets. Documentation, a faster CPU scope alone, or screenshots without timing/continuity evidence cannot close the implementation goal.
