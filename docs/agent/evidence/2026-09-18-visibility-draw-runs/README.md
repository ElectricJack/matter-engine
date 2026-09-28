# Shared opaque indirect draw batching

The G-buffer pass coalesced touching opaque part ranges, but visibility-ID
recording emitted a separate vkCmdDrawIndexedIndirect for every part range.
The geometry-cut-version-v1 final CPU trace had median cull.vis_dispatch 1.264ms
versus raster.draw_static 0.035ms. Both passes bind common pipeline/descriptors
and vertex/index buffers before iterating ranges. visibility_id.vert addresses
transforms using gl_InstanceIndex (including firstInstance), not gl_DrawID.

Added render/indirect_draw_runs.h and used its order-preserving traversal in
both passes. It rejects water/out-of-bounds/empty ranges, merges only touching
accepted ranges without sorting, and splits final runs to maxDrawIndirectCount.
Debug part_slot identifies the first range of a merged run, as in the previous
G-buffer path. No shader/geometry/material/selection data changes.

Native indirect_draw_runs_tests passed exact filtered command-order and
multiplicity comparisons for 1000 deterministic randomized cases, explicit
water holes, and near-UINT32_MAX merging/splitting. Native GPU and scene results
follow below. Build/test logs /tmp/indirect-runs-{build,tests}.log and
/tmp/visibility-runs-*.log. Both performance goals remain unproven.


Native geometry-pages and water-field GPU suites both ALL PASS, validation
errors 0. Existing mixed-cut raster/RT membership, feedback, BLAS cache, water
exclusion and device draw-limit tests remained correct. Editor build passed.

visibility-runs-v1 strict cache audit passed with expected keys, zero compilation,
zero coverage gaps, 2441 identity hits and 1061 VT hits with zero misses. Full
readiness 10.180s. Final 512-frame profile: cull.vis_dispatch median 0.110ms
(max 0.234), draw.cull_render median 0.467ms (max 0.975). Previous run medians
were 1.264ms and 1.697ms respectively. Whole steady frame intervals n=2266,
median 5.026ms, p95 7.764ms, 22 intervals >=10ms. Steady extraction uses VT
elapsed timestamps after process-readiness+2s and excludes marker rows; clock
origins differ, and these include whole-loop scheduling. This is encouraging
but does not prove every frame is <10ms or arbitrary-camera performance.
Sub-second loading remains unmet. A repeat is recorded below.


Repeat visibility-runs-v2 also passed all strict cache/coverage checks. Full
readiness 11.865s. Frame intervals n=2240, median 5.066ms, p95 8.868ms,
58 >=10ms. cull.vis_dispatch median 0.113ms/max 0.218ms; draw.cull_render
median 0.453ms/max 1.136ms. Both measured steady p95 values are now below 10ms,
but outliers remain and no moving-camera gate is claimed. Loading remains
roughly 10–12 seconds. Next work should prioritize prepared-sector I/O and
reconstruction without sacrificing this reduced draw-submission overhead.
Full evidence: C:/tmp/matter-blas-mountain/visibility-runs-v{1,2}.
