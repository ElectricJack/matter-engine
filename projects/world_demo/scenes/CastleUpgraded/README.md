# CastleUpgraded

A complete, connected four-wing castle using the new finished-surface backend.
Select `CastleUpgraded` in the editor. It is based on `clustered-court`: a
three-level keep, hall and chapel with upper galleries, a service wing, four
covered connections, and a paved inner court. Wings retain their authored
0/30/−15/15-degree frames, furnished interiors, glazing, stairs, collisions,
player spawn, 132 point lights and four spot lights.

The shared entry point is
`castleSiteWorldDefinition('clustered-court', {surface:true})`. Legacy worlds
keep their existing backend. The same option accepts the other site variants.

- Masonry: direct closed wall profiles with real openings, arch geometry and
  projecting sills. One `CastleBrickBondDetail` atlas, projected from eight
  source-only GPU SDF brick variants, supplies color, normals, roughness and
  parallax. Source brick meshes are neither instantiated nor cached.
- Structure: physical beveled beams, floorboards, slabs, stairs, roof tiles,
  joinery, pegs and straps emitted directly at their authored dimensions.
- Connectors: complete floor and wall profiles, original roof/fascia/tile
  geometry and rafters. Courtyard paving and entrance flags retain their
  physical footprints.
- Furniture: `surfaceMembers:1` replaces stock-fitted voxel member dependencies
  with exact-size beveled geometry. Metal ornament, gold, glass and soft details
  remain. All reusable Part placements are rigid; the foundation is physically
  dimensioned. Local ellipsoid construction still uses DSL scaling inside an
  ornament, never to fit or resize an instance.

All new surface Parts use a single LOD. Wood and floor microdetail have not yet
been projected into dedicated detail atlases; their present detail is geometric.
Parallax does not change outer silhouettes, collisions or hardware ray visibility.
Production wing plans currently contain straight local walls; site rotations
supply the oblique layout. The masonry adapter explicitly rejects curved wing
records until an equivalent radial surface adapter is implemented.

Run the full authoring-contract check with:

```
node --experimental-vm-modules projects/world_demo/tests/castle_upgraded_scene_tests.mjs
```

Visible native validation on 2026-09-11 completed all ten paired raster/RT
captures with zero bake errors and normal exit. All ten were visually reviewed.
The full native world-loader suite passed 51/51 worlds. Focused geometry tests
cover openings, closed shells, complete connector floors, direct furniture,
structure transforms and exact original collision/light records. Capture-driver
regressions pass 11 tests, including split native progress log messages.

Scene census: 267 roots, 84 unique finished Parts, 3,892 entities, zero child
mesh dependencies. Eight GPU SDF source bricks feed one texture atlas; native
logs confirm zero source meshes/bundles. The only remaining voxel-built unique
Part is bed upholstery. Baseline finished meshes total about 1.70 million unique
triangles; furniture still has substantial scope for simplification.

The second full rebuild took 184.1 seconds (82 rebakes, two existing mesh hits,
plus the existing brick atlas), versus 236.1 seconds in the first baseline.
These are observational runs, not an isolated benchmark. The instrumented
rebuild spent 123.7 seconds in JavaScript build spans, 9.6 seconds in meshing
(including 7.3 seconds for upholstery), and 24.5 seconds publishing. Nested
trace spans must not be added as independent costs. That baseline rebuilt each Part’s wing layout in a fresh QuickJS runtime;
the construction-record path below removes this repetition.

The subsequent warm run baked zero Parts and hit all 84 cached meshes: 30.6
seconds engine load, 31.5 seconds process-to-publication, 36.0 seconds to the
first screenshot. An example chapel RT frame measured about 400 ms CPU frame /
334 ms GPU at 1280×720 with validation enabled. The subsecond-loading and fast
RT goals are not met. Keep those optimization tasks open.

New batching reduced full-scene shape submissions from 16,377 to 3,812. Cached
per-operation rotations preserve the complete 1,031,712-vertex structure stream
within 1.78e-15; the Node median roof-emission measurement fell from 387 to 221
ms. Native whole-scene timings also include unchanged layout/asset work.

Screenshots and comparison gallery:
`D:/tmp/Castle Screenshots/2026-09-11 Full Upgraded Castle/gallery.html`.
Native receipts, logs, timelines and cold/warm traces:
`C:/tmp/castle-upgraded-captures/final/`. The native log also retains existing
loader warnings about stale EOS overlay registrations and unused shader
attributes. Fine roof joint aliasing and strongly faceted connector roof shading
remain visible; explicit roof normals/UVs did not eliminate the latter.

Use an SSD cache via `MATTER_CACHE_ROOT`. Press G in the editor to toggle walking
with the authored player. This run verifies collision parity and camera clearance;
it does not constitute a new exhaustive native traversal of every room.

## Startup optimization follow-up

Structural, connector and paving Parts now consume validated construction
records supplied by the world, avoiding repeated full-layout compilation.
The main structure vertex stream and authored collision/light records remain
unchanged. A subsequent full geometry rebuild measured 108.6 seconds; JS build
spans fell from 123.7 seconds to 40.3 seconds.

Supported static worlds now restore their complete authored definition and
ordered material registry from a validated resolve snapshot. PartStore also
reuses saved singleton BVHs and shares verified bundle bytes across admission,
policy and FLAT reads. The first successful optimized warm load measured
15.35 seconds engine time / 16.39 seconds process-to-ready. The older 30.6-second
accepted warm run used different validation/window conditions; these are
observations, not a tightly controlled benchmark. Subsecond startup is still
outstanding. See [the detailed startup profile](../../../../docs/castle-full-startup-profile-2026-09-11.md)
for phase costs, cache validity rules, repeat measurements and test evidence.

Final validation at a fixed 1280×720 completed all ten raster/RT captures with
zero bake/validation errors. Two final warm loads measured 14.75 and 14.61 seconds
engine time (16.49 and 15.42 seconds process-to-ready). Seven native test suites
pass. New reviewed gallery:
`D:/tmp/Castle Screenshots/2026-09-11 Startup Optimized Castle/gallery.html`.
