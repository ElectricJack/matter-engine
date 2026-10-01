# Sector resolution paging implementation plan

Spec: [sector-owned resolution bundles](../specs/2026-09-18-sector-resolution-paging.md).
This plan supersedes tuning the old eight-pages-per-frame scheduler as the
production performance strategy.

## 0. Fixed-bank allocation contract

Implement persistent bank ownership for CPU payload/decode/staging and GPU
geometry/BLAS/scratch domains before migrating the paging hot path. Establish
base quanta/classes and bounded slot metadata at initialization. Directory
records must provide allocation/decode bounds; page capacities are integer
multiples that fit bank allocations. Reuse existing renderer reserved buffers
where compatible, extending their suballocation/retirement contracts instead of
creating a bank per sector or page.

Provide generation-safe slot leases and explicit reserved/filling/ready/retiring
states. Reserve transition capacity before admission. Overflow defers refinement
or keeps coarse coverage; no runtime large-bank growth or release.

Tests: alignment/class rounding, malformed size rejection, internal fragmentation,
allocation failure without growth, cancellation/reissue, out-of-order I/O, shared
payloads, fence-delayed slot reuse, slot exhaustion with valid coarse fallback,
and a bank-allocation audit across repeated travel/refinement/eviction cycles.

## 1. Independent directory and demand oracle

Extend the binary geometry/asset cache with a versioned sector directory and
bounded directory chunks. Reuse `sector_grid` coordinates, world admission,
AssetStore packs/refs and existing generation-safe tickets. Include partition
policy in cache identity. Build a CPU reference selector that requests the
correct resolution directly without resident intermediate meshes.

Tests: cold directory-only traversal, exact coverage, missing/corrupt records,
negative coordinates, size limits, cache reopen and deterministic identities.
Assert that an unavailable parent payload does not block a desired child request.

## 2. Resolution bundle preparation

Group static procedural geometry by sector/subregion and error level. Preserve
small rendering clusters within larger payloads. Emit portable GPU-ready geometry,
shared instance/material dependencies and optional compatible BLAS records.
Pack neighboring levels spatially where useful; maintain content deduplication.
Prepare contiguous physical extents and compare 1/4/16 MiB read targets without
padding sparse sectors to those sizes or expanding shared instance geometry.
Avoid one renderer part/BLAS per tiny cluster. Prototype bundle-level BLAS with
stable cluster/material lookup and exact same visible/RT coverage.

Tests: high/low representations of the same coverage, complete bundle publication,
shared foliage references, page bounds, material identity and BLAS compatibility.

## 3. Budgeted asynchronous runtime

Replace page-count-per-frame scheduling with byte/time/resource-budget admission.
Batch reads/uploads/preparation, track in-flight bytes, and keep blocking work off
the app lane. Use persistent renderer registrations/buffers with incremental
changes. Keep active and predicted adjacent levels in a bounded CPU cache.
Separate I/O completion from decode/preparation so bounded outstanding reads,
CPU preparation and GPU transfers overlap. Retain generation-tagged bank leases
until actual I/O/GPU completion, including cancelled work. The renderer must
never wait on a disk request or worker completion. Validate the reader backend's
file-handle concurrency contract before allowing parallel submissions.

Tests: cancellation/reissue, partial arrivals, retained resources across frames,
GPU failure, memory/scratch pressure, no unbounded work per update, camera return,
and no coarse/fine overlap or uncovered geometry during replacement.

## 4. Sector-size experiments

Run the same travel route and visual settings across spatial size and payload
size candidates. Record per-frame traces, target-ready latency, useful bytes,
physical I/O operations, GPU pass timings and cache churn. Compare cold and warm
runs separately. Choose subdivision/merge thresholds from dense and sparse
regions; do not globally enlarge sectors based on one rock fixture.

## 5. Terrain and engine-wide adoption

Integrate displaced terrain receivers, VT bindings and mixed-resolution seams.
Validate corners, caves and across-sector material alignment. Preserve existing
voxel foliage reuse. Expand to additional static content classes only after the
bounded mountain region passes latency, memory and whole-frame gates.

## Gates

- POM off. Current user-approved terrain baseline disables RT/GI, foliage,
  scatter and other rendering; terrain geometry and VT remain enabled.
- Target resolution requested directly from directory metadata.
- Terrain coverage and VT parity at partial-arrival and mixed-level boundaries.
  RT parity remains required before RT is restored.
- No render-thread I/O and no fixed per-frame page throughput cap.
- Global RAM/VRAM/in-flight budgets respected through cancellation and eviction.
- Pages fit preallocated size classes; zero large backing-bank allocations/frees
  during interactive runtime, verified by allocation counters and stress tests.
- Report whole-frame p50/p95/max, not only a subsystem average. The requested
  <10 ms whole-engine target requires GPU rendering work as well as paging work;
  do not mark this goal achieved while either remains over budget.

## Implementation checkpoint: MemoryLib bank foundation

Implemented first, per the updated priority:

- `MemoryLib/mem_bank`: fixed, preallocated backing and metadata, configurable
  power-of-two allocation quantum, generation-checked leases, bounded admission,
  statistics, CPU-owned/external/offset-only modes.
- AssetStore `ReadBatch` accepts bounded caller-owned buffers and reads directly
  into them. `PageCache` payloads now lease bank ranges instead of creating an
  arena for each read batch. Existing immutable page handles retain their leases
  across eviction and cache destruction; final release returns the range.
- GeometryWorldRuntime preallocates one CPU payload bank at construction and
  reuses it across worker cache directory changes. Its 256-byte quantum is an
  initial payload allocation granularity, not a chosen sector/page size.
- `paging_bank` instrumentation reports committed/occupied bytes, largest free
  range and backing allocation count. Cache metadata still uses ordinary C++
  allocations.

Phase 0 is **partially complete**. GPU geometry/BLAS/scratch banks, decode and
upload staging pools, GPU-fence retirement, and the separate BLAS disk-cache
payload allocator still need migration. The generic offset allocator supports
that work but does not perform it. Sector bundles, the independent directory and
new scheduling remain later phases. No whole-engine zero-allocation or <10 ms
claim follows from this CPU payload integration.


## Implementation checkpoint: asynchronous read/preparation lanes

- Dedicated I/O and preparation threads now overlap page reads and geometry
  adaptation independently of the world-streaming coordinator. Store handles
  remain I/O-thread confined. A bounded pipeline charges active and undrained
  work, suppresses cancelled generations and joins active callbacks on shutdown.
- Optional physical-neighbor read-ahead uses the committed index's pack order,
  reads directly into bank ranges, and caches validated neighboring pages. Index
  snapshot revision changes rebuild locality, including append/repair commits.
  Speculation drops out before mandatory demand admission under memory pressure.
- Geometry dispatch no longer has an eight-ticket per-frame cap. Upload and BLAS
  warmup use byte/time budgets; queue/in-flight count bounds remain as defensive
  backpressure. These budgets are cooperative between indivisible operations.
- Mountain launcher: 1 GiB CPU payload bank, initial 4 MiB read-ahead target.

This is an incremental runtime improvement, **not completion of phase 2 or 3**.
The physical groups still contain the old individual node records. They do not
encode sector-level replacement coverage, and desired-detail discovery still
requires intermediate hierarchy pages. One I/O thread performs blocking OS reads
while preparation/rendering overlap it; parallel native asynchronous disk reads
and independent directory-driven demand remain future work. Decode/staging and
GPU banks also remain outstanding from phase 0.


The first higher-throughput mountain test exposed a BLAS-cache queue saturation
bug: a rejected enqueue was counted as a completed miss and triggered rebuilds.
`BlasDiskCache::poll` now distinguishes pending/backpressure from a completed
lookup, and the renderer retries without changing coarse coverage. A deterministic
full-queue regression test covers this path before GPU validation.


## Updated acceptance: terrain first, raster baseline (2026-09-18)

The user now authorizes disabling RT, GI, trees and other non-terrain rendering
while prioritizing **all StreamMountain terrain on virtual geometry + VT**.
POM remains disabled. Earlier requirements to keep RT/GI enabled during the
performance acceptance are superseded for this baseline; retain their supported
path and regression coverage.

Targets: requested terrain geometry and texture detail ready in **under 1 s**,
with approximately **10 ms frame time** in the terrain-only scene. Report startup
to usable scene separately from camera-move request-to-ready, and geometry-ready
separately from VT-ready. Report cold and warm cache cases. Do not substitute
first coarse coverage for requested-detail readiness, or exclude startup without
reporting it. Offline generation time is a separate measurement, not hidden in
loading results. No claim of success until both visible coverage and timings pass.

Immediate sequence:
1. Remove BLAS readiness, RT input allocations and scratch from raster-only page
   publication; test actual raster coverage and keep the RT regression fixture.
2. Route terrain-owned mesh through hierarchy pages while retaining sector VT
   coordinates and cross-sector boundary records. Exclude scattered children.
3. Preserve the sector VT domain across page refinement; independently page
   geometry without creating per-cluster texture atlases.
4. Prepare persistent sector/resolution bundles and metadata so runtime reads
   target detail directly. Move generation/charting out of timed cached loads.
5. Measure isolated terrain and optimize remaining CPU/GPU/upload/bank costs;
   validate travel, mixed-resolution seams and cold/warm readiness under budget.

This is the acceptance direction, not a claim that terrain migration or the
sub-second loading target has already been achieved.

## Terrain baseline follow-up: chart fragmentation

The raster terrain bridge exposed hundreds of independent root groups per
sector. Geometry compilation currently separates discontinuous material, UV,
normal and AO boundaries before forming leaf adjacency; this preserves source
appearance but turns chart fragmentation into page/renderer fragmentation.

Phase 2 must preserve those boundaries inside a bundle without requiring one
runtime part and draw submission per tiny disconnected chart group. Keep corner
attributes intact; do not weld/simplify across discontinuities merely to reduce
page count. A spatial bundle may contain several independently bounded clusters.
Measure cluster occupancy, roots/sector, commands/visible triangle, directory
size and useful bytes/read alongside total frame and readiness latency. Compare
identical fully prepared working regions; partial scenes with fewer resident
sectors cannot establish an improvement or the 10 ms gate.
