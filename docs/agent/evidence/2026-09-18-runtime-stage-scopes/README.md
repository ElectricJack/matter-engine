# Main-thread geometry stage attribution

Added aggregate ProfileLib scopes for hierarchy adoption, admission, page
streaming, scene completion, assembly, snapshot retention, reclamation/dispatch,
and scene publication. These cover regions omitted by the detailed paging
counters and appear in the existing frame trace and performance tooling.

Native MSVC editor build passed (`/tmp/geometry-stage-scopes-build.log`). The
same 240-camera motion run completed with exit 0, both with and without detailed
paging statistics. UI, RT/GI, clouds and POM remain disabled for this test.
Whole-scene assembly is synchronous; the per-asset hierarchy worker remains on.

The final 512 frames attribute approximately 10 ms to scene assembly, 5.7 ms
to page streaming, and 4.4 ms to scene publication. Snapshot retention is only
0.013 ms and hierarchy adoption 0.18 ms. Disabling detailed paging statistics
alone leaves these costs essentially unchanged. Full movement median was
27.97 ms with statistics and 26.62 ms without; single runs cannot establish
that small difference as a stable improvement. p95 remains about 39 ms.

Per-asset start timestamps were still read with detailed statistics disabled.
They are now conditional too; aggregate scopes remain active. This changes no
selection, resource lifetime, page publication or rendering behavior.

`results.json` records frame intervals over the motion window and aggregate
scope statistics over the final 512 trace frames. These are different windows.
Raw captures are under C:/tmp/matter-blas-mountain/<run-name>.
These cache-miss-permitted movement runs do not prove the all-visible cached
geometry + VT readiness deadline or the 10 ms frame-time target.

## Optional-clock result

Native build passed (`/tmp/geometry-optional-clocks-build.log`) and the identical
no-paging-statistics movement run exited 0. Median was 25.57 ms, p95 37.68 ms,
maximum 76.12 ms, versus 26.62 / 38.87 / 72.54 ms before guarding those reads.
Assembly averaged 9.77 ms versus 10.12 ms over the final 512 frames. The small
change in typical time does not resolve hitching; maximum time was worse in
this single run. Keep this as removal of unnecessary diagnostic work, not a
claim of a large or statistically established performance gain.

Next investigation should split the per-instance assembly prelude (asset/root
readiness, draw overrides and distance policy) from its already-timed snapshot,
CPU cut and packing stages, and address full-scene publication. Resource
retention and hierarchy adoption are not dominant in these traces.

Graft lookup timed out without reporting a savings count; exact source reads
were used as fallback.
