import assert from 'node:assert/strict';
import { CASTLE_SOLID_SOURCE_SEEDS, castleStoneSourceSpec, emitCastleStoneSource } from '../shared-lib/castle_solid_source.js';
const recipes = CASTLE_SOLID_SOURCE_SEEDS.map((seed) => castleStoneSourceSpec({ seed }));
assert.equal(recipes.length, 8);
assert.equal(new Set(recipes.map(JSON.stringify)).size, 8);
for (const seed of CASTLE_SOLID_SOURCE_SEEDS) {
  const recipe = castleStoneSourceSpec({ seed });
  assert.deepEqual(recipe, recipes[seed]);
  assert.equal(recipe.version, 1);
  assert.equal(recipe.voxelM, 0.003);
  assert.equal(recipe.ops.length, 23);
  assert.deepEqual(recipe.ops[0].centerM, [0, 0.07, 0]);
  assert.equal(recipe.ops[0].halfExtentsM[0] + recipe.ops[0].roundingM, 0.15);
  assert(recipe.ops.slice(1).every((op) => op.combine === 'difference'));
}
const wider = castleStoneSourceSpec({ length: 0.45 });
assert.equal(wider.ops[0].halfExtentsM[0] + wider.ops[0].roundingM, 0.225);
assert.deepEqual(wider.ops[1].radiiM, recipes[0].ops[1].radiiM, 'physical relief does not stretch with source dimensions');
for (const seed of [-1, 8, 0.5, NaN]) assert.throws(() => castleStoneSourceSpec({ seed }));
for (const length of [0, -1, Infinity, 3]) assert.throws(() => castleStoneSourceSpec({ length }));
let material, captured;
emitCastleStoneSource({ fill: (value) => { material = value; }, solidSource: (value) => { captured = value; } }, { material: 17, seed: 2 });
assert.equal(material, 17);
assert.deepEqual(captured, recipes[2]);
console.log('castle_solid_source_tests: PASS (8 physical source recipes)');

const chunky = CASTLE_SOLID_SOURCE_SEEDS.map(seed => castleStoneSourceSpec({seed, reliefStyle:1}));
assert.equal(new Set(chunky.map(JSON.stringify)).size,8);
for(let seed=0;seed<8;seed++) {
  const r=chunky[seed];
  assert.equal(r.ops.length,23,'broader relief without more CSG work');
  assert.equal(r.ops[0].roundingM,.005);
  assert.deepEqual(r.ops[0].halfExtentsM.map(v=>v+r.ops[0].roundingM),[.15,.07,.10]);
  for(const op of r.ops.slice(1,19)) {
    assert.ok(op.radiiM[0]>=.018 && op.radiiM[0]<=.040);
    assert.ok(op.radiiM[1]>=.012 && op.radiiM[1]<=.025);
    const penetration=.10+op.radiiM[2]-Math.abs(op.centerM[2]);
    assert.ok(penetration>=.003-1e-12 && penetration<=.006+1e-12);
    assert.ok(penetration+op.blendM<.012,'face hollows stay above nominal mortar recess');
  }
  for(const op of r.ops.slice(19))assert.ok(op.radiusM>=.014&&op.radiusM<=.028);
  assert.deepEqual(r,castleStoneSourceSpec({seed,reliefStyle:1}));
}
assert.throws(()=>castleStoneSourceSpec({reliefStyle:2}));
assert.throws(()=>castleStoneSourceSpec({reliefStyle:1,height:.04}));
assert.deepEqual(castleStoneSourceSpec(),castleStoneSourceSpec({reliefStyle:0}),'default fine recipe retained');
console.log('castle_solid_source_tests: PASS (8 chunky recipes; fixed op count, dimensions, finite relief)');
