# Repeated camera motion: confirmed storage growth stalls

User observed good settled rates but severe spikes when looking/moving. Ran
C:/tmp/matter-blas-mountain/moving-profile-v1 after warmup: 240 camera positions,
one each four frames, full rotation plus a short 12m/8m translation loop. Captured
profiler immediately after movement so its 512-frame tail covers movement.
Exit 0. This is an exploratory workload with cache misses permitted, NOT a
cache-only acceptance test. No UI (same performance profile), RT/GI/POM off.

Direct evidence in editor.log:
- CPU raster_staging_growth vertex 1073741768 -> 1610612608 bytes, live
  1073753032 bytes, **274.327ms**.
- STATIC CAPACITY OVERFLOW at vertices 1073753032/1073741824 bytes; 43273 parts,
  12201739 staging vs 12201735 live vertices: almost no free space. This is live
  growth, not just fragmentation. Triggers full O(world) GPU scene rewrite.
- Earlier startup crossed the initial 512MiB GPU reservation too.

Render-lane profiler tail: pf.static max334.96ms; build max348.17ms and
build.prepare_frame max347.74ms (nested scopes, do not add). Across 512 frames,
pf.vtslots total1799.6ms/max9.8ms, pf.flushtmpl total1629.5ms/max8.6ms,
pf.commands total1057.6ms/max6.4ms. These require attribution in addition to the
large growth spikes. Whole-run VT frame intervals include startup; worst693.84ms
is not asserted to be a movement-only measurement.

Conclusion: upfront CPU staging fixed the first stationary-view relocation,
but the motion path exceeds its 1GiB capacity and GPU reservation. Increasing
reservation alone is not a bounded streaming architecture. Next work must account
for source receivers plus virtual pages, evict/reuse bounded allocations before
growth, and reduce per-frame command/template preparation as residents change.
No sub-second or moving sub-10ms acceptance claimed.

Movement-window frame intervals (trace serial >2659): n=959, median=41.794ms, p95=73.237ms, max=693.839ms, frames>=10ms=947.
Boundary uses last retired geometry feedback serial before movement_start; it
may include a few boundary frames. Frame intervals include the whole loop.
