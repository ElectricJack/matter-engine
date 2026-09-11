import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  CASTLE_MANIFEST_SCHEMA, CASTLE_PLAN_SCHEMA, canonicalEdge, compilePlan,
  emitManifest, manifestPartRecipes, planToJSON, planToSVG, validatePlan,
} from '../shared-lib/castle_plan.js';
import { TWO_ROOM_TWO_LEVEL_PLAN } from './fixtures/castle_plan_two_room_two_level.js';

function clone(value) { return JSON.parse(JSON.stringify(value)); }
function expectInvalid(plan, pattern) {
  assert.throws(() => compilePlan(plan), pattern);
}
function simplePlan(rooms, edgeOverrides = [], extra = {}) {
  return {
    schema: CASTLE_PLAN_SCHEMA, id: extra.id ?? 'simple', seed: 7,
    entryRoomId: rooms[0].id,
    levels: [{ id: 'ground', baseY: 0, height: 4, rooms, edgeOverrides }],
    stairs: [], beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
    ...extra,
  };
}

function portalPlan() {
  return simplePlan([
    { id: 'a', use: 'hall', rect: { x: 0, z: 0, width: 1, depth: 2 } },
    { id: 'b', use: 'chamber', rect: { x: 1, z: 0, width: 1, depth: 2 } },
  ], [{
    id: 'portal', from: [1, 0], to: [1, 2], kind: 'door',
    connects: ['b', 'a'], opening: { offset: 0.4, width: 1.2, bottom: 0, height: 2.2 },
  }], { id: 'physical-portal' });
}

function stairApproachPlan({ entryAtUpper = false } = {}) {
  const entryLevelId = entryAtUpper ? 'upper' : 'ground';
  const entryRoomId = entryAtUpper ? 'upper-hall' : 'lower-hall';
  return {
    schema: CASTLE_PLAN_SCHEMA,
    id: entryAtUpper ? 'reverse-stair-route' : 'stair-approach-route',
    seed: 19,
    entryRoomId,
    levels: [
      { id: 'ground', baseY: 0, height: 4, rooms: [
        { id: 'lower-hall', use: 'hall', rect: { x: 0, z: 0, width: 10, depth: 4 } },
      ], edgeOverrides: entryLevelId === 'ground' ? [{
        id: 'entry', from: [10, 0], to: [10, 2], kind: 'door',
        connects: ['outside', entryRoomId],
        opening: { offset: 0.4, width: 1.2, bottom: 0, height: 2.2 },
      }] : [] },
      { id: 'upper', baseY: 4, height: 4, rooms: [
        { id: 'upper-hall', use: 'gallery', rect: { x: 0, z: 0, width: 10, depth: 4 } },
      ], edgeOverrides: entryLevelId === 'upper' ? [{
        id: 'entry', from: [10, 0], to: [10, 2], kind: 'door',
        connects: ['outside', entryRoomId],
        opening: { offset: 0.4, width: 1.2, bottom: 0, height: 2.2 },
      }] : [] },
    ],
    stairs: [{
      id: 'cross-room-stair', lowerLevelId: 'ground', upperLevelId: 'upper',
      lowerRoomId: 'lower-hall', upperRoomId: 'upper-hall',
      width: 1.2, maxRiser: 0.2, tread: 0.25, headroom: 2.2,
      entryDirection: 'E', exitDirection: 'E',
      flights: [{
        id: 'rising-flight', direction: 'E', stepCount: 20,
        footprint: { x: 2, z: 0.8, width: 5, depth: 1.2 },
      }],
      landings: [
        { id: 'lower', kind: 'lower', bounds: { x: 0.8, z: 0.8, width: 1.2, depth: 1.2 } },
        { id: 'upper', kind: 'upper', bounds: { x: 7, z: 0.8, width: 1.2, depth: 1.2 } },
      ],
    }],
    beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
  };
}

function samePoint(a, b) {
  return a.length === b.length && a.every((value, index) => Math.abs(value - b[index]) < 1e-9);
}

function containsPointSequence(points, expected) {
  return points.some((_, start) =>
    expected.every((point, index) => points[start + index] && samePoint(points[start + index], point)));
}

function segmentEntersExpandedRect(from, to, rect, radius) {
  const epsilon = 1e-7;
  const mins = [rect.x - radius, rect.z - radius];
  const maxs = [rect.x + rect.width + radius, rect.z + rect.depth + radius];
  const starts = [from[0], from[2]], deltas = [to[0] - from[0], to[2] - from[2]];
  let first = 0, last = 1;
  for (let axis = 0; axis < 2; ++axis) {
    if (Math.abs(deltas[axis]) < 1e-9) {
      if (starts[axis] <= mins[axis] + epsilon || starts[axis] >= maxs[axis] - epsilon)
        return false;
      continue;
    }
    const a = (mins[axis] - starts[axis]) / deltas[axis];
    const b = (maxs[axis] - starts[axis]) / deltas[axis];
    first = Math.max(first, Math.min(a, b));
    last = Math.min(last, Math.max(a, b));
  }
  return Math.max(first, epsilon) < Math.min(last, 1 - epsilon) - epsilon;
}

assert.deepEqual(canonicalEdge([1, 0], [0, 0]), {
  from: [0, 0], to: [1, 0], key: '0,0|1,0', axis: 'x',
});
assert.deepEqual(validatePlan(TWO_ROOM_TWO_LEVEL_PLAN), { valid: true, errors: [] });

const fixtureManifest = compilePlan(TWO_ROOM_TWO_LEVEL_PLAN);
assert.equal(fixtureManifest.schema, CASTLE_MANIFEST_SCHEMA);
assert.equal(fixtureManifest.levels.length, 2);
assert.equal(fixtureManifest.rooms.length, 2);
assert.deepEqual(fixtureManifest.roomGraph.reachableRoomIds,
  ['chamber', 'hall']);
assert.deepEqual(fixtureManifest.walkRoute.find(route => route.roomId === 'chamber').fromEntry,
  ['hall', 'chamber']);
assert.equal(new Set(fixtureManifest.walls.map(wall => wall.id)).size,
  fixtureManifest.walls.length, 'every shared wall is emitted once');

// Reordering every authored collection preserves byte-for-byte output.
const reordered = clone(TWO_ROOM_TWO_LEVEL_PLAN);
reordered.levels.reverse();
for (const level of reordered.levels) {
  level.rooms.reverse();
  level.edgeOverrides.reverse();
}
reordered.beams.reverse();
assert.equal(planToJSON(compilePlan(reordered)), planToJSON(fixtureManifest));

// Negative grid coordinates are ordinary cell coordinates.
const negative = compilePlan(simplePlan([
  { id: 'vault', use: 'guardroom', rect: { x: -3, z: -2, width: 2, depth: 2 } },
]));
assert.ok(negative.walls.some(wall => wall.from[0] === -3 && wall.from[1] === -2));

const overlap = simplePlan([
  { id: 'a', use: 'hall', rect: { x: 0, z: 0, width: 2, depth: 1 } },
  { id: 'b', use: 'kitchen', cells: [[1, 0]], required: false },
]);
expectInvalid(overlap, /cell 1,0 overlaps room a/);
const duplicateRoomsAcrossLevels = {
  schema: CASTLE_PLAN_SCHEMA, id: 'duplicate-room-levels', seed: 13, entryRoomId: 'same-room',
  levels: [
    { id: 'ground', baseY: 0, height: 4,
      rooms: [{ id: 'same-room', use: 'hall', cells: [[0, 0]] }], edgeOverrides: [] },
    { id: 'upper', baseY: 4, height: 4,
      rooms: [{ id: 'same-room', use: 'void', required: false, cells: [[0, 0]] }], edgeOverrides: [] },
  ],
  stairs: [], beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
};
expectInvalid(duplicateRoomsAcrossLevels, /rooms.*duplicate id same-room/);

const unreachable = simplePlan([
  { id: 'a', use: 'hall', cells: [[0, 0]] },
  { id: 'b', use: 'chamber', cells: [[1, 0]] },
]);
expectInvalid(unreachable, /required rooms are unreachable.*b/);
const windowOnly = simplePlan([
  { id: 'a', use: 'hall', cells: [[0, 0]] },
  { id: 'b', use: 'chamber', cells: [[1, 0]] },
], [{ id: 'not-a-portal', from: [1, 0], to: [1, 1], kind: 'window' }]);
expectInvalid(windowOnly, /required rooms are unreachable.*b/);
const connected = compilePlan(portalPlan());
assert.deepEqual(connected.roomGraph.reachableRoomIds, ['a', 'b']);
const shared = connected.walls.filter(wall => wall.roomIds.join(',') === 'a,b');
assert.equal(shared.length, 2, 'each shared cell boundary is emitted exactly once');
assert.ok(shared.every(wall => wall.kind === 'door'));

// Circulation metadata never substitutes for a physically usable portal.
for (const [field, value, pattern] of [
  ['width', 1.19, /portal.*width|clear width|at least 1\.2m/],
  ['bottom', 0.01, /opening\.bottom|walkable floor|threshold/],
  ['height', 2.09, /portal.*height|headroom|at least 2\.1m/],
]) {
  const bad = portalPlan();
  bad.levels[0].edgeOverrides[0].opening[field] = value;
  expectInvalid(bad, pattern);
}
const falseExteriorAdjacency = portalPlan();
falseExteriorAdjacency.levels[0].edgeOverrides[0].from = [0, 0];
falseExteriorAdjacency.levels[0].edgeOverrides[0].to = [0, 2];
expectInvalid(falseExteriorAdjacency, /exterior.*outside|not physically adjacent|connects room b not adjacent/);
const overHeightPortal = portalPlan();
overHeightPortal.levels[0].edgeOverrides[0].opening.height = 4.1;
expectInvalid(overHeightPortal, /opening.*height|opening.*fit.*wall|portal.*level height/);
for (const family of ['fixtures', 'roofs', 'localLights']) {
  const duplicatePassThrough = simplePlan([
    { id: 'record-room', use: 'hall', cells: [[0, 0]] },
  ], [], { id: `duplicate-${family}` });
  duplicatePassThrough[family] = [
    { id: 'same-record', levelId: 'ground' },
    { id: 'same-record', levelId: 'ground' },
  ];
  expectInvalid(duplicatePassThrough,
    new RegExp(`${family}.*duplicate id.*same-record|duplicate id.*${family}`));
}

// The full socket vocabulary is produced from real boundary incidence.
const socketKinds = new Set();
for (const plan of [
  simplePlan([{ id: 'l', use: 'hall', cells: [[0, 0]] }], [], { id: 'socket-l' }),
  simplePlan([{ id: 'straight', use: 'hall', rect: { x: 0, z: 0, width: 3, depth: 1 } }], [], { id: 'socket-straight' }),
  simplePlan([
    { id: 'left', use: 'hall', cells: [[0, 0]] },
    { id: 'right', use: 'hall', cells: [[1, 0]], required: false },
  ], [], { id: 'socket-t' }),
  simplePlan([
    { id: 'sw', use: 'hall', cells: [[0, 0]] },
    { id: 'se', use: 'hall', cells: [[1, 0]], required: false },
    { id: 'nw', use: 'hall', cells: [[0, 1]], required: false },
    { id: 'ne', use: 'hall', cells: [[1, 1]], required: false },
  ], [], { id: 'socket-cross' }),
  simplePlan([{ id: 'end', use: 'hall', rect: { x: 0, z: 0, width: 2, depth: 2 } }], [
    { id: 'open-s', from: [0, 0], to: [2, 0], kind: 'open' },
  ], { id: 'socket-end' }),
]) for (const junction of compilePlan(plan).junctions) socketKinds.add(junction.kind);
assert.deepEqual([...socketKinds].sort(), ['L', 'T', 'cross', 'end', 'straight'].sort());

// Junction lookup and module breaks are level-local even at identical X/Z.
const levelJunctionPlan = {
  schema: CASTLE_PLAN_SCHEMA, id: 'level-keyed-junctions', seed: 8, entryRoomId: 'lower-left',
  levels: [
    { id: 'ground', baseY: 0, height: 4, rooms: [
      { id: 'lower-left', use: 'hall', rect: { x: 0, z: 0, width: 4, depth: 1 } },
      { id: 'lower-right', use: 'guardroom', required: false, rect: { x: 4, z: 0, width: 4, depth: 1 } },
    ], edgeOverrides: [] },
    { id: 'upper', baseY: 4, height: 4, rooms: [
      { id: 'upper-room', use: 'chamber', required: false, rect: { x: 0, z: 0, width: 8, depth: 1 } },
    ], edgeOverrides: [] },
  ],
  stairs: [], beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
};
const levelJunctionManifest = compilePlan(levelJunctionPlan);
const lowerT = levelJunctionManifest.junctions.find(junction =>
  junction.levelId === 'ground' && junction.position[0] === 4 && junction.position[1] === 1);
const upperStraight = levelJunctionManifest.junctions.find(junction =>
  junction.levelId === 'upper' && junction.position[0] === 4 && junction.position[1] === 1);
assert.equal(lowerT.kind, 'T');
assert.equal(upperStraight.kind, 'straight');
const lowerNorthModules = levelJunctionManifest.wallModules.filter(module =>
  module.levelId === 'ground' && module.from[1] === 1 && module.to[1] === 1);
assert.deepEqual(lowerNorthModules.map(module => module.length).sort((a, b) => b - a), [4, 4],
  'an upper-storey straight junction must not erase the lower-storey T break');

// Junction trim and volume ownership is explicit and singular for geometry emitters.
assert.ok(lowerT.ownedVolume && typeof lowerT.ownedVolume === 'object');
const lowerTEnds = levelJunctionManifest.walls.flatMap(wall =>
  [wall.startSocket, wall.endSocket].filter(socket => socket?.vertexId === lowerT.id)
    .map(socket => ({ wall, socket })));
assert.equal(lowerTEnds.length, 3);
assert.equal(lowerTEnds.filter(({ socket }) => socket.ownsJunctionVolume === true).length, 1);
assert.equal(lowerTEnds.find(({ socket }) => socket.ownsJunctionVolume).wall.id, lowerT.ownerEdgeId);
assert.ok(lowerTEnds.every(({ socket }) => Number.isFinite(socket.trim) && socket.trim > 0));
const owningTModules = levelJunctionManifest.wallModules.filter(module =>
  module.ownedJunctionIds?.includes(lowerT.id));
assert.equal(owningTModules.length, 1);

// Authored override ids are scoped by level for open-boundary records.
const repeatedOpenIdPlan = {
  schema: CASTLE_PLAN_SCHEMA, id: 'level-scoped-open-id', seed: 12, entryRoomId: 'ground-room',
  levels: [
    { id: 'ground', baseY: 0, height: 4, rooms: [
      { id: 'ground-room', use: 'hall', rect: { x: 0, z: 0, width: 2, depth: 2 } },
    ], edgeOverrides: [{ id: 'repeated-open', from: [0, 0], to: [2, 0], kind: 'open' }] },
    { id: 'upper', baseY: 4, height: 4, rooms: [
      { id: 'upper-room', use: 'gallery', required: false,
        rect: { x: 0, z: 0, width: 2, depth: 2 } },
    ], edgeOverrides: [{ id: 'repeated-open', from: [0, 0], to: [2, 0], kind: 'open' }] },
  ],
  stairs: [], beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
};
const repeatedOpenIdManifest = compilePlan(repeatedOpenIdPlan);
const repeatedOpenBoundaries = repeatedOpenIdManifest.openBoundaries.filter(boundary =>
  boundary.sourceId === 'repeated-open');
assert.equal(repeatedOpenBoundaries.length, 2);
assert.deepEqual(repeatedOpenBoundaries.map(boundary => boundary.levelId).sort(), ['ground', 'upper']);
assert.equal(new Set(repeatedOpenBoundaries.map(boundary => boundary.id)).size, 2);

// Long compatible runs greedily decompose to the supported module lengths.
const longRun = compilePlan(simplePlan([
  { id: 'long', use: 'hall', rect: { x: -15, z: 0, width: 15, depth: 1 } },
]));
const northModules = longRun.wallModules.filter(module => module.from[1] === 1 && module.to[1] === 1);
assert.deepEqual(northModules.map(module => module.length).sort((a, b) => b - a), [8, 4, 2, 1]);
const emittedEdgeIds = longRun.wallModules.flatMap(module => module.sourceEdgeIds);
assert.equal(new Set(emittedEdgeIds).size, longRun.walls.filter(wall => wall.kind !== 'open').length);

// Identical adjacent apertures remain separately owned modules; override/aperture
// identity is part of the merge signature even when dimensions match.
const distinctApertures = compilePlan(simplePlan([
  { id: 'gallery', use: 'hall', rect: { x: 0, z: 0, width: 2, depth: 1 } },
], [
  { id: 'window-west', from: [0, 0], to: [1, 0], kind: 'window',
    opening: { offset: 0, width: 1, bottom: 1, height: 1.4 } },
  { id: 'window-east', from: [1, 0], to: [2, 0], kind: 'window',
    opening: { offset: 0, width: 1, bottom: 1, height: 1.4 } },
], { id: 'distinct-aperture-modules' }));
const windowModules = distinctApertures.wallModules.filter(module =>
  module.sourceEdgeIds.some(edgeId => distinctApertures.walls.find(wall => wall.id === edgeId)?.kind === 'window'));
assert.deepEqual(windowModules.map(module => module.length), [1, 1]);
assert.equal(new Set(windowModules.flatMap(module => module.apertureIds)).size, 2);
assert.ok(windowModules.every(module => Array.isArray(module.apertures) && module.apertures.length === 1));
assert.ok(windowModules.every(module => module.ownsAperture === true));

// A reversed multi-edge override measures offset from its authored from endpoint,
// while exposing one canonical aperture identity and global extent.
const reversedOpening = compilePlan(simplePlan([
  { id: 'reverse-hall', use: 'hall', rect: { x: 0, z: 0, width: 2, depth: 1 } },
], [{
  id: 'reverse-opening', from: [2, 0], to: [0, 0], kind: 'window',
  opening: { offset: 0.25, width: 1, bottom: 1, height: 1.4 },
}], { id: 'reversed-opening' }));
const reversedSlices = reversedOpening.walls.filter(wall => wall.overrideId === 'reverse-opening')
  .flatMap(wall => wall.openings.map(opening => ({ wall, opening })))
  .sort((a, b) => a.wall.from[0] - b.wall.from[0]);
assert.deepEqual(reversedSlices.map(({ wall, opening }) =>
  [wall.from[0] + opening.start, wall.from[0] + opening.end]), [[0.75, 1], [1, 1.75]]);
assert.equal(new Set(reversedSlices.map(({ opening }) => opening.apertureId)).size, 1);
assert.ok(reversedSlices.every(({ opening }) => opening.globalStart === 0.75 && opening.globalEnd === 1.75));
assert.ok(reversedSlices.every(({ opening }) =>
  JSON.stringify(opening.segmentFrom) === '[0,0]' && JSON.stringify(opening.segmentTo) === '[2,0]'));
const apertureOwnerEdgeId = reversedSlices[0].opening.ownerEdgeId;
assert.ok(reversedSlices.every(({ opening }) => opening.ownerEdgeId === apertureOwnerEdgeId));
assert.equal(reversedOpening.wallModules.filter(module =>
  module.sourceEdgeIds.includes(apertureOwnerEdgeId) && module.ownsAperture === true).length, 1);

// Stair derivation keeps lower ceiling ownership as void metadata, but only
// cuts the upper floor, once for each exact flight/landing hole region.
const stair = fixtureManifest.stairs[0];
assert.equal(stair.flights.length, 1);
assert.equal(stair.flights[0].riser, 0.2);
assert.equal(stair.flights[0].stepCount, 20);
assert.equal(stair.landings.length, 2);
assert.ok(stair.sweptVolumes.length >= 1);
for (const landing of stair.landings) {
  assert.equal(typeof landing.sweptVolumeId, 'string');
  assert.ok(stair.sweptVolumes.some(volume => volume.id === landing.sweptVolumeId));
  assert.ok(stair.route.sweptVolumeIds.includes(landing.sweptVolumeId));
}
assert.deepEqual(stair.voids.map(hole => hole.kind), ['ceiling', 'floor']);
assert.equal(fixtureManifest.floors.find(floor => floor.roomId === 'hall').holes.length, 0);
assert.equal(fixtureManifest.floors.find(floor => floor.roomId === 'chamber').holes.length,
  stair.holes.length);

// A top-level walk route is a directly executable capsule path: stair traversals
// retain every authored flight/landing waypoint, and flat approaches treat the
// rising flight as an obstacle expanded by half the route width.
const stairApproachManifest = compilePlan(stairApproachPlan());
const approachStair = stairApproachManifest.stairs[0];
const routeUpstairs = stairApproachManifest.walkRoute.find(route => route.roomId === 'upper-hall');
assert.ok(containsPointSequence(routeUpstairs.waypoints, approachStair.route.waypoints),
  'upward walkRoute retains every stair.route waypoint in traversal order');
const lowerApproach = routeUpstairs.roomSegments.find(segment => segment.roomId === 'lower-hall');
assert.ok(lowerApproach.segments.every(segment => !segmentEntersExpandedRect(
  segment.from, segment.to, approachStair.flights[0].footprint, lowerApproach.width / 2)),
  'flat lower-room approach stays outside the capsule-expanded rising flight');

const reverseStairManifest = compilePlan(stairApproachPlan({ entryAtUpper: true }));
const reverseStair = reverseStairManifest.stairs[0];
const routeDownstairs = reverseStairManifest.walkRoute.find(route => route.roomId === 'lower-hall');
assert.ok(containsPointSequence(routeDownstairs.waypoints, [...reverseStair.route.waypoints].reverse()),
  'downward walkRoute retains every stair.route waypoint in reverse traversal order');

const blockedApproach = stairApproachPlan();
blockedApproach.beams.push({
  levelId: 'ground', from: [5, 2.05, 2.2], to: [5, 2.05, 4],
  section: [0.2, 0.2], jointFamily: 'mortise-tenon', role: 'blocked-stair-approach',
});
expectInvalid(blockedApproach, /walkRoute.*(headroom|clearance)|approach.*headroom/i);

for (const [field, value, pattern] of [
  ['headroom', 2.09, /headroom.*at least 2\.1m/],
]) {
  const bad = clone(TWO_ROOM_TWO_LEVEL_PLAN);
  bad.stairs[0][field] = value;
  expectInvalid(bad, pattern);
}
const shortRun = clone(TWO_ROOM_TWO_LEVEL_PLAN);
shortRun.stairs[0].flights[0].footprint.width = 4.9;
expectInvalid(shortRun, /flight.*footprint.*run|footprint.*needs 5m/);
const shortLanding = clone(TWO_ROOM_TWO_LEVEL_PLAN);
shortLanding.stairs[0].landings[1].bounds.width = 1.19;
expectInvalid(shortLanding, /landing.*at least stair width|landing.*1\.2m/);
const narrowStair = clone(TWO_ROOM_TWO_LEVEL_PLAN);
narrowStair.stairs[0].width = 1.19;
expectInvalid(narrowStair, /stairs\[0\]\.width.*at least 1\.2m/);
const remoteStair = clone(TWO_ROOM_TWO_LEVEL_PLAN);
remoteStair.stairs[0].flights[0].footprint.x = 100;
expectInvalid(remoteStair, /stair.*footprint|footprint.*walkable|outside.*room/);
const pinchedStair = clone(TWO_ROOM_TWO_LEVEL_PLAN);
pinchedStair.stairs[0].flights[0].footprint.depth = 0.1;
expectInvalid(pinchedStair, /footprint.*width|stair width|cross.*extent/);
const crossingStair = clone(TWO_ROOM_TWO_LEVEL_PLAN);
const crossingGround = crossingStair.levels.find(level => level.id === 'ground');
crossingGround.rooms[0].rect.width = 4;
crossingGround.rooms.push({
  id: 'service', use: 'kitchen', required: false,
  rect: { x: 4, z: 0, width: 3, depth: 2 },
});
expectInvalid(crossingStair, /stair.*partition|footprint.*lower room|footprint.*stacked room floors|crosses room service/);
const obstructedStair = clone(TWO_ROOM_TWO_LEVEL_PLAN);
obstructedStair.beams.push({
  levelId: 'ground', from: [2, 2.05, 0], to: [2, 2.05, 1.5],
  section: [0.3, 0.3], jointFamily: 'mortise-tenon', role: 'floor-beam',
});
expectInvalid(obstructedStair, /stair.*headroom|headroom.*beam|swept.*beam/);
const obstructedLanding = clone(TWO_ROOM_TWO_LEVEL_PLAN);
obstructedLanding.beams.push({
  levelId: 'upper', from: [6.5, 5.8, 0], to: [6.5, 5.8, 1.2],
  section: [0.2, 0.2], jointFamily: 'mortise-tenon', role: 'landing-headroom-obstruction',
});
expectInvalid(obstructedLanding, /landing.*headroom|headroom.*landing|landings.*beam/);
const routedPortalPlan = portalPlan();
routedPortalPlan.levels[0].edgeOverrides.push({
  id: 'entry', from: [0, 0], to: [0, 2], kind: 'door',
  connects: ['outside', 'a'],
  opening: { offset: 0.4, width: 1.2, bottom: 0, height: 2.2 },
});
const routedPortalManifest = compilePlan(routedPortalPlan);
const routedToB = routedPortalManifest.walkRoute.find(route => route.roomId === 'b');
assert.deepEqual(routedToB.fromEntry, ['a', 'b']);
assert.deepEqual(routedToB.traversals.map(({ fromRoomId, toRoomId }) => [fromRoomId, toRoomId]),
  [['outside', 'a'], ['a', 'b']]);
assert.ok(routedToB.waypoints[0][0] < 0 && routedToB.waypoints[1][0] > 0,
  'ordered route starts outside, then crosses the entry threshold into a');
assert.ok(routedToB.waypoints.at(-1)[0] > 1,
  'ordered route finishes across the internal threshold in b');
const obstructedPortalRoute = clone(routedPortalPlan);
obstructedPortalRoute.beams.push({
  levelId: 'ground', from: [0.5, 2.05, 0.8], to: [0.5, 2.05, 1.2],
  section: [0.2, 0.2], jointFamily: 'mortise-tenon', role: 'door-header-obstruction',
});
expectInvalid(obstructedPortalRoute,
  /portal.*beam|room route.*beam|route.*headroom|headroom.*beam/);

// Floor cutouts preserve the exact L-shaped flight/landing union. The broad
// bounds are useful for culling, but must not remove the retained deck inside it.
const returnStairPlan = {
  schema: CASTLE_PLAN_SCHEMA, id: 'exact-stair-hole-regions', seed: 9, entryRoomId: 'lower-hall',
  levels: [
    { id: 'ground', baseY: 0, height: 4, rooms: [
      { id: 'lower-hall', use: 'hall', rect: { x: 0, z: 0, width: 8, depth: 8 } },
    ], edgeOverrides: [] },
    { id: 'upper', baseY: 4, height: 4, rooms: [
      { id: 'upper-hall', use: 'hall', rect: { x: 0, z: 0, width: 8, depth: 8 } },
    ], edgeOverrides: [] },
  ],
  stairs: [{
    id: 'return-stair', lowerLevelId: 'ground', upperLevelId: 'upper',
    lowerRoomId: 'lower-hall', upperRoomId: 'upper-hall',
    width: 1.2, maxRiser: 0.2, tread: 0.25, headroom: 2.2,
    entryDirection: 'E', exitDirection: 'N',
    flights: [
      { id: 'east-flight', direction: 'E', stepCount: 10,
        footprint: { x: 1.2, z: 1, width: 2.5, depth: 1.2 } },
      { id: 'north-flight', direction: 'N', stepCount: 10,
        footprint: { x: 3.7, z: 2.2, width: 1.2, depth: 2.5 } },
    ],
    landings: [
      { id: 'lower', kind: 'lower', bounds: { x: 0, z: 1, width: 1.2, depth: 1.2 } },
      { id: 'turn', kind: 'intermediate', elevation: 2,
        bounds: { x: 3.7, z: 1, width: 1.2, depth: 1.2 } },
      { id: 'upper', kind: 'upper', bounds: { x: 3.7, z: 4.7, width: 1.2, depth: 1.2 } },
    ],
  }],
  beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
};
const returnStairManifest = compilePlan(returnStairPlan);
const returnStair = returnStairManifest.stairs[0];
const upperStairFloor = returnStairManifest.floors.find(floor => floor.roomId === 'upper-hall');
const exactFloorHoles = upperStairFloor.holes.filter(hole =>
  hole.voidId === 'vertical-void:return-stair');
assert.deepEqual(exactFloorHoles.map(hole => hole.footprint),
  returnStair.holes.map(hole => hole.footprint));
assert.ok(exactFloorHoles.every(hole =>
  hole.regions.length === 1 && JSON.stringify(hole.regions[0]) === JSON.stringify(hole.footprint)));
assert.deepEqual(exactFloorHoles.filter(hole => hole.replacementLandingId)
  .map(hole => hole.replacementLandingId).sort(),
['landing:return-stair:turn', 'landing:return-stair:upper']);
const retainedDeckPoint = [2, 3];
const pointInRect = ([x, z], rect) => x >= rect.x && x < rect.x + rect.width &&
  z >= rect.z && z < rect.z + rect.depth;
const broadHoleBounds = { x: 1.2, z: 1, width: 3.7, depth: 4.9 };
assert.ok(pointInRect(retainedDeckPoint, broadHoleBounds));
assert.ok(!exactFloorHoles.some(hole => pointInRect(retainedDeckPoint, hole.footprint)),
  'deck inside the former broad bounds but outside every exact cutout is retained');
const duplicateFlightIds = clone(returnStairPlan);
duplicateFlightIds.stairs[0].flights[1].id = duplicateFlightIds.stairs[0].flights[0].id;
expectInvalid(duplicateFlightIds, /flights\[1\]\.id.*duplicate|duplicate.*flight.*id/);
const wrongTurnElevation = clone(returnStairPlan);
wrongTurnElevation.stairs[0].landings.find(landing => landing.kind === 'intermediate').elevation = 3;
expectInvalid(wrongTurnElevation, /intermediate.*elevation|landing.*elevation.*flight/);
const wrongDirectedContact = clone(returnStairPlan);
wrongDirectedContact.stairs[0].landings.find(landing => landing.kind === 'intermediate').bounds =
  { x: 2.5, z: 2.2, width: 1.2, depth: 1.2 };
expectInvalid(wrongDirectedContact, /directed.*end|landing.*flight.*end|intermediate.*contact/);

// Quarter arcs expose exact cardinal endpoints and tangent wall matches.
const arcPlan = simplePlan([
  { id: 'corridor', use: 'hall', cells: [[2, 0], [2, 1], [1, 1], [0, 1]] },
], [], {
  id: 'arc-sockets',
  curves: [{
    id: 'turn', levelId: 'ground', roomId: 'corridor', kind: 'quarter',
    center: [0, 0], radius: 2, start: 'E', end: 'N', requireGridJoin: true,
    thickness: 0.6, height: 4,
  }],
});
const arc = compilePlan(arcPlan).curves[0];
assert.deepEqual(arc.endpoints.map(endpoint => endpoint.position), [[2, 0], [0, 2]]);
assert.deepEqual(arc.endpoints.map(endpoint => endpoint.tangent), [[0, 1], [-1, 0]]);
assert.ok(arc.endpoints.every(endpoint => endpoint.socket.compatibleWallEdgeIds.length > 0));
const badArc = clone(arcPlan);
badArc.curves[0].end = 'W';
expectInvalid(badArc, /exactly one quarter turn/);
const duplicateCurveIds = clone(arcPlan);
duplicateCurveIds.curves.push(clone(duplicateCurveIds.curves[0]));
expectInvalid(duplicateCurveIds, /curves\[1\]\.id.*duplicate|duplicate.*curve.*id/);

// A circular tower is a real floor boundary with a portal into the room graph.
const towerPlan = simplePlan([
  { id: 'corridor', use: 'hall', cells: [[3, -1], [3, 0]] },
  { id: 'tower', use: 'guardroom', boundary: { kind: 'circle', center: [0, 0], radius: 3 } },
], [{
  id: 'tower-host', from: [3, -1], to: [3, 1], kind: 'arch',
  connects: ['corridor', 'outside'],
  opening: { offset: 0.4, width: 1.2, bottom: 0, height: 2.2 },
}], {
  id: 'tower-portal',
  curves: [{
    id: 'tower-ring', levelId: 'ground', roomId: 'tower', kind: 'ring',
    center: [0, 0], radius: 3,
    apertures: [{ id: 'tower-door', kind: 'door', startAngle: -12, endAngle: 12,
      height: 2.2, connects: ['tower', 'corridor'],
      throat: { targetRoomId: 'corridor', direction: 'E', width: 1.2, depth: 1,
        hostApertureId: 'tower-host' } }],
  }],
});
const tower = compilePlan(towerPlan);
assert.equal(tower.rooms.find(room => room.id === 'tower').boundary.kind, 'circle');
assert.ok(tower.roomGraph.edges.some(edge => edge.rooms.join(',') === 'corridor,tower'));
assert.equal(tower.radialThroats.length, 1);
assert.deepEqual(tower.curves[0].radialThroatIds, [tower.radialThroats[0].id]);
assert.equal(tower.curves[0].apertures[0].radialThroatId, tower.radialThroats[0].id);
assert.equal(tower.radialThroats[0].targetRoomId, 'corridor');
const radialThroat = tower.radialThroats[0];
const circleChordX = tower.curves[0].center[0] + Math.sqrt(
  tower.curves[0].radius ** 2 - (radialThroat.width / 2) ** 2);
assert.ok(radialThroat.floorPatch.bounds.minX <= circleChordX + 1e-9,
  'the bridge patch reaches the circle chord or deeper into the curved floor');
assert.ok(radialThroat.floorPatch.bounds.minX < radialThroat.targetFloorBounds.minX,
  'bridge floor patch and rectangular target-floor coverage are distinct bounds');
assert.equal(radialThroat.targetFloorBounds.minX,
  tower.curves[0].center[0] + tower.curves[0].radius);
const thickRingPlan = clone(towerPlan);
thickRingPlan.curves[0].thickness = 0.8;
thickRingPlan.levels[0].edgeOverrides[0].thickness = 0.8;
const thickRing = compilePlan(thickRingPlan);
const thickTowerFloor = thickRing.floors.find(floor => floor.roomId === 'tower');
const thickTowerOccupancy = thickRing.occupiedVolumes.find(volume =>
  volume.kind === 'room-circle' && volume.roomId === 'tower');
assert.equal(thickTowerFloor.regions[0].radius, 2.6);
assert.equal(thickTowerOccupancy.radius, 2.6,
  'curved-room occupied radius follows the matching ring thickness');
assert.ok(!tower.curves[0].endpoints.some(endpoint =>
  endpoint.socket?.id === tower.radialThroats[0].id), 'radial throats are not tangent endpoint sockets');
const detachedTower = clone(towerPlan);
detachedTower.curves[0].apertures = [];
expectInvalid(detachedTower, /requires a curve door\/arch\/open portal connection/);
const remoteTower = clone(towerPlan);
remoteTower.levels[0].rooms.find(room => room.id === 'corridor').cells = [[10, -1], [10, 0]];
remoteTower.levels[0].edgeOverrides[0].from = [10, -1];
remoteTower.levels[0].edgeOverrides[0].to = [10, 1];
expectInvalid(remoteTower, /throat.*target.*floor|portal.*physically adjacent|geometric.*connection/);
const crossLevelTower = clone(towerPlan);
crossLevelTower.levels[0].rooms = crossLevelTower.levels[0].rooms.filter(room => room.id !== 'corridor');
crossLevelTower.levels[0].edgeOverrides = [];
crossLevelTower.levels.push({
  id: 'upper', baseY: 4, height: 4,
  rooms: [{ id: 'corridor', use: 'hall', required: false, cells: [[3, -1], [3, 0]] }],
  edgeOverrides: [{
    id: 'tower-host', from: [3, -1], to: [3, 1], kind: 'arch',
    connects: ['corridor', 'outside'],
    opening: { offset: 0.4, width: 1.2, bottom: 0, height: 2.2 },
  }],
});
expectInvalid(crossLevelTower, /curves.*another level|same-level target room|target room.*level/);
const circleOverlap = clone(towerPlan);
circleOverlap.levels[0].rooms.find(room => room.id === 'corridor').cells.push([2, 0]);
expectInvalid(circleOverlap, /cell.*overlaps.*circle|circle.*overlap|overlaps circular room tower/);
const cyclicRingOverlap = clone(towerPlan);
cyclicRingOverlap.curves[0].apertures.push({
  id: 'wrapped-window', kind: 'window', startAngle: 350, endAngle: 370,
  bottom: 1, height: 1.4,
});
expectInvalid(cyclicRingOverlap, /aperture.*overlap|intervals overlap|cyclic.*overlap/);
const duplicateApertureIds = clone(towerPlan);
duplicateApertureIds.curves[0].apertures.push({
  id: 'tower-door', kind: 'window', startAngle: 90, endAngle: 100,
});
expectInvalid(duplicateApertureIds,
  /apertures\[1\]\.id.*duplicate|duplicate.*aperture.*id/);

const clockwiseQuarterOverlap = clone(arcPlan);
clockwiseQuarterOverlap.id = 'clockwise-quarter-overlap';
clockwiseQuarterOverlap.curves[0].clockwise = true;
clockwiseQuarterOverlap.curves[0].end = 'S';
clockwiseQuarterOverlap.curves[0].requireGridJoin = false;
clockwiseQuarterOverlap.curves[0].apertures = [
  { id: 'negative-window', kind: 'window', startAngle: -20, endAngle: -5 },
  { id: 'wrapped-window', kind: 'window', startAngle: 340, endAngle: 355 },
];
expectInvalid(clockwiseQuarterOverlap, /aperture.*overlap|intervals overlap|cyclic.*overlap/);

// A radial throat may claim the matching straight-wall aperture. The wall and
// its empty cut remain geometry inputs, but only the internal throat is a portal.
const hostedTower = tower;
const hostedThroat = hostedTower.radialThroats[0];
assert.equal(hostedThroat.hostApertureId, hostedThroat.targetWallApertureId);
const hostWalls = hostedTower.walls.filter(wall => wall.overrideId === 'tower-host');
assert.equal(hostWalls.length, 2);
assert.ok(hostWalls.every(wall => wall.kind === 'arch' && wall.openings.length > 0));
assert.ok(hostWalls.flatMap(wall => wall.openings).every(opening =>
  opening.apertureId === hostedThroat.hostApertureId &&
  opening.claimedByRadialThroatId === hostedThroat.id));
assert.ok(hostedTower.wallModules.some(module =>
  module.sourceEdgeIds.some(edgeId => hostWalls.some(wall => wall.id === edgeId)) &&
  module.apertureIds.includes(hostedThroat.hostApertureId)));
assert.equal(hostedTower.portals.filter(portal => portal.rooms.includes('outside')).length, 0,
  'claimed host cut does not leak a false corridor/outside circulation portal');
assert.equal(hostedTower.portals.filter(portal => portal.id === `portal:${hostedThroat.id}`).length, 1);

// Tangent endpoint matching excludes open boundaries and requires compatible
// physical wall sections, not merely a collinear edge at the same vertex.
const mismatchedArcSection = clone(arcPlan);
mismatchedArcSection.curves[0].thickness = 0.8;
expectInvalid(mismatchedArcSection, /section|no tangent straight-wall socket|compatible.*socket/);
const openArcSocket = clone(arcPlan);
openArcSocket.levels[0].rooms[0].cells.push([2, -1]);
openArcSocket.levels[0].edgeOverrides.push({
  id: 'open-start-socket', from: [2, -1], to: [2, 1], kind: 'open',
  connects: ['outside', 'corridor'], opening: { offset: 0.4, width: 1.2, height: 2.2 },
});
expectInvalid(openArcSocket, /no tangent straight-wall socket|no compatible.*socket|open.*socket/);

// Explicit double-height ownership names one lower ceiling owner and only the
// upper walkable floors actually penetrated by the void.
const verticalVoidPlan = {
  schema: CASTLE_PLAN_SCHEMA, id: 'vertical-void-floor-ownership', seed: 10,
  entryRoomId: 'lower-hall',
  levels: [
    { id: 'ground', baseY: 0, height: 4, rooms: [
      { id: 'lower-hall', use: 'hall', rect: { x: 0, z: 0, width: 4, depth: 4 } },
    ], edgeOverrides: [] },
    { id: 'upper', baseY: 4, height: 4, rooms: [
      { id: 'upper-deck', use: 'gallery', required: false,
        rect: { x: 0, z: 0, width: 4, depth: 4 } },
    ], edgeOverrides: [] },
  ],
  verticalVoids: [{
    id: 'hall-air', kind: 'double-height', lowerLevelId: 'ground', upperLevelId: 'upper',
    lowerRoomId: 'lower-hall', upperRoomIds: ['upper-deck'],
    footprint: { x: 1, z: 1, width: 2, depth: 2 },
  }],
  stairs: [], beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
};
const verticalVoidManifest = compilePlan(verticalVoidPlan);
const hallVoid = verticalVoidManifest.verticalVoids.find(record => record.id === 'vertical-void:hall-air');
assert.deepEqual(hallVoid.lowerRoomIds, ['lower-hall']);
assert.deepEqual(hallVoid.upperRoomIds, ['upper-deck']);
assert.deepEqual(hallVoid.penetratedFloorIds, ['floor:upper:upper-deck']);
assert.deepEqual(hallVoid.penetratedCeilingIds, ['ceiling:ground:lower-hall']);
const lowerHallFloor = verticalVoidManifest.floors.find(floor => floor.roomId === 'lower-hall');
const upperDeckFloor = verticalVoidManifest.floors.find(floor => floor.roomId === 'upper-deck');
assert.ok(!lowerHallFloor.holes.some(hole => hole.voidId === hallVoid.id),
  'a vertical void never cuts the occupied lower floor');
assert.ok(upperDeckFloor.holes.some(hole => hole.voidId === hallVoid.id));
const voidOutsideLowerOwner = clone(verticalVoidPlan);
voidOutsideLowerOwner.levels[0].rooms[0].rect.width = 2;
expectInvalid(voidOutsideLowerOwner, /void footprint.*lower room|lowerRoomId.*cover|lower.*footprint/);
const voidOutsideUpperOwner = clone(verticalVoidPlan);
voidOutsideUpperOwner.levels[1].rooms[0].rect.width = 2;
expectInvalid(voidOutsideUpperOwner, /void footprint.*upper room|upperRoomIds.*covered|upper.*footprint/);

// An upper air room owns enclosure topology but has no walkable floor, joists,
// occupied volume, or circulation edge. Its gallery boundary remains guarded.
const openToBelowPlan = {
  schema: CASTLE_PLAN_SCHEMA, id: 'open-to-below-enclosure', seed: 11,
  entryRoomId: 'lower-hall',
  levels: [
    { id: 'ground', baseY: 0, height: 4, rooms: [
      { id: 'lower-hall', use: 'hall', rect: { x: 0, z: 0, width: 4, depth: 2 } },
    ], edgeOverrides: [] },
    { id: 'upper', baseY: 4, height: 4, rooms: [
      { id: 'hall-air', use: 'void', required: false, openToBelow: true,
        lowerRoomId: 'lower-hall', rect: { x: 0, z: 0, width: 2, depth: 2 } },
      { id: 'gallery', use: 'gallery', required: false,
        rect: { x: 2, z: 0, width: 2, depth: 2 } },
    ], edgeOverrides: [{
      id: 'gallery-guard', from: [2, 0], to: [2, 2], kind: 'open',
      railProfile: 'castle.gallery-guardrail',
    }] },
  ],
  stairs: [], verticalVoids: [], beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
};
const openToBelowManifest = compilePlan(openToBelowPlan);
const airRoom = openToBelowManifest.rooms.find(room => room.id === 'hall-air');
assert.equal(airRoom.openToBelow, true);
assert.equal(airRoom.lowerRoomId, 'lower-hall');
assert.ok(openToBelowManifest.walls.some(wall =>
  wall.levelId === 'upper' && wall.roomIds.includes('hall-air') && wall.kind === 'wall'));
assert.ok(!openToBelowManifest.floors.some(floor => floor.roomId === 'hall-air'));
assert.ok(!openToBelowManifest.occupiedVolumes.some(volume => volume.roomId === 'hall-air'));
assert.ok(!openToBelowManifest.roomGraph.edges.some(edge => edge.rooms.includes('hall-air')));
assert.ok(!openToBelowManifest.roomGraph.reachableRoomIds.includes('hall-air'));
const guardedGalleryEdge = openToBelowManifest.openBoundaries.find(boundary =>
  boundary.sourceId === 'gallery-guard');
assert.equal(guardedGalleryEdge.kind, 'void-edge');
assert.equal(guardedGalleryEdge.rail.required, true);
assert.equal(guardedGalleryEdge.rail.profile, 'castle.gallery-guardrail');
const autoAirVoid = openToBelowManifest.verticalVoids.find(record =>
  record.lowerRoomIds?.includes('lower-hall') && record.upperRoomIds?.includes('hall-air'));
assert.ok(autoAirVoid, 'openToBelow emits vertical-void ownership when none is explicitly authored');
assert.deepEqual(autoAirVoid.penetratedFloorIds, []);
const partialAirVoid = clone(openToBelowPlan);
partialAirVoid.verticalVoids.push({
  id: 'partial-air', kind: 'double-height', lowerLevelId: 'ground', upperLevelId: 'upper',
  lowerRoomId: 'lower-hall', upperRoomIds: ['hall-air'],
  footprint: { x: 0, z: 0, width: 1, depth: 2 },
});
expectInvalid(partialAirVoid,
  /openToBelow.*exact|air room.*footprint|partial.*air|exactly cover|cover.*complete openToBelow/);
for (const mutate of [
  plan => { plan.levels[1].rooms[0].required = true; },
  plan => { plan.levels[1].rooms[0].use = 'hall'; },
  plan => { delete plan.levels[1].rooms[0].lowerRoomId; },
]) {
  const invalidAirRoom = clone(openToBelowPlan);
  mutate(invalidAirRoom);
  expectInvalid(invalidAirRoom,
    /openToBelow.*required:false.*use void|openToBelow|lowerRoomId.*non-empty/);
}

// Debug exporters and geometry handoff retain ids without nested Part params.
const json = planToJSON(fixtureManifest);
assert.ok(json.endsWith('\n') && json.includes('"matter.castle-manifest/v1"'));
const svg = planToSVG(fixtureManifest, { levelId: 'ground', scale: 20 });
assert.ok(svg.startsWith('<?xml') && svg.includes('front-door') && svg.includes('<text'));
assert.equal(readFileSync(new URL(
  '../../../docs/designs/examples/castle-plan-two-level.manifest.json', import.meta.url), 'utf8'), json);
assert.equal(readFileSync(new URL(
  '../../../docs/designs/examples/castle-plan-ground.svg', import.meta.url), 'utf8'),
planToSVG(fixtureManifest, { levelId: 'ground', scale: 28 }));
const calls = [];
const emitted = emitManifest(fixtureManifest, { stair(record) { calls.push(record.id); return record.sourceId; } });
assert.deepEqual(calls, ['stair:main-stair']);
assert.deepEqual(emitted, ['main-stair']);
const recipes = manifestPartRecipes(fixtureManifest, {
  wallModule: 'CastleWallRun', junction: 'CastleJunction', stair: 'CastleStair',
});
assert.ok(recipes.length > 0);
for (const recipe of recipes) {
  assert.equal(typeof recipe.module, 'string');
  for (const value of Object.values(recipe.params))
    assert.ok(['string', 'number', 'boolean'].includes(typeof value), 'Part params stay scalar');
}

console.log(`castle_plan_tests: ${fixtureManifest.walls.length} walls, ${fixtureManifest.wallModules.length} modules, all checks passed`);
