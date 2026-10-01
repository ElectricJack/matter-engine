# Device-derived BLAS cache checkpoint

Date: 2026-09-18.

## Implemented

- Bounded asynchronous disk worker over existing binary BlobStore packs and
  durable semantic references; coalesced reads and batched appends, with no
  render-thread filesystem access.
- Keys include geometry-page content, adapter/build policy, opacity class,
  device UUID and driver UUID. Vulkan compatibility and serialized size checks
  precede deserialization. A cache miss uses the existing triangle build path.
- Serialization-size query and GPU readback retire through frame fences. Failed
  submissions cannot publish captures. Captures retain both their source BLAS
  and the residency reservation, including when the page is evicted.
- Runtime geometry pages opt into the cache by default; matched A/B can set
  `MATTER_GEOMETRY_BLAS_CACHE=0`. Counter meanings: restored = commands recorded,
  captured = disk writes queued, miss = no bytes returned, rejected = incompatible
  or invalid-size bytes. These are not durability/presentation counters.
- Resident hierarchy snapshots and page-index maps are cached until publication
  or eviction/detachment. `MATTER_GEOMETRY_SNAPSHOT_CACHE=0` restores per-frame construction
  for measurement. Cache invalidation releases retained residency handles.

## Verification

The native MSVC disk-cache suite passes missing-data, write/read ordering,
reopen durability, content/device key separation, oversized input rejection and
truncated-store fallback, batched payload association, bounded read admission
and cancellation recovery. See [disk test output](disk-tests.log).

The native Vulkan geometry fixture has passed cold build/capture followed by
restoration under a different runtime part ID, matching raster coverage and RT
membership, with zero validation errors. Cache hits issue zero triangle BLAS
builds. Eviction during serialization retains the source reservation, and
readback completion releases it. See [GPU test output](geometry-gpu-tests.log).
The broader [native RT regression suite](rt-regression.log) also passes with zero
validation errors.

## Limits and remaining work

The user closed the editor. The updated native editor was rebuilt and three
sequential isolated StreamMountain runs completed successfully. Cold capture
recorded 7,263 BLAS records; the warm run restored all 7,263, with zero misses or
rejections. Geometry residency matched at 226,016,960 bytes and 7,263 pages.
The cache-disabled comparison also disabled CPU snapshot caching, so it does
not isolate either optimization. See `mountain/` for raw progress and timelines.
The completed instrumented pair isolates the BLAS switch with snapshot caching
left enabled in both runs. See [measurements](mountain/README.md), including a
final validation of finer CPU stage and memory-pressure counters. All owned
validation editors are closed.

No measured whole-scene speedup is claimed. BLAS storage/readback and tiny-page
lookup overhead must be compared against direct building. Physical disk cache
reclamation, BLAS compaction and an offline all-assets cooker are not implemented.
The cache is populated by runtime preparation of demanded pages. Its bounded
transient capture/upload buffers are additional to the geometry residency budget;
retained source BLAS allocations remain charged to residency.

Terrain remains on its current rendering path. Its transition requires displaced
source tessellation, receiver material coordinates, sector seam integration and
VT parity before disabling POM. The implementation plan is
[here](../../../superpowers/plans/2026-09-18-blas-cache-and-terrain-geometry.md).

## Terrain groundwork

The CPU hierarchy suite now passes uniform surface subdivision/displacement,
shared-edge deduplication, bounded height/output rejection, material-boundary
continuity, original receiver propagation through simplification, and optional
binary receiver-section roundtrips. See [test output](displacement-tests.log).
This is reference/compiler groundwork; it does not yet replace terrain POM or
connect displaced pages to VT, terrain-sector seams or the mountain scene.
