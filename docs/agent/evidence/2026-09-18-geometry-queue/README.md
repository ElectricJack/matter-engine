# Geometry queue and admission costs

## Changes

The residency dispatcher previously looked up every queued page, built a new
pointer vector and sorted it before issuing a bounded batch. With roughly
260,000 terrain root pages, this repeated backlog-sized work on the frame thread.
Ready requests now retain their root/priority/hash ordering in a set. A separate
retry-time queue promotes eligible delayed work. Requests, priority changes,
root ownership changes, cancellation and eviction update these queues directly.
The number of active operations, retry limits and shared-page lifetime rules
remain enforced.

Runtime admission remembers rejected manifest identities until geometry
capacity is released (detach/eviction), the manifest changes, or the source
leaves the admitted scene. It retains coarse coverage while blocked and reports
rejections/deferred attempts explicitly. This prevents repeated root-manifest
decode and allocation while capacity only grows; it does not make a rejected
asset count as fully virtualized terrain.

Added separate admission and dispatch timers to the existing paging report.
Native hierarchy tests cover raised/repeated request priorities, shared roots,
retry timing/limits, budget deferral, cancellation and stale ticket rejection.
The latest native test suite and RelWithDebInfo editor build both pass.

## Measurement protocol

Same cached StreamMountain region and camera as the prepared-sector audit,
1280x720, raster VG enabled, RT/GI/POM/clouds/foliage disabled. Native driver
runs for at most 90 seconds with prepared-sector generation and geometry
compilation forbidden. OS file caching is warm. No native builds overlap the
timed runs. Raw files are under `C:/tmp/matter-blas-mountain/` in
`paging-ordered-queue-v1` and `paging-admission-v1`.

The earlier prepared-sector full VG run reported late-window geometry update
means around 138–147 ms. With ordered dispatch alone, the final 120-frame
window measured 58.63 ms update, 40.61 ms admission and 0.038 ms dispatch.
It hit all 2441 prepared sectors and 815 geometry assets, with zero compilation,
but did not finish VG/VT loading within 90 seconds. Its final single-frame
snapshot was 85.28 ms CPU / 27.62 ms GPU, not a stable-render benchmark.

With both changes, the final 120-frame window measured **27.49 ms update,
0.288 ms admission, 0.036 ms dispatch**. Earlier windows were 18–19 ms update;
other costs continue to grow with resident page count. The last single-frame
snapshot was 67.99 ms CPU / 20.12 ms GPU. VT reported **zero queued fills** at
the cutoff (1036 material pages, zero rejected variants/evictions), but geometry
still had 128 in-flight operations, 718 admitted assets and 262089 page entries.
All 2441 prepared sectors and 815 geometry cache keys hit, zero compilation,
zero reported failures; the editor exited cleanly. Full readiness correctly
remained `valid: false` / `completed: false` at the 90-second cutoff.

These streaming windows have different residency progress and are diagnostic
cost comparisons, not an isolated steady-state FPS comparison. Cache key sets
are identical. The screenshot is a qualitative check; coarse terrain artifacts
remain and this does not establish complete virtualized geometry coverage.

## Next structural bottleneck

The terrain compiler grows leaves only across exactly continuous shading
attributes, leaving disconnected chart/material/normal/AO islands as separate
mandatory roots. The runtime approaches its 262144 page-entry ceiling before
all assets fit. Individual publication also creates thousands of renderer
parts and repeated scene/template work. Increasing the ceiling alone is not
the solution to sub-second readiness.

Next: pack spatially nearby exact terminal roots into larger geometry pages,
preserving triangle-corner attributes and coverage, and keep all hierarchy
refinement paths for roots that still have children. Validate aggregate
triangle/material/UV equivalence and end-to-end page counts. Update geometry
and prepared-sector cache policy epochs when the compiled representation
changes. Then measure root loading, scene publication and VT fill completion
separately. Neither <1 second loading nor <10 ms rendering is achieved here.
