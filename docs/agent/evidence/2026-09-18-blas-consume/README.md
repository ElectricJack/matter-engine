# Transfer prepared CPU geometry during publication

Prepared-sector commit previously called BLASManager::adopt_from, which copied
triangles, shading, BvhMesh arrays and prebuilt BVH nodes/indices onto the render
thread immediately before destroying the staged originals. Added consume_from
for disposable worker managers and wired PartStore::commit_staged to it. Unique
entries retain all original allocation addresses. Duplicate entries add the
complete staged reference multiplicity to the resident entry. Caller-owned
handles are patched through the existing remap. Staged is emptied and marked
dirty; existing non-destructive adopt_from remains available with its original
contract. Self-consumption does not mutate the manager.

Added blas_refcount_tests to native CMake CPU targets. Tests passed exact entry,
triangle, mesh, BVH node and index allocation preservation, changed handle IDs,
duplicate/new reference multiplicities through final release, and staged manager
reuse/self-consumption. Existing non-destructive adoption and material-aware
dedup tests passed. Native logs /tmp/blas-consume-{build,tests}.log.

Editor link initially failed LNK1201 writing editor.pdb; retry also failed with
ample disk space and no editor/link/compiler processes. Preserved the old PDB as
MatterEditor/build/windows-msvc/editor-before-consume.pdb, then regenerated it;
build passed (/tmp/blas-consume-editor-fresh-pdb.log). No engine data/cache reset.

This removes render-thread copies at publication, not worker-side prepared
archive decoding or GPU transfers. It does not by itself prove sub-second load
or sustained sub-10ms rendering. Full scene validation follows below.


Strict StreamMountain audit C:/tmp/matter-blas-mountain/blas-consume-v1 passed:
2441 identity hits, 815 geometry cache hits, zero compilation, complete expected
keys and zero coverage gaps, 1061 VT hits with zero misses/errors. Full readiness
11.009 seconds. Steady trace frame intervals (readiness+2s, non-marker rows,
n=1512) median 7.883 ms, p95 11.324 ms. Single run; prior 10.67–11.24s readiness
range overlaps this result, so no whole-scene speedup established. Native tests
prove removed allocation/copy work; rendering and cache checks prove this path
still serves the fixed-camera scene. Both acceptance targets remain unmet.
