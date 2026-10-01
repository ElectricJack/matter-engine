# Explicit periodic brick layout

Status: JS module definition and native three-box proof implemented and checked.
Independent periodic VT address spaces and sparse weathering are still pending.

## Implemented

`brickSurfaceModule` in `projects/world_demo/shared-lib/brick_wall_layout.js`
separates material repeat counts/identity from finite wall dimensions. The
8×4 proof module has a 2.04×0.376-m period, including terminal head/bed joints.
Finite wall width/height subtract the final 10-mm joint. The module's layout
key includes bond, brick dimensions, joints, counts and seed; it excludes wall
extent, phase and weathering. This layout key is not a complete native pixel key.

Optional `moduleColumns`/`moduleCourses` select periodic interior appearance.
Integer `phaseColumns`/`phaseCourses` support negative coordinates. Running-bond
period/phase preserve even course parity. Structural brick IDs and transforms
remain independent of the repeated appearance addresses. End headers remain
whole physical bricks, and internal repeat cuts do not acquire extra headers.
Existing callers without module parameters keep their behavior.

## Validation

`module-v1-js-checks.json` binds four passing suites to source/log hashes:

- New module suite: 1×/2×/4× sizes, translated physical placements, repeat cuts
  through joints and crossing stretchers, whole headers, geometry/source
  agreement, negative phase, large safe integer addresses, identity changes and
  unchanged source recipes across wall sizes.
- Existing wall layout/geometry suite: 40 cases and 780 whole bricks.
- Corner/U/curve suite: 4,970 bake bricks, legal geometry and source reuse.
- Weathering suite: 20 deterministic variants with unchanged source bank/layout.

The native editor uses the previously verified MSVC `grid-v3` executable;
this checkpoint changes project JS and capture tooling, not native code.
[`periodic-module-v1-result.json`](../2026-09-15-wall-surface/periodic-module-v1-result.json)
records eight passing views: group, three close cameras, grazing, native RT,
POM status and chart IDs. Sources/binary stayed unchanged, the editor exited
normally, and there were no reported validation/capture errors, rejected
variants or evictions. Every recorded queue was empty.

The native chart log reports three six-chart, twelve-triangle receivers. The
group draw census reports three draws and **36 raster triangles**. The first
wall prepares 48 source faces; the following walls each report 48 material-cache
hits. Close/RT inspection retains pores and chipped surface relief, and the
close POM diagnostic is fully green. This is evidence for the authoring/layout
step, not general visual approval or new corner/curve rendering acceptance.

![Three finite walls at 1×, 2× and 4× module counts](../2026-09-15-wall-surface/periodic-module-v1-group.png)

![Close view of the repeated brick layout](../2026-09-15-wall-surface/periodic-module-v1-close-right.png)

## Costs and next work

The final scripted path has 124 receiver pages, 116 material allocations and
eight saved duplicate references. Source/mesh budget census is 249.1 MiB;
configured VT pool reservation remains 4,064 MiB. These are different wall sizes
and cameras from `SharedBrickWallProof`, so the counts are not an A/B performance
comparison. Total capture time includes eight fixed 180-frame waits and does
not establish generation latency.

Current native composition still visits each wall's placed-source catalog and
stores receiver-space pages. A periodic pattern is not yet a shared periodic
composed texture. Next, follow
[`2026-09-16-periodic-material-domains.md`](../../../superpowers/plans/2026-09-16-periodic-material-domains.md):
independent module addressing, correct filtering across arbitrary page phases
and mip tails, reuse before generation, then bounded instance overrides. Keep
physical boundary/junction/curve treatment and all broader terrain/VT gates.
