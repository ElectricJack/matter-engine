# Indexed geometry traversal and POM-off performance checkpoint

Date: 2026-09-18. Native MSVC RelWithDebInfo, RTX 4090, 1280×720,
StreamMountain with RT and GI enabled. POM is explicitly disabled in the saved
scene props, geometry launcher and measurement commands.

## Changes

- Dense indexed CPU traversal replaces repeated hash lookup, ownership discovery
  and NodeView copying in camera-dependent LOD selection.
- GPU node templates and root indices are cached with residency snapshots.
- Per-frame RT membership uses byte flags. Page use timestamps remain updated;
  the eviction protection set is built only when pressure requires it.
- One shared immutable snapshot pin per asset/frame retains all page resources.
- Publication invalidates only snapshots containing the changed page descriptor.
- Snapshot traversal adopts shared resident descendants, preserving ownership
  after the original lease detaches.
- Final follow-up: rebuild root pins only on membership changes, clean page
  locations on detach/eviction, and use the renderer's versioned flat slot index
  for page readiness checks. These follow-ups are measured separately below.

## Validation

The native CPU suite passes exhaustive indexed/reference traversal parity across
all availability subsets of its small hierarchy, including selection/visit caps,
and snapshot-only shared ownership after detach. Existing displacement/receiver,
coverage, cache and residency checks also pass. See `cpu-tests.log`.

The native Vulkan fixture passes 24 CPU/GPU cut comparisons, cold/warm BLAS
restoration and resource-retirement checks with zero validation errors. See
`gpu-tests.log`.

The first optimized mountain run loaded all 7,263 pages and completed camera
movement and return without paging failures, watchdogs, memory stalls or evictions.
The returned view retains 396,914 raster triangles and 2,911 raster batches;
ordinary streamed instances/VT variants change during travel. The existing
speckled/grooved rock appearance remains; this is not a claim that previously
reported visual holes have been fixed. See `returned.png`.

## First measurements

Both baseline and optimized runs have POM disabled, identical camera/budgets,
and warm BLAS caches. Geometry-settled update windows fell from roughly 24–26 ms
to about 11 ms. Indexed CPU selection was around 0.045 ms per rock instance in
late windows. This is a subsystem improvement, not achievement of the user's
**whole-engine <10 ms frame-time target**. Whole-frame samples remain near
58–60 ms; GPU time is still substantial. The first optimized steady sample had
fewer ordinary instances and should be excluded from matched-census comparisons.

All 7,263 geometry pages fit the measured budgets and remain resident during
these captures. Zero evictions argues against geometry cache thrashing in these
specific tests, not in arbitrary long-distance travel or larger scenes.

## Architectural work still required

The current demand path discovers child descriptors through resident parent
pages. It cannot request the final desired detail directly from an independent
hierarchy directory. Separate compact hierarchy metadata from mesh payloads,
select the desired cut up front, and batch payload requests while retaining a
complete coarse fallback. Page-level renderer/BLAS work and frame-count admission
limits also need redesign/measurement before engine-wide adoption. Merely raising
a queue limit or masking coarse geometry is not acceptance.

## Final performance sample

The final native editor (including root/location bookkeeping and readiness-lookup
changes) completed an eight-second, 138-frame automated sample after static
geometry stabilized. POM is reported false, RT/GI remain enabled, static vertex
and cluster upload deltas are zero, and validation errors are zero. This sample
still includes VT work and some acceleration-structure timing readbacks; it is
not a fully idle rendering baseline. See `final-perf/perf.json`.

- Whole-frame median **58.07 ms**, p95 **71.21 ms**: <10 ms goal **not met**.
- Raw GPU median: total **56.62 ms**, GI **24.41 ms**, G-buffer **13.59 ms**,
  volumetrics **5.11 ms**, sun shadows **0.17 ms**. Pass intervals overlap in
  places and must not be blindly added.
- Late geometry-update windows averaged **10.10–12.28 ms**. Dense cut selection
  remained around 0.05–0.06 ms per asset. More recurring renderer/runtime work
  remains; the faster traversal alone cannot deliver the frame target.
- All 7,263 pages restored from BLAS cache. No geometry eviction or budget-stall
  evidence in this bounded test. It does not establish long-travel behavior.

Full per-frame VT and profiler traces remain at
`C:/tmp/matter-blas-mountain/indexed-final-perf/`. The native test/build artifacts
and benchmark editors completed; no validation editor is intentionally left open.

The user's subsequent direction is recorded in the sector resolution paging
spec/plan: independently available hierarchy metadata, sector-owned resolution
bundles, measured sector sizes, byte/time scheduling, and fixed preallocated
banks with allocation-compatible page size classes. That architecture is not
implemented by this optimization checkpoint.
