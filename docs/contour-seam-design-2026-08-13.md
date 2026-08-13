# Seams by shared contour: no overlap, no welder

Decision taken 2026-08-13, from issue `736f92da` ("I really hate these
overlapping strips with a passion ... We need a solution that doesn't use
these") and the design review that followed. This supersedes
`volumetric-sectors-design-2026-08-10.md` §4.1 (the runtime fan + overlap band)
if the prototype gates below go green.

## The requirement, as stated

**No overlapping polygons anywhere, at any scale, including coplanar.** Not "no
macroscopic double cover" — coplanar overlap produces texturing artefacts, so
machine-scale slivers are also out. That single constraint is what selects the
design; everything below follows from it.

It rules out, immediately:

* the **overlap band** (M0-WP7) — literal coplanar double surface, measured at
  0.31–0.47 fine voxels of interpenetration, and the direct cause of the weld's
  RT self-shadowing (`docs/seam-suite-2026-08-13.md` finding 2);
* any scheme that puts **two polylines on one plane and zippers them**. Two
  coplanar curves that cross each other produce folded slivers, and no float
  triangulation avoids that. This killed the first corrected version of the
  proposal.

## The construction

**One canonical contour per shared plane, computed independently by both sides,
with every tile's border cells terminating on it.**

* **Canonical rung `rc` = 0** (the 2 m voxel), globally, for every face plane in
  the world. See "why the resolution is forced" below.
* **The face contour** is marching squares on the plane at rung `rc`: for each
  canonical lattice edge lying in the plane whose density changes sign, one
  vertex at the linearly interpolated crossing. This is a function of (plane,
  field) alone — no neighbour, no tile — so two tiles sharing a plane compute it
  bitwise identically, and the tile's bake identity stays `(tile, rung, field)`.
* **Interior cells** mesh as today: surface nets, one dual vertex per mixed-sign
  cell, placed at the centroid of its edge crossings.
* **Border cells** (those touching a face plane) fan their dual vertex to the
  contour segments crossing that cell's face footprint. A coarse tile's border
  cell spans `2^L` canonical cells per axis and fans to all of them.

Across a shared plane, each contour segment therefore receives exactly one
triangle from each side, forming a tent over it: shared edge, opposite sides, no
overlap. The union of every drawn tile is one manifold with shared vertices and
each triangle emitted once.

**Tile edges and corners need no separate mechanism.** A face contour terminates
where it meets the face square's boundary, and that boundary line is a row of
the canonical lattice on *both* adjacent faces (every tile bound is a multiple
of the canonical voxel), so both faces independently compute the same crossing
point on it. The same statement one dimension down covers corners. There is no
cross-resolution reconciliation anywhere, on any tier.

That last point also disposes of a live hazard: `restrict_levels`
(`MatterEngine3/src/sector_streamer.cpp:756-769`) probes the **six faces only**,
so edge- and corner-diagonal neighbours may legally sit *two* levels apart. Any
scheme that reconciled resolutions at tile edges would have to span 4:1, where
the current face welder rejects 2:1 outright (`seam_weld.cpp:62-67`). With a
single canonical contour there is no mismatch to span, at any tier.

## What this deletes

The seam is closed **at bake, by construction, for every pair** — including
equal-level ones, which route through the contour like any other. So there is
nothing left for a runtime welder to do:

* `rebuild_weld_pair`, the pair pool, `WeldPairKey`, the fan, the cap, the
  overlap band, `seam_weld.{h,cpp}`, `seam_boundary.h`'s FaceRecord export;
* weld parts in the renderer and their exclusion from the RT TLAS
  (`VkSceneInstance::ray_traced`), the CPU tracer exclusion in `ensure_tracer`,
  and the weld branch in `resolve_vulkan_instances`;
* the per-publish weld fan-out and its churn counters.

A subsystem is removed rather than replaced. Keep `seam_suite.sh` and SeamLab —
they become the acceptance gate for the new mesher.

## Why the resolution is forced to the finest rung

For both sides to produce the same curve without knowing each other, the
contour's resolution must be a rule each can evaluate alone. Four candidates,
three dead:

| rule | verdict |
|---|---|
| the coarser neighbour's rung | needs neighbour knowledge — the retired `edge_mask`, which put a neighbour guess in the bake identity and was wrong through every split, merge, park and pending bake |
| derived from the plane's world coordinate (the largest power of two dividing it bounds the coarsest legal occupant) | agrees without negotiation, but degrades half of all faces on a fixed dyadic lattice, camera-independent, forever |
| each side at its own rung, then zipper | coplanar bowties where the two curves cross — violates the requirement |
| **one global constant** | agrees everywhere; no reconciliation on any tier |

And the constant is forced to be the **finest** rung: the design doc's own
argument (`volumetric-sectors-design-2026-08-10.md:226-233`) shows a rule `f`
with `f(L) = f(L+1)` for every legal pairing must be constant, and the constant
has to serve the finest tile that can ever touch a plane, which is level 0.

Note what that argument does *not* reach, and why this design is not the one it
kills: it is about **surface-nets border vertices**, which sit at a centroid of
up to twelve crossings *inside* a cell and so cannot be recomputed by a
neighbour at another resolution. A contour vertex is a linear interpolation on a
*named lattice edge of the plane* — a primal construction, which the same
paragraph explicitly concedes can be shared.

## The price, and where it lands

Analytic estimate on StreamMountain's authored bands (`sectorSize` 64, six
levels, 318/1186/2605/4702/7753/10095 m), assuming one contour line per lateral
face and one triangle per canonical segment per side:

| level | tile | border ÷ interior triangles |
|---|---|---|
| 0 | 64 m | 0.06 |
| 1 | 128 m | 0.12 |
| 2 | 256 m | 0.25 |
| 3 | 512 m | 0.50 |
| 4 | 1024 m | 1.00 |
| 5 | 2048 m | 2.00 |

**World total ≈ +40 % triangles.** Two caveats: it ignores ±y faces (a
near-horizontal surface crossing a horizontal tile plane adds contour there),
and the contour must be **locked through cluster decimation** because it is the
shared interface — so the border keeps full resolution while the interior
decimates, and the border's share rises at distance. Measure both in the
prototype rather than trusting this table.

Accepted by the project owner on the grounds that invisible, exact borders are
worth the rendering cost.

## Sampling cost, and why it is not O(area)

A coarse tile would need `(S_L / rc)²` samples per face to find the contour by
brute force — 1M per face at level 5, fatal. It is not needed: the contour is a
1D curve, so **seed from the tile's own-rung crossings on the face, then trace
at the canonical rung** until each component closes or leaves the square. Cost
is O(curve length), a few thousand samples for the coarsest tile.

Tracing from own-rung seeds misses components the tile's own lattice does not
see (a fine tunnel mouth in a coarse wall). That is correct, not a defect: if
this tile's lattice sees no crossing there, its surface does not reach the plane
there, so it owes no geometry. The finer neighbour, which does see it, traces
and closes it with a flat in-plane cap. Nothing to overlap, because this side
has no surface in that region at all.

## Acceptance gates for the prototype

CPU-only, no engine changes, in the style of `seam_integration_tests.cpp`:

1. **Shared contour is bitwise identical** from both sides, for every component
   both sides detect, at equal level and at 2:1.
2. **No overlap**: zero intersecting triangle pairs beyond shared edges, and in
   particular zero coplanar overlapping area. This is the requirement; it is a
   hard gate, not a budget.
3. **Watertight**: the existing plumb-line union scan reports zero uncovered
   columns across the shared plane.
4. **Grazing fixture**: a dome summit within one voxel of the shared plane, both
   orientations, at equal level and 2:1. This is the geometry that produced the
   worst defect of the previous design and the one that killed the inset
   variant.
5. **Topology mismatch**: a tunnel mouth the coarse lattice misses — the fine
   side caps flat, the coarse side emits nothing, no gap and no overlap.
6. **Cost**: report border ÷ interior triangles per level against the table
   above, and the same after cluster decimation.

If 4 fails even in this form, the constraint has to move and the incumbent
stays.
