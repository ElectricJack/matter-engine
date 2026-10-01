# BLAS caching, streaming performance, and displaced terrain

User-directed follow-up to the virtualized geometry pilot, 2026-09-18.

## Order and acceptance

1. Persist device-compatible static triangle BLAS data beside portable geometry
   pages. Prove that a second load restores it, produces matching raster/RT
   results, and issues no triangle BLAS build for that page.
2. Resolve reported holes; measure matched cold/warm loads, page arrival,
   allocation, CPU submission, and GPU cost. Preserve a complete coarse cut
   while replacements load. Cache invariant hierarchy metadata between residency
   changes. Do not attribute whole-mountain frame time to geometry without an
   otherwise identical A/B capture.
3. Use the same hierarchy/page/BLAS path for terrain displacement. Keep the
   existing world streamer responsible for sector admission and scatter.
   Retire terrain POM only when geometry and material parity are demonstrated.

## Device-derived BLAS cache

The portable geometry cache remains authoritative. Its page hash plus adapter
version identifies the actual triangle stream. The RT key adds a format/build
policy version, opaque/any-hit classification, device UUID and driver UUID.
Vulkan's acceleration-structure compatibility query is still mandatory on load.
No GPU address or per-session part ID is a persistent content identity.

The renderer requests an asynchronous lookup from a bounded disk worker. This
worker alone opens the derived `geometry-pages/blas` BlobStore/RefTable, verifies
payload CRCs, and commits batches of payloads before publishing references.
Missing, incompatible, truncated and unavailable cache data use the existing
triangle build path. A store lock held by another process must not stall rendering.

Cold builds record serialization-size queries after the build barrier. After
that frame slot retires, record serialization into aligned host-visible buffers.
After their frame slots retire, copy the bytes to the disk worker. Failed frame
submissions publish no capture. GPU buffers, query pools and source BLAS objects
remain retained through every command that references them. Cache hits upload
and deserialize into a new BLAS; existing publication rules still require both
raster and RT readiness. TLAS membership and transforms remain runtime work.

Initial scope is ordinary static triangle BLAS without micromaps. This is a
local derived cache populated by page preparation; it is not a portable cooked
BLAS distribution or a complete offline all-assets cooker. Compaction, physical
disk reclamation and build-versus-deserialize size thresholds require measured
follow-up. Capture/restore counts must distinguish commands recorded from data
durably written or presented.

## Terrain geometry contract

Terrain already has real coarse volumetric geometry and runtime cross-sector
welds. POM supplies surface relief. Moving the coarse terrain into geometry pages
alone would preserve its flat appearance and does not satisfy this task.

Use the existing `SurfaceRuntime::source_at` height as the CPU reference for the
authored continuous world-space material. Bake sufficient tessellation to capture
stone bodies, clods and fractured rock faces. The displacement sample footprint
must follow the source tessellation spacing so centimetre grain is not aliased
into large triangles. Retain smaller unresolved detail in material normals.

Material evaluation uses the undisplaced receiver position and its stable field
context; evaluating again at the displaced position would move the pattern.
Each geometry vertex must retain this receiver mapping across simplification.
VT continues to provide color, roughness, normals and blending. Surface relief
must be applied once: geometry-covered surfaces bypass POM while ordinary
surfaces retain their existing behavior.

Do not compile a WorldSector's scattered children into its terrain hierarchy.
Extract its owned terrain mesh as a separately paged asset, preserving child
ownership and stable scatter identity. Include the field, surface tape, mesher,
displacement sampling policy and boundary policy in cache identity.

Shared borders require one canonical source position/normal/displacement and a
compatible boundary tessellation. Internal hierarchy cuts must preserve these
constraints; cross-sector and coarse/fine joins must integrate with the current
seam welder. Existing coarse boundary records cannot silently remain unchanged
after displacement. Test equal-level and mixed-level tiles, corners, negative
coordinates, steep slopes and caves. Do not ship a heightfield-only replacement
for the existing volumetric terrain.

Start with a bounded terrain fixture before enabling the entire 20 km mountain
scene. Acceptance includes wireframe and silhouette captures, RT shadows/GI,
stable VT appearance across refinement, matched POM-disabled images, camera
travel/return, memory pressure and a warm restart from the binary/BLAS caches.

## Measured checkpoint and instrumentation (2026-09-18)

The native editor now restores the mountain demo's 7,263 BLAS records on a warm
run with no cache misses/rejections. This proves cache reuse, not a frame-rate
improvement. Cold, warm and direct runs are retained under
`docs/agent/evidence/2026-09-18-blas-cache/mountain/`.

The first complete instrumented warm run read 130,424,416 bytes in 5,827 storage
read operations. Page-cache batch calls averaged 0.438 ms; decode/renderer-part
preparation averaged 0.111 ms per page. Queue and render-readiness latency were
much larger. No CPU budget deferrals, upload reservation stalls, read failures
or watchdog expirations occurred. These are OS-warm filesystem measurements,
not a cold physical-drive throughput test.

Even after requests/publications stopped, the geometry runtime used 24–26 ms
per frame in the sampled windows. Break out CPU cut selection, snapshot rebuild,
hierarchy packing and renderer cut submission before choosing an optimization.
Keep BLAS lookup/restore and CPU snapshot switches isolated in comparisons.
The current 8-page admission / 32-in-flight policy is a measurable scheduling
constraint; raising it requires frame-time and memory-pressure validation.

The receiver/displacement CPU reference and optional page section pass the
hierarchy suite. Terrain-sector extraction, VT binding, POM bypass and seam
integration remain to be implemented and visually validated.

## Performance priority and POM policy

The user requires **whole-engine frame times below 10 ms** and fast streaming
before expanding adoption. Geometry-update time alone is not acceptance. Keep
RT/GI and authored scene content enabled in comparisons; report CPU update,
whole-frame/GPU time, publication latency and camera-movement behavior separately.
StreamMountain now explicitly saves `render.pom.enabled=false`; the geometry
launcher and performance timelines also apply that setting. Both raster and RT
height marching use the shared zero-step setting. VT material channels remain.

Current implementation work replaces repeated hash-based CPU traversal with
indexed traversal of a validated resident snapshot. It caches GPU node templates,
uses byte flags instead of per-frame page sets for RT membership, pins one shared
snapshot per asset/frame, and invalidates only snapshots affected by publication.
The reference traversal remains the exhaustive CPU parity oracle. Snapshot
construction adopts shared resident descendants so another owner's departure
cannot invalidate a live asset's cached traversal.

Performance acceptance must distinguish these changes from disabling POM: run a
POM-off baseline and a POM-off optimized build at the same camera and budgets.
The indexed path still traverses/repackages nodes each frame; persistent renderer
buffers, incremental publication and time-budgeted admission remain candidates
if measured costs require them. Do not claim the <10 ms goal from a faster
subsystem or a stationary-only shortcut.

## Updated production paging direction

The user chose sector-owned, spatially adjacent resolution bundles with tuned
sector sizes, independent hierarchy metadata, and preallocated reusable memory
banks. Pages must use size multiples that fit those existing allocations; runtime
must not allocate/free large backing banks. See the newer
[spec](../specs/2026-09-18-sector-resolution-paging.md) and
[implementation plan](2026-09-18-sector-resolution-paging.md). These requirements
are not yet implemented by the current small-page pilot.


## Superseding acceptance: isolated terrain baseline

The user subsequently authorized RT/GI and all non-terrain rendering to be
turned off while pursuing all-terrain VG + VT, under-one-second loading and
approximately 10 ms frames. See the updated acceptance section in
[sector/resolution paging](2026-09-18-sector-resolution-paging.md). Earlier
requirements above to retain RT/GI in performance comparisons no longer apply
to that baseline. RT correctness remains separately regression-tested.

The first opt-in terrain bridge preserves the source sector's charted rung as
its shared VT receiver and initial coverage, compiles its owned mesh into
hierarchy pages, and routes page raster draws through the source VT slot.
Existing boundary records remain attached. This does not yet bake new surface
displacement, remove runtime preparation, or satisfy the loading target.
