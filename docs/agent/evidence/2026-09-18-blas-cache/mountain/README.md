# StreamMountain paging measurements

Sequential native RT/GI runs at 1280×720, fixed camera, three detailed rock
assets. Geometry budget 256 MiB; CPU payload budget 128 MiB. No editor was open
before these owned runs. Each run exited successfully and closed its editor.

## Isolated BLAS comparison

`profile-warm` and `profile-build` both enable CPU hierarchy snapshot caching.
Only the BLAS disk cache differs. Logs contain 12 completed 120-frame windows
(the initial window is partial). Times below are weighted means over completed
samples, except frame time, which is the median of five geometry-settled STATS
samples. Samples still had pending VT work; these are not fully idle scene runs.

| Measurement | Warm BLAS restore | Direct triangle BLAS build |
| --- | ---: | ---: |
| Published geometry pages | 7,263 | 7,263 |
| Geometry-runtime update, ms/frame | 28.738 | 28.111 |
| Page-cache batch call, ms/batch | 0.438 | 0.430 |
| Decode/renderer-part prep, ms/page | 0.111 | 0.110 |
| Accepted upload → render ready, ms/page | 229.356 | 106.757 |
| Request → publication, ms/page | 425.089 | 384.295 |
| Geometry-settled frame median, ms | 61.15 | 59.76 |

The warm cache restored every BLAS with no misses/rejections. It did not show a
whole-scene speedup in this pair. Waiting for an asynchronous BLAS lookup adds
frame boundaries; readiness latency includes lookup, command submission and
fence retirement, not just GPU execution. No build-vs-restore threshold is
chosen from this single pair.

Read time includes OS-cached storage operations and validation. It is not a
cold-drive bandwidth benchmark. The warm geometry run read 130,424,416 bytes in
5,827 storage operations across 7,260 page requests (three roots were pinned
already). Both runs had zero CPU budget deferrals, reservation stalls, read
failures and watchdog expirations. The eight-page upload limit was reached with
remaining work on 821 warm-run frames and 876 direct-build frames.

In the final warm windows, with no geometry I/O or publications, runtime updates
still averaged 24–26 ms/frame. The next instrumentation revision splits this
into CPU cut selection, snapshot construction, hierarchy packing and cut-data
submission. These stage totals overlap with update time and must not be summed.

`cold`, `warm`, and `direct` precede stage instrumentation. Their direct run
also disabled snapshot caching, so use them only as an initial diagnostic,
not an isolated BLAS A/B. Raw progress/timelines and weighted profile JSON are
stored in each run directory. Screenshots/raw complete logs remain under
`C:/tmp/matter-blas-mountain/`.

## Final finer-stage validation

`profile-stages` uses the final instrumentation build, exits successfully, and
publishes all 7,263 pages with zero read failures, watchdogs, budget stalls or
evictions. Its shorter timeline does not provide a fully geometry-settled
120-frame profile window; do not treat its aggregate as a steady-state or
matched A/B result. The last window includes the last 264 publications.

Across 1,063 profiled updates, mean app-lane time per update was:

| Stage (total divided by update count) | ms/update |
| --- | ---: |
| Whole geometry update | 29.34 |
| CPU LOD cut selection | 7.36 |
| Hierarchy descriptor / RT proxy packing | 5.06 |
| Invalidated snapshot rebuilding | 4.72 |
| Renderer cut-data admission | 2.66 |

The sub-stages are contained in whole-update time. Other update work remains
outside these sub-stage timers. This identifies recurring CPU hierarchy work
as a material cost; it does not establish CPU work as the sole frame bottleneck.
Whole-scene GPU times remain substantial. Batch page reads averaged 0.434 ms,
and decode/renderer-part preparation averaged 0.114 ms/page.

Run `python3 tools/geometry_paging_report.py <controller.log> --json` to reproduce
the weighted summaries. Two report tests pass for weighted aggregation, maxima,
queue gauges and empty samples. The native geometry hierarchy suite passes,
including the new displacement/receiver tests. The final editor build includes
all stage counters; validation sessions are closed.
