# Fixed MemoryLib bank and CPU page-cache integration

## Implemented

MemoryLib owns the reusable fixed-range allocator. AssetStore page-cache payloads
use synchronized bank leases; geometry streaming creates its bank at runtime
construction and retains it across directory/cache changes. Disk payload bytes
are read directly into the lease, without a staging copy or per-batch arena.
Geometry allocations round up to 256 bytes. Backing capacity is configured by
`MATTER_GEOMETRY_CPU_MB`; default generic PageCache allocation quantum is 8 bytes.

The allocator preallocates payload and range metadata, rejects exhaustion, and
supports externally owned storage or offset-only GPU regions. GPU resources are
not yet wired to this allocator. Live immutable handles retain their leases
through cache destruction; final release returns space to the bank.

## Validation

- Linux MemoryLib C/C++ suites passed with ASan/UBSan. A linker-interposed test
  ran 100,000 randomized bank operations: zero malloc/calloc/realloc/free calls
  during acquisition/release and no overlapping live ranges.
- Native Windows MemoryLib tests passed, including alignment, metadata capacity,
  exhaustion, fragmentation, stale/foreign leases, external storage and reuse.
- Native AssetStore tests: **577 checks, zero failures**, covering shared banks across cache reopen, pinned range
  pressure, address reuse, cross-thread retirement and bounded reads with guards.
- Geometry hierarchy CPU tests: ALL PASS.
- Paging report tests: 3 passed. Native editor build succeeded.

## Streaming Mountains run

`perf.json`, `progress.jsonl`, `profile-summary.json`, and `result.json` capture a
1280x720 fixed-camera run with a 128 MiB CPU payload budget, 256 MiB geometry GPU
budget, native RT/GI and POM disabled. The editor exited normally after 216.83 s.

- 7,263 pages published/resident; 7,263 BLAS restores, zero cache misses.
- CPU bank capacity: 134,217,728 bytes; backing allocations: **1** throughout.
- Final/peak occupied payload capacity: 129,720,832 bytes (~123.71 MiB).
- Largest free range at end: 4,496,896 bytes (~4.29 MiB).
- No sampled CPU budget deferrals, read failures, watchdogs or GPU budget stalls.
- Vulkan validation errors: **0**.
- 144 measured frames: median **53.1335 ms**, p95 **64.201 ms**.

This is a bank-integration check, not proof of a frame-rate improvement or the
<10 ms goal. One static geometry upload occurred during the sample, so it is
not a strict steady-state comparison. The camera did not travel far enough to
exercise runtime eviction; small-bank automated tests cover pressure and reuse.
Bank figures exclude root-cache banks, decoded meshes, BLAS disk-cache payloads,
C++ containers, upload staging and GPU memory. Those domains still need migration.
The current scheduler still has its old per-frame upload-count limit.

Full raw controller/profile traces and the launcher are in
`C:/tmp/matter-blas-mountain/bank-payload-perf` and
`C:/tmp/matter-blas-mountain/perf-bank.py`; concise evidence is retained here.
`source-hashes.json` identifies the relevant implementation used by the build.
