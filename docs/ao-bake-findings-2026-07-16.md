# Baked Part AO: Seam Root Cause & Rollback Findings (2026-07-16)

Status: the baked-AO feature is being **rolled back** from `feature/rt-lighting-phase2`
(overlaps with the in-flight triangle-soup → indexed-mesh format change). This
document records what we learned so the next attempt does not rediscover it.

## Symptom

After the Vulkan+RTX port, baked ambient occlusion showed **dark seams at
chunk/cluster boundaries**, visible as thin black lines. Key observation
(Jack): the darkening is **one-sided per seam** — "like it's picking up the
hollow geometry there on the inside" — on boundaries that are fully exposed
and should not be dark at all.

## What was tried first (necessary but insufficient)

Cross-cluster boundary canonicalization in `part_flatten.cpp`: shared-position
vertices at cluster borders get averaged (canonical) normals and AO, applied at
L0 and re-applied after every QEM ladder level, on both the classic and
segmented flatten paths (`kFormatVersionFlat` 7→8→9). This work was **verified
correct** — probes showed `|dAO| = 0.00000` at every LOD level across cluster
boundaries in the final world flat — and the seams **persisted**, because the
flat was now faithfully carrying black *source* data. Consistency across
clusters was never the root cause.

## Root cause (confirmed by ray forensics)

The black AO is produced by the **per-part AO bake itself**
(`MatterEngine3/src/part_ao_bake.cpp`, hooked in `script_host.cpp` just before
`save_v2`). Instrumented re-runs of the exact bake rays (`ao_at`) on black
vertices of real cached `.part` artifacts showed:

- Black vertices (`ao < 0.05` with up-facing normals) sit **exactly on integer
  part-local planes** — voxel *cell* boundaries (`Cell::build_cell_meshes` in
  MatterSurfaceLib meshes each cell independently; script_host registers each
  cell's groups into the part's BLAS soup).
- At these planes the part's mesh soup contains **coincident geometry** that
  the AO rays hit at t ≈ 0.001–0.01 (with occlusion distance-attenuated over
  radius 2.0, a t≈0 hit counts as ~full occlusion → AO ≈ 0):

  1. **Back-to-back interior closure walls.** Example vertex at part-local
     x = −2.0000: 61/64 rays hit the *frontface* of an opposite-facing wall at
     t ≈ 0.008 (a sheet straddling the plane, geometric normal pointing into
     the ground). Each cell's mesher closes its own volume at the cell border
     even when the solid continues into the neighbor; the walls are invisible
     to rendering (inside the solid) but fatal to AO rays from surface
     vertices on the plane.
  2. **Near-duplicate overlapping sheets.** Example vertices on z = −1.0000:
     57–58/64 rays hit the *backface* of a near-twin of the vertex's own
     triangle at t ≈ 0.001. Adjacent cells mesh the same boundary strip with
     slightly different vertex placement (per-cell grids can differ in
     division/detail), yielding two almost-coplanar copies of the surface.
     Also observed on isolated rock parts as overlapping slivers /
     T-junction geometry from the mesher.

- **One-sidedness explained:** the wall/duplicate belongs to one cell's mesh;
  the *other* cell's surface vertices on the plane are the ones occluded. The
  AO bake welds by (position, normal), so an exposed vertex's rays are traced
  with the correct exposed normal and still die instantly on the coincident
  geometry.

Affected scale: dozens of ground-sector parts with 30–90 black up-facing
corners each, always on the boundary planes — i.e. every region seam.

## Verified mitigation (not landed)

Filtered AO tracing in `ao_at`: reject **backface hits** AND hits **closer
than ~1% of radius**, re-tracing past rejected hits (both filters are
required — the back-to-back wall pair presents a frontface first, then a
backface). Measured recovery on real culprit vertices:

| vertex | shipped AO | backcull + t_min re-trace |
|---|---|---|
| rock crease (sliver overlap) | 0.003 | 0.98 |
| sector wall vert (x=−2) | 0.06 | 0.67 |
| sector duplicate sheet (z=−1) | 0.05 | 0.52 |

Any bake-semantics fix must bump `kEngineBakeVersion` to invalidate cached
`.part` files.

## Architectural findings (relevant to the indexed-mesh work)

1. **Nothing in the pipeline ever computes normals or AO on the assembled
   mesh.** Normals are computed per voxel cell at meshing time
   (SDF-gradient, welded only within the cell); AO was baked per part;
   flatten (`Gatherer::gather`) only *transforms* inherited normals
   (`nm.apply`) and merges. Hard shading at cell/part boundaries (e.g. the
   flat-shaded look on trees at junctions) is structural: no stage smooths
   across connected edges of the assembly. The cluster-boundary
   canonicalization was a band-aid over this.
2. **The recommended future home for both smooth normals and AO is the
   flatten merge stage**: position-weld the merged world-space mesh,
   recompute area-weighted normals with a crease-angle threshold, and bake
   assembly-context AO there (with the filtered tracing above — the closure
   walls remain in the soup either way). Bonus: per-part bakes drop their
   32M-ray AO budget entirely, directly reducing world bake time; and
   assembly AO gets contact shadows (rock-on-ground) that per-part AO can
   never produce. Cost: flatten-time AO over the whole soup; inlined scatter
   instances get traced per placement. Instanced children (`FlatInstanceRef`,
   e.g. 480 tree branches) are not inlined, so cross-instance junction
   smoothing/AO stays a separate problem.
3. **The mesher emits redundant geometry at cell boundaries** — interior
   closure walls and near-duplicate boundary sheets. Beyond AO, this is
   wasted triangles and a latent z-fighting/welding hazard. The indexed-mesh
   conversion is the natural place to address it: a position-welded indexed
   representation surfaces these duplicates immediately (non-manifold edges,
   coincident faces). Options: neighbor-aware cell meshing (skip faces
   against solid neighbor cells) or post-merge interior-face removal.

## Rolled-back change set (for future reference)

Reverted commits: `f3d3cbc` (part_ao_bake baker), `5e6cf4e` (bake hook +
schema `ao.quality` + `kEngineBakeVersion` salt), `cd16497` (ladder AO test),
`0298bf2` (MSL reproject_triex AO interpolation), `2b7be10` (Windows build
list), `f044508` (ray budget on unique welded vertices), `fb11867`
(bake version 2). Discarded uncommitted: segmented-flatten AO
canonicalization + `kFormatVersionFlat` 8→9, MSL `mesh_transform.cpp` AO
reprojection rework, AO test additions, `tests/flat_ao_probe.cpp` diagnostic.
`TriEx.ao0/1/2` and the Vulkan plumbing that consumes them predate the
feature and remain (values default to 1.0 = unoccluded).

Diagnostics worth recreating when this returns: a flat-artifact AO probe
(per-cluster stats, cross-cluster pair compare, AO-vs-distance band profile
around a plane) and a `.part` scanner for black up-facing corners; both were
scratch tools driven by `load_flat_v3` / `load_v2`.
