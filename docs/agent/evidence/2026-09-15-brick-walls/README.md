# Walls laid out from whole bricks

`ClayBrickWall` is a reusable source-geometry inspection assembly backed by
[`brick_wall_layout.js`](../../../../projects/world_demo/shared-lib/brick_wall_layout.js).
Wall dimensions, child placement, mortar and metric source-face frames come
from that one layout. Low-poly receiver/VT stamp integration remains open.

## Authoring

Use a root with `module: 'ClayBrickWall'` and scalar parameters:

```js
// 1.010 m wide × 0.460 m high × 0.245 m thick.
{ columns: 4, courses: 5, bond: 'running-headers', seed: 0 }

// Requested dimensions: nearest complete layout is 0.500 × 0.554 m.
{ widthM: 0.5, heightM: 0.6, fit: 'nearest' }

// Thin wall with aligned rows: 0.500 × 0.272 × 0.1175 m.
{ columns: 2, courses: 3, bond: 'stack' }
```

Each axis accepts a count **or** a requested size. An omitted axis defaults to
four bricks/courses. `fit` supports `nearest`, `inside` and `outside`; an inside
request smaller than one brick fails. The pure layout function returns actual
dimensions and adjustments, allowing an authoring UI to report the result.
No editor sizing widget has been added yet.

The profile is 245 × 84 × 117.5 mm with 10 mm head/bed joints. Unlike the earlier
112 mm-deep inspection brick, this profile obeys `length = 2*depth + headJoint`.
Stack bond uses one brick depth. `running-headers` uses two rows through the
thickness, with whole rotated header bricks at both ends of alternate courses.
The shorter visible end face belongs to a complete brick running through the
wall. This is a supported modular termination, not a general masonry junction
solver or a structural engineering claim. Nonmodular profiles and arbitrary
thicknesses fail explicitly rather than stretching/cropping the source.

Wall-local coordinates start at the lower-left near corner. Existing anchored
brick IDs, variants and transforms survive added columns/courses. The right
header moves with the exposed boundary. Each placement has a proper rigid
transform and source variant; all six source-face frames use metres. The
geometry adapter consumes these placements directly and uses the same layout
for recessed mortar geometry with metric UVs. Its children share source assets
across wall sizes. Native material overrides use `matBrick0` through `matBrick7`
and `mortarMaterial`, because Part roots transport scalar handles.

## Validation

Run `node projects/world_demo/tests/brick_wall_layout_tests.mjs` from the root.
The tests cover 40 size/bond combinations and 780 whole bricks, including 1×1,
narrow/odd/even sizes, requested-size fitting, exact extents, noninterpenetration,
mortar confined to joints, rigid source transforms, six oriented metric face
frames and resize stability. Geometry emission reuses the same placements and
source keys; scalar material bindings are covered too.

`ClayBrickWallProof` contains the three examples above. Native capture evidence
is recorded separately with source/binary hashes and fixed cameras. Source mesh
geometry is deliberately used to inspect top/end continuity; it does not meet
the intended runtime wall budget. Appearance here is scalar clay color and
roughness plus real geometry, not the finished textured VT stamps.

Accepted native geometry review:
[three wall sizes](whole-bricks-v4-group.png),
[close](whole-bricks-v4-close.png), [top and end](whole-bricks-v4-grazing.png),
[RT](whole-bricks-v4-rt.png), [albedo](whole-bricks-v4-albedo.png),
[normals](whole-bricks-v4-normals.png), [wireframe](whole-bricks-v4-wireframe.png).
The [capture results](whole-bricks-v4-result.json) and
[source/binary manifest](whole-bricks-v4-source.json) record a successful native
run with no source changes, bake failures or Vulkan validation errors. The
scene contains 70 brick instances and three mortar assemblies. Images confirm
the 2×3, 4×5 and fitted 2×6 arrangements, square boundaries, complete top courses
and alternating full-depth header ends. Albedo confirms mortar occupies the
joints; its blue cast in lit views is a lighting issue, not missing geometry.

The first successful bake (`v3`, no captures) generated eight shared source
meshes. After removing unsupported assembly LOD metadata, `v4` baked three
parent assemblies and reused the children: 21 cache hits, no `solid-source`
meshing entries. This is reuse in the inspection representation, distinct from
the [finite projection cache](../2026-09-15-face-preparation/README.md).

The source meshes have about 26–29k triangles each at 3 mm preview pitch; the
native view reports about 3.07M submitted raster triangles including its
representation overhead. Startup publication took about 23 s in this run.
These are reasons to continue the planned low-poly/VT integration, not a
performance acceptance result. Fine pores show aliasing and the smooth top/end
faces lack the final clay material. Raster/RT lighting differs. This checkpoint
accepts layout/whole-brick correspondence only, not realism or final materials.

## Remaining integration

- Bake clay appearance at source hits and prepare textured finite stamps,
  including exposed top/end faces. Reuse the new geometry cache.
- Use this layout for the low-poly receiver, finite stamp selection and mortar
  blending; carry stable layout identity into dirty bounds and splat placement.
- Add whole-brick openings and corner/return layouts. They are explicitly
  unsupported in this first solid-panel implementation.
- Review weathering, paint/moss, raster/RT material consistency, collision
  integration and performance on the final representation.

Earlier capture attempts are retained as failed evidence: v1 used the wrong
world argument; v2 exposed array-valued material parameter transport and was
stopped; v3 baked all three walls successfully but its command-file path was
wrong, so it has no images. None is accepted as visual evidence.
