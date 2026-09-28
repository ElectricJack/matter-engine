# Bounded, synchronized material snapshots

The request-prefix-shared-v2 startup failure had exit 0xc0000374. Its native
WER dump is C:/Users/webde/AppData/Local/CrashDumps/editor.exe.21764.dmp.
CDB resolved engine frames: heap corruption was detected allocating the
renderer material vector, in WorldSession::render. Windows system symbols were
unavailable, so !analyze's WRONG_SYMBOLS label is not a root-cause diagnosis.
The engine stack is preserved in heap-dump-analysis.log.

Code inspection found a definite overrun: the renderer allocated Count()
records, then PackRtForGPU independently read Count() again. Concurrent world
material registration could grow the second count beyond the allocation.
Packing also read mutable registry state without synchronization. The dump
shows detection rather than the offending write, so this is a verified bug
consistent with the crash, not proof excluding other heap-corruption causes.

Both pack APIs now require a capacity in material records and return the
number copied. They serialize snapshots with definition/reset/slot writes
using SRWLOCK on Windows and pthread mutex on Unix. All live engine call sites
pass their actual capacities. Pointer-returning registry APIs still require
consumers to quiesce before reset; packing returns copied snapshots. The live
renderer packs into a fixed stack array sized MATERIAL_MAX_TOTAL and updates
its retained vector only when the snapshot changes, eliminating the temporary
per-frame heap allocation.

Native material_registry_tests passed stale-count canaries for both layouts,
null/zero output, and concurrent reset/definition/snapshot stress. Existing
material schema/flags tests also passed. MSVC editor build passed. Unix C11
syntax check passed; no Unix runtime claim. Logs /tmp/material-snapshot-*.log.

Strict repeated Streaming Mountains audits are recorded below. Performance
acceptance still requires sub-1s complete readiness and sustained sub-10ms
frames; startup stability is also necessary and finite repeated runs cannot
prove absence of all intermittent corruption.


## Repeated scene results

All three strict audits passed, with exit 0 and complete expected sector/geometry
keys, 2441 identity manifest hits, zero geometry compilation, zero coverage gaps,
and 1061 encoded VT hits with zero misses/errors. No startup crash recurred.

| Run | Full readiness | Steady frame interval median | p95 |
| --- | ---: | ---: | ---: |
| material-snapshot-v1 | 11.244 s | 7.837 ms | 12.845 ms |
| material-snapshot-v2 | 10.673 s | 7.746 ms | 12.975 ms |
| material-snapshot-v3 | 10.821 s | 7.976 ms | 12.438 ms |

Runs: C:/tmp/matter-blas-mountain/material-snapshot-v{1,2,3}.
Frame intervals are successive VT trace timestamps after process-readiness+2s,
excluding marker rows; samples 1485/1522/1496. These include whole-loop scheduling,
not isolated GPU execution, and the trace/startup clocks have different origins.
This conservative steady subset still exceeds 10ms at p95, so sustained sub-10ms
is not achieved. Three clean launches support the fix but cannot prove that this
race was the only source of intermittent heap corruption. The deterministic
stale-count canary test proves the formerly unsafe packing interleaving is bounded.
