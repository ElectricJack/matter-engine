# Castle plan compiler API

Status: stable v1 topology contract for the grid-castle kit. The source of
truth is `projects/world_demo/shared-lib/castle_plan.js`. It is pure JavaScript:
it does not call `World`, `Part`, the filesystem, or a random API.

All authored X/Z distances are metres, Y is up, and a grid cell at `[x,z]`
covers `[x,x+1] × [z,z+1]`. Integer X/Z coordinates may be negative. Geometry
inside a storey, including stair and clearance rectangles, may use finite
floating-point coordinates.

## Public exports

```js
CASTLE_PLAN_SCHEMA       // "matter.castle-plan/v1"
CASTLE_MANIFEST_SCHEMA   // "matter.castle-manifest/v1"
canonicalEdge(from, to)
validatePlan(plan)
compilePlan(plan)
planToJSON(manifest, space = 2)
planToSVG(manifest, { levelId, scale = 32, padding = 24 })
emitManifest(manifest, emitters)
manifestPartRecipes(manifest, moduleNames = {})
```

`canonicalEdge` accepts one axis-aligned, one-metre integer-grid edge and
returns canonical `from`, `to`, `key`, and `axis` fields. `validatePlan`
performs the same compilation and validation as `compilePlan`; on success it
returns `{ valid: true, errors: [] }`, and on failure both functions throw an
`Error` whose message starts with `castle plan` and includes an input path.

Compilation does not mutate the input. Unordered collections and semantic IDs
are sorted from level IDs, room IDs, coordinates, endpoints, or explicit
record IDs, so reordering levels, rooms, overrides, beams, and other unordered
record families does not change `planToJSON(compilePlan(plan))`. Flight order
and the order of intermediate landings are semantic stair sequence.

## Authored plan

The top-level shape is:

```js
{
  schema: CASTLE_PLAN_SCHEMA,       // optional, but must match when present
  id: "plan-id",                   // required
  seed: 0,                          // optional integer
  entryRoomId: "entry-room",       // required
  style: {                          // optional
    wallThickness: 0.6,
    wallMaterial: "castle.stone",
    bond: "ashlar",
    exteriorProfile: "exterior",
    interiorProfile: "partition",
  },
  levels: [/* level records */],    // at least one
  curves: [],
  stairs: [],
  beams: [],
  verticalVoids: [],
  roofs: [],
  fixtures: [],
  localLights: [],
}
```

Omitted optional collections behave as empty arrays.

### Levels, rooms, and physical boundaries

A level requires `{ id, baseY, height, rooms }`; `height` must be in `[2,6]`.
Room IDs are unique across the whole plan. A room requires `{ id, use }`, may
set `required:false` to opt out of reachability, and chooses exactly one
boundary representation:

```js
{ rect: { x, z, width, depth } }                 // positive integer extent
{ cells: [[x,z], ...] }                          // non-empty integer cells
{ boundary: { kind: "circle", center:[x,z], radius } } // integer centre/radius
```

Explicit cell unions must be edge-connected and contain no duplicates. Grid
rooms on a level may not share cells. Circular rooms may not overlap another
circle or the interior of any grid cell on that level; tangency alone is
allowed. A circular room also requires a same-centre, same-radius `ring` curve
and at least one finite radial-throat portal (described below).

An upper-storey air volume over a lower room is authored as a cell/rectangle
room with `{use:"void", required:false, openToBelow:true, lowerRoomId}`. Its
cells must all be covered by the named room on a lower level; circular
`openToBelow` rooms are not supported. It remains a topology boundary for wall
and void ownership but emits no floor or room occupied volume, is marked
`walkable:false` in `roomGraph.nodes`, cannot be the entry room, and cannot be
a circulation-portal endpoint.

Each room may additionally author:

```js
{
  floorType: "stone",       // default
  floorThickness: 0.25,     // default; must be positive
  floorStructure: {
    bearingEdgeIds: [/* compiled wall ids */],
    intermediateSupportIds: [/* compiled beam ids */],
    joistDirection: "x" | "z",
    joistSpacing: 0.5,
    joistMemberIds: [/* compiled beam ids */],
  },
}
```

When `bearingEdgeIds` is omitted, every non-open compiled boundary wall for the
room is a bearing edge. Intermediate-support and joist-member IDs are checked
against compiled `beamMembers`. A room whose `use` is `court` still emits a
floor record, with `openToSky:true`.

`level.edgeOverrides` replaces one or more existing room-boundary metres:

```js
{
  id: "hall-gallery-open",
  from: [x0,z0], to: [x1,z1],       // non-zero, axis-aligned integer segment
  kind: "wall" | "open" | "door" | "window" | "arch",
  connects: ["hall", "gallery"],   // only for open/door/arch
  opening: { offset, width, bottom, height },
  thickness, material, bond, profile,
  railProfile,
}
```

Overrides may not overlap and every covered metre must already be a physical
room boundary. `connects` is not trusted as abstract graph metadata: for a
partition it must name exactly the two rooms that own the two sides, and for an
exterior boundary it must name that room plus `outside`. An internal door or
arch may omit `connects`; the two physically adjacent rooms are then inferred.
An `open` circulation join and every exterior connection must declare
`connects` explicitly. Omitting it from `open` deliberately creates a guarded
void edge, not an inferred portal.

Door, arch, and open circulation clearances must be at least 1.2m wide, begin
at `bottom:0`, and be at least 2.1m high. Windows default to a 1m sill and do
not create graph edges. `offset` is measured from the authored `from` endpoint;
the compiler canonicalizes reversed segments and emits one aperture identity
and one global interval across all affected metre edges.
Circulation apertures are rejected if any metre-local slice enters the owned
trim volume at a non-straight junction.

`kind:"open"` is a full-span boundary, not a partial aperture: its opening
width must equal the complete override length. An open boundary with two room
IDs is a physical room/gallery join; one with a room and `outside` is an
exterior entry. An open boundary without `connects` is classified as a
`void-edge` and emits a required rail, using `railProfile` or
`castle.guardrail`. Open edges do not emit wall modules or wall junction
incidence. This is the contract for balcony and gallery edges: author the
complete open span and its room IDs when it is a circulation join; leave it
unconnected only when it is a guarded void edge.

### Curves: tangent transitions versus radial room entries

A curve record is authored independently of grid-wall overrides:

```js
{
  id: "turn-or-ring",
  levelId: "ground",
  roomId: "room-owning-the-curve",
  kind: "quarter" | "ring",         // default: quarter
  center: [x,z], radius: 3,          // integer grid values, radius > 0
  start: "E", end: "N",            // quarter only
  clockwise: false,                  // quarter only
  requireGridJoin: true,             // optional quarter endpoint gate
  thickness, height, material, bond, profile,
  apertures: [/* angular aperture records */],
}
```

A quarter must describe exactly 90 degrees. Each endpoint emits an exact
cardinal position and tangent vector. Its `socket` is strictly a
`kind:"tangent-wall"` socket: matches are on the same level, at the same
vertex, tangent rather than radial, not `open`, and compatible in thickness,
height, material, and bond. `requireGridJoin:true` rejects an endpoint with no
compatible straight wall. Each actual match also emits a `curveTransition`
record. A tangent transition joins wall sections; it is not a doorway and adds
no circulation edge.

Angular apertures are ordered, non-overlapping intervals:

```js
{
  id: "tower-door",                 // optional deterministic default
  kind: "open" | "door" | "window" | "arch",
  startAngle: -12, endAngle: 12,
  bottom: 0, height: 2.2,
  connects: ["tower", "lobby"],
  throat: {
    targetRoomId: "lobby",
    direction: "E",
    width: 1.2,
    depth: 1,
    overlapDepth: 0.3,               // default: half the curved wall thickness
    hostApertureId: "lobby-arch",    // optional if exactly one host matches
  },
}
```

An aperture must fit the curve sweep and wall height. A circulation aperture
must start at its level floor and provide at least 2.1m height. `connects` must
include the curve's owning room and a known room on the same level (or
`outside`), but `connects` alone never creates a route.

A doorway from a circular room to a grid room needs the nested finite-width
`throat`. The direction must pass through the aperture interval, its arc must
be at least as wide as the throat, width must be at least 1.2m, and its positive
depth must reach a compatible exterior boundary cut belonging to the named
same-level target room. `overlapDepth` must be positive and smaller than the
curve radius. The target room floor must cover the complete
`targetFloorBounds` rectangle from the cardinal wall plane outward.

The paired straight boundary must have one door/arch/open aperture which spans
the complete throat width, starts at the floor, and is at least as high as the
radial aperture. If exactly one aperture matches, `hostApertureId` may be
omitted; otherwise author its override ID or full canonical aperture ID. The
compiler rejects no match and ambiguity. It records the canonical
`hostApertureId`/`targetWallApertureId`, marks every host opening slice and
module aperture with `claimedByRadialThroatId`, and omits that claimed aperture
from ordinary grid portals. The one radial portal therefore joins the circle
room to the target room without also creating a false target-to-`outside`
route.

For usable circular-floor radius `r - curve.section.thickness/2`, the compiler derives
`chordDistance = sqrt(usableRadius^2 - (width/2)^2)`. `bridgeDistance` is the
smaller of that chord distance and `radius-overlapDepth`; the inner
`threshold` lies at that distance, `wallThreshold` lies on the authored curve,
and `farThreshold` lies `depth` beyond it. Thus `floorPatch.bounds` and
clearance `bounds` overlap the true circle interior across the full door width
and bridge to the grid plane rather than beginning at a single tangent point.
The compiled record also includes `chordDistance`, `bridgeDistance`,
`targetFloorBounds`, target wall edge IDs, reveal IDs, side-return IDs, and the
owned angular `wallCut`.

Radial room-entry records and tangent curve-transition records are deliberately
separate collections and ID families. A ring has no tangent endpoints. A
quarter endpoint socket never doubles as a radial throat.

### Stairs, landings, and holes

Stairs use explicit flights and landings; the old aggregate footprint/label is
not a v1 staircase:

```js
{
  id: "main-stair",
  lowerLevelId: "ground", upperLevelId: "upper",
  lowerRoomId: "hall", upperRoomId: "chamber",
  width: 1.2, tread: 0.25, maxRiser: 0.2, headroom: 2.2,
  entryDirection: "E", exitDirection: "E", // optional; inferred from flights
  flights: [{
    id: "flight-1", direction: "E", stepCount: 20,
    footprint: { x, z, width, depth },
  }],
  landings: [
    { id:"lower", kind:"lower", bounds:{ x,z,width,depth } },
    { id:"turn", kind:"intermediate", bounds:{...}, elevation:2 },
    { id:"upper", kind:"upper", bounds:{ x,z,width,depth } },
  ],
}
```

The upper level must be above the lower. Width is at least 1.2m, tread at least
0.25m, `maxRiser` is in `(0,0.2]`, and headroom is at least 2.1m. The compiler
derives total rise, `ceil(rise/maxRiser)` steps, and the actual riser. Authored
flight step counts must total that derived count. Each flight footprint must fit
its derived travel run and transverse stair width, and must be covered by both
named stacked room floors. This rejects remote flights and flights crossing a
partition into another room.

There must be exactly one lower and one upper landing and one intermediate
landing between every consecutive pair of flights. Both dimensions of every
landing are at least the stair width. The end landings must cover the complete
directed, full-width end of the first/last flight. Each intermediate landing's
elevation equals the adjoining flights' derived join elevation and its bounds
cover both directed full-width flight ends. Lower and intermediate landings
belong to the lower room; the upper landing belongs to the upper room. Each
landing publishes `sweptVolumeId`. Flight and landing swept volumes extend
through the required headroom and are rejected when they intersect a compiled
beam member.

Compiled `stair.holes` and the synthesized `verticalVoid.footprints` are the
exact union of flight footprints plus non-lower (intermediate and upper)
landing footprints. `stair.voids[].bounds` is only an enclosing diagnostic
rectangle; its `regions` are the cut geometry. Each exact vertical-void
footprint attaches a separate floor-hole record with `{id,voidId,kind,
footprint,regions:[footprint],replacementLandingId}`. Consumers subtract each
record independently and retain deck inside any gap in the stair's aggregate
bounds. The lower landing is not an upper-deck cutout; an intermediate or upper
landing matching a cutout is named by `replacementLandingId`.

The stair also emits flight centre lines, landing links, ordered route
waypoints, flight and landing swept-volume IDs, lower-ceiling and upper-floor
voids, and one room-graph edge between its named rooms.

### Beams, vertical voids, and pass-through records

A beam member is:

```js
{
  levelId, from:[x,y,z], to:[x,y,z], section:[width,height],
  jointFamily, role, material,
  bearing, joist, spacing,
}
```

Endpoints are finite and distinct; section dimensions are positive. Reversed
endpoints canonicalize to the same member, and duplicate level/endpoints are
rejected even when roles differ. Compiled members include `bearing` metadata
and `joist` metadata (for `role:"joist"`, `{spacing}` is synthesized if
`joist` is omitted).

Authored non-stair vertical voids use:

```js
{
  id, kind: "double-height" | "shaft",
  lowerLevelId, upperLevelId,
  footprint: {x,z,width,depth},
  lowerRoomId,                       // lower ceiling owner
  upperRoomIds: [],                  // upper floors/air rooms crossed
  roomIds: [],                       // optional combined form
  railProfile,
}
```

At least one lower-level room must be identified by `lowerRoomId` or
`roomIds`; it owns the penetrated ceiling. Every explicit upper room must be on
`upperLevelId`, and each walkable upper room must cover the whole footprint.
Overlapping `openToBelow` rooms tied to a named lower owner are included
automatically. Only real upper floors appear in `penetratedFloorIds`; air rooms
appear in `openToBelowRoomIds` and have no synthetic floor to cut.

Compiled records expose `lowerRoomIds`, `upperRoomIds`,
`openToBelowRoomIds`, exact `footprints`, Y bounds, penetrated floor/ceiling
IDs, and a rail profile. An `openToBelow` room not claimed by an authored void
generates its own `vertical-void:open-to-below:*` record with one footprint per
cell. Every stair also synthesizes one `kind:"stair"` vertical void. Floor,
ceiling, rail, and roof emitters must consume these records rather than
reconstructing shafts independently.

`roofs`, `fixtures`, and `localLights` are stable pass-through records. Each
requires `id` and an existing `levelId`; the compiler prefixes the ID with the
singular collection name and retains the authored fields. A fixture with a
`clearance` field also contributes a `fixture-clearance` occupied volume.

## Frozen two-room/two-level fixture

`projects/world_demo/tests/fixtures/castle_plan_two_room_two_level.js` is the
small executable contract fixture. It contains exactly two stacked 8m × 2m
rooms, not the obsolete 4m rooms or aggregate 6.25m stair footprint:

```js
{
  schema: CASTLE_PLAN_SCHEMA,
  id: "two-room-two-level",
  seed: 240911,
  entryRoomId: "hall",
  style: {
    wallThickness: 0.6,
    wallMaterial: "castle.limestone",
    bond: "ashlar",
  },
  levels: [
    {
      id: "upper", baseY: 4, height: 4,
      rooms: [{ id:"chamber", use:"chamber", floorType:"oak",
        rect:{ x:0, z:0, width:8, depth:2 } }],
      edgeOverrides: [],
    },
    {
      id: "ground", baseY: 0, height: 4,
      rooms: [{ id:"hall", use:"hall", floorType:"flags",
        rect:{ x:0, z:0, width:8, depth:2 } }],
      edgeOverrides: [{
        id:"front-door", from:[0,0], to:[0,2], kind:"door",
        connects:["outside","hall"],
        opening:{ width:1.2, height:2.2, offset:0.4 },
      }],
    },
  ],
  stairs: [{
    id:"main-stair",
    lowerLevelId:"ground", upperLevelId:"upper",
    lowerRoomId:"hall", upperRoomId:"chamber",
    width:1.2, maxRiser:0.2, tread:0.25, headroom:2.2,
    entryDirection:"E", exitDirection:"E",
    flights:[{
      id:"flight-1", direction:"E", stepCount:20,
      footprint:{ x:1.2, z:0.4, width:5, depth:1.2 },
    }],
    landings:[
      { id:"lower", kind:"lower",
        bounds:{ x:0, z:0.4, width:1.2, depth:1.2 } },
      { id:"upper", kind:"upper",
        bounds:{ x:6.2, z:0.4, width:1.2, depth:1.2 } },
    ],
  }],
  beams: [
    { levelId:"upper", from:[0,4,2], to:[8,4,2],
      section:[0.2,0.3], jointFamily:"mortise-tenon", role:"floor-beam" },
    { levelId:"ground", from:[0,0,0], to:[0,4,0],
      section:[0.25,0.25], jointFamily:"pegged", role:"post" },
  ],
  fixtures: [], roofs: [], localLights: [], curves: [],
}
```

The flight has a real 4m rise, 20 steps, 0.2m risers, and 5m run. Separate
1.2m lower and upper landings fit within both stacked rooms. The west-boundary
front door is physically aligned with the hall and `outside`. The committed
examples are generated directly from this fixture:

* `docs/designs/examples/castle-plan-two-level.manifest.json`
* `docs/designs/examples/castle-plan-ground.svg`

## Compiled manifest

`compilePlan` returns a JSON-safe object with `schema` equal to
`matter.castle-manifest/v1`, plus `planId`, `seed`, `style`, and these canonical
collections:

| Field | Emitted contract |
| --- | --- |
| `levels` | `{id,baseY,height}` in base-Y/ID order. |
| `rooms` | `{id,levelId,use,required,walkable,openToBelow,lowerRoomId,floorType,boundary}`; boundary is canonical cells or circle. |
| `walls` | Each undirected room boundary metre exactly once, including physical adjacency, section, override, aperture slices, sockets, trim, and radial-throat host/claim metadata. |
| `wallModules` | Greedy compatible 8/4/2/1m runs; source edges, aperture ownership/global extents, end trim, and owned junction IDs. |
| `junctions` | Level-scoped grid vertex with `end`, `straight`, `L`, `T`, or `cross` incidence, one owner edge, and an owned volume for non-straight junctions. |
| `floors` | Elevation/thickness/type, cells or circle boundary, regions/extensions/holes, walkable volume IDs, bearing/joist metadata, open-boundary IDs, and `openToSky`. |
| `curves` | Quarter/ring topology, section, validated apertures, tangent endpoint sockets, and radial-throat references. |
| `radialThroats` | Finite room-entry bridge, wall cut, target edge/cut ownership, reveals/returns, floor patch, thresholds, and clearance volume. |
| `curveTransitions` | Same-level compatible tangent curve-to-straight-wall joins only. |
| `portals` | One record per usable grid aperture or radial throat, with rooms, floors, clear width/height, bounds, thresholds, hosts, and swept-volume ID. |
| `openBoundaries` | Grouped full-span open overrides, classified as room join, exterior entry, or guarded void edge. |
| `beamMembers` | Canonical beam graph endpoints, section, joint family, role/material, bearing, and joist metadata. |
| `stairs` | Derived stair dimensions, explicit flights/landings, exact holes/void regions, route, and swept volumes. |
| `verticalVoids` | Synthesized stair/open-to-below voids plus authored double-height/shaft records, cut-floor/ceiling IDs, and rail ownership. |
| `roofs`, `fixtures`, `localLights` | ID-prefixed, level-checked authored records for sibling emitters. |
| `occupiedVolumes` | Room cells/circles, stair and radial-throat clearances, portal clearances, and fixture clearances. |
| `roomGraph` | Entry portal, room nodes, physical portal/stair edges, reachability, and embedded walk routes. |
| `walkRoute` | Alias of the canonical route array for direct consumers. |

Circular floor `boundary` retains the authored wall centreline radius. Its first
`regions` item is clipped inward by half the plan style's wall thickness, so
floor tiles stop at the inner wall face. Radial `extensions` then add the
finite throat bridge. Each floor `holes` item is one exact vertical-void
footprint and repeats that rectangle in `regions`; stair aggregate bounds live
only on the stair's diagnostic void records.

### Wall, aperture, module, and junction ownership

Every wall `section` contains `thickness`, `height`, `material`, `bond`, and
`profile`. Every metre-edge aperture slice contains:

```js
{
  apertureId, kind,
  start, end, localStart, localEnd,
  segmentFrom, segmentTo, globalStart, globalEnd,
  bottom, top,
  ownerEdgeId,
}
```

All slices of one authored opening share `apertureId`, canonical segment/global
extent, and the lexically first intersected `ownerEdgeId`. An opening that
crosses multiple wall metres therefore has one masonry aperture owner, not one
arch per edge.

Wall modules never cross a non-straight junction, opening/override identity, or
section/profile change. Each module publishes `sourceEdgeIds`, `apertureIds`,
deduplicated `apertures`, `ownsAperture`, `{start,end}` trim, and
`ownedJunctionIds`. A module aperture claimed by a radial throat additionally
publishes `claimedByRadialThroatId`. A wall endpoint socket publishes
`vertexId`, direction, junction kind, section, trim distance, owner flag, and
`ownsJunctionVolume`. Exactly one incident edge/socket owns each non-straight
junction's `ownedVolume`; emitters trim every incident run and build that volume
once. Junction identity and lookup always include `levelId`, so coincident X/Z
vertices on different storeys cannot alter each other's run decomposition.

### Portals, graph edges, and walk routes

Grid-wall portals are assembled by canonical aperture ID across all host edge
slices, except apertures claimed by a radial throat. They include `floorIds`
for every non-`outside` side, an aperture-sized 3D `bounds`, two points across
the wall thickness, a `roomThresholds` lookup selecting the point inside each
room, all host edge/module IDs, and a `sweptVolumeId`. Radial portals have the
same consumer fields, use the finite throat's inner/far thresholds, list the
curve plus target wall edges/modules as hosts, and publish
`claimedApertureId`.

`roomGraph.edges` contains only validated wall portals, radial throats, and
stairs. Every edge carries its rooms, source/portal ID, floor IDs, threshold
points, per-room thresholds, clear width, and swept-volume IDs. Starting at
`entryRoomId`, the compiler rejects every unreachable room except those
authored `required:false`.
`roomGraph.entryPortalId` is the first physical `outside` portal for the entry
room, or `null` if none exists.

Each `walkRoute` item publishes `{roomId,fromEntry,edgeIds,sweptVolumeIds,
waypoints,traversals,roomSegments}`. Each `traversal` names its `edgeId`,
`fromRoomId`, `toRoomId`, and directionally ordered `from`/`to` thresholds;
`outside` is explicit on an entry traversal. A stair graph edge uses the first
and last point of `stair.route.waypoints`. The top-level route includes every
point from that stair route in traversal order (reversed when descending), so a
consumer following only `walkRoute[].waypoints` still visits each flight and
landing rather than taking a lower-to-upper shortcut. Between consecutive
connectors the compiler emits a `roomSegment`
with `roomId`, `from`, `to`, `width`, routed `waypoints`, per-leg `segments`,
and aggregate `sweptBounds`. It uses 1.2m width, checks width-offset samples
remain in the shared room and do not enter a floor hole, and rejects headroom
overlap with beam bounds or fixture clearance. On a stair's lower level, every
rising flight footprint is a routing obstacle expanded by half the 1.2m route
width; a low intermediate landing is also blocked when its underside does not
leave 2.1m clearance. The declared lower landing remains the only flat-route
handoff into `stair.route`; upper landings with sufficient under-clearance do
not seal a return stair's approach. Each room-segment ID is also in
`sweptVolumeIds`. The top-level `waypoints` form one ordered polyline by
interleaving directed traversals and those in-room segment waypoints. Room ID
reachability alone is therefore not clearance evidence.

## Debug export and geometry handoff

`planToJSON` accepts only a v1 manifest and returns pretty JSON with a trailing
newline. `planToSVG` accepts only a v1 manifest, defaults to the first manifest
level, and draws walls, rings/quarter arcs, and room labels. Its classes
distinguish exterior, partition, door, arch, open, window, and curve geometry.

`emitManifest(manifest, emitters)` visits these collection/emitter pairs in
this exact order:

```text
walls/wall
wallModules/wallModule
junctions/junction
floors/floor
curves/curve
radialThroats/radialThroat
curveTransitions/curveTransition
portals/portal
openBoundaries/openBoundary
beamMembers/beamMember
stairs/stair
verticalVoids/verticalVoid
roofs/roof
fixtures/fixture
localLights/localLight
```

Each present emitter must be a function and receives `(record, manifest)` in
canonical record order. Missing emitter names skip that family. The return
value is an array of each invoked emitter's return value.

`manifestPartRecipes(manifest, moduleNames)` uses the same families. For every
mapping such as `{ wallModule:"CastleWallRun", stair:"CastleStair" }`, it
emits `{module,params}` with scalar params only:

```js
{
  manifestId: manifest.planId,
  recordId: record.id,
  recordKind: "wallModule", // emitter-family name
  recordIndex,              // index in that manifest collection
  seed: manifest.seed,
}
```

The geometry module imports or closes over the manifest and resolves the
record by ID/index. Nested plans or manifest records must never be placed in
engine `Part` params.
