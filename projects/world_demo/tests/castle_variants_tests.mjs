import assert from 'node:assert/strict';

await import('./castle_shared_lib_hooks.mjs');
const { castleManifest } = await import('../shared-lib/castle_variants.js');

// Upper gallery edges open as railed balconies only where they overlook air: a
// double-height void beside them, or an open court below their exterior side.
// A gallery wall lying on a hall's or chapel's outer edge is the building's
// outer wall, not a balcony (roundkeep's hall galleries, the cloister choir).
const SEEDS = { courtyard: 9411, roundkeep: 17029, cloister: 9411 };
const cellKey = cell => cell.join(',');
const summary = {};

for (const [name, seed] of Object.entries(SEEDS)) {
  const manifest = castleManifest(name, seed);
  const rooms = new Map(manifest.rooms.map(room => [room.id, room]));
  const cells = new Map(manifest.rooms.map(room => [room.id, new Set((room.boundary.cells || []).map(cellKey))]));
  const courtCells = new Set(manifest.rooms.filter(room => room.levelId === 'ground' && room.use === 'court')
    .flatMap(room => room.boundary.cells.map(cellKey)));
  const balconies = manifest.walls.filter(wall => wall.levelId === 'upper' && wall.kind === 'open' &&
    String(wall.overrideId).startsWith('balcony-'));
  let exterior = 0;
  for (const wall of balconies) {
    const adjacent = wall.roomIds.map(id => rooms.get(id));
    const gallery = adjacent.find(room => room.use === 'gallery');
    assert.ok(gallery, `${name} ${wall.id}: balcony edge belongs to a gallery`);
    assert.equal(wall.railProfile, 'castle.oak-guardrail', `${name} ${wall.id}: balcony edge is railed`);
    if (wall.boundary !== 'exterior') {
      assert.ok(adjacent.some(room => room.openToBelow), `${name} ${wall.id}: partition balcony faces a double-height void`);
      continue;
    }
    ++exterior;
    const [x, z] = wall.from.map((value, i) => Math.min(value, wall.to[i]));
    const sides = wall.axis === 'x' ? [[x, z - 1], [x, z]] : [[x - 1, z], [x, z]];
    const beyond = sides.find(cell => !cells.get(gallery.id).has(cellKey(cell)));
    assert.ok(courtCells.has(cellKey(beyond)), `${name} ${wall.id}: exterior balcony overlooks an open court, not the outside`);
  }
  summary[name] = { balconies: balconies.length, exterior };
}

// Court-facing galleries keep their open arcades.
assert.ok(summary.courtyard.exterior > 0, 'courtyard galleries still open onto the court');
assert.ok(summary.cloister.exterior > 0, 'cloister upper walks still open onto the garth');
assert.equal(summary.roundkeep.exterior, 0, 'roundkeep has no court, so no exterior balconies');

// The roundkeep hall galleries' outer walls (x=4 / x=20 above the service
// lean-tos) and the cloister choir's east wall (the chapel's outer face) stay
// enclosed, with the facade's window bays rather than guardrails.
const outerWall = (name, x, z0, z1) => castleManifest(name, SEEDS[name]).walls.filter(wall =>
  wall.levelId === 'upper' && wall.axis === 'z' && wall.from[0] === x &&
  Math.min(wall.from[1], wall.to[1]) >= z0 && Math.max(wall.from[1], wall.to[1]) <= z1);
for (const [name, x, z0, z1, length] of [['roundkeep', 4, 8, 22, 14], ['roundkeep', 20, 8, 22, 14], ['cloister', 44, 18, 20, 2]]) {
  const walls = outerWall(name, x, z0, z1);
  assert.equal(walls.length, length, `${name} x=${x}: every metre of the outer wall line is emitted`);
  assert.ok(walls.every(wall => wall.boundary === 'exterior' && wall.kind !== 'open' && !wall.railProfile),
    `${name} x=${x} z${z0}..${z1}: outer gallery wall stays enclosed`);
}
assert.ok(outerWall('roundkeep', 4, 8, 22).some(wall => wall.kind === 'window'), 'roundkeep west gallery wall gets facade windows');
assert.ok(outerWall('roundkeep', 20, 8, 22).some(wall => wall.kind === 'window'), 'roundkeep east gallery wall gets facade windows');

console.log(`castle variants: PASS - ${JSON.stringify(summary)}`);
