# Shared mountain road/site layout — JS checkpoint

Implemented in [mountain_routes.js](../../../../projects/world_demo/shared-lib/mountain_routes.js).
This module is **not imported by Streaming Mountains yet**. The editor's current
RT/GI review session remains open and unchanged by this work.

## What exists

- World-space road crowns, crossfall and shoulder ribbons. Shared miter rows
  keep adjoining segments coincident. Grading interpolates the same triangles
  a geometry/collision consumer receives; shoulders and end caps blend smoothly.
- Bounded spatial indexes for local grading and overlap queries. Rendering has
  a separate half-open ownership query; tile size never changes the layout.
- Shared tunnel ranges and entry/exit frames. Deep tunnel interiors suppress
  surface grading so a later excavation pass can preserve the hill above them.
  This is a placement/grade contract, **not an excavated tunnel**.
- Oriented house-site footprints and shared rock/vegetation clearance queries.
  Site pads are records only: foundation grading and house geometry remain.
- Grade, size, sample/index-budget and bend validation. Folded ribbons and
  intersecting roads are rejected; explicit junctions/bridges remain future work.
- Immutable records; stable ordering across route/tunnel declaration order.

Road elevations must be fitted against unmodified terrain before compilation.
There is no implicit terrain sampling inside a sector query, and no claim that
the diagnostic coordinates are a suitable route through the current mountain.

## Evidence and limits

Run from the repository root:

```sh
node projects/world_demo/tests/mountain_routes_tests.mjs
```

[Recorded result and source hashes](routes-js-v1.json), [log](routes-js-v1.log).
The suite checks analytic road/crossfall heights, mesh-to-grade agreement,
shoulder derivatives, end caps, curved and 90-degree joints, translated world
coordinates, subdivision independence, half-open ownership and portal/site
coordinates. It also exercises the existing rock planner: rejected candidates
disappear without changing any remaining instance pose or identity.

All checks pass. The diagnostic fixture has 34 straight sections and 38 curved
sections. Its 10,000 JS grading queries took 23.8 ms on this machine. This is
neither a native terrain benchmark nor a frame-time acceptance result.

No native build, meshing, collision traversal, POM, RT/raster, streaming or visual
acceptance is established by these Node checks. Houses, power lines, tunnel
excavation/interiors and scene integration are unfinished.

## Native integration constraint

The existing field parser limits programs to 96 operations. Expanding every
ribbon/tunnel segment into field arithmetic would exceed that budget and add
work to every terrain sample, including terrain far from a route.

The engine already has an immutable `terrain_field::HeightOverlay`, currently
used by rivers. `FieldRuntime::height_at` and its column cache both apply it.
That interface currently requires a heightfield and cannot represent a tunnel.
Use the spatially bounded route records as data for the next integration;
do not silently raise the field instruction cap or fake tunnels with a road
paint mask. See the [environment plan](../../../superpowers/plans/2026-09-17-streammountain-environment.md#shared-route-and-site-authoring-checkpoint)
for the required native consumers and acceptance.
