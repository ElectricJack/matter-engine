# Completion-driven streaming wakeup

The world coordinator now wakes when bake workers or publication slots finish, instead of waiting for its 50 ms fallback timeout. Real commands and shutdown retain priority; idle wakeups coalesce without cancelling commands. Unused reserved publication slots do not trigger wakeups.

Native async queue tests passed, including parked-consumer wakeup, coalescing, command priority, and shutdown. RelWithDebInfo editor build passed.

## Prepared-cache reopen

Cached sector fill fell from 12.94 s to 6.01 s. Sampled worker utilization rose from 43% to 90%. These are individual runs, not a statistical benchmark.

## Full terrain load

The audit passed with zero geometry compilations. Geometry streaming first reported idle for all 775 assets at approximately 13.82 s after process launch; final VT demand drained at 29.92 s. The geometry timestamp is aligned to the preceding one-second VT poll, not an exact event timestamp. Complete readiness was recognized at 30.91 s.

Final VT state: 1,036 resident pages, 775 pinned tails, zero pending fills, zero evictions, zero rejected variants. Thus VT continued catching up for roughly 16 seconds after geometry streaming finished. Source tileset caching does not imply that composed receiver VT pages are persisted. The remaining texture delay needs preparation/composition/upload timing before selecting a fix.

Raw artifacts: C:/tmp/matter-blas-mountain/completion-wake-{reopen,load}-v1/. Expected cache manifest: C:/tmp/matter-blas-mountain/packed-roots-cook-v1/result.json.

The sub-second complete load and sustained sub-10 ms frame goals remain unmet.
