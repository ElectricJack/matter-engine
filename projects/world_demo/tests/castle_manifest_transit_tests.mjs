import assert from 'node:assert/strict';
import {
  CASTLE_PLAN_SCHEMA,
  compilePlan,
  routeManifestRoomSegment,
} from '../shared-lib/castle_plan.js';

function volumesOverlap(a, b) {
  return a.minX < b.maxX - 1e-9 && b.minX < a.maxX - 1e-9 &&
    a.minY < b.maxY - 1e-9 && b.minY < a.maxY - 1e-9 &&
    a.minZ < b.maxZ - 1e-9 && b.minZ < a.maxZ - 1e-9;
}

function deepFreeze(value) {
  if (!value || typeof value !== 'object' || Object.isFrozen(value)) return value;
  Object.freeze(value);
  for (const child of Object.values(value)) deepFreeze(child);
  return value;
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

const fixtureClearance = {
  minX: 2.8, maxX: 5.2, minY: 4, maxY: 6.1,
  minZ: 0.7, maxZ: 1.6,
};
const plan = {
  schema: CASTLE_PLAN_SCHEMA,
  id: 'manifest-room-transit',
  seed: 41,
  entryRoomId: 'lower-hall',
  levels: [
    {
      id: 'ground', baseY: 0, height: 4,
      rooms: [{ id: 'lower-hall', use: 'hall', rect: { x: 0, z: 0, width: 8, depth: 6 } }],
      edgeOverrides: [],
    },
    {
      id: 'upper', baseY: 4, height: 4,
      rooms: [{
        id: 'upper-deck', use: 'gallery', required: false,
        rect: { x: 0, z: 0, width: 8, depth: 6 },
      }],
      edgeOverrides: [],
    },
  ],
  verticalVoids: [{
    id: 'central-opening', kind: 'double-height',
    lowerLevelId: 'ground', upperLevelId: 'upper',
    lowerRoomId: 'lower-hall', upperRoomIds: ['upper-deck'],
    footprint: { x: 3, z: 2, width: 2, depth: 2 },
  }],
  fixtures: [{ id: 'display-case', levelId: 'upper', clearance: fixtureClearance }],
  stairs: [], beams: [], curves: [], roofs: [], localLights: [],
};

const manifest = deepFreeze(compilePlan(plan));
const before = JSON.stringify(manifest);
const from = [0.6, 4, 3], to = [7.4, 4, 3];
const route = routeManifestRoomSegment(manifest, 'upper-deck', from, to);
const floorHole = manifest.floors.find(floor => floor.roomId === 'upper-deck').holes[0];

assert.equal(JSON.stringify(manifest), before, 'routing does not mutate the compiled manifest');
assert.deepEqual(route, routeManifestRoomSegment(manifest, 'upper-deck', from, to),
  'manifest routing is deterministic');
assert.equal(route.roomId, 'upper-deck');
assert.equal(route.width, 1.2, 'the public helper defaults to compiler walk clearance');
assert.deepEqual(route.from, from);
assert.deepEqual(route.to, to);
assert.ok(route.segments.length >= 3 && route.sweptBounds,
  'the public result reports executable legs and aggregate swept bounds');
assert.ok(route.waypoints.some(point => point[2] >= 4.6 - 1e-9),
  'the route takes the upper lane around the central opening');
for (const leg of route.segments) {
  assert.ok(!segmentEntersExpandedRect(leg.from, leg.to, floorHole.footprint, route.width / 2),
    'every route leg avoids the capsule-expanded compiled floor hole');
  assert.ok(!volumesOverlap(leg.bounds, fixtureClearance),
    'every swept route leg avoids the compiled fixture clearance');
}

console.log('castle_manifest_transit_tests: public pure manifest routing detours around floor holes and fixtures');
