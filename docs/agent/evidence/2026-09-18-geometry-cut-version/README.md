# Versioned geometry hierarchy uploads

The renderer uploaded geometry_cut_words every frame even when the runtime
reused its hierarchy snapshot. Added a renderer hierarchy generation and a
last-uploaded generation on each frame resource. Generation changes on cut
publication/clear and changes to the static/dynamic instance-count header.
Buffer replacement always forces upload; the frame generation advances only
after a successful upload. The shader hierarchy binding is readonly, and each
frame slot's fence owns safe CPU reuse. Camera changes still reach the GPU via
ordinary frame data; this does not freeze GPU LOD selection.

Native geometry-pages GPU tests passed with zero Vulkan validation errors.
Tests explicitly cycle all frame slots, require no uploads for an unchanged
hierarchy, then require changed cuts to upload before dispatch. Existing fine/
coarse CPU/GPU membership, feedback identity, raster coverage, and BLAS cache
roundtrip assertions passed. Logs /tmp/geometry-cut-version-{build,tests}.log.

The editor build encountered the recurring LNK1201 PDB write issue; preserving
editor.pdb as editor-before-cut-version.pdb and regenerating symbols succeeded.
Log /tmp/geometry-cut-version-editor-fresh-pdb.log. No source/data rollback.

Full scene results follow below. This optimization targets recurring frame
work; it does not eliminate prepared-sector decoding or initial GPU uploads.


Strict scene audit geometry-cut-version-v1 passed: 2441 manifest identity hits,
815 geometry asset hits, no compilation/missing keys/coverage gaps, and 1061 VT
hits with zero misses. Full readiness 11.079 s versus 11.009 s in blas-consume-v1.

In the final 512-frame CPU profile, pf.vtslots median fell from 0.390 to 0.164 ms,
maximum 4.948 to 1.122 ms. This region includes other tables, so it is not a pure
geometry-upload timer. Native counter assertions prove skipped uploads directly.

Whole steady frame intervals did NOT improve in this run: median 8.307 ms,
p95 13.979 ms (n=1384, 326 >=10ms), versus 7.883/11.324 ms (n=1512, 195 >=10ms).
Measured from VT trace elapsed times after readiness+2s excluding marker rows;
these are whole-loop intervals, with startup/trace clock-origin caveats. The
trace demonstrates reduced upload-region cost, not a proven overall framerate
increase. Loading and sustained sub-10ms targets remain unmet. Investigate other
prepare-frame/draw submission costs and variance next, alongside prepared-sector
loading; do not use this regional improvement as end-to-end acceptance.

Full logs/traces: C:/tmp/matter-blas-mountain/geometry-cut-version-v1.
