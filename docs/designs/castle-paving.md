# Courtyard paving

`shared-lib/castle_paving.js` accepts a compiled courtyard record:

```js
{ id, baseY, clearPolygon: [[worldX, worldZ], ...],
  floor: { thickness: 0.25, material: 'stone' } }
```

The polygon must be convex, with 3–16 vertices. Clockwise input is canonicalized.
`baseY` is the exact physical floor top. Thickness defaults to 0.25m and must be
at least 0.14m for the visible paving stack.

- `pavingPlacements(record, {materials: {stone, mortar}, detail, joint})` returns
  `{id, role, module, params, matrix, transform, polygon, ...}` records.
  Both matrix names contain the same row-major 4×4 transform. Apply the full
  matrix; interior stones rotate and scale their dressed face toward the sky.
- `pavingChildVariants(record, options)` returns unique `{module, params}` keys
  for a wrapper's `requires`. All Part parameters are finite scalar numbers.
- `emitPaving(part, record, options)` emits only `placeChild` calls with those
  transforms. A site-specific lookup wrapper can therefore use `expand: true`.
- `pavingCollisionEntities(record, {prefix})` emits one exact convex static slab
  with at most 32 hull vertices. Its transform places local hull points in the
  courtyard's world coordinates. It is the walking surface, including joints.

Rows are 0.64m wide and stagger by half a module. Stone lengths repeat 0.6,
0.75, and 0.9m. Interior cells reuse the shared `castle_stock` catalogue: at most two
`CastleStone` variants per material/detail pair (0.72 × 0.28 × 0.42m, seeds 0/1).
`stockPlacement` postmultiplies the complete upward-facing placement by the local
fit, so the three authored lengths change instance scale rather than bake keys. A 45mm lateral core inset reserves room for relief within the joint cell;
tests check actual stock brush bounds after the complete fitted transform. The dressed face points upward; vertical scaling fits all
relief below the physical floor top. Boundary flags fan-triangulate into two shared `CastleClippedFlag` seeds.
The canonical voxel prism is the CCW unit right triangle
`[-.5,-.5], [.5,-.5], [-.5,.5]`, height 0.18m. Each triangle receives a full
positive-determinant affine XZ fit plus vertical scale. Apply this matrix
unchanged; decomposing it into translation/rotation/scale would discard shear.
Normals require the engine's full inverse-transpose, including for top wear.

Only the original flag cell has a grout inset. Fan triangles cover its clipped
polygon exactly, with no inset, bevel, or wear at their shared diagonals. Top
wear remains strictly inside the stock triangle. Its final three outward
halfspace cuts use zero smoothing, and all preceding detail is subtractive.
The generic clipped-flag Part can still accept up to eight planes, but courtyard
paving always uses these two fixed three-plane keys. Different courtyard shapes
and floor depths change transforms, never the triangle bake parameters.

The four current courtyards share eight keys altogether: two interior stones,
two triangular flags, and four individual slab meshes. Their respective visible
piece counts are 286, 603, 148, and 194. This replaces 257 unique boundary bakes
with two while retaining each authored flag outline.

`CastlePavingSlab` is a small closed triangle mesh for the recessed mortar bed;
it has no expensive courtyard-sized voxel bake. Its cap winding and direct XYZ
vertices avoid the native +Y extrusion profile basis. Joint-only corner slivers
remain physically supported by the complete slab and collider.

The nominal joint is 20mm; the inherited stone core is inset further to reserve
its face relief. Boundary flags have top-face dents and shallow chisel strokes,
fine `.026/detail` voxel spacing in canonical stock space, and no author
simplification. Affine fitting preserves thin corner pieces without separately
voxel-meshing each sliver. The exact supporting slab remains complete.

Validation: `node projects/world_demo/tests/castle_paving_tests.mjs` replays actual
Part matrices and CSG cutter boxes, checks inherited relief bounds, clipping,
non-overlap and coverage without diagonal grout, positive determinants, full 3D
normal transforms, two shared triangle keys, slab winding/manifold volume, and
exact collider vertices. Native screenshot acceptance remains a scene-level check.
