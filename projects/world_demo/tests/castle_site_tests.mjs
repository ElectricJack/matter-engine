import assert from 'node:assert/strict';
import {
  CASTLE_SITE_CAPSULE_RADIUS,
  CASTLE_SITE_CLEARANCE,
  CASTLE_SITE_MANIFEST_SCHEMA,
  compileSite,
  siteToJSON,
  siteToSVG,
} from '../shared-lib/castle_site.js';
import {
  transformPoint,
  transformVector,
  yawDegToQuaternion,
} from '../shared-lib/castle_frames.js';
import {
  ANGLED_STUDY_CORE_PLAN,
  ANGLED_STUDY_HALL_PLAN,
  CASTLE_SITE_ANGLED_STUDY,
  angledStudySite,
} from './fixtures/castle_site_angled_study.js';

const EPSILON = 1e-8;
const clone = value => structuredClone(value);

function near(actual, expected, label = 'value', epsilon = EPSILON) {
  if (Array.isArray(expected)) {
    assert.ok(Array.isArray(actual), `${label} must be an array`);
    assert.equal(actual.length, expected.length, `${label} length`);
    for (let index = 0; index < expected.length; ++index)
      near(actual[index], expected[index], `${label}[${index}]`, epsilon);
    return;
  }
  assert.ok(Math.abs(actual - expected) <= epsilon,
    `${label}: expected ${expected}, received ${actual}`);
}

function midpoint(a, b) {
  return a.map((value, index) => (value + b[index]) / 2);
}

function distance2(a, b) {
  return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2;
}

function signedArea(polygon) {
  return polygon.reduce((sum, point, index) => {
    const next = polygon[(index + 1) % polygon.length];
    return sum + point[0] * next[1] - point[1] * next[0];
  }, 0) / 2;
}

function pointInConvexPolygon(point, polygon, epsilon = EPSILON) {
  let sign = 0;
  for (let index = 0; index < polygon.length; ++index) {
    const a = polygon[index], b = polygon[(index + 1) % polygon.length];
    const cross = (b[0] - a[0]) * (point[1] - a[1]) -
      (b[1] - a[1]) * (point[0] - a[0]);
    if (Math.abs(cross) <= epsilon) continue;
    if (sign && Math.sign(cross) !== sign) return false;
    sign = Math.sign(cross);
  }
  return true;
}

function portalCenter(planManifest, sourceId) {
  const portal = planManifest.portals.find(candidate => candidate.sourceId === sourceId);
  assert.ok(portal, `missing portal ${sourceId}`);
  return midpoint(portal.thresholds[0], portal.thresholds[1]);
}

function expectInvalid(site, pattern) {
  assert.throws(() => compileSite(site), pattern);
}

const manifest = compileSite(CASTLE_SITE_ANGLED_STUDY);
assert.equal(manifest.schema, CASTLE_SITE_MANIFEST_SCHEMA);
assert.equal(manifest.siteId, 'angled-study-30');
assert.deepEqual(manifest.wings.map(wing => wing.id), ['core', 'hall']);

const core = manifest.wings.find(wing => wing.id === 'core');
const hall = manifest.wings.find(wing => wing.id === 'hall');
near(portalCenter(core.manifest, 'east-hall'), [12, 0, 6], 'core local east mouth');
near(portalCenter(hall.manifest, 'west-entry'), [0, 0, 4], 'hall local west mouth');
near(hall.frame.origin, [16, 0, 2.5358983848622456], '30 degree hall origin');

const connector = manifest.connectors[0];
assert.equal(connector.id, 'vestibule');
assert.equal(connector.level, 'ground');
assert.equal(connector.baseY, 0);
assert.equal(connector.floor.material, 'stone');
assert.equal(connector.floor.owner, 'connector:vestibule:floor');
assert.equal(connector.clearHeight, 3.2);
assert.equal(connector.roof.kind, 'low-hip');
assert.ok(signedArea(connector.clearPolygon) > 0, 'connector polygon is counter-clockwise');
assert.ok(connector.minimumWidth + EPSILON >=
  2 * (CASTLE_SITE_CAPSULE_RADIUS + CASTLE_SITE_CLEARANCE));
assert.deepEqual(connector.mouths.map(mouth => mouth.portalId), ['east-hall', 'west-entry']);
near(midpoint(...connector.mouths[0].segment), [12, 6], 'core world mouth');
near(midpoint(...connector.mouths[1].segment), [18, 6], 'hall world mouth');

for (const mouth of connector.mouths) {
  assert.equal(mouth.segment.length, 2);
  assert.equal(mouth.tangent.length, 2);
  assert.equal(mouth.outward.length, 2);
  near(Math.hypot(...mouth.tangent), 1, `${mouth.wing} tangent length`);
  near(Math.hypot(...mouth.outward), 1, `${mouth.wing} outward length`);
  assert.ok(mouth.hostModules.length > 0);
  assert.ok(mouth.hostModules.every(id => id.startsWith(`${mouth.wing}:`)));
  near(Math.hypot(mouth.insideSegment[1][0] - mouth.insideSegment[0][0],
    mouth.insideSegment[1][1] - mouth.insideSegment[0][1]), mouth.clearWidth,
  `${mouth.wing} inside face preserves authored clear width`);
  near(Math.hypot(mouth.outsideSegment[1][0] - mouth.outsideSegment[0][0],
    mouth.outsideSegment[1][1] - mouth.outsideSegment[0][1]), mouth.clearWidth,
  `${mouth.wing} outside face preserves authored clear width`);
  for (const point of [...mouth.segment, [mouth.inside[0], mouth.inside[2]],
    [mouth.outside[0], mouth.outside[2]]])
    assert.ok(pointInConvexPolygon(point, connector.clearPolygon),
      `${mouth.wing} full mouth and thresholds lie on connector support`);
}

const polygonCentroid = connector.clearPolygon.reduce((sum, point) =>
  [sum[0] + point[0] / connector.clearPolygon.length,
    sum[1] + point[1] / connector.clearPolygon.length], [0, 0]);
const cornerOwners = [];
for (const span of connector.wallSpans) {
  assert.equal(span.segment.length, 2);
  near(Math.hypot(...span.tangent), 1, `${span.id} tangent length`);
  near(Math.hypot(...span.normal), 1, `${span.id} normal length`);
  near(span.tangent[0] * span.normal[0] + span.tangent[1] * span.normal[1], 0,
    `${span.id} frame orthogonality`);
  const center = midpoint(...span.segment);
  const towardInterior = [polygonCentroid[0] - center[0], polygonCentroid[1] - center[1]];
  assert.ok(span.normal[0] * towardInterior[0] + span.normal[1] * towardInterior[1] < 0,
    `${span.id} normal points outward`);
  assert.equal(span.trimPlanes.length, 2);
  assert.ok(span.cornerOwners.every(owner => owner.startsWith(`${span.id}:`)));
  span.jambOwners.forEach((owner, endpointIndex) => {
    const ownedMouth = connector.mouths.find(mouth => mouth.jambOwner === owner);
    const otherMouth = connector.mouths.find(mouth => mouth.jambOwner !== owner);
    assert.ok(ownedMouth && otherMouth);
    const ownedPoints = [...ownedMouth.insideSegment, ...ownedMouth.segment,
      ...ownedMouth.outsideSegment];
    const otherPoints = [...otherMouth.insideSegment, ...otherMouth.segment,
      ...otherMouth.outsideSegment];
    assert.ok(Math.min(...ownedPoints.map(point => distance2(span.segment[endpointIndex], point))) <=
      Math.min(...otherPoints.map(point => distance2(span.segment[endpointIndex], point))) + EPSILON,
    `${span.id} endpoint ${endpointIndex} belongs to its nearest mouth`);
  });
  cornerOwners.push(...span.cornerOwners);
}
assert.equal(new Set(cornerOwners).size, cornerOwners.length, 'miter corners have one owner each');
assert.deepEqual(connector.mouths.map(mouth => mouth.jambOwner),
  ['connector:vestibule:mouth:a:jambs', 'connector:vestibule:mouth:b:jambs']);

// The connector route is supported at floor baseY and includes both wall-face
// thresholds. Reversing it gives the exact hall-to-core traversal.
assert.ok(connector.routeWaypoints.every(point => point[1] === connector.baseY));
near(connector.routeWaypoints[0], connector.mouths[0].inside, 'connector route core start');
near(connector.routeWaypoints.at(-1), connector.mouths[1].inside, 'connector route hall end');
const reverseTraversal = [...connector.routeWaypoints].reverse();
near(reverseTraversal[0], connector.mouths[1].inside, 'reverse route hall start');
near(reverseTraversal.at(-1), connector.mouths[0].inside, 'reverse route core end');
for (const point of connector.routeWaypoints)
  assert.ok(pointInConvexPolygon([point[0], point[2]], connector.clearPolygon),
    'route remains on connector floor support');

const connectorEdge = manifest.roomGraph.edges.find(edge => edge.kind === 'connector');
assert.deepEqual(connectorEdge.rooms, ['core:core-ground', 'hall:hall-ground']);
assert.deepEqual(connectorEdge.floorIds, ['connector:vestibule:floor']);
assert.deepEqual(connectorEdge.sweptVolumeIds, ['connector:vestibule:clearance']);
assert.ok(manifest.roomGraph.reachableRoomIds.includes('hall:hall-ground'));
const hallRoute = manifest.walkRoutes.find(route => route.roomId === 'hall:hall-ground');
assert.deepEqual(hallRoute.fromEntry, ['core:core-ground', 'hall:hall-ground']);
assert.deepEqual(hallRoute.edgeIds, ['route:connector:vestibule']);
near(hallRoute.waypoints.at(-1), connector.mouths[1].inside, 'entry-to-hall route');

const upperRoute = manifest.walkRoutes.find(route => route.roomId === 'core:core-upper');
assert.deepEqual(upperRoute.fromEntry, ['core:core-ground', 'core:core-upper']);
assert.ok(upperRoute.edgeIds.some(id => id.includes('stair')));
assert.equal(upperRoute.waypoints[0][1], 0);
assert.equal(upperRoute.waypoints.at(-1)[1], 4);

// Global stitching must call the plan compiler's obstacle-aware room transit,
// not draw a straight entry-to-connector chord through central furniture.
const obstructedTransitSite = clone(CASTLE_SITE_ANGLED_STUDY);
obstructedTransitSite.wings.find(wing => wing.id === 'core').plan.fixtures.push({
  id: 'central-table', levelId: 'ground',
  clearance: { minX: 4, maxX: 8, minY: 0, maxY: 2.2, minZ: 5.2, maxZ: 6.8 },
});
const obstructedHallRoute = compileSite(obstructedTransitSite).walkRoutes
  .find(route => route.roomId === 'hall:hall-ground');
assert.equal(obstructedHallRoute.roomSegments.length, 1);
assert.equal(obstructedHallRoute.roomSegments[0].roomId, 'core:core-ground');
assert.ok(obstructedHallRoute.roomSegments[0].waypoints.length >= 4,
  'global route detours around the central furnishing clearance');
assert.ok(obstructedHallRoute.roomSegments[0].waypoints.some(point =>
  point[2] <= 4.6 + EPSILON || point[2] >= 7.4 - EPSILON));
assert.ok(obstructedHallRoute.roomSegments[0].segments.every(segment =>
  !('bounds' in segment) && segment.orientedBounds && segment.localBounds),
  'rotated global swept bounds remain oriented instead of becoming world AABBs');

// A consumed compound-link socket must no longer retain an independent outside
// graph edge. The sole site exterior edge is the selected core entry.
const outsideEdges = manifest.roomGraph.edges.filter(edge => edge.rooms.includes('outside'));
assert.equal(outsideEdges.length, 1);
assert.equal(outsideEdges[0].sourceId, 'core:main-entry');
assert.equal(manifest.entry.portalId, 'main-entry');
assert.equal(manifest.entry.roomId, 'core:core-ground');
near(manifest.spawn, [-0.2, 0, 6], 'entry-opening spawn');

// Authored portal source IDs are level-scoped. A same-named upper portal must
// not survive as a second site exterior edge for the selected ground entry.
const levelQualifiedEntry = clone(CASTLE_SITE_ANGLED_STUDY);
levelQualifiedEntry.wings.find(wing => wing.id === 'core').plan.levels[1].edgeOverrides.push({
  id: 'main-entry', from: [0, 0], to: [0, 12], kind: 'door',
  connects: ['outside', 'core-upper'],
  opening: { width: 2.4, height: 3.2, offset: 4.8 },
});
const qualifiedManifest = compileSite(levelQualifiedEntry);
assert.equal(qualifiedManifest.roomGraph.edges.filter(edge => edge.rooms.includes('outside')).length, 1);
assert.equal(qualifiedManifest.roomGraph.edges.find(edge => edge.rooms.includes('outside')).portalVolumeId,
  'core:portal:aperture:ground:main-entry');

// Every supported angle uses the exact placement equation; world coordinates
// remain fractional instead of being rounded to the metre grid.
for (const yawDeg of [15, 30, 45, -15, -30, -45]) {
  const angled = compileSite(angledStudySite(yawDeg));
  const angledHall = angled.wings.find(wing => wing.id === 'hall');
  const radians = yawDeg * Math.PI / 180;
  near(angledHall.frame.origin,
    [18 - Math.sin(radians) * 4, 0, 6 - Math.cos(radians) * 4],
    `${yawDeg} degree solved origin`);
  near(midpoint(...angled.connectors[0].mouths[1].segment), [18, 6],
    `${yawDeg} degree hall mouth`);
  assert.equal(angledHall.frame.yawDeg, yawDeg);
}

// Add a north wing at runtime to make wing and connection ordering genuinely
// observable while leaving the canonical fixture frozen.
function multiConnectionSite() {
  const site = clone(CASTLE_SITE_ANGLED_STUDY);
  const coreSource = site.wings.find(wing => wing.id === 'core');
  coreSource.plan.levels.find(level => level.id === 'ground').edgeOverrides.push({
    id: 'north-hall', from: [0, 12], to: [12, 12], kind: 'arch',
    connects: ['outside', 'core-ground'],
    opening: { width: 2.4, height: 3.2, offset: 4.8 },
  });
  site.wings.push({
    id: 'north', plan: clone(ANGLED_STUDY_HALL_PLAN),
    placement: {
      socket: { level: 'ground', portal: 'west-entry' },
      relativeTo: { wing: 'core', level: 'ground', portal: 'north-hall' },
      yawDeg: -30, outset: 6, lateral: 0,
    },
  });
  site.connections.push({
    id: 'north-link',
    a: { wing: 'core', level: 'ground', portal: 'north-hall' },
    b: { wing: 'north', level: 'ground', portal: 'west-entry' },
    floor: 'stone', height: 3.2, roof: { kind: 'low-hip', rise: 0.8 },
  });
  return site;
}

const ordered = multiConnectionSite();
const reordered = clone(ordered);
reordered.wings.reverse();
reordered.connections.reverse();
assert.deepEqual(compileSite(reordered), compileSite(ordered),
  'wing and connection input order does not affect the manifest');

// Outdoor courts are explicit walkable site nodes, not aliases for the one
// global outside node. Their stone polygon supports each consumed socket.
const courtyardSite = clone(CASTLE_SITE_ANGLED_STUDY);
courtyardSite.wings.find(wing => wing.id === 'core').plan.levels[0].edgeOverrides.push({
  id: 'north-court', from: [0, 12], to: [12, 12], kind: 'arch',
  connects: ['outside', 'core-ground'],
  opening: { width: 2.4, height: 3.2, offset: 4.8 },
});
courtyardSite.courtyards = [{
  id: 'inner-court', level: 'ground',
  clearPolygon: [[4, 11.7], [8, 11.7], [8, 18], [4, 18]],
  floor: { material: 'stone', thickness: 0.3 },
  sockets: [{ wing: 'core', level: 'ground', portal: 'north-court' }],
}];
const courtyardManifest = compileSite(courtyardSite);
assert.equal(courtyardManifest.courtyards[0].nodeId, 'site:courtyard:inner-court');
assert.ok(courtyardManifest.roomGraph.reachableRoomIds.includes('site:courtyard:inner-court'));
assert.equal(courtyardManifest.roomGraph.edges.filter(edge => edge.kind === 'courtyard').length, 1);
assert.equal(courtyardManifest.roomGraph.edges.filter(edge => edge.rooms.includes('outside')).length, 1,
  'courtyard access never creates a second global outside connection');
const courtRoute = courtyardManifest.walkRoutes.find(route =>
  route.roomId === 'site:courtyard:inner-court');
assert.ok(courtRoute.waypoints.length >= 4);
assert.match(courtRoute.traversals.at(-1).edgeId, /^route:courtyard:/);

const intrudingCourt = clone(courtyardSite);
intrudingCourt.courtyards[0].clearPolygon = [[-5, -5], [20, -5], [20, 20], [-5, 20]];
expectInvalid(intrudingCourt, /courtyards\.inner-court.*intrudes into wing volume/);

// Socket arrays are semantically unordered. IDs and routes remain stable when
// a court has multiple openings on the same facade and their input is reversed.
const orderedCourt = clone(courtyardSite);
const originalNorthCourt = orderedCourt.wings.find(wing => wing.id === 'core')
  .plan.levels[0].edgeOverrides.find(override => override.id === 'north-court');
originalNorthCourt.from = [3, 12];
originalNorthCourt.to = [9, 12];
originalNorthCourt.opening.offset = 1.8;
orderedCourt.wings.find(wing => wing.id === 'core').plan.levels[0].edgeOverrides.push({
  id: 'north-court-west', from: [0, 12], to: [3, 12], kind: 'arch',
  connects: ['outside', 'core-ground'],
  opening: { width: 1.2, height: 3.2, offset: 0.9 },
});
orderedCourt.courtyards[0].clearPolygon = [[0.9, 11.7], [8, 11.7], [8, 18], [0.9, 18]];
orderedCourt.courtyards[0].sockets.push({
  wing: 'core', level: 'ground', portal: 'north-court-west',
});
const reversedCourt = clone(orderedCourt);
reversedCourt.courtyards[0].sockets.reverse();
assert.equal(siteToJSON(compileSite(reversedCourt)), siteToJSON(compileSite(orderedCourt)),
  'courtyard socket input order does not change compiled IDs or routes');

const overlappingCourts = clone(orderedCourt);
const sharedCourtPolygon = clone(overlappingCourts.courtyards[0].clearPolygon);
const [firstCourtSocket, secondCourtSocket] = overlappingCourts.courtyards[0].sockets;
overlappingCourts.courtyards = [
  { id: 'court-a', level: 'ground', clearPolygon: sharedCourtPolygon,
    floor: 'stone', sockets: [firstCourtSocket] },
  { id: 'court-b', level: 'ground', clearPolygon: sharedCourtPolygon,
    floor: 'stone', sockets: [secondCourtSocket] },
];
expectInvalid(overlappingCourts, /courtyards.*positive-area overlap between court-a and court-b/);

const connectorCourtOverlap = clone(angledStudySite(0));
const connectorCoreLevel = connectorCourtOverlap.wings.find(wing => wing.id === 'core').plan.levels[0];
const connectorPortal = connectorCoreLevel.edgeOverrides.find(override => override.id === 'east-hall');
connectorPortal.from = [12, 3];
connectorPortal.to = [12, 9];
connectorPortal.opening.offset = 1.8;
connectorCoreLevel.edgeOverrides.push({
  id: 'east-court', from: [12, 9], to: [12, 12], kind: 'arch',
  connects: ['outside', 'core-ground'],
  opening: { width: 1.2, height: 2.8, offset: 0.9 },
});
connectorCourtOverlap.courtyards = [{
  id: 'connector-court', level: 'ground', baseY: 0,
  clearPolygon: [[11.7, 5], [18, 5], [18, 11.1], [11.7, 11.1]],
  floor: { material: 'stone', thickness: 0.25 },
  sockets: [{ wing: 'core', level: 'ground', portal: 'east-court' }],
}];
expectInvalid(connectorCourtOverlap,
  /courtyards\.connector-court.*positive-area floor overlap with connector vestibule/);

// A raised courtyard slab can descend into the top of a lower connector wall
// even when its authored walking surface is above that wall.
const raisedCourtWallOverlap = clone(angledStudySite(0));
raisedCourtWallOverlap.wings.push({
  id: 'mezzanine',
  frame: { origin: [12, 0, -10], yawDeg: 0 },
  plan: {
    schema: ANGLED_STUDY_CORE_PLAN.schema, id: 'raised-court-wing', seed: 9414,
    entryRoomId: 'mezz',
    style: { wallThickness: 0.6, wallMaterial: 'castle.limestone', bond: 'ashlar' },
    levels: [{
      id: 'ground', baseY: 3.3, height: 4,
      rooms: [{ id: 'mezz', use: 'gallery', floorType: 'flags',
        rect: { x: 0, z: 0, width: 4, depth: 4 } }],
      edgeOverrides: [{
        id: 'court-door', from: [0, 4], to: [4, 4], kind: 'door',
        connects: ['outside', 'mezz'], opening: { width: 1.2, height: 2.8, offset: 1.4 },
      }],
    }],
    stairs: [], beams: [], fixtures: [], roofs: [], localLights: [], curves: [],
  },
});
raisedCourtWallOverlap.courtyards = [{
  id: 'raised-court', level: 'ground', baseY: 3.3,
  clearPolygon: [[13.4, -6.3], [14.6, -6.3], [14.6, 4.5], [13.4, 4.5]],
  floor: { material: 'stone', thickness: 0.25 },
  sockets: [{ wing: 'mezzanine', level: 'ground', portal: 'court-door' }],
}];
expectInvalid(raisedCourtWallOverlap,
  /courtyards\.raised-court.*floor overlaps wall solid.*connector vestibule/);

// Tuple components are escaped before global namespacing, so authored colons
// cannot alias the reserved courtyard node namespace.
const collisionSafe = clone(courtyardSite);
collisionSafe.wings = [clone(collisionSafe.wings.find(wing => wing.id === 'core'))];
collisionSafe.wings[0].id = 'site';
collisionSafe.wings[0].frame = { origin: [0, 0, 0], yawDeg: 0 };
delete collisionSafe.wings[0].placement;
collisionSafe.wings[0].plan.levels = [collisionSafe.wings[0].plan.levels[0]];
collisionSafe.wings[0].plan.stairs = [];
collisionSafe.wings[0].plan.entryRoomId = 'courtyard:court';
collisionSafe.wings[0].plan.levels[0].rooms[0].id = 'courtyard:court';
for (const override of collisionSafe.wings[0].plan.levels[0].edgeOverrides)
  override.connects = override.connects.map(room => room === 'core-ground' ? 'courtyard:court' : room);
collisionSafe.connections = [];
collisionSafe.entry.wing = 'site';
collisionSafe.courtyards[0].id = 'court';
collisionSafe.courtyards[0].sockets[0].wing = 'site';
const collisionSafeManifest = compileSite(collisionSafe);
assert.equal(new Set(collisionSafeManifest.roomGraph.nodes.map(node => node.id)).size,
  collisionSafeManifest.roomGraph.nodes.length);
assert.ok(collisionSafeManifest.roomGraph.nodes.some(node => node.id === 'site:courtyard%3Acourt'));
assert.ok(collisionSafeManifest.roomGraph.nodes.some(node => node.id === 'site:courtyard:court'));

const tokenSafe = clone(collisionSafe);
tokenSafe.courtyards = [];
tokenSafe.wings[0].plan.entryRoomId = 'a:b';
tokenSafe.wings[0].plan.levels[0].rooms = [
  { id: 'a:b', use: 'hall', floorType: 'flags', rect: { x: 0, z: 0, width: 6, depth: 12 } },
  { id: 'a~3Ab', use: 'solar', floorType: 'flags', rect: { x: 6, z: 0, width: 6, depth: 12 } },
];
tokenSafe.wings[0].plan.levels[0].edgeOverrides = [
  { id: 'main-entry', from: [0, 0], to: [0, 12], kind: 'door',
    connects: ['outside', 'a:b'], opening: { width: 2.4, height: 3.2, offset: 4.8 } },
  { id: 'internal', from: [6, 0], to: [6, 12], kind: 'arch',
    connects: ['a:b', 'a~3Ab'], opening: { width: 2.4, height: 3.2, offset: 4.8 } },
];
const tokenSafeManifest = compileSite(tokenSafe);
assert.ok(tokenSafeManifest.roomGraph.nodes.some(node => node.id === 'site:a%3Ab'));
assert.ok(tokenSafeManifest.roomGraph.nodes.some(node => node.id === 'site:a~3Ab'));

// Site summaries rotate record positions, AABBs and spot-light directions with
// the solved wing frame rather than replacing them with axis-aligned bounds.
const transformedRecordsSite = clone(CASTLE_SITE_ANGLED_STUDY);
const transformedHallPlan = transformedRecordsSite.wings.find(wing => wing.id === 'hall').plan;
transformedHallPlan.fixtures.push({
  id: 'bench', levelId: 'ground', position: [3, 0.5, 2],
  bounds: { minX: 2, maxX: 4, minY: 0, maxY: 1, minZ: 1.5, maxZ: 2.5 },
});
transformedHallPlan.localLights.push({
  id: 'west-spot', levelId: 'ground', kind: 'spot',
  position: [2, 2.5, 4], direction: [1, -0.5, 0], intensity: 900,
});
const transformedRecords = compileSite(transformedRecordsSite).wings.find(wing => wing.id === 'hall');
const frame = transformedRecords.frame;
const bench = transformedRecords.world.fixtures.find(fixture => fixture.sourceId === 'bench');
near(bench.position, transformPoint(frame, [3, 0.5, 2]), 'fixture position');
near(bench.orientedBounds.center, transformPoint(frame, [3, 0.5, 2]), 'fixture OBB center');
near(bench.orientedBounds.halfExtents, [1, 0.5, 0.5], 'fixture OBB half extents');
near(bench.orientedBounds.rotation, yawDegToQuaternion(30), 'fixture OBB rotation');
const spot = transformedRecords.world.localLights.find(light => light.sourceId === 'west-spot');
near(spot.position, transformPoint(frame, [2, 2.5, 4]), 'spot position');
near(spot.direction, transformVector(frame, [1, -0.5, 0]), 'spot direction');

// Exporters retain the semantic ids, wing angles, connector and routes.
const json = siteToJSON(manifest);
assert.equal(json, siteToJSON(compileSite(clone(CASTLE_SITE_ANGLED_STUDY))));
assert.equal(JSON.parse(json).connectors[0].id, 'vestibule');
const svg = siteToSVG(manifest, { scale: 10, padding: 12 });
assert.match(svg, /^<\?xml version="1\.0"/);
assert.match(svg, /data-wing="hall"/);
assert.match(svg, /data-link="vestibule"/);
assert.match(svg, /30°/);
assert.match(svg, /class="route"/);
assert.match(svg, /data-level="ground"/);
assert.doesNotMatch(svg, /core solar/);
const upperSvg = siteToSVG(manifest, { scale: 10, padding: 12, levelId: 'upper' });
assert.match(upperSvg, /data-level="upper"/);
assert.match(upperSvg, /core solar/);
assert.doesNotMatch(upperSvg, /hall feasting-hall/);
const courtyardSvg = siteToSVG(courtyardManifest, { scale: 10, padding: 12 });
assert.match(courtyardSvg, /class="courtyard" data-courtyard="inner-court"/);
assert.match(courtyardSvg, /class="portal arch" data-portal="north-court"/);
assert.match(courtyardSvg, /class="label-halo"/);
assert.match(courtyardSvg, /class="label"/);
assert.doesNotMatch(courtyardSvg, /paint-order/);

// Validation failures: off-grid yaw, cyclic placement, socket reuse, narrow
// capsule clearance, through-wing intrusion, positive overlap and elevations.
const offGrid = angledStudySite(22.5);
expectInvalid(offGrid, /yawDeg.*angleStep 15/);

const cyclic = clone(CASTLE_SITE_ANGLED_STUDY);
cyclic.wings[0] = {
  id: 'core', plan: clone(ANGLED_STUDY_CORE_PLAN),
  placement: {
    socket: { level: 'ground', portal: 'east-hall' },
    relativeTo: { wing: 'hall', level: 'ground', portal: 'west-entry' },
    yawDeg: 0, outset: 6, lateral: 0,
  },
};
expectInvalid(cyclic, /cyclic placement dependency/);

const reused = clone(CASTLE_SITE_ANGLED_STUDY);
reused.connections.push({
  ...clone(reused.connections[0]), id: 'reused-link',
});
expectInvalid(reused, /socket core:ground:east-hall is already consumed/);

const narrow = clone(CASTLE_SITE_ANGLED_STUDY);
for (const wing of narrow.wings) for (const override of wing.plan.levels[0].edgeOverrides) {
  if (override.id === 'east-hall') {
    override.opening.width = 1.19;
    override.opening.offset = 5.405;
  } else if (override.id === 'west-entry') {
    override.opening.width = 1.19;
    override.opening.offset = 3.405;
  }
}
expectInvalid(narrow, /circulation portal must be at least 1\.2m wide/);

const intruding = clone(CASTLE_SITE_ANGLED_STUDY);
intruding.wings.find(wing => wing.id === 'hall').placement.outset = -1;
expectInvalid(intruding, /mouths do not face the connector|intrud/);

const participantIntrusion = clone(CASTLE_SITE_ANGLED_STUDY);
participantIntrusion.connections[0].wallThickness = 1.2;
participantIntrusion.wings.find(wing => wing.id === 'core').plan.levels[0].rooms.push({
  id: 'outboard-turret', use: 'guardroom', required: false,
  rect: { x: 14, z: 5, width: 2, depth: 2 },
});
expectInvalid(participantIntrusion,
  /intrudes into wing volume core:occupied:room:outboard-turret/);

const participantWallIntrusion = angledStudySite(0);
participantWallIntrusion.connections[0].wallThickness = 1.2;
const wallObstaclePlan = clone(ANGLED_STUDY_HALL_PLAN);
wallObstaclePlan.id = 'wall-obstacle-plan';
wallObstaclePlan.entryRoomId = 'outboard-wall-room';
wallObstaclePlan.levels[0].rooms = [{
  id: 'outboard-wall-room', use: 'guardroom', required: false,
  rect: { x: 0, z: 0, width: 2, depth: 2 },
}];
wallObstaclePlan.levels[0].edgeOverrides = [{
  id: 'obstacle-entry', from: [0, 0], to: [0, 2], kind: 'arch',
  connects: ['outside', 'outboard-wall-room'],
  opening: { width: 1.2, height: 2.8, offset: 0.4 },
}];
participantWallIntrusion.wings.push({
  id: 'wall-obstacle', plan: wallObstaclePlan,
  frame: { origin: [14, 0, 2.7], yawDeg: 0 },
});
expectInvalid(participantWallIntrusion, /wallSpans.*solid intrudes into wing volume.*outboard-wall-room/);

const lowHeadroom = clone(CASTLE_SITE_ANGLED_STUDY);
lowHeadroom.connections[0].height = 1;
expectInvalid(lowHeadroom, /clearHeight.*at least 2\.1m headroom/);

const unsupportedRoof = clone(CASTLE_SITE_ANGLED_STUDY);
unsupportedRoof.connections[0].roof.kind = 'flat';
expectInvalid(unsupportedRoof, /roof\.kind.*only low-hip is supported/);

const unsafeRoute = angledStudySite(15);
unsafeRoute.wings[1].placement.outset = 2;
unsafeRoute.wings[1].placement.lateral = 4;
expectInvalid(unsafeRoute, /routeWaypoints\[2\].*lacks 0\.6m side clearance/);

const overlapping = clone(CASTLE_SITE_ANGLED_STUDY);
overlapping.wings.push({
  id: 'overlap', plan: clone(ANGLED_STUDY_HALL_PLAN),
  frame: { origin: [2, 0, 2], yawDeg: 0 },
});
expectInvalid(overlapping, /positive-area overlap/);

// Circular occupied volumes use exact circle tests, not an inscribed polygon
// that can miss a shallow lens between differently rotated wings.
const circleWingPlan = {
  schema: ANGLED_STUDY_CORE_PLAN.schema, id: 'circle-overlap-wing', seed: 9413,
  entryRoomId: 'hall', style: clone(ANGLED_STUDY_CORE_PLAN.style),
  levels: [{ id: 'ground', baseY: 0, height: 4, rooms: [
    { id: 'hall', use: 'hall', floorType: 'flags', rect: { x: 0, z: 0, width: 9, depth: 6 } },
    { id: 'tower', use: 'guardroom', floorType: 'flags',
      boundary: { kind: 'circle', center: [13, 3], radius: 4 } },
  ], edgeOverrides: [
    { id: 'main-entry', from: [0, 0], to: [2, 0], kind: 'door',
      connects: ['outside', 'hall'], opening: { width: 1.2, height: 2.8, offset: 0.4 } },
    { id: 'hall-tower-door', from: [9, 2], to: [9, 4], kind: 'door',
      connects: ['hall', 'outside'], opening: { width: 1.4, height: 2.8, offset: 0.3 } },
  ] }],
  curves: [{ id: 'tower-ring', levelId: 'ground', roomId: 'tower', kind: 'ring',
    center: [13, 3], radius: 4, apertures: [{
      id: 'tower-throat', kind: 'door', startAngle: 160, endAngle: 200,
      bottom: 0, height: 2.8, connects: ['tower', 'hall'],
      throat: { targetRoomId: 'hall', direction: 'W', width: 1.4, depth: 1.2 },
    }] }],
  stairs: [], beams: [], fixtures: [], roofs: [], localLights: [], verticalVoids: [],
};
const circleOverlap = {
  schema: CASTLE_SITE_ANGLED_STUDY.schema, id: 'exact-circle-overlap', seed: 9413,
  grid: 1, angleStep: 15,
  entry: { wing: 'west', level: 'ground', portal: 'main-entry' },
  wings: [
    { id: 'west', plan: circleWingPlan, frame: { origin: [0, 0, 0], yawDeg: 0 } },
    { id: 'east', plan: { ...clone(circleWingPlan),
      levels: clone(circleWingPlan.levels).map(level => ({ ...level,
        rooms: level.rooms.map(room => ({ ...room, required: false })) })) },
    frame: { origin: [33.38, 0, 6], yawDeg: 180 } },
  ], connections: [],
};
expectInvalid(circleOverlap, /positive-area overlap.*tower/);

const circleCourtIntrusion = clone(courtyardSite);
circleCourtIntrusion.courtyards[0].clearPolygon =
  [[4, 11.7], [8, 11.7], [50, 30], [30, 50]];
const edgeFrom = [8, 11.7], edgeTo = [50, 30];
const edgeLength = Math.hypot(edgeTo[0] - edgeFrom[0], edgeTo[1] - edgeFrom[1]);
const edgeMidpoint = midpoint(edgeFrom, edgeTo);
const edgeOutward = [(edgeTo[1] - edgeFrom[1]) / edgeLength,
  -(edgeTo[0] - edgeFrom[0]) / edgeLength];
const circleTarget = [edgeMidpoint[0] + edgeOutward[0] * 3.695,
  edgeMidpoint[1] + edgeOutward[1] * 3.695];
const circleLocalWorld = transformPoint({ origin: [0, 0, 0], yawDeg: -135 }, [13, 0, 3]);
const circleCourtPlan = clone(circleWingPlan);
for (const room of circleCourtPlan.levels[0].rooms) room.required = false;
circleCourtIntrusion.wings.push({
  id: 'circle-probe', plan: circleCourtPlan,
  frame: { origin: [circleTarget[0] - circleLocalWorld[0], 0,
    circleTarget[1] - circleLocalWorld[2]], yawDeg: -135 },
});
expectInvalid(circleCourtIntrusion, /courtyards\.inner-court.*circle-probe:occupied:room:tower/);

const elevated = clone(CASTLE_SITE_ANGLED_STUDY);
elevated.wings[1] = {
  id: 'hall', plan: clone(ANGLED_STUDY_HALL_PLAN),
  frame: { origin: [16, 1, 2.5358983848622456], yawDeg: 30 },
};
expectInvalid(elevated, /incompatible elevations 0 and 1/);

console.log('castle_site_tests: ok');
