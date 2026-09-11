# Script voxel sampling

`beginVoxels(spacing)` requests lattice spacing in metres in the baked Part's
coordinate system. A bake expression uses the smallest positive finite spacing
of **all** its brushes: spheres, boxes, ellipsoids, capsules and cylinders,
including difference/intersection brushes. Modifier regions are separate
expressions. A coarse core followed by fine carving therefore gets the same
sampling as an entirely fine expression; a single fine session also works.

Each expression uses one resolution across all its one-metre cells and material
groups. This keeps neighbouring marching-cubes grids aligned. The script mesher
chooses the first permitted power-of-two sample count whose interval is no
larger than requested, bounded to the existing 16–64 samples per axis:

| Requested spacing | Samples per axis | Actual interval |
| --- | --- | --- |
| 0.08 m | 16 | 0.06667 m |
| 0.04 m | 32 | 0.03226 m |
| 0.026 m | 64 | 0.01587 m |
| 0.014 m or smaller | 64 (ceiling) | 0.01587 m |

Sub-centimetre details still need explicit mesh geometry or a future higher
sampling ceiling. A finer request never creates an unbounded lattice. This
change does not alter the live particle Cluster's relative detail tiers.

Previously the script mesher inferred detail only from additive sphere
particles, relative to the first brush's spacing. A Part made solely from boxes
and other analytic primitives stayed at 16 samples per metre regardless of its
authored detail. Engine bake version 9 invalidates those old cached meshes.

The `script_host_tests` suite bakes 100mm planks and 120mm beams, checks finite
bounded geometry, compares fine subtractive-only sessions against entirely fine
sessions, and traces a vertical ray through a shallow groove in the resulting
triangle mesh. Run it from `MatterEngine3/tests`, as configured by CTest.

## Known particle-path meshing warnings

The broad script-host suite's valid particle-path stamping tests already emitted
invalid-vertex warnings before this change. A saved engine-bake-8 executable,
run against the same current shared modules and test working directory, emitted
2,926 warnings and passed. Engine bake 9 emitted 6,005 and also passed: 1,513 in
`test_pf_stamp_paths_positive` and 4,492 across the two bakes in
`test_pf_determinism_double_bake` (previously 744 and 2,182 respectively).
Those tests do not intentionally author malformed input. The finer lattice
increases the warning count from this existing particle meshing defect; passing
assertions do not establish that those particle surfaces are free of holes.
The new thin-timber sampling fixtures emit no such warnings. The particle defect
is not fixed by the sampling correction. Binary hashes and logs are recorded in
`build/qa/castle-grid/voxel-sampling-baseline-comparison.json`.
