# Castle site compiler API

Status: v1 contract for composing complete, independently rotated castle
wings. The source of truth is
`projects/world_demo/shared-lib/castle_site.js`; rigid-frame helpers live in
`castle_frames.js`. Both modules are pure JavaScript and run under QuickJS or
Node.

The site compiler never rewrites a wing's metre-grid manifest. It solves one
rigid world frame per wing, publishes that unchanged local manifest beside its
frame/root transform, and builds separate world-space connectors, room graph,
routes, entry and spawn records.

## Public exports

```js
CASTLE_SITE_SCHEMA                 // "matter.castle-site/v1"
CASTLE_SITE_MANIFEST_SCHEMA        // "matter.castle-site-manifest/v1"
CASTLE_SITE_CAPSULE_RADIUS         // 0.4m
CASTLE_SITE_CLEARANCE              // 0.2m
CASTLE_SITE_MIN_CLEAR_WIDTH        // 1.2m
compileSite(site)
validateSite(site)
siteToJSON(manifest, space = 2)
siteToSVG(manifest, {scale = 24, padding = 24, levelId = 'ground'})
emitSite(manifest, {wing, connector})
sitePartRecipes(manifest, {wing, connector})
```

`validateSite` performs the same compilation and validation as `compileSite`;
both throw an error beginning with `castle site` and an input path on failure.
`siteToJSON` returns pretty JSON with a trailing newline. `siteToSVG` defaults
to a legible ground-level sheet and filters walls, rooms, links and routes by
`levelId`; request `upper` (or another authored level) for a separate sheet.
It draws actual transformed walls, courtyard floors and portals, convex
connector polygons, named room uses, wing angles and global routes.
Green portal marks show the finite clear opening, with the remaining wall
drawn separately. Labels use separate halo and text elements for portable SVG
renderers. `tools/export-castle-sites.mjs` exports every authored level.

`castle_frames.js` publishes point/vector and inverse transforms,
frame solve/compose, yaw/direction/quaternion conversion, row-major root
matrix composition, ECS entity transforms, AABB-to-OBB conversion, box and
convex-hull collider transforms, and spot-light transforms. Positive yaw is
always:

```text
x' = cos(yaw) * x + sin(yaw) * z
z' = -sin(yaw) * x + cos(yaw) * z
```

Points receive the frame origin; vectors do not. Rotated coordinates are never
rounded to the global grid.

## Authored site

```js
{
  schema: CASTLE_SITE_SCHEMA,
  id: 'angled-study',
  seed: 9411,
  grid: 1,
  angleStep: 15,
  entry: {wing:'core', level:'ground', portal:'main-entry'},
  wings: [
    {id:'core', plan:corePlan, frame:{origin:[0,0,0], yawDeg:0}},
    {id:'hall', plan:hallPlan, placement:{
      socket:{level:'ground', portal:'west-entry'},
      relativeTo:{wing:'core', level:'ground', portal:'east-hall'},
      yawDeg:30, outset:6, lateral:0,
    }},
  ],
  connections: [{
    id:'vestibule',
    a:{wing:'core', level:'ground', portal:'east-hall'},
    b:{wing:'hall', level:'ground', portal:'west-entry'},
    floor:'stone', height:3.6,
    roof:{kind:'low-hip', rise:0.8},
  }],
  courtyards: [{
    id:'inner-court', level:'ground',
    clearPolygon:[[4,11.7],[8,11.7],[8,18],[4,18]],
    floor:{material:'stone',thickness:0.3},
    sockets:[{wing:'core',level:'ground',portal:'north-court'}],
  }],
}
```

Every wing declares exactly one of `frame` or `placement`. A placement depends
on an already explicit or recursively solved target wing, but input order is
irrelevant. `outset` advances along the target socket's transformed outward
normal and `lateral` along its stable transformed tangent. The moving origin
is solved so its selected local mouth center reaches that target point after
yaw. Cycles, missing dependencies and yaw values off `angleStep` are rejected.

A socket is resolved by the exact triple `wing + level + authored portal
sourceId`. Only an exterior `door` or `arch` with one real room and physical
1.2m-by-2.1m clearance is eligible. Exterior threshold centres are derived
from portal geometry because `roomThresholds.outside` intentionally does not
exist. The entry socket is reserved; each connection socket can be consumed
only once.

`courtyards` is optional. Each record declares a counter-clockwise convex
world-space supported floor polygon and one or more exterior wing sockets.
Those sockets are consumed just like connector mouths. Compilation emits a
walkable `site:courtyard:<id>` node plus one threshold edge per socket; it does
not merge the court with `outside`. This lets an enclosed outdoor court remain
reachable while the selected `entry` stays the site's only outside edge. Court
socket arrays are sorted by their semantic wing/level/portal tuple, and a court
floor that cuts through a wing beyond its host wall interface is rejected.
At overlapping elevations, positive-area court/court, court/connector-floor
and court/connector-wall intersections are also rejected; shared edges remain
valid. Elevation tests use the complete floor slab interval
`[baseY - floor.thickness, baseY]`, not only the walking surface, so a raised
court cannot descend into the top of a connector wall. Connector walls are
also tested against the walking surface itself: a wall standing on the court's
level may share an edge with its clear polygon but never occupy it. Solids
that only meet the slab's underside are permitted.

## Compiled site

Each `wings[]` record contains:

```js
{
  id,
  frame:{origin:[x,y,z], yawDeg},
  rootTransform:[/* row-major 4x4 */],
  manifest,                         // unchanged local castle manifest
  world:{occupiedVolumes, fixtures, localLights},
}
```

`rootTransform` is
`[c,0,s,tx, 0,1,0,ty, -s,0,c,tz, 0,0,0,1]`. World AABBs become OBBs rather
than enlarged axis-aligned boxes. Fixture positions, degree yaw and direction
vectors transform together; spot positions transform as points and their
directions as vectors. For an ECS entity, `transformEntity` composes only its
`LocalTransform` and leaves local collider geometry unchanged, preventing a
double transform. Use the separate hull/box helpers when the collider itself
is stored in world space.

The global `roomGraph` namespaces every wing-local room, edge, portal, floor
and swept-volume identity with the wing ID. Local manifests retain their own
outside edges, but the global graph suppresses all of them except the selected
site entry, matched by wing, level and portal. Namespace tuple components are
percent-escaped (`%` is serialized as `~`) and compiled node/edge uniqueness is
asserted, so authored colons cannot alias reserved site IDs. A consumed socket
is represented only by its compound connector
edge. Global `walkRoutes` are ordered world-space polylines and preserve every
compiled stair waypoint, including intermediate landing turns. Between graph
edges the site compiler calls `routeManifestRoomSegment` from
`castle_plan.js`, so the same floor-hole, stair, beam and fixture-clearance
solver produces each namespaced `roomSegments` record. Rotated world swept
rectangles are published as OBBs rather than misleading global AABBs.

## Connector handoff

`site.connectors` is the geometry-neutral input to
`castle_connector_kit.js`. Each record is:

```js
{
  id, level, baseY,
  clearPolygon:[[x,z], ...],        // convex, counter-clockwise, world space
  mouths:[{
    wing, level, portalId, roomId, clearWidth, center:[x,y,z],
    inside:[x,y,z], outside:[x,y,z],
    segment:[[x,z],[x,z]], tangent:[x,z], outward:[x,z],
    insideSegment:[[x,z],[x,z]], outsideSegment:[[x,z],[x,z]],
    throatPolygon:[[x,z],...],
    wallThickness, hostModules:[...], jambOwner,
  }, {…}],
  wallSpans:[{
    id, segment:[[x,z],[x,z]], tangent:[x,z], normal:[x,z],
    thickness, height, material, courseOrigin:[x,y,z],
    cornerOwners:[start,end], jambOwners:[a,b],
    trimPlanes:[{normal:[x,z],offset,keepSign}, …],
  }],
  floor:{thickness,material,owner},
  height,                            // full wall/eave enclosure height
  clearHeight,                       // effective passage headroom
  roof:{kind,rise,material,overhang},
  routeWaypoints:[[x,y,z], ...],
}
```

`portalId` is the authored source ID, not the compiler's prefixed aperture ID.
All connector geometry is world-space. `segment` is the authored clear-width
mouth line on the wall centre plane. `insideSegment` and `outsideSegment` keep
the authored clear-width endpoints translated exactly onto both wall faces;
`throatPolygon` is their finite convex wall-thickness prism. The connector
polygon extends through each wall to the interior threshold
and its minimum caliper width must fit a radius-0.4 capsule plus 0.2m clearance
on both sides. Narrow joins, non-facing/intruding mouths,
positive-area wing overlap (including exact circular boundaries), courtyard
floor overlap and connector floor/wall-solid intrusion,
incompatible elevations, duplicate sockets, invalid polygons and globally
disconnected required rooms are rejected.
Courtyard overlap uses the full slab elevation interval, so a raised floor
cannot cut through a lower connector wall. Shared boundaries remain valid.
Every emitted connector route waypoint is checked against its finite wall
spans with the same capsule radius and side clearance used by the kit.

Participant wings are not exempt from intrusion checks. Only the finite prism
through the host wall and the bounded, owned jamb join may meet the host wing;
positive area in the actual room interior or another wing is rejected. This
lets asymmetric connector stones own the miter without widening the declared
portal opening or allowing a long connector to cut through another room.

Participant wings are not exempt from intrusion checks. Only the finite prism
through the host wall and the bounded, owned jamb join may meet the host wing;
positive area in the actual room interior or another wing is rejected. This
lets asymmetric connector stones own the miter without widening the declared
portal opening or allowing a long connector to cut through another room.

The authored vestibule `height` may exceed a door aperture: a 3.6m enclosure
meeting a 2.8m arch is ordinary architecture. `height` drives side walls and
the roof base; `clearHeight` is `min(height, both portal clear heights)` and
drives walk/capsule clearance. The connector kit additionally checks the finite
18 by 14 cm rafter cores, transformed with the same yaw/pitch as the rendered
stock beams and clipped against the full `clearPolygon`. If a core dips below
`baseY + clearHeight`, normalization fails with `roof.rafters[index]` and requires
an explicitly higher enclosure. The closed roof shell sits above `height`;
rafters sit below its timber soffit. `connectorRoofClearance(record)` exports
per-rafter oriented faces, clipped minimum Y and the clearance reserve for
inspection. This validation preserves the declared passage instead of silently
reducing headroom on low-rise or long connectors.

The compiler validates its emitted route waypoints against every finite wall
span using the same radius-0.4 plus 0.2m side-clearance gate as the connector
kit. A site record accepted by `compileSite` therefore does not fail later at
the mandatory connector-record boundary.

The frozen executable fixture is
`projects/world_demo/tests/fixtures/castle_site_angled_study.js`. It contains a
12x12m core whose east mouth is `[12,0,6]` and a 14x8m hall whose west mouth is
local `[0,0,4]`. At yaw 30 and outset 6 the solved hall origin is
`[16,0,2.5358983848622456]` and its mouth is exactly `[18,0,6]`.
