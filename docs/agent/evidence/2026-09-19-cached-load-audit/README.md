# Cached terrain + VT loading after command-prefix optimization

Native MSVC editor, StreamMountain, 1280x720, raster virtual geometry and VT;
POM, RT/GI, clouds and vegetation disabled in the audit profile. The editor
completed the cache-only load audit with exit 0. No compilation or cache miss
occurred: 815 geometry hits, 2441 prepared identity manifest hits, and 1061
GPU-format VT hits. No rejected/failed VT pages were reported.

## Observed timings

All times below start at process launch and include editor startup. These are
first reported component milestones, not a combined visible-readiness gate:

- First cache event: 1.156 s.
- Current visible sectors (251) have no pending work: 3.003 s.
- VT stats first report an active cache with no queue/rejections: 7.264 s.
- Matching camera/cut GPU feedback first reports no requests, overflow or
  refinement fallback: 8.387 s.
- Existing stricter global-idle gate: 9.247 s, confirmed after 15 s stability.
- Final stationary STATS frame time: 6.83 ms; not a movement result.

The one-second target remains unproven and this run does not meet it. Cache
validity is distinct from deadline acceptance; `valid: true` in the audit means
cache correctness, not performance success. VT stats are polled approximately
once per second, so their milestone is not a fine-grained completion timestamp.

## Next bottleneck

Page-cache disk requests totaled 371.8 MB across 2154 reads. One read batch took
1117 ms. Main-lane geometry update timers totaled 3458 ms over the entire run;
worker decode totaled 1271 ms. These overlap and must not be summed into a
single wall-time estimate. The sampled prepared-upload queue peaked at 739
pages; 62 frames reached an upload limit. No GPU/bank budget stalls, watchdogs,
page failures or evictions occurred. One bank backing allocation was used.

Code inspection found a scheduling mismatch: Residency::QueueKey always sorts
root pages ahead of refinement pages, and GeometryWorldRuntime submits GPU
feedback with default priority zero. Neither applies current-frustum priority
to geometry page dispatch. Prepared sector prioritization alone therefore does
not ensure visible fine geometry precedes offscreen root uploads. Next work
should establish current-view priorities through dispatch and upload admission,
with explicit handling of stale requests when the camera changes.

The final screenshot was inspected and shows textured terrain; it does not
prove visual seam quality, full-resolution selection or loading deadlines.

Raw run: C:/tmp/matter-blas-mountain/command-prefix-cache-load-v1.
Controller: /tmp/command-prefix-cache-load-controller.log.
Graft lookup timed out without reporting savings; direct reads were used.
