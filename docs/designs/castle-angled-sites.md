# Angled castle sites

The user rejected the rectangular courtyard study on 2026-09-11 and requested
15/30/45-degree walls and more interesting real floor plans. This supplements
the castle kit spec; the final three scenes must reflect this direction.

## Architectural references

- [Burg Eltz plan](https://commons.wikimedia.org/wiki/File:Grundriss_Burg_Eltz.svg):
  clustered differently oriented houses, a narrow irregular court, attached
  stair towers and uneven building heights. Use this spatial pattern for a
  compact residential fortress, without copying the surveyed plan.
- [Warkworth phased plan](https://www.english-heritage.org.uk/siteassets/home/visit/places-to-visit/warkworth-castle/history/warkworth-castle-and-hermitage-phased-plan.pdf):
  an angled curtain enclosure, substantial open bailey, offset keep and service
  buildings around the perimeter. Use this for a more defensive variation.
- [Chillon visitor plan](https://www.chillon.ch/wp-content/uploads/2020/02/notices-de-visites-all.pdf):
  elongated enclosure, successive courtyards, changing wing directions and a
  central keep. Use this for a long, bent palace/chapel variation.

Reference downloads for local inspection: `/mnt/d/tmp/castle-plan-references/`.
These are inspiration and research, not assets to embed into the product.

## Retain local grids and compose them

Keep `compilePlan()` and each complete wing manifest in its existing local
one-metre coordinates. A new `compileSite()` composes independently rotated
wing scaffolds with explicit polygon vestibules. Do not rotate a manifest and
replace its floors/walls with world-axis-aligned bounding boxes. Rotation is a
real rigid frame applied consistently to rendered roots, colliders, lights,
fixtures, spawn and routes. Positive yaw uses the engine's Y rotation:
`x' = cos(yaw)*x + sin(yaw)*z`, `z' = -sin(yaw)*x + cos(yaw)*z`.
One module owns matrix/quaternion/point/vector conventions and tests.

```js
compileSite({
  schema: 'matter.castle-site/v1', id: 'angled-study', seed: 9411,
  grid: 1, angleStep: 15,
  entry: {wing: 'core', level: 'ground', portal: 'main-entry'},
  wings: [
    {id: 'core', plan: corePlan,
      frame: {origin: [0,0,0], yawDeg: 0}},
    {id: 'hall', plan: hallPlan, placement: {
      socket: {level: 'ground', portal: 'west-entry'},
      relativeTo: {wing: 'core', level: 'ground', portal: 'east-hall'},
      yawDeg: 30, outset: 6, lateral: 0}},
  ],
  connections: [{id: 'vestibule',
    a: {wing: 'core', level: 'ground', portal: 'east-hall'},
    b: {wing: 'hall', level: 'ground', portal: 'west-entry'},
    floor: 'stone', height: 3.6,
    roof: {kind: 'low-hip', rise: 0.8}}],
});
```

`portal` means the existing portal's authored sourceId, qualified by level and
wing. `outset`/`lateral` are metres along the target socket's outward normal and
tangent. Solve the moving wing's origin so the selected socket reaches that
target point after rotation. Reject cyclic placement dependencies and yaw that
does not match angleStep. Rotated world coordinates may be fractional; never
round them to the global grid. Reuse shared endpoint IDs and solved coordinates.

Sockets include inside/outside threshold centers, tangent/outward normal,
clear-width mouth endpoints on both wall faces, level elevation, clear height,
wall thickness, host modules and jamb ownership. Derive the exterior threshold
geometrically; `roomThresholds.outside` is not guaranteed to exist. Only exterior
doors/arches can become compound links. Consumed sockets cease to connect to an
independent outside node. Only the selected site entrance does so.

Output retains `wings: [{id,frame,manifest}]`, deterministic `connectors`, a
namespaced global roomGraph, continuous world-space walkRoutes and entry/spawn.
Validate positive-area wing overlap, wing/connector intrusion, incompatible
elevations, reused sockets, missing/disconnected required rooms and invalid
polygons. Keep volume checks at their actual elevations. Publish JSON and SVG
site exports, including room uses, links, angles and routes.

## Connector geometry contract

The site compiler and connector kit owners must publish one concrete JSON
fixture immediately and communicate record changes directly. Each connector
record carries: id, level/baseY, clearPolygon (convex XZ vertices), two mouth
segments and portal IDs, wall spans with finite local tangent/normal frames,
wall thickness, course origin, explicit corner/jamb ownership and trim planes,
floor thickness/material, clear height, roof record and route waypoints.

The polygon joins the finite portal mouths and provides continuous supported
passage through both wing wall thicknesses to the interior thresholds. Its
clear passage must fit a radius0.4 capsule with0.2m clearance; reject a narrow
join instead of silently narrowing the route. The polygon must not intrude
into either wing outside the declared throat. Corner/threshold ownership must
prevent duplicated floors or masonry across interfaces.

`castle_connector_kit.js` owns detailed geometry and collision derived from
these records. Interior stones/planks reuse the existing parts. Boundary floor
pieces and angled wall ends must be clipped against the real polygon/planes,
not approximated by enclosing boxes. Add a voxel-CSG cut-stone Part with flat
scalar parameters for asymmetric cuts; ordinary overlapping bricks and the
existing symmetric wedge alone are insufficient. Course heights match across
adjacent spans, miter stones have one owner, and mortar remains recessed.
Roof facets/tiles/rafters are clipped to the connector and leave usable
headroom. A low hipped vestibule roof is sufficient initially.

Use existing ConvexHullCollider.points for exact convex floor/wall prisms
(flat xyz array, at most32 vertices); decompose if needed. Reuse oriented boxes
for rectangular pieces. No new engine feature is required for this extension.

Library API: `connectorRecipes(siteOrRecords,{module,materials,detail})`,
`connectorChildVariants(record,params)`, `emitConnector(part,record,params)`,
`connectorCollisionEntities(record,{prefix})`, and geometry validation/solid
volume exports. Recipes use flat lookup parameters and existing root transform
conventions. Any root with inline geometry must remain unexpanded.

## Ownership, parallel work and acceptance

Two new Codex tasks implement (1) frames/site compiler and (2) connector kit.
They may use subagents for independent tests/fixtures, with explicit file
ownership. Existing masonry/structure workers retain their modules. Avoid
overlapping edits; publish executable append-only checkpoints early. Each task
must use real DSL/native geometry where applicable. Coordinate GPU captures;
kill only its own editor PID. Astra owns redesigned wing programs, world
assembly, final screenshots, lighting balance and physical walkthroughs.

First fixture:12x12m core, east mouth at[12,0,6];14x8m hall with west mouth at
local[0,0,4], yaw30, world mouth[18,0,6]. Hall origin is[16,0,2.5358983849].
Prove core-to-vestibule-to-hall traversal, reverse traversal, and an upper
floor/stair route. Repeat15/30/45-degree arrangements and negative rotations.
Test exact floor support, non-overlapping miter ownership, invalid/narrow links,
rotated boxes/hulls/spot directions and deterministic input-order behavior.

Final acceptance requires three visibly different irregular castle sites, not
three complete rectangular castles rotated as units. Rendering a blank ground
plane is collision-only evidence and never visual acceptance.
