# Prepared surface reuse and reader bank pressure

Trusted prepared-sector loads now reuse saved surface weights/field lanes when
hash, material/field counts, LOD row counts and exact vertex array lengths match.
Other build paths retain classification; incomplete cached data recomputes.
No cache format/key changes. Native geometry_hierarchy_tests passed classification
shape coverage; editor build passed. Audit reports prepared_surface_reuse.

Single reader: C:/tmp/matter-blas-mountain/prepared-surface-reuse-v1 passed,
815 reused classifications, full readiness 12.025 s, no missing expected assets
or geometry coverage gaps. Last sampled prebuild mean dropped from ~8.5 ms to
~0.3 ms per task; total loading remains far above the sub-second target.

Four readers: prepared-surface-reuse-readers4-v1 FAILED with a payload unavailable
error. Diagnostic run prepared-reader-diagnostic-v1 also FAILED and identified
BudgetExceeded: bank occupied 394961664 B, largest free 2804480 B, 925 active
allocations in a 512 MiB shared bank. Another rejection had occupied 364938496 B
and largest free 2821632 B. These are correctness failures, not speed results.

Independent caches could evict only their own entries from the shared address
space, leaving stranded free holes between other readers' allocations. Readers
now each reserve their own bank at construction with unchanged aggregate capacity.
No runtime backing-bank allocation. Added mixed-size residency-pressure coverage
and descriptive PageStatus/bank diagnostics. Validation of this fix follows below.

## Isolated-bank validation

Native geometry_hierarchy_tests ALL PASS, including mixed-size pressure and
concurrent read/reference replacement cases. Editor rebuild passed. Logs:
/tmp/prepared-isolated-banks-{build,tests,editor-build}.log.

Both strict four-reader scene audits passed, each with 2441 manifest identities,
815 reused classifications, zero runtime compilation or geometry coverage gaps,
and no payload failures. Full paths under C:/tmp/matter-blas-mountain/.

- prepared-isolated-banks-v1: ready 11.026s; steady whole-loop interval median 5.387ms, p95 9.486ms, n=2044, >=10ms 75
- prepared-isolated-banks-v2: ready 10.712s; steady whole-loop interval median 5.255ms, p95 9.525ms, n=2138, >=10ms 79

Frame intervals use elapsed_ms differences after ready+2s, excluding markers.
They measure the whole loop, not isolated GPU rendering. Fixed-camera, OS-warm,
profiled runs; neither moving-camera stability nor sub-second loading is proven.
The reader default stays one: this repair enables safe experimentation but does
not demonstrate an end-to-end speedup beyond baseline variability.

Run v1 still spent aggregate 1.46s in page reads/validation and 1.49s in geometry
decode, plus 0.68s completion acceptance, 0.64s upload loop and 0.75s publication.
Stages overlap; do not add these as wall time. Largest single upload was 234ms.
Next focus is prepared data delivery and render publication, including that spike.
