import assert from 'node:assert/strict';
import { compilePlan, CASTLE_PLAN_SCHEMA } from '../shared-lib/castle_plan.js';
import { castleCollisionEntities } from '../shared-lib/castle_collision.js';

const clone = value => JSON.parse(JSON.stringify(value));
const same = (a, b) => a.every((v, i) => Math.abs(v - b[i]) < 1e-8);
const sequence = (points, expected) => points.some((_, start) =>
  expected.every((p, i) => points[start + i] && same(p, points[start + i])));
function plan() {
  return {
    schema: CASTLE_PLAN_SCHEMA, id: 'landing-turn-regression', seed: 17,
    entryRoomId: 'lower',
    levels: [
      { id: 'ground', baseY: 0, height: 4, rooms: [
        { id: 'lower', use: 'hall', rect: { x: 0, z: 0, width: 8, depth: 8 } },
      ], edgeOverrides: [] },
      { id: 'upper', baseY: 4, height: 4, rooms: [
        { id: 'upper', use: 'gallery', rect: { x: 0, z: 0, width: 8, depth: 8 } },
      ], edgeOverrides: [] },
    ],
    stairs: [{
      id: 'return', lowerLevelId: 'ground', upperLevelId: 'upper',
      lowerRoomId: 'lower', upperRoomId: 'upper', width: 1.5,
      tread: .25, maxRiser: .2, headroom: 2.2, entryDirection: 'N', exitDirection: 'S',
      flights: [
        { id: 'out', direction: 'N', stepCount: 10, footprint: { x: 1, z: 2, width: 1.5, depth: 2.5 } },
        { id: 'back', direction: 'S', stepCount: 10, footprint: { x: 2.5, z: 2, width: 1.5, depth: 2.5 } },
      ],
      landings: [
        { id: 'lower', kind: 'lower', bounds: { x: 1, z: .5, width: 1.5, depth: 1.5 } },
        { id: 'middle', kind: 'intermediate', elevation: 2, bounds: { x: 1, z: 4.5, width: 3, depth: 1.5 } },
        { id: 'upper', kind: 'upper', bounds: { x: 2.5, z: .5, width: 1.5, depth: 1.5 } },
      ],
    }],
    beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
  };
}
const input = plan(), manifest = compilePlan(input), stair = manifest.stairs[0];
const turn = [[1.75, 2, 4.5], [1.75, 2, 5.25], [3.25, 2, 5.25], [3.25, 2, 4.5]];
assert.ok(sequence(stair.route.waypoints, turn), 'U turn moves into landing before traversing sideways');
assert.ok(sequence(manifest.walkRoute.find(r => r.roomId === 'upper').waypoints, stair.route.waypoints),
  'full authored walk route contains inset landing transit and every flight endpoint');
assert.deepEqual(compilePlan(input), manifest, 'route generation is deterministic and does not mutate source');

// Native failure reproduced geometrically: the old turn at z4.5 touches the
// return flight's raised first tread. The new turn clears that actual box by
// .75m, exceeding the .4m capsule radius plus .2m route clearance.
const tread = castleCollisionEntities(manifest).find(e => e.id.includes('flight:return:back:tread:0:'));
assert.ok(tread);
const position = tread.components.LocalTransform.translation;
const extent = tread.components.BoxCollider.halfExtents;
assert.ok(Math.abs(position[2] + extent[2] - turn[0][2]) < 1e-8);
assert.ok(position[1] + extent[1] > turn[0][1], 'return first tread rises above intermediate landing');
assert.ok(turn[1][2] - (position[2] + extent[2]) >= .6);

const reverse = clone(input); reverse.entryRoomId = 'upper';
assert.ok(sequence(compilePlan(reverse).walkRoute.find(r => r.roomId === 'lower').waypoints,
  [...stair.route.waypoints].reverse()), 'descending route reverses the complete inset turn');

// Same geometry in all four orientations, including west/south and negative
// travel directions. Rotating the whole plan preserves supported turn shape.
const rotatePoint = p => [8 - p[2], p[1], p[0]];
const rotateRect = r => ({ x: 8 - r.z - r.depth, z: r.x, width: r.depth, depth: r.width });
const nextDirection = { N: 'W', W: 'S', S: 'E', E: 'N' };
let rotated = clone(input), expected = turn;
for (let quarter = 1; quarter < 4; ++quarter) {
  const s = rotated.stairs[0];
  for (const f of s.flights) { f.footprint = rotateRect(f.footprint); f.direction = nextDirection[f.direction]; }
  for (const l of s.landings) l.bounds = rotateRect(l.bounds);
  s.entryDirection = nextDirection[s.entryDirection]; s.exitDirection = nextDirection[s.exitDirection];
  expected = expected.map(rotatePoint);
  assert.ok(sequence(compilePlan(rotated).stairs[0].route.waypoints, expected));
}

// A landing behind both flight endpoints used to pass full-width point contact,
// but cannot support a turn in the declared incoming/outgoing directions.
const backwards = clone(input);
backwards.stairs[0].landings[1].bounds.z = 3;
assert.throws(() => compilePlan(backwards), /intermediate landing.*directed inset walking clearance/);

const obstructed = clone(input);
obstructed.beams.push({ levelId: 'ground', from: [1.5, 3.9, 5.25], to: [3.5, 3.9, 5.25],
  section: [.2, .2], jointFamily: 'mortise-tenon', role: 'landing-obstruction' });
assert.throws(() => compilePlan(obstructed), /headroom volume intersects beam/);

// Minimum-size L landing: both transit anchors coincide at its inset center.
const elbow = clone(input), s = elbow.stairs[0];
s.width = 1.2; s.entryDirection = 'E'; s.exitDirection = 'N';
s.flights = [
  { id: 'out', direction: 'E', stepCount: 10, footprint: { x: 1.2, z: 1, width: 2.5, depth: 1.2 } },
  { id: 'back', direction: 'N', stepCount: 10, footprint: { x: 3.7, z: 2.2, width: 1.2, depth: 2.5 } },
];
s.landings = [
  { id: 'lower', kind: 'lower', bounds: { x: 0, z: 1, width: 1.2, depth: 1.2 } },
  { id: 'middle', kind: 'intermediate', elevation: 2, bounds: { x: 3.7, z: 1, width: 1.2, depth: 1.2 } },
  { id: 'upper', kind: 'upper', bounds: { x: 3.7, z: 4.7, width: 1.2, depth: 1.2 } },
];
assert.ok(sequence(compilePlan(elbow).stairs[0].route.waypoints, [[3.7, 2, 1.6], [4.3, 2, 1.6], [4.3, 2, 2.2]]));
console.log('castle_landing_transit_tests: U/L turns, four directions, reverse routes, collider clearance and headroom passed');
