# Next work

No native test/editor/build remains running from this task. Latest editor
includes conservative per-node frustum rejection before geometry requests and
emission. Native geometry-pages Vulkan smoke passed, zero validation errors.
Both `visible-refinement-v1` and `visible-refinement-turn-v1` full readiness
audits passed; full goal remains unmet. See README.md for timings/limits.

Next confirmed bottleneck: matter_engine.cpp worker_loop always
commands.pop_wait(...,50,...); execute_sector_stream_step caps active+queued
at stream_worker_count (12). Thus even instantaneous tasks require ~10.2 s
for 2441 requests. Completion-driven wakeup should preserve command priority,
supersession and shutdown rather than busy-spin or speed up unrelated refine
work blindly. CommandQueue lives in async_bake.h/.cpp, wraps evt::Channel;
async_queue_tests.cpp covers timed waits, command delivery and shutdown.

Potential implementation: a coalesced idle wake supported by CommandQueue;
notify on sector executor completion (after decrementing bake_pool_active)
and publication capacity release. Existing real commands must always win.
Do not introduce cancellation tokens or queued fake commands for every wake.
No implementation of this scheduling change exists yet.

The cached-load driver supports --turn-after-ready ex,ey,ez,tx,ty,tz and
--read-ahead-mb. Default remains 4 MiB. Baseline prepared manifest:
C:/tmp/matter-blas-mountain/packed-roots-cook-v1/result.json.
RT/GI/POM/clouds/foliage remain off for this objective's test profile.
