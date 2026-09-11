import assert from 'node:assert/strict';
import {
  CASTLE_BEAM_VARIANT_COUNT,
  CASTLE_PLANK_VARIANT_COUNT,
  CASTLE_STONE_VARIANT_COUNT,
  beamChildVariants,
  beamParams,
  emitBeam,
  emitPlank,
  emitStone,
  placeBeam,
  placePlank,
  placeStone,
  plankChildVariants,
  plankParams,
  stoneChildVariants,
  stoneParams,
} from '../shared-lib/castle_primitives.js';
import {
  CASTLE_MATERIAL_SPECS,
  defineCastleMaterials,
} from '../shared-lib/castle_materials.js';

function numbers(value, out = []) {
  if (typeof value === 'number') out.push(value);
  else if (Array.isArray(value)) for (const child of value) numbers(child, out);
  else if (value && typeof value === 'object')
    for (const child of Object.values(value)) numbers(child, out);
  return out;
}

class RecordingPart {
  constructor() {
    this.ops = [];
    this.material = -1;
    this.matrixDepth = 0;
    this.voxelDepth = 0;
    this.modifierDepth = 0;
    this.placements = [];
    this.lastGeometry = null;
  }

  record(kind, args = []) {
    assert.ok(numbers(args).every(Number.isFinite), kind + ' received non-finite geometry');
    const op = { kind, args, material: this.material, csg: 'union' };
    this.ops.push(op);
    return op;
  }

  beginModifier() { ++this.modifierDepth; this.record('beginModifier'); }
  endModifier(stack) {
    assert.ok(this.modifierDepth-- > 0);
    this.record('endModifier', stack);
  }
  beginVoxels(spacing) {
    assert.ok(spacing > 0);
    ++this.voxelDepth;
    this.lastGeometry = null;
    this.record('beginVoxels', [spacing]);
  }
  endVoxels() {
    assert.ok(this.voxelDepth-- > 0);
    this.lastGeometry = null;
    this.record('endVoxels');
  }
  fill(value) { this.material = value; this.record('fill', [value]); }
  smoothing(value) { this.record('smoothing', [value]); }
  box(center, halfExtents) { this.lastGeometry = this.record('box', [center, halfExtents]); }
  sphere(center, radius) { this.lastGeometry = this.record('sphere', [center, radius]); }
  capsule(a, b, radius) { this.lastGeometry = this.record('capsule', [a, b, radius]); }
  cylinder(a, b, radius) { this.lastGeometry = this.record('cylinder', [a, b, radius]); }
  difference() {
    assert.equal(this.voxelDepth, 1, 'postfix CSG is session-scoped');
    assert.ok(this.lastGeometry, 'postfix difference must follow geometry');
    this.lastGeometry.csg = 'difference';
  }
  pushMatrix() { ++this.matrixDepth; this.record('pushMatrix'); }
  popMatrix() { assert.ok(this.matrixDepth-- > 0); this.record('popMatrix'); }
  translate(...args) { this.record('translate', args); }
  scale(...args) { this.record('scale', args); }
  rotateX(value) { this.record('rotateX', [value]); }
  rotateY(value) { this.record('rotateY', [value]); }
  rotateZ(value) { this.record('rotateZ', [value]); }
  lookAt(target, up) { this.record('lookAt', [target, up]); }
  placeChild(module, params, opts) {
    this.placements.push({ module, params, opts });
  }

  balanced() {
    assert.equal(this.matrixDepth, 0, 'matrix stack balances');
    assert.equal(this.voxelDepth, 0, 'voxel session balances');
    assert.equal(this.modifierDepth, 0, 'modifier stack balances');
  }
}

assert.equal(CASTLE_STONE_VARIANT_COUNT, 12);
assert.equal(CASTLE_BEAM_VARIANT_COUNT, 8);
assert.equal(CASTLE_PLANK_VARIANT_COUNT, 8);

const normalizedStone = stoneParams({ seed: -1, material: 41 });
assert.equal(normalizedStone.seed, 11);
assert.equal(normalizedStone.material, 41);
assert.deepEqual(stoneParams({ seed: 12 }), stoneParams({ seed: 0 }));
assert.ok(Object.values(normalizedStone).every(Number.isFinite));
assert.ok(Object.values(beamParams()).every(Number.isFinite));
assert.ok(Object.values(plankParams()).every(Number.isFinite));

const stoneBase = {
  length: 0.73, height: 0.29, depth: 0.41, material: 41, detail: 1.2,
};
const declarations = stoneChildVariants(stoneBase);
assert.equal(declarations.length, CASTLE_STONE_VARIANT_COUNT);
assert.deepEqual(declarations.map((item) => item.params.seed),
  Array.from({ length: 12 }, (_, seed) => seed));
const parent = new RecordingPart();
const placedStone = placeStone(parent, { ...stoneBase, seed: 7 });
assert.deepEqual(parent.placements[0], {
  module: 'CastleStone', params: declarations[7].params, opts: undefined,
});
assert.deepEqual(placedStone, declarations[7].params);

const beamBase = { material: 50, endMaterial: 51 };
const plankBase = { material: 50, endMaterial: 51 };
const beamDeclarations = beamChildVariants(beamBase, [2]);
const plankDeclarations = plankChildVariants(plankBase, [3]);
placeBeam(parent, { ...beamBase, seed: 2 }, { instanced: true });
placePlank(parent, { ...plankBase, seed: 3 });
assert.deepEqual(parent.placements[1].params, beamDeclarations[0].params);
assert.deepEqual(parent.placements[2].params, plankDeclarations[0].params);

const stoneSignatures = new Set();
for (let seed = 0; seed < CASTLE_STONE_VARIANT_COUNT; ++seed) {
  const part = new RecordingPart();
  const result = emitStone(part, { ...stoneBase, seed });
  part.balanced();
  assert.equal(result.seed, seed);
  const voxelSpacings = part.ops.filter((op) => op.kind === 'beginVoxels')
    .map((op) => op.args[0]);
  assert.equal(voxelSpacings.length, 2);
  assert.equal(voxelSpacings[0], 0.08);
  assert.ok(voxelSpacings[1] < 0.05);
  assert.equal(part.ops.filter((op) => op.kind === 'box').length, 13,
    'one core and twelve oriented cutters form real stone bevels');
  assert.ok(part.ops.some((op) => op.kind === 'sphere' && op.csg === 'difference'),
    'stone has subtractive chips');
  assert.ok(part.ops.some((op) => op.kind === 'capsule' && op.csg === 'difference'),
    'stone has subtractive tool marks');
  assert.ok(part.ops.filter((op) => op.kind === 'sphere').length >= 8,
    'stone has additive/subtractive face undulation');
  // The structural core retains exact y=0/height bed planes before bevel cuts.
  const core = part.ops.find((op) => op.kind === 'box');
  assert.equal(core.args[0][1] - core.args[1][1], 0);
  assert.equal(core.args[0][1] + core.args[1][1], stoneBase.height);
  const repeat = new RecordingPart();
  emitStone(repeat, { ...stoneBase, seed });
  assert.equal(JSON.stringify(repeat.ops), JSON.stringify(part.ops));
  stoneSignatures.add(JSON.stringify(part.ops));
}
assert.equal(stoneSignatures.size, CASTLE_STONE_VARIANT_COUNT,
  'all canonical stone seeds emit different deterministic CSG');

for (const [emit, input, expected] of [
  [emitBeam, {
    seed: 4, length: 3.5, width: 0.3, height: 0.36,
    material: 50, endMaterial: 51, ironMaterial: 52,
    joint: 2, strap: 1, detail: 1.2,
  }, 'beam'],
  [emitPlank, {
    seed: 3, length: 2.4, width: 0.34, thickness: 0.09,
    material: 50, endMaterial: 51, ironMaterial: 52,
    joint: 3, strap: 1, detail: 1.2,
  }, 'plank'],
]) {
  const part = new RecordingPart();
  emit(part, input);
  part.balanced();
  assert.ok(part.ops.some((op) => op.kind === 'capsule' && op.csg === 'difference'),
    expected + ' has longitudinal checks');
  assert.ok(part.ops.some((op) => op.kind === 'sphere'), expected + ' has knot relief');
  assert.ok(part.ops.filter((op) => op.kind === 'fill')
    .some((op) => op.args[0] === input.endMaterial), expected + ' has end grain');
  assert.ok(part.ops.filter((op) => op.kind === 'fill')
    .some((op) => op.args[0] === input.ironMaterial), expected + ' has iron straps');
  assert.ok(part.ops.some((op) => op.kind === 'box' &&
    op.material === input.endMaterial), expected + ' end caps own end-grain material');
  assert.ok(part.ops.some((op) => op.kind === 'box' &&
    op.material === input.ironMaterial), expected + ' straps own iron material');
  assert.ok(part.ops.some((op) => op.csg === 'difference'), expected + ' uses CSG joinery');
  const lastVoxel = part.ops.map((op) => op.kind).lastIndexOf('endVoxels');
  const directRelief = part.ops.slice(lastVoxel + 1)
    .filter((op) => op.kind === 'capsule' && op.csg === 'union');
  assert.ok(directRelief.length >= 14,
    expected + ' retains raised face grain and radial end detail below voxel size');
}

const jointStreams = [];
for (let joint = 0; joint < 4; ++joint) {
  const part = new RecordingPart();
  emitBeam(part, {
    seed: 2, length: 3, width: 0.3, height: 0.34,
    material: 50, endMaterial: 51, ironMaterial: 52,
    joint, strap: 0,
  });
  part.balanced();
  jointStreams.push(part.ops);
}
assert.equal(jointStreams[0].filter((op) => op.kind === 'cylinder').length, 0);
assert.equal(jointStreams[1].filter((op) => op.kind === 'cylinder').length, 2);
assert.ok(jointStreams[2].some((op) => op.kind === 'box' && op.csg === 'difference'));
assert.ok(jointStreams[2].filter((op) => op.kind === 'cylinder').length >= 2);
assert.ok(jointStreams[3].filter((op) => op.kind === 'box' && op.csg === 'difference')
  .length >= jointStreams[0].filter((op) => op.kind === 'box' && op.csg === 'difference')
    .length + 2);

const materialCalls = [];
let nextMaterial = 100;
globalThis.defineMaterial = (name, spec) => {
  materialCalls.push({ name, spec });
  return nextMaterial++;
};
const materials = defineCastleMaterials('CastleTest');
delete globalThis.defineMaterial;
assert.equal(materialCalls.length, 16);
assert.deepEqual(materials.limestone, [100, 101, 102, 103]);
assert.equal(materials.foundation, 104);
assert.equal(materials.plaster, 115);
assert.equal(CASTLE_MATERIAL_SPECS.gold.metallic, 1);
assert.ok(CASTLE_MATERIAL_SPECS.gold.roughness >= 0.12 &&
  CASTLE_MATERIAL_SPECS.gold.roughness <= 0.28);
assert.equal(CASTLE_MATERIAL_SPECS.clearGlass.volumeBoundary, true);
assert.equal(CASTLE_MATERIAL_SPECS.clearGlass.translucency,
  CASTLE_MATERIAL_SPECS.clearGlass.transmission);
assert.ok(CASTLE_MATERIAL_SPECS.clearGlass.transmission > 0.9);
assert.ok(CASTLE_MATERIAL_SPECS.clearGlass.ior > 1);

console.log('castle primitives: PASS - 12 stones, timber relief/joinery, PBR handles');
