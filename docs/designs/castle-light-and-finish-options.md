# Castle light budgets and surface finishes

These changes require native shader/build and visible GPU acceptance before a
performance or image-quality claim. CPU authoring tests are not render tests.

## Light controls

`MATTER_RT_LOCAL_PRIMARY_BUDGET` and `MATTER_RT_LOCAL_SECONDARY_BUDGET` accept
integers 0 through 8, read once per process. Zero (default) is unlimited;
invalid input uses zero with a warning. Suggested experimental values are 8
and 4. Transmission remains unlimited. Raster lighting remains exact.

Selection runs after per-receiver range, cone, normal and BRDF evaluation.
Luminance of the transmission-weighted unoccluded RGB contribution ranks
shadow-casting lights; equal scores tie by stable light index. Unshadowed
lights do not consume the budget. Zero follows the original loop/RNG path.
The two budgets occupy the existing metadata reserved word; no buffer layout
or push-constant size changes. Changing controls requires process restart.

This is deliberately lossy: shadowed high-score lights can displace lower-score
visible lights. It can darken corners and introduce selection transitions.
Default GPU timing must also be checked for compiler register-allocation costs.
The CPU selector oracle covers stable selection and zero-budget behavior;
native gates must cover mean/variance, motion, disocclusion and gold/glass.

CPU analysis of 136 authored castle lights reproduced the native 8m index:
126 cells, 256 buckets, 21,948 bytes, maximum 86 candidates. At 4m the index is
739 cells, 2,048 buckets, 126,064 bytes, maximum 73 candidates. Across 10,556
synthetic receiver points mean candidates dropped from 53.81 to 37.69 (30%).
These are spatial-query counts, not measured shadow-ray or GPU-time savings.
Use the existing publication index config to opt into 4m; range scaling uses
`make_scaled_local_light_publication` so authored records remain unmodified.

## Connected metal rings

`surfaceMembers:1` now emits full circular rings as connected eight-sided tubes
with smooth outward normals and metric UVs. Default furniture retains capsule
chains; open arcs also retain their old construction. A ring segment previously
emitted a default 16-sector, 6-band capsule (384 triangles); it now emits 16
triangles, a 24x reduction for rings only. Overall furniture savings depend on
the recipe. The smooth tube no longer has the previous overlapping cap bumps.

## Brick relief

`CastleStoneSource.reliefStyle` defaults to 0, preserving the fine recipe.
`CastleBrickBondDetail` opts into style 1: 5mm rounding, 18–40mm face-cut radii,
3–6mm face penetration, and 14–28mm corner cuts. Eight seeds, 23 operations,
physical brick envelopes, face projection and atlas/POM formats are unchanged.
Style 1 rejects physical dimensions below 0.10m. Changed source hashes require
new atlas projection. Repeat the actual 16-face CPU/GPU parity and grazing POM
visibility tests before acceptance.

## Optional wood and floor detail

`CastleWoodGrainDetail` and `CastleFloorWearDetail` are ordinary existing
`Tileset` heightfield recipes with no source Part dependencies or scatter
layers. Both are one metre periodic at 512 texels/metre. Wood amplitude is
bounded by 0.38mm; floor wear by 1.30mm. The atlas pipeline supplies height and
same-field normals; albedo/roughness remain authored material treatments.
`castleFinishMaterialSpec(kind, base)` adds the existing `detail` and
`detailMode:'surface'` fields without mutating the caller's material.

`castleSiteWorldDefinition(name, {surface:true, floorWear:true})` explicitly
binds floor wear. The default scene does not add this atlas dependency.
Directional wood is intentionally not bound to mixed-axis wing assemblies:
surface POM uses Part-local triplanar coordinates, not each beam's face UV frame.
Use it only on an appropriately oriented standalone timber Part until a
frame-aware chart bake can carry every member's grain direction.

Fastener atlas replacement is deferred for the same correctness reason. A
periodic nail texture would invent nail locations and erase authored placements.
A bounded future implementation should bake the existing fastener placement
records into each receiving face chart, preserve protruding hardware in geometry,
and validate atlas gutters/mips and raster/RT agreement. No nails, pegs, plank
gaps, beam bevels or colliders were removed by these optional finish recipes.

## Focused CPU tests

- `MatterEngine3/tests/local_light_index_tests.cpp` (native build pending)
- `projects/world_demo/tests/castle_ring_surface_tests.mjs`
- `projects/world_demo/tests/castle_surface_furnishings_tests.mjs`
- `projects/world_demo/tests/castle_furnishings_tests.mjs`
- `projects/world_demo/tests/castle_solid_source_tests.mjs`
- `projects/world_demo/tests/castle_finish_detail_tests.mjs`
- `projects/world_demo/tests/castle_upgraded_scene_tests.mjs` (`--experimental-vm-modules`)

All listed JS tests passed during implementation. Whole-scene authoring retained
267 roots, 84 unique parts, 3,892 entities and 132 point plus four spot lights.
