# Geometry pipeline window experiment

Native MSVC editor, prepared-sector-only terrain audit, GPU-format VT cache,
POM/RT/GI/foliage disabled, unchanged 1 GiB geometry banks and 4 ms / 8 MiB
upload budgets. Single runs, OS file cache not flushed.

| Change | Observed full readiness |
|---|---:|
| 128 in flight, 32 read batch, vector pending upload | 16.080 s |
| 1,024 in flight, 128 read batch, vector pending upload | 14.254 s |
| Same larger window, deque pending upload | 13.186 s |

All three exit 0, publish 17,954 geometry pages, compile no geometry, and
finish with zero rejected/unready/source-fallback geometry assets. All three
strict audits remain invalid due to 25 additional VT detail-page misses; all
1,036 saved VT pages hit. No sub-second loading claim. Window controls are
opt-in; defaults remain 128/32 pending broader measurements.

The prepared queue was a vector erased from the front after every upload.
Changing it to a deque avoids shifting all remaining decoded VkScenePart
objects. Larger-window sampled prepared peak falls from 928 to 804 and
upload-budget-limited frames from 151 to 87. Timing variation and other
overlapping stages prevent attributing the whole wall-time difference to
this one change. No bank backing allocations added during streaming, no
geometry failures, watchdogs, budget deferrals or evictions in these runs.

Decode remains about 3.6–3.7 seconds of work on one preparation thread.
App-thread collection/publication waits and prepared-sector delivery also
remain substantial. Stage totals overlap and per-page waits cannot be summed
into CPU cost. Further investigation should instrument the unaccounted update
work and remove avoidable publication/queue work before raising upload budgets.

Steady GPU samples from the larger-window/vector run, using unique retired
readback sequences after readiness+2 seconds: n=1,406, median 7.343 ms,
p95 9.566 ms, max 12.513 ms, 27 samples >=10 ms. This is GPU total only,
not proof of whole-frame latency or a sustained <10 ms guarantee.

Authoritative full logs, images and traces: C:/tmp/matter-blas-mountain under
the three run names above. Editor build log, audit manifests and weighted
paging reports are copied here.

## Flat vertex deduplication

Replaced the per-page vertex-key std::map with a flat open-addressed index table.
The table compares complete vertex bytes and assigns indices in first-occurrence
order, retaining indexed sharing and deterministic output. Output arrays reserve
capacity before decoding. Cache format and page identities are unchanged.

Native geometry-pages GPU suite passed, including the added per-corner position,
UV, material, normal and tint assertions, CPU/GPU hierarchy comparisons, mixed
cut rendering and RT/BLAS cache coverage. Zero Vulkan validation errors. Native
editor rebuild passed.

geometry-flat-dedup-v1: observed readiness 12.095 s versus deque baseline
13.186 s. Same 17,954 pages published, 1,036 VT hits, 25 extra VT misses,
zero geometry compilation, exit 0, and strict valid=false because of those
misses. Decode worker execution total fell from 3,724 ms to 2,173 ms. These
are single runs; no repeated guarantee or sub-second loading claim. Remaining
publication/queue and prepared-sector delivery delays dominate the gap.

## App-thread stage profiling and event-driven retirement scans

Added feedback, retired-page scan, read acceptance, whole upload-loop,
publication and unchanged-scene comparison timers. They are nested inside
update_cpu; upload_cpu is nested inside upload_loop_cpu. Native MSVC builds
passed and both scene audits exited 0.

The baseline scan examined all GPU pages every frame, even with no detach or
eviction. Collection now runs only after those events and continues while
retired pages remain pinned by submitted frames. Reset explicitly clears the
flag and releases all pages. Reviewed all detach/evict call sites in the runtime.
This fixed-camera run verifies the no-retirement path; it does not independently
exercise a delayed detach/eviction with in-flight frame pins.

| Metric | Instrumented baseline | Event-driven collection |
|---|---:|---:|
| observed readiness | 13.329 s | 13.245 s |
| collection CPU total | 3207.325 ms | 0.763 ms |
| total update CPU | 8766.540 ms / 2614 updates | 4895.733 ms / 2866 updates |
| steady frame interval median | 9.502 ms | 7.878 ms |
| steady frame interval p95 | 11.353 ms | 11.414 ms |
| steady GPU total median | 7.281 ms | 7.522 ms |

Steady samples begin readiness+2 seconds, exclude cache_audit_end marker,
and GPU values deduplicate retired readback sequences. Frame intervals derive
from successive VT trace elapsed_ms timestamps; these include whole-loop
scheduling and may include stalls. Baseline n=1291, changed n=1491; changed
216 intervals >=10 ms and maximum176.592 ms. Therefore sustained <10ms
remains unproven. Single runs, no meaningful loading improvement established.

Both runs still have 25 VT detail misses, 1036 hits, 17954 geometry pages, no
geometry compilation, zero final geometry coverage errors; strict audit false.
Next focus: render-thread read validation/publication and sector delivery,
plus the incomplete VT cook coverage.


## Direct validated-page raster conversion

The raster adapter now validates the immutable node once and reads its binary
position/index/shading sections directly into VkRasterVertex output. Removed
intermediate MeshIndexed positions/indices/TriEx allocations and the runtime's
redundant decode_node call. Output bounds come from validated page metadata,
including when the caller supplies only a page handle. Existing page format,
vertex sharing/order, material mapping, and corruption checks are retained.
This is not yet a GPU-ready geometry disk format: vertex deduplication and
output vector construction still occur at runtime.

MSVC vulkan_smoke_tests and editor builds passed. Native geometry-pages GPU
suite ALL PASS, zero validation errors, BLAS restored=1/miss=1/captured=1;
added page-handle-only conversion equivalence/bounds check. Existing per-corner
attribute assertions and mixed-cut raster/RT coverage passed.

Strict scene audit C:/tmp/matter-blas-mountain/direct-page-adapter-v1 valid=true:
2441 identity manifest hits, 815 geometry asset hits, 17954 pages published,
zero compilation/coverage gaps/evictions, 1061 VT hits with zero misses/errors.
Decode work fell from 2173.026 ms (identity-manifest-warm-v1) to 1323.966 ms,
about 39%, for identical page count. Full readiness was 11.409 s versus 10.830 s;
first cache result arrived at 1.701 s versus 1.020 s. Single instrumented runs
show reduced decode work, not an established whole-scene loading improvement.
Read/validation work 1405.168 ms, upload CPU 557.567 ms; 76 upload-limit frames.
These overlap and must not be added into wall time. Sub-second loading and
sustained sub-10ms frames remain unmet. Next major step remains prepared GPU
geometry payloads plus sector delivery/publication reduction.

Build/test controller logs: /tmp/direct-page-adapter-{build,tests,editor-build,audit}.log.
