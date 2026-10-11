# CPU procedural geometry extraction

`matter_geometry` implements primitive/profile and ordered CSG geometry, batched
surface queries and optional mesh modifiers through the frozen
[contracts-v1 gate](../../docs/component-separation/contracts-v1.md).
Its only linked library is `matter_contracts`. The public
[geometry.h](include/matter/geometry/geometry.h) exposes owned neutral recipe,
asset, material and diagnostic values, without legacy graphics types.

## Native use

`PrimitiveGeometry::evaluate` accepts a direct `RecipeDocument`, resolved inputs,
finite evaluation budgets and cooperative job control. Success publishes an
immutable validated `AssetProduct`. Failure publishes no asset. Nodes may be
listed in any order; repeated affine placements share one mesh prototype.
Geometry stays in local coordinates, with row-major affine instance transforms
in metres. `bake_placement` supplies an atomic CPU alternative using inverse
transpose normals, mirrored winding and tangent handedness.

Version 1 implements these operations:

| Operation | Parameters |
| --- | --- |
| `primitive.box` | Positive `size: [x,y,z]`, centred at the origin |
| `primitive.sphere` | Positive `radius`, integer `segments` |
| `primitive.line`, `primitive.cylinder`, `primitive.cone` | Endpoints `a`, `b`, nonnegative `radius0`, `radius1`, integer `segments`; cylinder requires equal radii |
| `primitive.capsule` | Endpoints `a`, `b`, positive `radius`, integer `segments`, positive integer `rings`; coincident endpoints use the canonical sphere fallback |
| `mesh.triangles` | `positions`, `mode` (0 triangles, 1 strip, 2 fan), optional paired `normals` and `uv0` |
| `profile.polygon` | `points: [[x,y],...]` |
| `profile.contours` | `points` plus `holes: [[[x,y],...],...]` |
| `profile.extrude` | One profile input, positive `depth` along local +Z |
| `profile.sweep` | One profile input, `path: [[x,y,z],...]`, `join` (0 miter, 1 bevel, 2 round) |
| `profile.fill` | One profile input; triangulate in the local XY plane |
| `placement.affine` | One mesh input and a nonsingular affine `transform` |
| `solid.csg` | Two or more ordered solid node inputs, `operator`: `union`, `subtract`, `intersect`; hard left fold |
| `solid.ordered` | One or more ordered solid node inputs; matching `operations` array, positive `spacing`, nonnegative `smoothing`, optional nonnegative `referenceRadius` |

The core box, sphere, polygon, extrude, placement and CSG operations use
`matter.recipe.core@1`. Each other operation requires its declared
`matter.operation.<operation>@1` capability. `capabilities()` reports the exact
set. Primitives bind one PBR material; an omitted binding creates a neutral
default. Placement preserves prototype material bindings. ROUND preserves the
legacy BEVEL chamfer. Profiles support either contour winding, concavity and
disjoint holes. Touching/crossing contours, nested holes, incomplete cap
triangulation, zero path segments and reversing cusps fail explicitly. Closed
sweep seams are unqualified and return `unsupported_capability`.

`Session` builds recipes using transform, material and tint cursors. Retaining a
profile snapshots all three. A later cursor change does not move or recolour the
retained profile. Extrusion consumes it and suppresses the flat fill; a new
profile, triangle shape, explicit flush or finish emits an unconsumed flat fill.
Tint becomes an owned PBR material snapshot with a canonical digest identity:
`baseRGB *= 1 + (tintRGB - 1) * tint.w`. A failed mutator poisons the session so
finish cannot publish an earlier successful prefix.

Assembly and mesh-resource import belong to subsequent extraction tasks and
return `unsupported_capability` here. Batched surface queries and mesh
modifiers are described below.
Required document extensions are rejected rather than silently ignored. This
delivery qualifies native CPU geometry; browser/WASM and live editor wiring
remain integration work.

## Ordered solids and CPU meshing

`matter.geometry.csg.cpu.v1@1` selects this CPU algorithm through
`EvaluationOptions::algorithms`. The default service supports it without a
required selector. Frozen `solid.csg@1` has only its core `operator` parameter;
it applies that operator from left to right, with the first operand as the seed.
For example, subtracting B then C from A computes `(A - B) - C`.

The declared extension `matter.operation.solid.ordered@1` preserves the bake
path's brush staging. `operations` contains one of `union`, `subtract`,
`intersect` for each input. Consecutive equal operators form a stage: their
distances combine using the canonical log-sum-exp smooth minimum, then the
stage folds into the running field using hard min/max. Thus consecutive
intersection brushes form one unioned clipping stage. An opening subtract or
intersect stage starts from empty space. Smoothing at or below `1e-5` uses a
hard minimum. Ordered core CSG and grouped stages deliberately have distinct
operation identities. Neither reorders authored inputs.

Analytic operands are boxes, spheres, capsules, cylinders, cones and lines,
optionally placed or composed as further solids. Triangle meshes, profiles and
imported mesh resources have no analytic solid representation and fail with
`unsupported_capability`. Inside a solid, only proper rigid transforms and
positive uniform scales are supported. Mirroring, nonuniform scaling and shear
fail explicitly; singular transforms are invalid input. Outer placement of a
completed prototype retains the usual general affine placement support. As in
the existing bake path, uniform spheres measure distance in the containing
frame; the other canonical fat primitives retain their local distance metric.
Consequently smoothing under scale follows that existing metric, not a new
world-distance correction. A solid binds one material for its entire output;
its operand bindings do not paint the resulting surface.

`PrimitiveGeometry(CsgMeshingSettings{spacing, reference_radius})` configures
core CSG. The defaults are spacing `0.1` metres and the zero level set.
`solid.ordered` serializes its spacing, smoothing and optional reference radius
in the recipe. A positive reference radius preserves the canonical coarse-grid
isovalue `-max(0, (spacing / radius - 1) * radius * 0.3)`; zero disables that
offset. Sampling uses a globally aligned lattice in prototype-local space,
with two samples of padding around conservative analytic and smoothing bounds.
Spacing is never silently coarsened. Nested ordered nodes must request the same
spacing and any explicit reference radius as the output lattice.

The backend lowers to an owned postorder field program, admits grid storage,
samples the field, counts nondegenerate triangles, admits output storage, emits
32-bit triangle soup and computes normals from six field probes. It reuses the
canonical marching-cubes tables, interpolation and smooth-min math. Canonical
endpoint snapping can create zero-area triangles; those are omitted because
the asset contract rejects them. Empty boolean results publish a valid empty
asset with no mesh instance. Primitive meshes referenced by a solid are not
emitted separately unless also requested as outputs. Repeated outer placements
share the completed prototype.

Operations, peak working storage, output vertices/indices, encoded output,
dependency depth and elapsed execution are bounded. Grid admission and work
units count conservatively, including the sampling reservation. Lowering also
has an explicit 128-level stack limit. Overflow or unrepresentable spacing fails
before publication. Cancellation checkpoints cover lowering, field samples,
cell traversal, normals and final publication; no failure publishes a prefix.
Progress stages are `csg.sample`, `csg.mesh`, `csg.normals`, then `complete`.

## Batched surface queries

`PrimitiveGeometry::query` answers a `QueryBatch` against an already published
`AssetProduct`, so staged generation can evaluate a ground, sample it and then
author details from the returned positions and normals without a
per-vertex host callback. `matter.geometry.query.cpu.v1@1` names the CPU
algorithm; like the CSG selector it is advertised and needs no explicit
selection, and any other `EvaluationOptions::algorithms` entry fails with
`unsupported_capability`.

Every sample names a mesh prototype and is answered in that prototype's local
frame. A caller that holds a world point applies the inverse of the instance
transform first and maps the returned position and normal back with the
instance transform, as the staged fixture does. `closest_surface` returns the
nearest surface point within `max_distance_metres` (unsigned, so a point inside
a closed mesh reports its nearest face). `ray_surface` normalises `direction`
before intersecting, so `distance_metres` is always the metric distance along
the ray and `max_distance_metres` bounds it. Results keep input order: `hit`,
position, the triangle index in the prototype's index buffer and a unit
normal. Normals interpolate authored normals barycentrically; meshes without
normals, or whose interpolated normal cancels, report the geometric normal of
the v1 counterclockwise winding. Equal-distance candidates resolve to the
lowest triangle index, so a repeated batch returns identical bytes.

Each referenced prototype gets one median-split BVH (four triangles per leaf),
built on first use and shared by every sample in the batch. Its storage is
admitted as `128` bytes per triangle plus `4096` against `working_bytes`.
The `queries` budget caps the batch size, and the returned samples are charged
to `output_bytes`, before any work starts. `operations` is charged as the work
runs: one per triangle in each build scan and partition (roughly
`N log N` per index), one per node visited (a leaf tests at most four
triangles) and one per sample. Budget sizing guide: the GEOM-QUERY-BATCH
fixture spends about 45 operations per closest-surface sample on a
512-triangle sphere, so its 20000 samples use about 0.9M of the default 1M.
Builds and traversals use fixed 64-entry stacks that fail with
`backend_failure` rather than overflow. Cancellation and budget checkpoints run
every 64 samples and every 4096 operations, including inside build scans; one
partition step is the coarsest uninterruptible unit. Progress reports only
`validate` and `complete`. Failure publishes no partial sample list.

The engine's SpatialQueryLib `BVH` is deliberately not reused here. It
answers only ray queries, uses float precision with a 1e-4 near-hit floor
tuned for secondary rays, allocates with unchecked `MALLOC64`, walks an
unchecked 64-entry stack, and packs triangle indices into 20 bits. The local
index is double precision, bounded, admitted against the job budget and
allocation-failure safe.

## Mesh modifiers

`PrimitiveGeometry::process` applies ordered modifier stacks to whole mesh
prototypes of a published asset. When `MATTER_GEOMETRY_MODIFIERS` is ON
(the default), `capabilities()` advertises `matter.operation.mesh.simplify@1`
and `matter.operation.mesh.smooth@1`. Each operation a request uses must also
appear in `ProcessingRequest::required_capabilities`. `ProcessingRequest::settings`
has this schema; unknown fields are rejected:

```json
{"regions": [
  {"prototypes": ["<mesh id>", ...],
   "modifiers": [
     {"operation": "mesh.simplify", "operation_version": 1, "ratio": 0.5},
     {"operation": "mesh.smooth", "iterations": 2, "lambda": 0.5, "mu": -0.53}]}]}
```

`operation_version` defaults to 1. `mesh.simplify` requires `ratio` in
`(0, 1]`, the target fraction of input triangles. `mesh.smooth` is Taubin
smoothing: `iterations` is an integer in `[1, 1024]`, `lambda` lies in `(0, 1]`
and `mu` in `[-1, 0)`, with the legacy defaults shown. Regions run in authored
order and each stack in listed order. A prototype may belong to only one
region, so overlap fails instead of picking an implicit order. Unlisted
prototypes, prototype order, materials, instances and instance IDs are
unchanged. Each prototype is welded, run through the canonical MatterSurfaceLib
stages the bake path's `apply_stack` uses (`mesh_simplifier`, `mesh_smooth`)
and unwelded into an owned neutral prototype. Corner normals and a single UV
set are carried and reprojected; geometry-only meshes stay geometry-only.
Submesh materials stay contiguous. Tangents, skinned prototypes and more than one UV
set fail with `unsupported_capability` instead of being silently dropped.
Contracts v1 admits counterclockwise prototypes only, which is also the
winding the MSL face normals assume.

The legacy bake path warns and skips an unavailable or failing modifier. A
service job instead fails explicitly and publishes nothing. `mesh.retopology`
has no supported backend here and is never advertised: it fails with
`unsupported_capability`, as do unknown operations, unknown required
capabilities and algorithms. The autoremesher backend needs subprocess
isolation before it can run inside a service job. An out-of-range parameter,
undeclared operation capability or simplify that removes every triangle fails
with `invalid_input`; a newer `operation_version` fails with
`unsupported_version`.

Each successful job appends one record to the asset metadata array
`matter.geometry.modifiers`. It holds the normalised regions in order, their
fully resolved modifier parameters and per-prototype `input_triangles`/
`output_triangles`. Reprocessing appends a further record. Asset bounds grow to
the union of the previous bounds and every placement of each modified
prototype. They are conservative and never shrink: the job does not recompute
bounds for unmodified content it never inspected.

The owned copy of the asset's mesh data is admitted against `working_bytes`,
`vertices` and `indices` before it is made. MSL stages cannot report their own
allocations or poll cancellation. Before each stage the service therefore admits `1024` bytes per input triangle plus
`65536` against `working_bytes` and charges operations and vertices. It
checks cancellation and budgets between MSL calls, so one stage on a large
mesh is the cancellation granularity. A stage that fails at run time (budget,
cancellation, backend) after earlier regions completed fails the whole job;
none of the earlier regions' output is published. Progress reports `validate`,
`modifiers` and `complete`.

With `-DMATTER_GEOMETRY_MODIFIERS=OFF` the MSL sources are not compiled, no
modifier capability is advertised and every modifier request fails with
`unsupported_capability` ("mesh modifier backend omitted from this build").
Queries do not depend on that option.

## Canonical implementation and dependencies

The target compiles the existing canonical polygon triangulator. At configure
time it also generates the geometry prefix of the canonical `triangle_emit.cpp`
into the build directory, stopping at `VariationRecorder::instance`. That final
function depends on the world/cache backend and MSVC resolves its unused
reference even with function-section elimination. The seam is guarded and the
canonical source is a configure dependency. There is no checked-in algorithm
copy or symlink. Central manifests, DSL state and legacy sources stay under their
integration owners.

CSG additionally compiles canonical `fat_primitive.c` directly and includes the
canonical `mc_tables.h`. Guarded configure-time extraction generates only
`smin_set` and `VertexInterpolation` from `surface.c` into the build directory.
The original monolithic traversal has no cooperative cancellation, 16-bit
indices and truncating neighbor/stage limits. It is compiled only into the
baseline test, alongside its spatial hash and memory pool. The production CPU
target has no link to those components, BLAS construction or renderer runtime.

When `MATTER_GEOMETRY_MODIFIERS` is ON, the target compiles canonical
MatterSurfaceLib `mesh_indexed.cpp`, `mesh_simplifier.cpp`, `mesh_smooth.cpp`
and `mesh_transform.cpp` from their source directory, with
`MATTER_VULKAN_ONLY` so they allocate through raylib's plain malloc macros
rather than its windowing runtime. The dependency fixture checks that exact
source list, and checks that the sources are absent when the option is OFF.

Legacy triangle/emitter POD headers currently bring private raylib/render header
includes. They neither escape the public API nor link runtime backends. The
dependency fixture checks exact target links, source lists, public includes and
native archive symbols. The direct recipe executable includes only public
headers and runs without script host, renderer, terrain or cache targets.

Frozen contracts-v1 `JobResult::success` drops a resource diagnostic when its
final envelope validation cannot allocate. A local publication correction
requires an owned const envelope that passed admission, the exact sole late
fallback, and successful envelope revalidation. These invariants distinguish the
swallowed resource error from invalid caller input. The supervisor recorded the
underlying contract issue for coordinated integration. Exhaustive transient
allocation injection and genuine invalid-envelope regressions cover this path.
As with the contract result containers, reporting sustained total heap exhaustion
still requires enough memory to construct a diagnostic.

## Build and verification

GNU headless build (also suitable as an independent consumer):

```sh
cmake -S libs/ProceduralGeometry -B /tmp/matter-primitive-geometry -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/matter-primitive-geometry -j 2
ctest --test-dir /tmp/matter-primitive-geometry --output-on-failure -j 1
```

Supported native Windows toolchain, resolved by the repository toolchain module:

```powershell
.\libs\ProceduralGeometry\build-windows.ps1 -Config RelWithDebInfo -Test
```

Nine CTests (eight with modifiers OFF) cover functional fixtures, native CSG comparison, a direct recipe,
query/modifier processing, allocation failures and dependency independence. Topology, prototype identity and material bindings are
exact. Position/normal/UV checks use this fixture suite's preselected absolute plus relative
tolerance, `1e-6 + 1e-6 * abs(expected)`; coordinates and absolute position
tolerance are metres. Profile cap area coverage uses
`1e-6 * max(1, filled_area)` square metres. CSG baseline scalar/positions use
the same predeclared absolute/relative tolerance; normal components use `2e-3`
absolute plus `1e-6` relative. Nondegenerate oriented triangle sequences on the
matched lattice must agree exactly in count and within those coordinate
tolerances; welded vertex indices are intentionally replaced by triangle soup.

| Fixture | Recorded expectation |
| --- | --- |
| GEOM-BOX | 12 triangles, size (2,4,6), bounds (-1,-2,-3) to (1,2,3), clay material, outward unit normals |
| GEOM-SPHERE | Radius 1, segments 12, 264 triangles, unit radial smooth normals |
| GEOM-ROUND | Segments 16: line/cylinder 64, cone 32, capsule rings 6: 384 triangles; radius 0.5 and axial bounds checked |
| GEOM-EXTRUDE / FLUSH | Unit square swept 2 m: 12 triangles, no flat duplicate; unconsumed square: 2 triangles |
| GEOM-HOLES / JOINS | Square with square hole: 32 triangles; bent unit square: miter 20, bevel/round 28 |
| GEOM-CURSOR / TRIANGLES | Retained transform/material/tint snapshots, profile flush at boundaries, paired authored UV/normals, triangle/strip/fan topology |
| GEOM-SHARED / AFFINE | One sphere prototype, two distinct placements; mirrored nonuniform scale (-2,3,4), analytical inverse transpose normals, tangents and atomic singular failure |
| GEOM-MALFORMED / EXHAUSTION / CANCEL / CAPABILITIES | Invalid profiles/envelopes, vertex/working/output limits, cancellation before publication, callback exception, required capability/extension rejection, source mapping; no partial assets |
| GEOM-ALLOCATION | Fail every allocation position in a successful profile/extrusion invocation, require resource diagnostics and no asset, then verify recovery |
| GEOM-INDEPENDENT / DIRECT-RECIPE | Contracts-only native archive and public consumer; one extruded prototype, two placements, 12 triangles |
| GEOM-CSG | Sphere minus translated box: cut face x=0.4 within 0.02 m, cavity normal +X >0.95; reversed difference, core intersection, restore-after-cut, empty opening stages, spacing refinement and prototype sharing |
| GEOM-CSG-BASELINE | Production `ProbeFieldScalar` and `GenerateMeshStaged`: hard/smooth union, difference, union after difference, intersection, adaptive coarse offset; matched 32-cubed lattice with spacing 0.125 m |
| GEOM-CSG-BOUNDS | Uniform scale accepted; mirror/shear/nonuniform/singular diagnostics; malformed/unknown capabilities; operation, memory, vertex, index, output, depth and time budgets; cancellation at each phase; no partial asset |
| GEOM-CSG-ALLOCATION | Fail every transient allocation in a small successful CSG evaluation, require resource diagnostics, then verify recovery |
| GEOM-QUERY-STAGED | Generate-query-generate: 20 x 2 x 20 m ground placed at (3,0,-2); 4 x 4 non-unit downward rays from y=10 hit y=1 at metric distance 9 with normal +Y, plus one miss; 16 `detail.<i>` placements of one shared 0.5 m box at hit + 0.25 x normal; stable IDs, bounds max y 1.5, byte-identical repeat |
| GEOM-QUERY-CLOSEST / BATCH | Closest point above and inside the ground, `max_distance_metres` cutoff, octahedron vertex distance 1 with authored normal, diagonal radial normal, ray into a sphere's south pole at distance 3; 20000 closest samples on a radius-3 shell all hit within [2, 2.05) with 2..64 progress events |
| GEOM-QUERY-LIMITS | Query count, working memory, output bytes, operations (including per-visit traversal charge), cancellation, missing prototype, zero ray direction, nonpositive distance, unknown algorithm; empty batch succeeds |
| GEOM-QUERY-ALLOCATION | Fail every allocation of an 8-sample mixed ray/closest batch on a box, require resource diagnostics and no samples, then verify recovery |
| GEOM-MODIFIER-ORDER | Legacy `test_stack_order_matters`: simplify-then-smooth differs from smooth-then-simplify, repeat is byte-identical, provenance keeps stack order and smooth defaults (mu -0.53), 128 to [48,128) triangles |
| GEOM-MODIFIER-REGIONS | Two regions (smooth rock, simplify ball) leave the ground prototype and all instance identities untouched; region order, triangle counts and appended provenance records; bounds cover placements; overlapping regions fail |
| GEOM-MODIFIER-ATTRIBUTES | Legacy smooth-corner and geometry-only fixtures: unit smooth corner normals and the authored UV survive simplify, no invented normals/UVs, two submesh materials stay contiguous |
| GEOM-MODIFIER-UNSUPPORTED | Legacy skip fixtures now fail explicitly: retopology (declared or not), unknown capability/algorithm/operation, invalid smooth parameter not skipped, undeclared capability, operation version, unknown parameter/setting, missing prototype, tangents; with the backend omitted, simplify/smooth are unadvertised and fail |
| GEOM-MODIFIER-LIMITS | Operations, working memory admitted before the MSL call, vertex budget, pre-cancellation, cancellation observed between stages, later region failing at run time after an earlier region ran publishes nothing |
| GEOM-MODIFIER-ALLOCATION | Fail every allocation of simplify+smooth on a 4-segment sphere, require resource diagnostics and no asset, then verify recovery (modifiers ON only) |
