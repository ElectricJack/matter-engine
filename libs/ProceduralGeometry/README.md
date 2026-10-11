# Primitive geometry extraction

`matter_geometry` implements the primitive/profile portion of the frozen
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
in millimetres. `bake_placement` supplies an atomic CPU alternative using inverse
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

The core box, sphere, polygon, extrude and placement operations use
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

Spatial queries, mesh processing, CSG, assembly and mesh-resource import belong
to subsequent extraction tasks and return `unsupported_capability` here.
Required document extensions are rejected rather than silently ignored. This
delivery qualifies native CPU geometry; browser/WASM and live editor wiring
remain integration work.

## Canonical implementation and dependencies

The target compiles the existing canonical polygon triangulator. At configure
time it also generates the geometry prefix of the canonical `triangle_emit.cpp`
into the build directory, stopping at `VariationRecorder::instance`. That final
function depends on the world/cache backend and MSVC resolves its unused
reference even with function-section elimination. The seam is guarded and the
canonical source is a configure dependency. There is no checked-in algorithm
copy or symlink. Central manifests, DSL state and legacy sources stay under their
integration owners.

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

Four CTests cover functional fixtures, a direct recipe, allocation failures and
dependency independence. Topology, prototype identity and material bindings are
exact. Position/normal/UV checks use this fixture suite's preselected absolute plus relative
tolerance, `1e-6 + 1e-6 * abs(expected)`; coordinates are millimetres (absolute
position tolerance `1e-9` metres, relative `1e-6`). Profile cap
area coverage uses `1e-6 * max(1, filled_area)` square millimetres.

| Fixture | Recorded expectation |
| --- | --- |
| GEOM-BOX | 12 triangles, size (2,4,6), bounds (-1,-2,-3) to (1,2,3), clay material, outward unit normals |
| GEOM-SPHERE | Radius 1, segments 12, 264 triangles, unit radial smooth normals |
| GEOM-ROUND | Segments 16: line/cylinder 64, cone 32, capsule rings 6: 384 triangles; radius 0.5 and axial bounds checked |
| GEOM-EXTRUDE / FLUSH | Unit square swept 2 mm: 12 triangles, no flat duplicate; unconsumed square: 2 triangles |
| GEOM-HOLES / JOINS | Square with square hole: 32 triangles; bent unit square: miter 20, bevel/round 28 |
| GEOM-CURSOR / TRIANGLES | Retained transform/material/tint snapshots, profile flush at boundaries, paired authored UV/normals, triangle/strip/fan topology |
| GEOM-SHARED / AFFINE | One sphere prototype, two distinct placements; mirrored nonuniform scale (-2,3,4), analytical inverse transpose normals, tangents and atomic singular failure |
| GEOM-MALFORMED / EXHAUSTION / CANCEL / CAPABILITIES | Invalid profiles/envelopes, vertex/working/output limits, cancellation before publication, callback exception, required capability/extension rejection, source mapping; no partial assets |
| GEOM-ALLOCATION | Fail every allocation position in a successful profile/extrusion invocation, require resource diagnostics and no asset, then verify recovery |
| GEOM-INDEPENDENT / DIRECT-RECIPE | Contracts-only native archive and public consumer; one extruded prototype, two placements, 12 triangles |
