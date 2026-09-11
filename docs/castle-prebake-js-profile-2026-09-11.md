# Castle pre-bake JavaScript profile — 2026-09-11

A native `CastleClusteredCourt` launch reportedly consumed about 300 CPU seconds
before its first bake progress log. **That interval is not an isolated world
construction measurement.** The provider evaluates the world, then resolves the
complete Part dependency DAG before the first bake callback. This investigation
profiles the shared JavaScript construction separately and applies one safe
whole-wing overlap broadphase.

## Measurements

Node construction of `castleSiteWorldDefinition('clustered-court')` produced
242 roots, 3,931 entities and 132 point lights in 1.06 seconds with CPU sampling.
A separate `node --jitless` run took 1.52 seconds. V8 without JIT is not QuickJS;
these measurements do not establish native runtime or explain the entire
300-second interval.

An instrumented cold construction reported these inclusive costs before the
broadphase change. Nested timings overlap and must not be added together.

| Function/work | Calls | Inclusive time |
| --- | ---: | ---: |
| Interior decorator | 4 | 300.5 ms |
| Plan compilation | 22 | 162.8 ms |
| Furniture placement descriptors | 6,338 | 84.2 ms |
| Structural collision adapter | 4 | 328.9 ms |
| Structural layout | 4 | 263.3 ms |
| `opSolids` | 10,932 | 165.4 ms |
| `frameRows` | 85,189 | 107.8 ms |
| Wing overlap validation | 1 | 57.1 ms |
| Detailed room-volume overlap tests | 77,064 | 46.3 ms |
| SAT projection tests | 84,556 | 31.2 ms |

The decorator deliberately recompiles routes from each exterior portal, which
contributes to the 22 plan compilations. Its candidate search creates 6,338
placement descriptors despite the much smaller number of accepted fixtures.
Structural record anchors currently derive their union bounds from sliced
`opSolids`; transforming each corner repeatedly rebuilds the same frame matrix.
These are distinct potential follow-ups, not changes made in this patch.

## Implemented broadphase

`castle_site.js` now groups room volumes by wing and computes conservative
whole-wing bounds. Only wing groups whose bounds can intersect contribute
candidate cell pairs. The original detailed test and tolerances remain intact.
Candidate indices retain the original `i,j` visitation order, preserving the
first reported overlap when several wings collide. Circular rooms use analytic
center/radius extents rather than their inscribed display polygons.

The profiled compact-castle validator fell from 57.1 ms to 3.34 ms. Complete
compiled outputs are unchanged for all three decorated sites:

| Site | Old detailed overlap calls | New calls |
| --- | ---: | ---: |
| Clustered court | 77,064 | 0 |
| Angled bailey | 116,664 | 27,648 |
| Bent palace | 172,832 | 18,944 |

`castle_site_broadphase_tests.mjs` compares the optimization with the original
exhaustive narrowphase over 183 synthetic cases and all three full site
manifests. Cases include rotated cells, circle/circle and circle/cell overlap,
height separation, an empty wing, exact first-error ordering, and a small square
intersecting the true circle just beyond its inscribed polygon. The existing
site and decorated-site integration suites also pass.

```bash
node projects/world_demo/tests/castle_site_broadphase_tests.mjs
node projects/world_demo/tests/castle_site_tests.mjs
node projects/world_demo/tests/castle_site_programs_integration_tests.mjs
```

## Native attribution

`LocalProvider::load_authored_world` calls the statics loader
`load_world_definition`, which evaluates the World once and canonically
deduplicates its imported modules. These castles declare no field method;
the separately documented `ScriptHost::eval_world` fallback that may evaluate a
field world twice does not explain this launch.

`LocalProvider` subsequently calls `PartGraph::install`. Its recursive resolve
walk evaluates `get_requires`, merges/defaults parameters and computes hashes
for the entire dependency DAG before the topological bake loop starts. Script
host declaration/evaluation operations use fresh QuickJS contexts. Module-local
JavaScript caches therefore cannot share computed manifests across those
operations.

Castle structure/masonry requirements call `castleSiteWingManifest`; its cold
lookup constructs the decorated site program, including other wings. Connector
and paving requirements similarly call the compiled-site lookup. The retained
gallery census contains 40 `CastleWingStructure` recipes and 16 each of the
connector mesh/assembly recipes, in addition to 15 masonry and 15 structure
assembly recipes. Repeating authoring work in isolated contexts can therefore
multiply costs during **pre-bake DAG resolution**. Native phase timing is needed
to quantify that contribution; no native performance improvement is claimed
from the Node result alone.

Possible future work is a narrowly scoped cache/index for repeated frame
matrices or lazy decoration of only the requested wing. Cross-context reuse
belongs in separately designed engine work. Neither is implemented here.

Temporary evidence remains in `/mnt/d/tmp/castle-world-profile/`:
`clustered.cpuprofile`, `profile.mjs`, `instrument.mjs`, `instrumented.json`, and
`castle_site_before.js`. The CPU profile and saved source describe the state
before the broadphase; the instrumented JSON describes the state afterward.
No native editor or GPU was launched for this diagnostic.
