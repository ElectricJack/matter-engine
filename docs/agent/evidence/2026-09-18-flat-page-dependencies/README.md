# Contiguous hierarchy dependency checks

Geometry publication previously probed each asset's ordered page-to-index map
for newly published pages. Assets now keep sorted contiguous page-ID lists;
publication checks intersection by merging sorted lists. The hierarchy worker
still uses a temporary map to construct local child/root indices, then destroys
it on the worker rather than transferring it to the app lane.

Invalidation checks both the displayed hierarchy and the pending replacement's
frontier. Missing child descriptors remain included, and every asset referencing
a shared page is checked. Pending lists are sorted during main-lane snapshot
capture before asynchronous submission. Revision checks and retained resource
ownership are unchanged. No work is skipped on a probabilistic match.

Added geometry_page_dependencies.h with an exhaustive 64-by-64 subset test,
including empty/disjoint sets, first/last matches and duplicates. Native
geometry_hierarchy_tests reports ALL PASS (/tmp/geometry-flat-dependencies-tests.log).
Native editor build passed after preserving/replacing a corrupt MSVC PDB
(/tmp/geometry-flat-dependencies-editor-rebuild.log). A new aggregate scope,
geometry.invalidate_published, exposes the affected work independently.

Matched movement comparison uses moving-ready-cache-v1 and
moving-flat-dependencies-v1 under C:/tmp/matter-blas-mountain, with detailed
paging statistics on and whole-scene assembly synchronous. The per-asset
hierarchy worker is enabled. Reload stress explicitly enables the whole-scene
worker to exercise pending replacements and cancellation.

These movement runs permit cache misses and do not establish the all-visible
sub-second cached loading goal. Frame-time targets remain subject to measurement.

## Movement result

| Metric | Ordered maps | Contiguous lists |
| --- | ---: | ---: |
| Median frame ms | 24.97 | 23.26 |
| p95 frame ms | 36.55 | 35.37 |
| Maximum frame ms | 68.12 | 79.16 |
| Publication ms/motion frame | 3.634 | 2.279 |
| Geometry update ms/motion frame | 15.175 | 13.187 |

Each comparison is one completed run of 959 movement intervals, not a repeated
average. Publication time decreased while worst-frame time increased; this is
an improvement to repeated work, not evidence that hitching is resolved.
The new invalidation scope averaged 0.457 ms per invocation in the final 512
trace frames (325 invocations). No page failures, watchdogs, evictions or bank
reservation stalls were reported. The bank used one backing allocation.

## Reload validation

flat-dependencies-reload-v1 completed exit 0 with both after_reload markers.
Each cycle changed cameras rapidly, reloaded the world, then waited for settled
frames. Whole-scene assembly was explicitly enabled. Final STATS frame times
were 5.90 and 5.48 ms; these are stationary observations and are not substitutes
for movement-frame or visible-loading acceptance. Log:
C:/tmp/matter-blas-mountain/flat-dependencies-reload-v1/editor.log.

Graft lookup timed out without reporting savings; direct source reads were used.
