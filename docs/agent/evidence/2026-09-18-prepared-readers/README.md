# Bounded concurrent prepared-sector readers

Prepared-sector cache reads previously held one mutex across reference/index
lookup, I/O and validation. Added an experimental 1–8 reader configuration;
keys hash to independently locked PageCache objects sharing one preallocated
bank. Each PageCache ledger is capped at 128 MiB, and bank capacity equals the
sum of those caps. Default remains one reader. Writes have a separate mutex;
PageCache::read_manifest checks committed reference stamps and refreshes the
index before reading, so writer code never mutates a reader object directly.

Native geometry_hierarchy_tests passed, including eight concurrent callers on
four readers and later committed replacement visibility across keys, plus all
existing prepared cache/geometry tests. Logs /tmp/prepared-readers-{build,tests}.log.
Editor build passed after the recurring LNK1201 PDB failure: preserved symbols
as editor-before-prepared-readers.pdb and regenerated them. Python audit syntax
check passed. Audit now accepts --sector-readers 1/2/4/8 and explicitly records
the setting in environment.json. No source/cache identity change or recook.

The one-reader/four-reader scene comparison is recorded below. More readers
cost more reserved RAM and metadata; reduced mutex wait is not sufficient to
establish an end-to-end speedup. Both original performance targets remain active.


## Controlled scene comparison

Both same-build audits passed all strict checks (2441 identities, 815 geometry
assets, no compilation/coverage gaps, 1061 VT hits with zero misses/errors).

| Readers | Reserved payload bank | Sector delivery | Full readiness | Aggregate read mutex wait |
| --- | ---: | ---: | ---: | ---: |
| 1 | 128 MiB | 4.77 s | 11.147 s | 5191.26 ms |
| 4 | 512 MiB | 4.75 s | 10.827 s | 906.67 ms |

Four readers reduced mutex wait by 82.5%, but did not materially improve sector
delivery. Aggregate I/O/validation work rose 2195.95 -> 2473.85 ms, archive decode
1673.30 -> 2045.16 ms, and root lookup 2659.14 -> 3362.31 ms. Worker times overlap;
these sums are not elapsed scene time. Single runs; readiness difference lies
inside recent baseline variability. Default remains ONE reader to avoid extra
reserved memory without a proven end-to-end gain. The opt-in path remains a
measured diagnostic for future pipeline changes, not an enabled speedup.

Full logs: C:/tmp/matter-blas-mountain/prepared-readers-{1,4}-v1. Both full loading
and sustained frame acceptance remain incomplete. Further work must inspect
prepared reconstruction, receiver conversion and publication beyond this mutex.

## Subsequent correctness finding

The faster prepared-classification reuse workload exposed fragmentation in the
original shared bank. See ../2026-09-18-prepared-surface-reuse/README.md. The
implementation now uses one preallocated bank per reader, keeping total capacity
unchanged and eviction domains independent. The measurements above describe the
original experiment, not proof that its shared-bank design was safe under pressure.
