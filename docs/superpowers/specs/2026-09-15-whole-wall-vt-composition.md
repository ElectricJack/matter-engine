# Whole-wall VT composition

Date: 2026-09-15. Status: straight box and maze receivers implemented; native raster proof captured.

## Required result

`ClayBrickWallSurfaceProof` must draw **three boxes, twelve triangles each**.
The detailed brick assemblies supply bake inputs; individual brick boxes and
mortar boxes are not runtime geometry in this scene. The earlier seventy
individual brick receivers did not satisfy this requirement; the current proof
uses three boxes (36 triangles).

The authoritative physical layout still determines wall width, height, depth,
whole-brick bond termination, variant identity and source placement. Resizing
must neither stretch bricks nor crop an incomplete top course or exposed end.
This refines the receiver requirement in the layered-texturing design; the
broader terrain, splat, weathering, VT stability and performance objectives remain.

## Authoring boundary

All brick-specific decisions remain JavaScript: brick geometry, dents, pores,
scratches, clay material, bond layout, mortar, and weathering placement. Native
code provides generic source preparation, rigid placement, composition and VT.
`clayBrickSourceSpec` is already a project JS function, not an engine DSL opcode.

Keep the existing single-source `static finiteSurface(p)` declaration for source
inspection and reusable source preparation. Add a versioned **composite receiver**
declaration with these responsibilities (version 2 is the box implementation;
version 3 adds explicit planar receiver frames):

- Physical receiver bounds and a base material program.
- A table of reusable finite source recipes with independent geometry and
  appearance identities. Compile each unique recipe once, not once per brick.
- Stable placements referencing that table, with rigid transforms in receiver
  metres, projection selection and a composition operation.
- Explicit bounds, counts, supported transform constraints and failure behavior.
  Invalid or excessive input leaves the prior complete surface resident.

The wall JS helper derives this declaration and the box mesh from the same
`brickWallLayout` result. `build()` emits six quads only and does not place brick
or mortar children. The existing detailed wall scene remains the geometry
reference for fixed-camera comparisons. The twelve-triangle per-brick receiver
remains useful as a diagnostic, not as the whole-wall delivery.

## Source preparation and composition

1. Prepare the eight detailed clay variants and all six source projections
   using the existing geometry/material pipeline. Share unchanged source
   preparation across wall sizes and placements. A wall resize changes layout
   and receiver pages, not the brick's geometry or material bake.
2. Transform source frames into the receiver's physical coordinate system.
   Front/back, top/bottom and end faces must use the appropriate projections;
   quarter-turned header bricks preserve physical scale and source orientation.
   Do not reuse a front image indiscriminately or wrap a finite patch.
3. Build a bounded spatial index of placed projected source bounds per receiver
   face. For each requested VT page, collect a deterministic candidate list
   intersecting its physical footprint, including filter support and guards.
   Page work must not scan every brick at every texel. Oversized work is split
   or rejected with an explicit diagnostic; candidates are never silently lost.
4. Evaluate a recessed mortar base, then composite the candidates into the
   existing color, normal, ORM and metre-height VT channels. Geometry sources
   use nearest-surface depth along the receiver projection direction, with
   finite coverage at boundaries. Hidden interior bricks must not overwrite
   the outer wythe or mortar. Resolve all channels from the same visible hit.
5. Convert source depth to displacement from the wall receiver plane. A brick's
   local projection origin is not the wall's displacement datum. Filter coverage,
   color, normals, squared roughness and height coherently; normal rotation and
   header orientation must agree in raster and RT.
6. Apply ordered weathering layers after the structural material result. Moss,
   stains and peeling paint use wall coordinates and may span several bricks
   and joints. Keep structural visibility selection separate from the explicit
   order of paint/deposit/repair operations.
7. Publish complete page content with its compatible source snapshot. An
   appearance edit must not force geometry preparation or expose partial data.

## Relationship to splats

A placed brick projection is a bounded reusable source contributing to a
receiver. General splats share its source table, physical transform, bounds,
coverage/filtering, spatial query, channel composition and content identities.
The brick layout generates structural placements; analytic masks or authored
placements generate deposits and repairs. A painted splat is an ordered layer;
a geometry-source union uses depth visibility. These are explicit operations,
not one ambiguous blend mode. Physics is optional source/placement preparation,
and is absent from this wall bake.

The current one-source-ID-per-triangle binding cannot describe several bricks
on a two-triangle wall face. It must become a receiver-face/candidate-set
selection or equivalent page-candidate mechanism. Adding more mesh triangles
to preserve that old selector is not an acceptable workaround.

## Storage and costs

Keep reusable source payload identity separate from placement/catalog identity.
Three walls using the same clay sources must not reproject or shade the same
sources three times. Deduplicate CPU ownership and GPU source uploads across
their catalogs, including after re-registration. Geometry caches retain their
appearance-independent keys. Prepared float pixels are still temporary
interchange data; final compression and prepared-artifact caching remain required.

Record unique source bytes separately from binding/index bytes, resident VT
pages and GPU geometry. Report cold source generation, warm loading, wall
composition and first complete frame separately. A small triangle count alone
does not prove a fast or memory-efficient texturing system.

When adding shared periodic composed pages, separate receiver-specific chart
validity, connectivity and geometry lifetime from the shareable material pixels.
The current connected-POM implementation publishes geometry with each physical
page; that binding must not force a private copy of the base pixels for every
wall. Shared-page acceptance must include different receiver geometries, correct
corner/end traversal for each owner, independent sparse weathering, and deletion
of one owner while the others continue using the shared pixels. Pixel/source
snapshot coherence and receiver-geometry lifetime remain required independently.

The first storage split keeps receiver AUX/chart coverage and traversal geometry
private and addresses color/normal/ORM/height through a separate material-page
allocation. An explicit producer identity may alias only identical encoded
pixels including gutters; material phase, orientation, resolution and filtering
must agree. Edits use copy-on-write and preserve deleted owners' reader horizons.
This is a prerequisite, not the complete periodic-wall solution: canonical
material coordinates and source/page lookup must avoid redundant composition,
and bounded weathering must select sparse overrides. Geometry-dependent ORM
enrichment needs private overrides before its payload can share. Report both
unique material bytes and private coverage bytes, separately from pool reserve.

The September 16 fixed-grid/candidate-filtering checkpoint proves actual shared
material pages across resized walls: `SharedBrickWallProof` uses 195 receiver
pages, 141 material allocations and 54 shared references, versus 183/183/zero
before grid alignment. Occupied channel payload decreases 9.84% after counting
extra coverage pages; GPU image reservation is unchanged. Native CPU/compositor
and six GPU modes pass. This remains equal-grid reuse, not general periodic
material lookup. Completion requires a material domain independent of receiver
extents, plus correct receiver-to-material sampling; matching source images or
rounding unequal producer keys is insufficient. Preserve distinct wall sizes
and physical brick dimensions in acceptance. Any chart-grid alignment approach
must report padding and density cost; independent material lookup must preserve
filtering, POM traversal and immutable publication.
See [native results and diagnosis](../../agent/evidence/2026-09-16-shared-vt-pixels/README.md).

## Silhouette and visibility contract

The first required scene is exactly three box meshes. POM reproduces recessed
surface depth and shading inside those receiver faces; it does not create a
chipped mesh outline or arbitrary overhangs. Do not secretly add brick meshes
to supply silhouettes or shadows. Coverage-aware visibility or limited boundary
geometry would be a later explicit representation choice, with its triangle
cost reported. Existing cross-chart POM traversal limitations remain open.

## Acceptance

- Native wireframe and geometry census: three receiver boxes, 36 triangles,
  no brick/mortar child instances and no overlapping merged wall mesh.
- All three wall sizes show source-derived dents, pores, scratches, rounding
  and clay variation; albedo, normal and height diagnostics are non-flat.
- Fixed-camera front, grazing, top, end and back comparisons against detailed
  source geometry. Use a controlled identical lighting/exposure configuration.
- POM off/on changes parallax while color/normal/roughness remain registered.
  Report box-silhouette and chart-traversal discrepancies explicitly.
- Layout tests retain whole bricks, valid headers and stable identities across
  count/metric resizing. Native changes in wall dimensions do not stretch sources.
- Cold/warm and resize/edit tests verify unique-source reuse, complete snapshots,
  cancellation safety and bounded candidate counts/memory; no blanket per-texel
  scan, partial publication or unbounded catalog duplication.
- Raster/RT material agreement and source/page residency checks pass. Follow
  visual iteration with the existing performance acceptance targets.
- Demonstrate cross-brick moss and peeling-paint layers through the shared splat
  mechanism. Preserve the natural terrain proof and optional Wang functionality.

## Implementation sequence

1. Add and validate generic composite-receiver authoring; preserve single-source
   recipes and separate source/placement/material identities.
2. Prepare a deduplicated source bank and transform placements; validate surface
   depth/coverage with small analytic CPU/GPU fixtures before the complete wall.
3. Add page candidate gathering and GPU composition, preserving snapshot,
   memory-admission and upload-budget contracts.
4. Replace `ClayBrickWallSurface` runtime geometry with the layout-sized box;
   bind its six receiver faces to composite source sets and capture all views.
5. Add wall-scale weathering/splats, then finish source storage and performance
   acceptance alongside the broader layered-texturing plan.

The earlier inspection also found that Vulkan drops the resolver's fine/coarse
segment choice, potentially drawing merged geometry over instanced bricks.
Record and fix that independently with raster/RT regression coverage. Correcting
that bug alone does not deliver the three-box wall requirement.

## Repeating modules, corners and curves (user requirement, September 15)

Wall dimensions must not enter the identity of the reusable material module.
Author a periodic interior in physical metres, including its head and bed
joints. A finite wall of N bricks has length `N*(brickLength+headJoint)-headJoint`;
the corresponding repeat domain has length `N*(brickLength+headJoint)`.
Confusing these lengths removes a joint at every repeat. Running bond needs an
even number of courses per vertical repeat. Seed/variant selection is periodic
inside the module; wall-scale weathering is a separate layer and may be unique.

Expose module counts and phase in JS. Resizing repeats the same source material
without stretching, new geometry projection or source shading. Modulo applies
to the unwrapped material coordinates, never to derivatives: filtering samples
across opposite boundaries, including the mip tail, must include wrapped
neighbours. Test negative coordinates and exact period boundaries. Reuse means
shared immutable source pixels/uploads; receiver-specific VT pages may still
exist and their memory must be reported separately.

Use explicit boundary treatments for open ends, the final course, wall caps,
and corners. An interior running-bond seam may cross a brick because that brick
continues in the next repeat; an exposed end must contain complete real bricks.
End/corner treatments reuse projected brick sources and do not justify baking
a new interior texture for each wall size. All six receiver faces must agree
about the orientation and identity of bricks meeting at their shared edge.

### Explicit module authoring checkpoint (September 16)

`brickSurfaceModule` in project JS now defines a repeat independently of wall
extent. `moduleColumns`/`moduleCourses` opt a wall into periodic interior variant
selection; defaults for an explicitly requested module are 8×4. Its physical
period includes the terminal joints (2.04×0.376 m for the current brick).
`phaseColumns`/`phaseCourses` select appearance phase in whole brick/course
counts, including negative values. Running bond requires an even course period
and even course phase. Phase leaves brick transforms and finite receiver bounds
unchanged. Existing callers without module parameters retain their layout.

The canonical **layout** key includes bond, brick dimensions, joints, module
counts and seed. It excludes finite wall dimensions, phase and weathering.
It is not a native material/page cache key: source appearance, base program,
filtering, resolution and immutable input dependencies still qualify those keys.
Physical brick IDs stay unique; periodic surface addresses choose appearance.
Open-end headers remain separate whole-brick boundary treatments, never extra
headers at an internal repeat. The same placements drive geometry inspection
and finite-source composition.

`PeriodicBrickWallProof` exercises 1×/2×/4× wall counts with three boxes. This
checkpoint defines the repeat correctly; current VT still composes placed
sources into receiver pages. Independent module lookup and sparse weathering
are tracked in [the material-domain implementation plan](../plans/2026-09-16-periodic-material-domains.md).

### 90-degree turns

Author the junction as one physical brick layout with alternating corner
bricks, then derive both receiving faces and caps from that layout. Rotating
two independently textured boxes is insufficient: it can double the corner
volume, misalign the course phase, and show two unrelated bricks at the edge.
An L-shaped low-poly receiver is acceptable; record its actual triangle count.
Test both handednesses, inside/outside views, odd/even course counts and
multiple leg lengths. Keep material U phase continuous where intended, but
sample the correct source end/side face around a corner instead of stretching
a brick's front image through the turn.

### Curved walls

Use a bounded low-poly strip for the receiver and explicit metric surface
coordinates. Along-wall distance and height define material coordinates;
receiver triangle UVs must not restart the pattern at every segment. Tangents,
source normals and POM rays follow the same local frame. Increase receiver
segments from a geometric error tolerance, independently of brick count or
texture resolution. Report geometry and POM silhouette limitations.

Rigid source bricks remain rigid. A curved wall has different inner and outer
lengths, so a single stretched straight-wall texture cannot be the physical
reference for both. Solve brick placements and wedge-shaped joints against
radius, wall thickness and allowed joint widths. Reject an impossible tight
bend or offer explicit radius/joint/count snapping; never silently scale the
bricks. Special radial/wedge brick assets are an optional later solution.

### Required proof matrix

- A 1x, 2x and 4x periodic interior: identical physical brick size, matching
  period boundaries in color/normal/roughness/height, and shared source hashes.
- Whole-brick terminations for each size; a separate tiling test includes
  the terminal joint and joins two modules without a doubled/missing joint.
- L corners in both directions, inspected from inside, outside and above;
  no duplicate faces, corner overlap or broken bond at course transitions.
- Gentle and tighter quarter-circle walls, both faces and caps; physical
  reference brick placement, inner/outer joint bounds, no UV restart at strip
  segments. Include an impossible-radius rejection fixture.
- POM on/off and grazing captures crossing each repeat, corner, chart and curve
  segment. Flat/albedo/normal/height views isolate seams from lighting changes.
- Cold/warm/resize captures distinguish source reuse from page generation.

Implement the straight three-box POM proof first. Periodic mapping and boundary
modules follow before accepting arbitrary wall sizes; corner and curve proofs
are required before claiming support for wall paths. The existing segmented
Vulkan bug and cross-chart POM gate remain separately tracked. Sparse-voxel
silhouette rendering remains deferred under the user's POM-first decision.

#### Curved placement oracle

For a single wythe of rigid rectangular bricks, let `L` be brick length, `D`
its radial depth, `J` the center joint, and `a` the angular spacing of adjacent
bricks. The center radius that preserves these dimensions is
`R = (L*cos(a/2)+J)/(2*sin(a/2))`. The inner and outer joint openings measured
in the bisector frame are `J-D*sin(a/2)` and `J+D*sin(a/2)` respectively.
Use transformed brick corners to independently verify this oracle in tests.

For the current 245 x 117.5 mm brick and 10 mm center joint, a quarter turn
with 8 units has radius 1.295 m and an inner opening of -1.52 mm (overlap).
With 20 units the radius is 3.245 m and openings are 5.39–14.61 mm. This is a
placement calculation, not a rendered acceptance result. Multiple wythes and
running bond additionally need a junction/closure solve; the single-wythe
oracle cannot authorize arbitrary solid curved walls.

## Maze implementation checkpoint (2026-09-15)

`ClayBrickMaze` adds finite L/U receiver shells and two faceted quarter-circle
walls alongside repeated box receivers. `clay_brick_wall_path.js` owns physical
brick layout and shell construction. L/U layouts tile the union of both arms
with whole bricks; curves preserve rigid brick dimensions and reject head joints
outside 4–36 mm. Courses set exact heights. Curve face count follows the column
angles; adding courses adds no runtime triangles.

Generic declaration version 3 adds explicit outward planar frames and physical
UV domains. Oriented-box validation permits rigid off-axis placements while
continuing to reject overlapping solid-source bounds. One receiver group covers
all coplanar top/bottom cap pieces, avoiding ambiguous vertex bindings.

All composite layouts retain a canonical full source bank. Payload identity is
separate from placement identity, allowing one GPU pixel/mip allocation across
wall catalogs. Allocation, upload and retirement remain bounded. Bindings admit
up to six projected faces per placement (24,576 total), with the existing 128 MiB
source bank and bounded spatial candidate budgets.

See `docs/agent/evidence/2026-09-15-brick-maze/README.md` for native checks and
visual evidence. This checkpoint does not accept shared periodic composed pages,
general splat/weathering authoring, arbitrary spline mapping or POM/RT seams.


## Maze weathering and thin coatings (2026-09-15)

The `ClayBrickMaze` examples opt into JS-authored weathering with one stable
`weatherSeed` per wall ID. `shared-lib/wall_weathering.js` defines metre-scale
warped color variation, low-course damp staining/algae, pollution, runoff
streaks, irregular worn paint and sparse bounded graffiti strokes. The same
helper runs on straight, L/U and curved receivers. Wall-local 3D fields remain
continuous around joined faces; graffiti uses a bounded face/tangent frame and
depth mask. Geometry, source dimensions and the eight reusable brick variants
remain unchanged. Clean walls opt out by omitting `weathering`.

The generic scalar surface API adds `s.coat({baseColor:[r,g,b], roughness}, coverage)`.
It is one thin dielectric coating output per tape, evaluated **after** finite
source/base composition and the existing appearance modifiers. Values may be
scalars or surface nodes. It linearly blends RGB, blends squared perceptual
roughness, and attenuates substrate metallic by uncovered fraction. Height,
normal, AO and geometry coverage are preserved. Absent/zero coverage is an
identity. Malformed or duplicate output directives fail closed. Register
liveness, CPU reference shading and both GPU consumers share this contract.

The JS helper flattens ordered paint/grime/ink layers into a single coating
color, roughness and coverage, with footprint fading for fine erosion and
antialiased stroke edges. It adds no runtime decal draws or texture layers;
the result is cached in the wall's ordinary VT pages. Different weathering
creates different composed pages and recipe identities while the original
brick source pixel bank remains shared. This is not a claim of identical-page
reuse across differently painted walls.

Paint peeling here is missing paint coverage exposing the original brick
relief; lifted paint edges and new coating displacement are not implemented.
Graffiti is a small procedural stroke library, not a general decal-placement
editor. Arbitrary splat authoring, seam traversal and presentation performance
remain separate goals.


Weathering art constraint: runoff must not form a regular comb near the brick
pitch. Such stripes read as misregistered mortar even when the normal/height
and material coordinates agree. Use broader continuous fields, breakup along
height and restrained opacity. See the maze evidence's “False vertical joints”
section for the isolated-mask test and matched POM/channel captures.
