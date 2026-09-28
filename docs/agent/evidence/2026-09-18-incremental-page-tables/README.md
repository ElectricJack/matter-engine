# Incremental renderer tables on part registration

New geometry-page registration previously marked all VT routes, draw overrides,
and occlusion classes dirty. Registration now preserves clean tables and updates
only the new slot/range. VT registration that creates a real variant still marks
the full routing table dirty; activation, release and global material/override
changes retain existing global invalidation. Neutral VT entries clear reused
cluster spans as well as appended storage. Capacity padding remains initialized.

Occluder classification is shared between incremental and full rebuilds, preserving
alpha-tested/thin-walled exclusions and the neutral class for unknown materials.
Overrides use the same sorted hash lookup and neutral defaults as full rebuilding.

The existing pf.vtslots scope includes geometry hierarchy upload and other tables,
so its full duration must not be reported as VT routing cost alone.

Measurement keeps MATTER_GEOMETRY_SCENE_ASYNC=0, matching the faster comparison
profile while background whole-scene publication is still being developed.

Culling suite ALL PASS including occlusion exclusions, zero Vulkan validation
errors (/tmp/incremental-page-tables-cull.log). VT suite ALL PASS, zero validation
errors (/tmp/incremental-page-tables-vt.log). Editor build passed after preserving
and replacing the PDB that hit LNK1201.

Isolated movement median35.358ms/p95 48.603ms/max93.127ms; earlier same profile
32.426/49.373/82.863ms. No whole-run improvement established. Last512-frame
pf.vtslots mean0.795ms versus3.009ms previously; prepare_frame5.146ms versus7.128ms.
The local table-stage reduction did not resolve geometry.update, still28.35ms in
the profiler tail. Paging analysis during movement shows hierarchy bookkeeping/
assembly7.43ms per frame, publication3.88ms, cut encoding3.73ms, snapshot capture
1.99ms, CPU selection1.47ms. Scopes overlap and must not be added as independent
frame totals. Next action: eliminate per-selected-node tree lookups for LRU touches.
