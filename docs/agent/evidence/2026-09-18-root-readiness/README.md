# Cache successful root readiness for an attached geometry lease

The detailed assembly run attributes 3.87 ms per movement frame to instance
setup, ahead of snapshot preparation (2.47 ms), CPU selection (1.30 ms), and
hierarchy packing (1.52 ms). These are motion-window totals divided by update
count, not per-asset averages. Instance setup includes root readiness checks,
asset lookup, transform scale and draw overrides; it does not isolate readiness.

Residency::ready now remembers a successful all-roots-ready result per lease.
Unready assets still check all roots until publication completes. A ready root
cannot be evicted while the lease owns it (Residency::evict rejects root pages),
and detach destroys the lease record. A fresh attachment starts uncached. No
new cross-thread residency access or GPU-resource lifetime change is introduced.

Added lifecycle assertions covering readiness after rejected root eviction,
stale detached leases, and fresh attachments. Existing tests cover shared roots,
publication only after upload, fine-page eviction, and cancellation/reissuance.
Native geometry_hierarchy_tests: ALL PASS, exit 0. Native editor build passed.
Logs: /tmp/geometry-ready-cache-tests.log and
/tmp/geometry-ready-cache-editor-build.log.

Added instance_setup paging telemetry and made snapshot timing include the
snapshot-admission check even when no rebuild is issued. Earlier snapshot
sample counts therefore cannot be directly compared with the new counts;
per-frame total cost remains comparable.

Movement captures use the same 240-camera path and detailed paging statistics
on both sides: moving-assembly-detail-v1 and moving-ready-cache-v1 under
C:/tmp/matter-blas-mountain. Whole-scene worker is off; hierarchy worker is on.
These allow cache misses and do not validate the sub-second cached-readiness goal.

## Matched movement result

Both runs completed exit 0. Across 959 motion intervals:

| Metric | Before | Cached readiness |
| --- | ---: | ---: |
| Median frame ms | 31.80 | 24.97 |
| p95 frame ms | 42.90 | 36.55 |
| Maximum frame ms | 83.67 | 68.12 |
| Instance setup ms/motion frame | 3.871 | 0.298 |
| Geometry update ms/motion frame | 19.084 | 15.175 |

These are single matched runs with the new per-instance timing enabled in both;
they must not be treated as repeated averages or directly compared to the older
no-paging-statistics run. Snapshot, CPU cut and packing cost remained similar.
The isolated setup-stage reduction (~3.57 ms/frame) supports the optimization
independently of the larger, noisier whole-frame improvement. Native tests plus
motion evidence do not establish the full 10 ms or sub-second loading goal.

Graft timed out without a reported savings count; source reads were used as fallback.
