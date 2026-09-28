# Virtualized procedural geometry: implementation plan

Date: 2026-09-18. Status: implementation underway; no complete P0–P6 acceptance milestone is claimed.
Authority for this work: [design](../specs/2026-09-18-virtualized-procedural-geometry-design.md).

## Current checkpoint — 2026-09-18

The opt-in static-mesh pilot now connects compilation, binary pages, mandatory
root admission, worker reads, bounded GPU upload/BLAS publication, GPU hierarchy
selection, indirect drawing, and asynchronous GPU missing-page feedback. The CPU
reference supplies the matching RT cut. GeometryDetailProof provides the small
reference fixture; StreamMountain now includes three unique displaced procedural
rocks, each with 110,592 source triangles, placed by the existing sector streamer.

[Mountain demo evidence and launcher](../../agent/evidence/2026-09-18-stream-mountain-geometry/README.md)
record the actual scope and limitations. This is a working integration pilot,
not completion of every P0–P6 acceptance gate. In particular, generic imported
mesh scaling, parent reclustering, robust pressure/recovery sweeps, stable VT
material mapping, adjoining displaced terrain, and total-cost optimization
remain. Ordinary terrain and shared foliage keep their existing paths in the
targeted mountain demo.

## Current follow-up

The user requested a persistent BLAS cache, continued performance work, and
terrain displacement through the geometry-page system in place of POM.
[Implementation and acceptance plan](2026-09-18-blas-cache-and-terrain-geometry.md).
This changes the next implementation priorities, not the paused goal status.

## Deliverable

A generic triangle hierarchy and geometry-page streaming path for large unique
meshes, compatible with procedural displacement/debris, VT materials, ordinary
rasterization and ray tracing. World streaming continues to own world admission;
the new geometry layer refines admitted assets without rebuilding their sectors.

The first user review must demonstrate independently selected and streamed
detail within one complex unique mesh. A more detailed scene rendered entirely
through the existing whole-part path is not that milestone. No changes to the
active goal's objective/status or acceptance targets are implied by this document.

## Reuse and changes

| Reuse directly or through adapters | Improve/add |
| --- | --- |
| World-session ownership, generation tags, asynchronous request completion | Geometry-page demand under an admitted asset, independent of sector rebakes |
| Part/content identities and worker staging | Versioned hierarchy manifests, root admission and page-only reads |
| Mesh simplification/error tools and indexed geometry | Connected group simplification, attribute preservation and error propagation |
| Existing GPU bounds/culling and indirect draw plumbing | GPU hierarchy traversal, valid-cut selection and bounded feedback |
| Retained Vulkan resource lifetimes and triangle BLAS support | Page/group lifetime tracking, RT cut publication and bounded build scheduling |
| VT material generation and fallback concepts | Stable geometry-to-material mapping across cuts |
| Shared assembly geometry from foliage | Optional reuse of repeated detail, without depending on repetition for scaling |

## P0 — Freeze the contracts and reference fixture

- [ ] Audit live caller paths before edits: `SectorStreamer`, coordinator,
  `PartStore`, flat artifacts, `part_cluster`, LOD/error helpers, `cull.comp`,
  `build_ray_geometry`/`emit_ray_instances`, shared surfaces and terrain seams.
  Graft discovery timed out during planning; repeat the appropriate bounded
  caller query when making source changes and fall back to source inspection.
- [ ] Produce a reproducible unique-mesh fixture with a connected surface,
  undercut, disconnected debris and material boundaries. Preserve its detailed
  reference mesh. Include a separate imported/static mesh to test generic input.
- [ ] Record ordinary-mesh baseline captures and timings, including shadows/GI,
  memory and startup. Add source-complexity variants without repeating instances.
- [x] Define asset/page/group IDs, format version, bounds/error units, group
  replacement rules, byte limits, material mapping and GPU publication epochs.
- [x] Specify the canonical LOD error conversion and clearly distinguish
  measured deviation from conservative bounds. Do not fork the existing rule.

Exit: a reviewable artifact contract and reference scenes with reproducible
measurements. The existing renderer remains the oracle for detailed input.

## P1 — Build a connected hierarchy on CPU

- [x] Add a compiler module beside the existing cluster/LOD code. Start with
  indexed triangles, adjacency and material/shading boundaries.
- [ ] Partition leaves, group neighbors, simplify combined groups and recluster
  parents. Store complete replacement dependencies and accumulated error.
- [x] Implement a CPU cut selector as an independent correctness reference.
  Exercise all cuts of small fixtures, including missing children.
- [x] Serialize manifest, coarse roots and fine pages without requiring the
  legacy merged whole-part mesh/BLAS representation on the new path.
- [ ] Verify determinism, corrupt/truncated input, attribute preservation,
  reduction ratios, boundary complexity and bounded compiler memory.

Exit: several simultaneously selected LODs within one unique object, rendered
through a temporary ordinary-mesh adapter with no cracks or double coverage.
This is a correctness checkpoint, not the streaming scalability milestone.

## P2 — Add geometry-page residency under world streaming

Storage contract and checklist:
[binary asset pages and spatial packs](../specs/2026-09-18-binary-asset-page-cache-design.md).
The storage workstream can start with P0/P1; real page I/O is required for the
streaming milestone even when compiler development uses a fixture backend.

- [x] Add immutable asset metadata/root handles at the PartStore boundary and a
  page residency service. Asset ownership and page residency have distinct lives.
- [x] Adapt existing worker/publication queues to page-sized operations. Preserve
  owner/generation/issuance checks and rollback-before-failure acknowledgement.
- [ ] Add content-keyed request deduplication, cancellation, bounded retries,
  priority and separate reservations for CPU staging, uploads, GPU geometry and
  acceleration-structure scratch. Preserve mandatory roots under pressure.
- [ ] Use a page-store interface. Evaluate existing AssetStoreLib packed blobs;
  batch its synchronous reads on workers. Add bounded spatial/dependency-aware
  coalescing, binary page views, retained RAM allocations, and batched append/
  manifest commits. Measure checksum/decode cost and read/write amplification
  with equal-integrity cold/warm comparisons. Coordinate compaction with readers.
  A fixture store may be used first, but the native streaming milestone needs
  measured real disk-page reads.
- [x] Implement snapshots and retirement tracking. Coarse geometry remains
  usable until an entire compatible refinement is render-ready.
- [ ] Ensure world detach, reload, source edits and page-ID reuse cannot publish
  stale geometry. Multiple region/instance references retain shared assets.

Exit: forced tiny-cache runs and delayed/failed reads preserve complete geometry;
requesting a small area does not load every LOD of the asset.

## P3 — Select the resident cut on GPU and rasterize it

- [x] Add bounded hierarchy traversal and request feedback, with CPU/GPU cut
  comparisons. Reuse existing instance transforms and LOD conventions.
- [x] Emit ordinary indexed indirect draws from the resident cut. Avoid
  dispatching every source leaf or reading the full cut back to CPU each frame.
- [ ] Support valid parent fallback on unavailable pages and queue/work overflow.
  Add conservative frustum culling first; validate any occlusion refinement.
- [ ] Bind stable material coordinates and VT fallback. Geometry LOD changes
  must not force a complete material rebake. Keep picking and motion identity.
- [ ] Measure traversal, draw/triangle work, page requests and frame cost across
  close, wide, grazing, moving and hidden-object views.

Exit: unique-mesh source complexity can increase without an equal increase in
resident fine geometry or leaf-selection work for a fixed view/error target.
No universal constant-time claim: visible complexity and material work matter.

## P4 — Make ray tracing share the geometry lifecycle

Design and instrument this from P0; complete it before the first user acceptance.

- [ ] Compare page/group BLAS granularity and retain reusable triangle BLASes.
  Do not assume one BLAS per microcluster or one rebuild per full asset is viable.
- [ ] Maintain changes to actual RT membership and selected groups; audit
  remaining O(world) CPU scans and full TLAS rebuild costs explicitly.
- [x] Publish geometry addresses, RT material records and acceleration resources
  together. An upload without an RT-ready replacement retains its old cut.
- [ ] Keep coarse off-screen coverage for secondary rays and direct shadows.
  Start visible primary/RT comparisons with the same geometric cut; optimize
  secondary detail only behind a measured error/lighting contract.
- [ ] Bound build work and scratch memory. Record deferred builds and fallback
  duration, including camera teleport and rapid edits.
- [ ] Validate hit position/material, silhouettes, self-shadowing, reflected
  detail, GI, instance transforms and resource retirement against the reference.

Exit / **first user-reviewable milestone**: a large unique mesh with independently
streamed internal detail, connected LOD boundaries, missing-page fallback,
correct materials, actual ray-traced detail and an honest full cost breakdown.

## P5 — Integrate world streaming and procedural surface producers

- [ ] Admit hierarchy roots through existing world/sector lifecycle. Refinement
  requests read/generate geometry pages instead of changing sector identities.
- [ ] Coordinate worker/read/upload/RT budgets with existing terrain and VT work.
  Collect queue latency and retry reasons to resolve starvation and duplicate work.
- [ ] Compile displacement from the stable material/source-field contract, with
  a fixed finest-source sampling convention and measured simplification after
  displacement. Include final displaced bounds and remove double displacement.
- [ ] Compile unique debris geometry and optional shared-prototype assemblies.
  Verify gaps/undercuts and stable ownership under sector subdivision.
- [ ] Audit active terrain boundary mode; preserve shared contour/transition
  semantics through displacement and page refinement. Test unequal LOD neighbors,
  corners, negative coordinates, caves/vertical surfaces and asynchronous arrival.
- [ ] Integrate one actual StreamMountain region, then several unique regions
  and assets. Convert POM only where the replacement passes visual/lighting tests.
- [ ] Make local edits preserve unaffected cached pages and visible coverage.
  Schedule affected ancestor rebuilds without blocking the render thread.

Exit: real-world loading, traversal, edits and unload use the new path without
holes, full-region rebakes for camera movement, or loss of lighting features.

## P6 — Scale and close acceptance gaps

- [ ] Run unique-asset and source-complexity sweeps under fixed RAM/VRAM and view
  conditions. Count mandatory-root/metadata overhead as well as fine pages.
- [ ] Add compression, better grouping, upload batching or faster rasterization
  where measurements identify a bottleneck. Evaluate specialized cluster RT
  only as an optional backend with capability checks and a standard fallback.
- [ ] Evaluate sparse aggregates for dense disconnected distant geometry using
  the same source/reference, material contract and coverage tests.
- [ ] Measure native saved terrain issue views at 2796x1044 with their original
  terrain-only settings. Report whole GPU frame, G-buffer, CPU and presented
  times. Separately report RT/GI/shadow passes and their enabled configurations.
- [ ] Complete cold/warm startup, streaming, camera return/teleport, tiny-cache,
  source-edit, corruption/retry, world-switch and in-flight retirement runs.
  Record median/p95, stalls, build/source hashes, raw captures and validation.

No milestone may be closed solely by repeated-instance counts, POM removal,
reduced resolution, hidden content, changed lighting settings, or a passing
single-pass timing. The current whole-frame terrain target remains below 5 ms.

## Existing code boundaries and test homes

Extend modules at their responsibility boundaries; do not place a second world
streamer inside the renderer or duplicate shared library code. Candidate new
modules are a geometry-hierarchy compiler/format and a renderer geometry-page
residency/selection service; names are implementation choices.

Use existing streaming/coordinator lifecycle tests for ownership and stale
completion integration; add hierarchy/cut and page-lifetime tests alongside
engine tests. Native Vulkan fixtures must compare actual raster and RT output,
not only counters. Use `tools/build-windows-from-wsl.sh` / the supported MSVC
wrappers and the existing agent capture/validation tools. Rebuild only when no
editor executable is running. Record native build/test evidence for implementation changes; passing CPU
checks alone does not close the Vulkan or visual acceptance gates.

## Main risks and decisions to measure

1. Connected simplification: boundary locking can preserve correctness while
   preventing useful reduction. Reject that result as a scalability endpoint.
2. Compilation: huge unique sources can exceed staging RAM or generation time
   even when final rendering is cheap. Bound and measure cold production.
3. RT updates: raster cuts can change faster than conventional acceleration
   structures can be rebuilt. Coarse coverage and build admission are required.
4. Materials: existing per-rung/chart paths and the foliage surface-query shader
   cannot be assumed to provide stable complete material mapping automatically.
5. Scheduling: sector, geometry and VT demand must share admission priorities
   without global invalidation or parent eviction under temporary pressure.
6. Scalability: no renderer supports unlimited simultaneously visible complexity.
   Publish the measured content, hardware, error, memory and frame-time envelope.
