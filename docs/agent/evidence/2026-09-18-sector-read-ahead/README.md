# Prepared-sector physical read-ahead experiment

Added opt-in MATTER_PREPARED_SECTOR_READ_AHEAD_MB (0–16 MiB, default0),
plumbed through --sector-read-ahead-mb in the native terrain audit. The
128 MiB preallocated bank and all geometry/upload budgets remain unchanged.
MSVC editor build and Python syntax check passed. All scene audits below
pass strict cache checks with zero VT misses and zero geometry compilation.

| Run | Sector fill | Serialized prepared I/O | Aggregate mutex wait |
|---|---:|---:|---:|
| Prior demand-only baseline | 6.90 s | 2160 ms | 3541 ms |
| 4 MiB read-ahead | 9.14 s | 4683 ms | 24473 ms |
| Same-build demand-only control | 6.68 s | 2093 ms | 3513 ms |

Keep demand-only as default. This does not disprove large contiguous reads;
it shows that generic physical-neighbor prefetch within this small bank and
existing pack layout worsens this workload. No new read-byte counters were
collected, so extra actual disk traffic versus validation/group construction
cost is not yet separated. Do not claim a specific cause from timing alone.
Prefetch cannot force eviction; the demand set contains about 1.6 GB of sector
payloads and exceeds the bank. OS file cache was not flushed.

Next useful direction is batching known requested sectors or preparing explicit
spatial/resolution bundles, rather than enlarging blind prefetch. Persistent
resolved-identity manifests could also eliminate repeated JS evaluation on a
fully prepared scene while retaining source/dependency/version invalidation.
These are remaining implementation work, not completed features.

Full logs and profiles remain C:/tmp/matter-blas-mountain/<run>.
