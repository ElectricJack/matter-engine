// Tests for shared-lib/castle_furnishings.js: the furnished-interiors kit
// (tables, seating, storage, altar, candle fixtures with paired analytic
// lights, and leaded window glazing). Loaded as data: URLs the same way
// river_hydrology_scene_tests.mjs loads its shared-lib dependencies, so the
// module's bare 'shared-lib/*' specifiers resolve without a bundler.
//
// A RecordingPart implements a real row-major 4x4 matrix stack (matching the
// module's own basisMatrix()/furnishingTransform() row-major layout) so
// transform-dependent assertions (light/geometry agreement under rotation,
// wrapper build() replays) can be checked against actual world-space math,
// not just op counts.

import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

globalThis.World = class {};

async function requiredText(url, label) {
  try {
    return await readFile(url, 'utf8');
  } catch (error) {
    assert.fail(`${label} must exist: ${error.message}`);
  }
}

function dataUrl(source) {
  return `data:text/javascript;base64,${Buffer.from(source).toString('base64')}`;
}

// ---------------------------------------------------------------------------
// Load castle_furnishings.js with its two bare specifiers rewritten to data:
// URLs for its sibling modules (same technique as river_hydrology_scene_tests).

const primitivesSource = await requiredText(new URL(
  '../shared-lib/castle_primitives.js', import.meta.url), 'castle primitives module');
const materialsSource = await requiredText(new URL(
  '../shared-lib/castle_materials.js', import.meta.url), 'castle materials module');
const furnishingsSourceRaw = await requiredText(new URL(
  '../shared-lib/castle_furnishings.js', import.meta.url), 'castle furnishings module');

const primitivesUrl = dataUrl(primitivesSource);
const materialsUrl = dataUrl(materialsSource);

let furnishingsSource = furnishingsSourceRaw.replace(
  "'shared-lib/castle_primitives'", JSON.stringify(primitivesUrl));
assert.notEqual(furnishingsSource, furnishingsSourceRaw,
  'castle_primitives specifier must be present to rewrite');
const afterPrimitives = furnishingsSource;
furnishingsSource = furnishingsSource.replace(
  "'shared-lib/castle_materials'", JSON.stringify(materialsUrl));
assert.notEqual(furnishingsSource, afterPrimitives,
  'castle_materials specifier must be present to rewrite');
const furnishingsUrl = dataUrl(furnishingsSource);

const F = await import(furnishingsUrl);
const CM = await import(materialsUrl);

const {
  FURNISHING_KINDS, FURNISHING_MODULES, FURNISHING_DEFAULTS, FIXTURE_GLOW_DEFAULTS,
  FURNISHING_KIND_ALIASES, FURNISHING_MATERIAL_SPECS, MEMBER_VARIANTS, FIXTURE_CODES,
  tableParams, benchParams, chairParams, bedParams, chestParams, cupboardParams,
  altarParams, sconceParams, chandelierParams, barrelParams, windowParams,
  furnishingParams, furnishingChildren, emitFurnishing, emitFixtureGlow,
  furnishingPlacement, furnishingPlacements, fixtureFlamePoints, glowParams,
  transformPoint, transformDirection, recordFromManifest, defineFurnishingMaterials,
  furnishingMaterialParams,
} = F;

// ---------------------------------------------------------------------------
// Row-major 4x4 matrix helpers (World.roots transform layout: row i occupies
// m[4i .. 4i+3], m[4i+3] is that row's translation term).

function mat4Identity() { return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]; }

function mat4Multiply(a, b) {
  const r = new Array(16);
  for (let i = 0; i < 4; ++i) {
    for (let j = 0; j < 4; ++j) {
      let s = 0;
      for (let k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = s;
    }
  }
  return r;
}

function mat4Translate(x, y, z) { return [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1]; }
function mat4Scale(x, y, z) { return [x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1]; }
function mat4RotateX(t) {
  const c = Math.cos(t), s = Math.sin(t);
  return [1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0, 0, 0, 0, 1];
}
function mat4RotateY(t) {
  const c = Math.cos(t), s = Math.sin(t);
  return [c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1];
}
function mat4RotateZ(t) {
  const c = Math.cos(t), s = Math.sin(t);
  return [c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}

// ---------------------------------------------------------------------------
// RecordingPart: a fake engine Part that records every op (kind, args,
// material, and a snapshot of the current world matrix) instead of building
// real geometry, so tests can assert on the emitted CSG stream and replay
// world-space transforms.

function numbers(value, out = []) {
  if (typeof value === 'number') out.push(value);
  else if (Array.isArray(value)) for (const child of value) numbers(child, out);
  else if (value && typeof value === 'object')
    for (const child of Object.values(value)) numbers(child, out);
  return out;
}

class RecordingPart {
  constructor(initialMatrix) {
    this.ops = [];
    this.placements = [];
    this.rayTracedCalls = [];
    this.material = -1;
    this.stack = [initialMatrix ? initialMatrix.slice() : mat4Identity()];
    this.voxelDepth = 0;
    this.modifierDepth = 0;
    this.lastGeometry = null;
  }

  get matrix() { return this.stack[this.stack.length - 1]; }

  record(kind, args = []) {
    assert.ok(numbers(args).every(Number.isFinite), kind + ' received non-finite geometry');
    const op = { kind, args, material: this.material, matrix: this.matrix.slice(), csg: 'union' };
    this.ops.push(op);
    return op;
  }

  applyTop(delta) { this.stack[this.stack.length - 1] = mat4Multiply(this.matrix, delta); }

  pushMatrix() { this.stack.push(this.matrix.slice()); this.record('pushMatrix'); }
  popMatrix() {
    assert.ok(this.stack.length > 1, 'matrix stack underflow');
    this.record('popMatrix');
    this.stack.pop();
  }
  translate(x, y, z) { this.applyTop(mat4Translate(x, y, z)); this.record('translate', [x, y, z]); }
  scale(x, y, z) { this.applyTop(mat4Scale(x, y, z)); this.record('scale', [x, y, z]); }
  rotateX(v) { this.applyTop(mat4RotateX(v)); this.record('rotateX', [v]); }
  rotateY(v) { this.applyTop(mat4RotateY(v)); this.record('rotateY', [v]); }
  rotateZ(v) { this.applyTop(mat4RotateZ(v)); this.record('rotateZ', [v]); }
  applyMatrix(m) {
    assert.ok(Array.isArray(m) && m.length === 16, 'applyMatrix requires a 16-element matrix');
    assert.ok(m.every(Number.isFinite), 'applyMatrix requires a finite matrix');
    this.applyTop(m);
    this.record('applyMatrix', m);
  }

  fill(value) { this.material = value; this.record('fill', [value]); }
  smoothing(value) { this.record('smoothing', [value]); }
  box(center, half) { this.lastGeometry = this.record('box', [center, half]); }
  sphere(center, radius) { this.lastGeometry = this.record('sphere', [center, radius]); }
  cylinder(a, b, radius) { this.lastGeometry = this.record('cylinder', [a, b, radius]); }
  cone(a, b, r0, r1) { this.lastGeometry = this.record('cone', [a, b, r0, r1]); }
  capsule(a, b, radius) { this.lastGeometry = this.record('capsule', [a, b, radius]); }
  difference() {
    assert.equal(this.voxelDepth, 1, 'postfix CSG is session-scoped');
    assert.ok(this.lastGeometry, 'postfix difference must follow geometry');
    this.lastGeometry.csg = 'difference';
  }
  smoothingSession() { /* placeholder kept out of API on purpose */ }
  beginVoxels(spacing) {
    assert.ok(spacing > 0, 'voxel spacing must be positive');
    ++this.voxelDepth;
    this.lastGeometry = null;
    this.record('beginVoxels', [spacing]);
  }
  endVoxels() {
    assert.ok(this.voxelDepth-- > 0, 'endVoxels without beginVoxels');
    this.lastGeometry = null;
    this.record('endVoxels');
  }
  beginModifier() { ++this.modifierDepth; this.record('beginModifier'); }
  endModifier(stack) {
    assert.ok(this.modifierDepth-- > 0, 'endModifier without beginModifier');
    this.record('endModifier', stack);
  }
  rayTraced(flag) { this.rayTracedCalls.push(flag); this.record('rayTraced', [flag]); }
  placeChild(module, params, opts) {
    assert.ok(numbers(params).every(Number.isFinite), 'placeChild received non-finite params');
    this.placements.push({ module, params, opts, matrix: this.matrix.slice() });
  }

  balanced() {
    assert.equal(this.stack.length, 1, 'matrix stack balances');
    assert.equal(this.voxelDepth, 0, 'voxel session balances');
    assert.equal(this.modifierDepth, 0, 'modifier stack balances');
  }
}

let areasCovered = 0;
async function area(name, fn) {
  await fn();
  areasCovered += 1;
}

// ===========================================================================
// 1. Kind coverage, determinism, canonical params, garbage clamping.

const EXTRA_KINDS = ['throne', 'cabinet'];
const ALL_TEST_KINDS = [...FURNISHING_KINDS, ...EXTRA_KINDS];

const CANONICAL_FNS = {
  table: tableParams, bench: benchParams, chair: chairParams, bed: bedParams,
  chest: chestParams, cupboard: cupboardParams, altar: altarParams,
  sconce: sconceParams, chandelier: chandelierParams, barrel: barrelParams,
  window: windowParams,
};

const GARBAGE = Object.freeze({
  seed: NaN, length: NaN, width: -50, height: 'nope', topThickness: -1, boards: NaN,
  gold: NaN, depth: -1, throne: NaN, seatHeight: -1, backHeight: -1, railHeight: -1,
  postHeight: -1, canopy: NaN, candles: NaN, cross: NaN, arms: -9, style: 99, reach: -1,
  candleHeight: -1, spot: NaN, spotPitch: 999, spotInner: 999, spotOuter: -1, radius: -1,
  drop: -1, tiers: -1, chains: -1, diameter: -1, staves: -1, hoops: -1, mullions: -1,
  transom: NaN, barWidth: -1, barDepth: -1, glassThickness: -1, thin: NaN, stained: NaN,
  quarry: -1, detail: -1, material: -5, endMaterial: NaN, ironMaterial: 'x',
  goldMaterial: -3, waxMaterial: NaN, flameMaterial: NaN, linenMaterial: NaN,
  woolMaterial: NaN, cushionMaterial: NaN, clothMaterial: NaN, stoneMaterial: NaN,
  mortarMaterial: NaN, traceryMaterial: NaN, glassMaterial: NaN, coloredMaterial: NaN,
  rubyMaterial: NaN, thinGlassMaterial: NaN, cameMaterial: NaN, lightIntensity: -10,
  lightRange: -10, castsShadow: NaN,
});

await area('kinds emit deterministically with defaults and a non-default seed', () => {
  for (const kind of ALL_TEST_KINDS) {
    for (const input of [{}, { seed: 12345 }]) {
      const a = new RecordingPart();
      emitFurnishing(a, kind, input);
      a.balanced();
      const b = new RecordingPart();
      emitFurnishing(b, kind, input);
      b.balanced();
      assert.equal(JSON.stringify(b.ops), JSON.stringify(a.ops),
        `${kind} emission is deterministic for ${JSON.stringify(input)}`);
      assert.ok(a.ops.length > 0, `${kind} emits geometry`);
    }
  }
});

await area('canonical *Params functions and furnishingParams: finite, idempotent, clamp garbage', () => {
  for (const [kind, fn] of Object.entries(CANONICAL_FNS)) {
    const clean = fn({});
    assert.ok(Object.values(clean).every(Number.isFinite), `${kind}Params defaults are finite`);
    assert.deepEqual(fn(clean), clean, `${kind}Params is idempotent on canonical input`);

    const dirty = fn(GARBAGE);
    assert.ok(Object.values(dirty).every(Number.isFinite),
      `${kind}Params clamps garbage input to finite values`);
    assert.deepEqual(fn(dirty), dirty, `${kind}Params is idempotent on clamped garbage`);
  }
  for (const kind of ALL_TEST_KINDS) {
    const clean = furnishingParams(kind, {});
    assert.ok(Object.values(clean).every(Number.isFinite), `furnishingParams(${kind}) defaults finite`);
    assert.deepEqual(furnishingParams(kind, clean), clean, `furnishingParams(${kind}) idempotent`);
    const dirty = furnishingParams(kind, GARBAGE);
    assert.ok(Object.values(dirty).every(Number.isFinite), `furnishingParams(${kind}) clamps garbage`);
    assert.deepEqual(furnishingParams(kind, dirty), dirty, `furnishingParams(${kind}) idempotent on garbage`);
  }
});

await area('unknown furnishing kind throws', () => {
  assert.throws(() => furnishingParams('nonsense', {}), TypeError);
  assert.throws(() => emitFurnishing(new RecordingPart(), 'nonsense', {}), TypeError);
  assert.throws(() => furnishingChildren('nonsense', {}), TypeError);
});

// ===========================================================================
// 2. Child parameter contract.

const MEMBER_KINDS = ['table', 'bench', 'chair', 'bed', 'chest', 'cupboard', 'altar'];

function childSignature(module, params) { return module + '|' + JSON.stringify(params); }

await area('declared children match distinct placeChild calls; modules restricted; unique; seed-periodic', () => {
  const sizeVariants = [
    {},
    { length: 3.4, width: 1.3, depth: 0.7, height: 1.4, backHeight: 1.6, postHeight: 2.3 },
  ];
  for (const kind of MEMBER_KINDS) {
    for (const sizeInput of sizeVariants) {
      for (const seed of [0, 1, 5, 17]) {
        const input = { ...sizeInput, seed };
        const p = furnishingParams(kind, input);
        const declared = furnishingChildren(kind, p);

        const declaredSigs = declared.map((c) => childSignature(c.module, c.params));
        assert.equal(new Set(declaredSigs).size, declaredSigs.length,
          `${kind} furnishingChildren has no duplicates`);
        for (const c of declared)
          assert.ok(['CastleBeam', 'CastlePlank', 'CastleStone'].includes(c.module),
            `${kind} child module is a primitive kit module`);

        const part = new RecordingPart();
        emitFurnishing(part, kind, p);
        part.balanced();
        for (const pl of part.placements)
          assert.ok(['CastleBeam', 'CastlePlank', 'CastleStone'].includes(pl.module),
            `${kind} placeChild module is a primitive kit module`);

        const placedSigSet = new Set(part.placements.map((pl) => childSignature(pl.module, pl.params)));
        assert.deepEqual([...placedSigSet].sort(), declaredSigs.slice().sort(),
          `${kind} declared children match distinct placeChild calls (${JSON.stringify(input)})`);
      }
    }
  }
});

await area('member seeds repeat every MEMBER_VARIANTS', () => {
  assert.equal(MEMBER_VARIANTS, 2);
  for (const kind of MEMBER_KINDS) {
    const base = furnishingChildren(kind, furnishingParams(kind, { seed: 3 }));
    for (const multiple of [1, 3]) {
      const other = furnishingChildren(kind, furnishingParams(kind, { seed: 3 + multiple * MEMBER_VARIANTS }));
      assert.deepEqual(other, base, `${kind} children identical for seeds 3 mod ${MEMBER_VARIANTS} apart`);
    }
  }
});

// ===========================================================================
// 3. Light/geometry agreement under transforms.

const FLAME_LIFT = 0.024;

const TRANSFORM_RECORDS = [
  { id: 's1', kind: 'sconce', x: 1, y: 2, z: -3, yaw: 0, arms: 1, style: 0, seed: 5 },
  { id: 's2', kind: 'sconce', x: -2, y: 0.5, z: 4, yaw: 0.65, arms: 2, style: 0, seed: 9 },
  { id: 's3', kind: 'sconce', x: 0, y: 1, z: 0, yaw: Math.PI / 2, arms: 1, style: 1, spot: 1, seed: 2 },
  { id: 's4', kind: 'sconce', x: -1, y: -1, z: 2, yaw: Math.PI, arms: 2, style: 1, spot: 1, seed: 7 },
  { id: 's5', kind: 'sconce', x: 2, y: 3, z: -1, yaw: -2.1, arms: 1, style: 0, seed: 1 },
  { id: 's6', kind: 'sconce', x: 1, y: -1, z: 1, quarterTurns: 3, arms: 2, style: 0, seed: 4 },
  { id: 'c1', kind: 'chandelier', x: 0, y: 5, z: 0, yaw: 0, tiers: 1, seed: 3 },
  { id: 'c2', kind: 'chandelier', x: -3, y: 4, z: 2, yaw: 0.65, tiers: 2, seed: 6 },
  { id: 'c3', kind: 'chandelier', x: 1, y: 2, z: -2, yaw: Math.PI / 2, tiers: 1, seed: 8 },
  { id: 'c4', kind: 'chandelier', x: 0, y: -1, z: 0, yaw: Math.PI, tiers: 2, seed: 0 },
  { id: 'c5', kind: 'chandelier', x: -1, y: -2, z: 3, yaw: -2.1, tiers: 1, seed: 11 },
  { id: 'c6', kind: 'chandelier', x: 2, y: 1, z: -1, quarterTurns: 1, tiers: 2, seed: 12 },
  { id: 'a1', kind: 'altar', x: 0, y: 0, z: 0, yaw: 0, candles: 1, seed: 1 },
  { id: 'a2', kind: 'altar', x: -2, y: 1, z: -3, yaw: 0.65, candles: 1, seed: 2 },
  { id: 'a3', kind: 'altar', x: 1, y: -1, z: 2, yaw: Math.PI / 2, candles: 1, seed: 3 },
  { id: 'a4', kind: 'altar', x: 0, y: 2, z: 0, yaw: Math.PI, candles: 1, seed: 4 },
  { id: 'a5', kind: 'altar', x: 3, y: 0, z: -1, yaw: -2.1, candles: 1, seed: 5 },
  { id: 'a6', kind: 'altar', x: -1, y: 0, z: 1, quarterTurns: 2, candles: 1, seed: 6 },
];

await area('flame centres, candle wicks and spot directions agree between glow/body replay and lights', () => {
  for (const record of TRANSFORM_RECORDS) {
    const placement = furnishingPlacement(record);
    assert.equal(placement.roots.length, 2, `${record.id} has a body root and a glow root`);
    const [bodyRoot, glowRoot] = placement.roots;
    assert.equal(glowRoot.module, 'CastleFixtureGlow');
    assert.deepEqual(glowRoot.transform, bodyRoot.transform,
      `${record.id} glow and body roots share the identical transform`);

    const bodyPart = new RecordingPart(bodyRoot.transform);
    emitFurnishing(bodyPart, placement.kind, bodyRoot.params);
    bodyPart.balanced();
    assert.equal(bodyPart.rayTracedCalls.length, 0, `${record.id} body never calls rayTraced`);

    const glowPart = new RecordingPart(glowRoot.transform);
    emitFixtureGlow(glowPart, glowRoot.params);
    glowPart.balanced();
    assert.deepEqual(glowPart.rayTracedCalls, [false], `${record.id} glow calls rayTraced(false)`);

    // Flame centres: the sphere emitted at local origin under translate+scale.
    const glowSpheres = glowPart.ops.filter((op) => op.kind === 'sphere');
    assert.equal(glowSpheres.length, placement.lights.points.length,
      `${record.id} one glow sphere per light point`);
    glowSpheres.forEach((op, i) => {
      const world = transformPoint(op.matrix, [0, 0, 0]);
      const expected = placement.lights.points[i].position;
      for (let axis = 0; axis < 3; ++axis)
        assert.ok(Math.abs(world[axis] - expected[axis]) < 1e-9,
          `${record.id} flame ${i} axis ${axis} world position matches light`);
    });

    // Candle wicks: thin (radius 0.0025) cylinders whose "a" endpoint sits at
    // local [flameX, flameY - FLAME_LIFT, flameZ]; verify they lie directly
    // below their flame in world space (same X/Z, strictly lower Y).
    const localFlames = fixtureFlamePoints(placement.kind, bodyRoot.params);
    const wickOps = bodyPart.ops.filter((op) => op.kind === 'cylinder' && Math.abs(op.args[2] - 0.0025) < 1e-9);
    assert.equal(wickOps.length, localFlames.length, `${record.id} one wick per flame`);
    localFlames.forEach((flame, i) => {
      const wick = wickOps[i];
      const a = wick.args[0];
      assert.ok(Math.abs(a[0] - flame[0]) < 1e-9 && Math.abs(a[2] - flame[2]) < 1e-9 &&
        Math.abs(a[1] - (flame[1] - FLAME_LIFT)) < 1e-9,
        `${record.id} wick ${i} local base sits at flame - FLAME_LIFT`);
      const worldWick = transformPoint(wick.matrix, a);
      const worldFlame = placement.lights.points[i].position;
      assert.ok(Math.abs(worldWick[0] - worldFlame[0]) < 1e-9, `${record.id} wick ${i} world X below flame`);
      assert.ok(Math.abs(worldWick[2] - worldFlame[2]) < 1e-9, `${record.id} wick ${i} world Z below flame`);
      assert.ok(worldFlame[1] - worldWick[1] > 0, `${record.id} wick ${i} world Y strictly below flame`);
    });

    // Spot directions: sconces with spot:1 report a spot per point light.
    if (placement.kind === 'sconce' && bodyRoot.params.spot) {
      assert.ok(placement.lights.spots.length > 0, `${record.id} has spot lights`);
      const pitch = bodyRoot.params.spotPitch * Math.PI / 180;
      const localDir = [0, -Math.sin(pitch), Math.cos(pitch)];
      const expectedDir = transformDirection(bodyRoot.transform, localDir);
      for (const spot of placement.lights.spots) {
        for (let axis = 0; axis < 3; ++axis)
          assert.ok(Math.abs(spot.direction[axis] - expectedDir[axis]) < 1e-9,
            `${record.id} spot direction matches transformDirection`);
        const len = Math.hypot(...spot.direction);
        assert.ok(Math.abs(len - 1) < 1e-9, `${record.id} spot direction is unit length`);
        assert.ok(spot.inner <= spot.outer, `${record.id} spot inner <= outer`);
      }
    } else {
      assert.equal(placement.lights.spots.length, 0, `${record.id} no spots without spot:1`);
    }
  }
});

// ===========================================================================
// 4. Gold / glass / iron detail.

await area('gold:1 (and throne) kinds emit geometry filled with goldMaterial', () => {
  const cases = [
    ['table', { gold: 1 }], ['chair', { throne: 1 }], ['bed', { gold: 1 }],
    ['chest', { gold: 1 }], ['cupboard', { gold: 1 }], ['altar', {}],
  ];
  for (const [kind, extra] of cases) {
    const p = furnishingParams(kind, extra);
    const part = new RecordingPart();
    emitFurnishing(part, kind, p);
    part.balanced();
    const geomKinds = new Set(['box', 'sphere', 'cylinder', 'cone', 'capsule']);
    assert.ok(part.ops.some((op) => geomKinds.has(op.kind) && op.material === p.goldMaterial),
      `${kind} emits gold-filled geometry`);
  }
});

await area('defineFurnishingMaterials declares castle(16) + furnishing(8) materials', () => {
  const calls = [];
  let nextId = 5000;
  globalThis.defineMaterial = (name, spec) => { calls.push({ name, spec }); return nextId++; };
  let materials;
  try {
    materials = defineFurnishingMaterials('CastleTest');
  } finally {
    delete globalThis.defineMaterial;
  }
  assert.equal(calls.length, 24, 'castle(16) + furnishing(8) = 24 defineMaterial calls');
  for (const key of ['wax', 'flame', 'linen', 'wool', 'velvet', 'lead', 'thinGlass', 'rubyGlass'])
    assert.ok(Number.isInteger(materials[key]), `materials.${key} is a handle`);

  assert.equal(FURNISHING_MATERIAL_SPECS.thinGlass.thinWalled, true);
  assert.equal(FURNISHING_MATERIAL_SPECS.rubyGlass.volumeBoundary, true);
  assert.ok(FURNISHING_MATERIAL_SPECS.flame.emission > 0);
  assert.equal(CM.CASTLE_MATERIAL_SPECS.gold.metallic, 1, 'castle gold is metallic');

  // Window glazing: closed glass boxes with true thickness, cames wider than
  // the glass, tracery on its own material.
  const matParams = furnishingMaterialParams(materials);
  const winParams = furnishingParams('window',
    { ...matParams, width: 1.4, height: 2.6, mullions: 1, thin: 0, stained: 0 });
  const winPart = new RecordingPart();
  emitFurnishing(winPart, 'window', winParams);
  winPart.balanced();

  const glassOps = winPart.ops.filter((op) => op.kind === 'box' && op.material === winParams.glassMaterial);
  assert.ok(glassOps.length > 0, 'window emits glass panes');
  for (const op of glassOps) {
    const halfZ = op.args[1][2];
    assert.ok(halfZ > 0, 'glass pane has positive thickness');
    assert.ok(Math.abs(halfZ - winParams.glassThickness / 2) < 1e-9,
      'glass pane half-z equals glassThickness/2');
  }
  const cameOps = winPart.ops.filter((op) => op.kind === 'box' && op.material === winParams.cameMaterial);
  assert.ok(cameOps.length > 0, 'window emits cames');
  assert.ok(cameOps.some((op) => op.args[1][2] > winParams.glassThickness / 2),
    'came half-z exceeds glass half-z');
  const traceryOps = winPart.ops.filter((op) => op.material === winParams.traceryMaterial);
  assert.ok(traceryOps.length > 0, 'window emits tracery on traceryMaterial');

  // thin:1 forces thinGlassMaterial and a fixed 0.004 m thickness.
  const thinParams = furnishingParams('window',
    { ...matParams, width: 1.4, height: 2.6, mullions: 1, thin: 1 });
  const thinPart = new RecordingPart();
  emitFurnishing(thinPart, 'window', thinParams);
  thinPart.balanced();
  const thinGlassOps = thinPart.ops.filter((op) => op.kind === 'box' && op.material === thinParams.thinGlassMaterial);
  assert.ok(thinGlassOps.length > 0, 'thin:1 window uses thinGlassMaterial');
  for (const op of thinGlassOps)
    assert.ok(Math.abs(op.args[1][2] - 0.002) < 1e-9, 'thin:1 pane half-z is 0.002 (0.004 thickness)');

  // Sconce style 1 (lantern) emits glass panes.
  const sconceParamsStyle1 = furnishingParams('sconce', { ...matParams, style: 1 });
  const sconcePart = new RecordingPart();
  emitFurnishing(sconcePart, 'sconce', sconceParamsStyle1);
  sconcePart.balanced();
  assert.ok(sconcePart.ops.some((op) => op.kind === 'box' && op.material === sconceParamsStyle1.glassMaterial),
    'sconce style 1 emits glass panes');
});

// ===========================================================================
// 5. Placement metadata.

const FLOOR_KINDS = ['table', 'bench', 'chair', 'bed', 'chest', 'cupboard', 'altar'];

await area('floor-kind clearance AABB contains footprint; clearance corners form a rotated rectangle', () => {
  for (const kind of FLOOR_KINDS) {
    const record = { id: kind + '-clear', kind, x: 2, y: 0, z: -1, yaw: 0.4, seed: 5 };
    const placement = furnishingPlacement(record);
    const fa = placement.footprint.aabb, ca = placement.clearance.aabb;
    assert.ok(ca.minX <= fa.minX + 1e-9 && ca.maxX >= fa.maxX - 1e-9 &&
      ca.minZ <= fa.minZ + 1e-9 && ca.maxZ >= fa.maxZ - 1e-9,
      `${kind} clearance AABB contains footprint AABB in XZ`);
    assert.ok(Math.abs(ca.minY - fa.minY) < 1e-9 && Math.abs(ca.maxY - fa.maxY) < 1e-9,
      `${kind} clearance AABB matches footprint AABB in Y`);

    const corners = placement.clearance.corners;
    assert.equal(corners.length, 4);
    const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);
    const d01 = dist(corners[0], corners[1]), d12 = dist(corners[1], corners[2]);
    const d23 = dist(corners[2], corners[3]), d30 = dist(corners[3], corners[0]);
    const diag02 = dist(corners[0], corners[2]), diag13 = dist(corners[1], corners[3]);
    assert.ok(Math.abs(d01 - d23) < 1e-9 && Math.abs(d12 - d30) < 1e-9,
      `${kind} clearance corners: opposite sides equal (still a rectangle after rotation)`);
    assert.ok(Math.abs(diag02 - diag13) < 1e-9, `${kind} clearance corners: diagonals equal`);
    const local = placement.clearance.local;
    assert.ok(Math.abs(d01 - (local.maxX - local.minX)) < 1e-9,
      `${kind} clearance width preserved under yaw rotation`);
    assert.ok(Math.abs(d12 - (local.maxZ - local.minZ)) < 1e-9,
      `${kind} clearance depth preserved under yaw rotation`);
    assert.equal(placement.headroom, undefined, `${kind} is a floor fixture: no headroom metadata`);
  }
});

await area('footprint AABB swaps width/length under a 90 degree yaw', () => {
  const rec0 = { id: 't0', kind: 'table', x: 0, y: 0, z: 0, yaw: 0, length: 3, width: 1 };
  const rec90 = { id: 't90', kind: 'table', x: 0, y: 0, z: 0, yaw: Math.PI / 2, length: 3, width: 1 };
  const pl0 = furnishingPlacement(rec0), pl90 = furnishingPlacement(rec90);
  const spanX = (a) => a.maxX - a.minX, spanZ = (a) => a.maxZ - a.minZ;
  assert.ok(Math.abs(spanX(pl0.footprint.aabb) - 3) < 1e-9);
  assert.ok(Math.abs(spanZ(pl0.footprint.aabb) - 1) < 1e-9);
  assert.ok(Math.abs(spanX(pl90.footprint.aabb) - 1) < 1e-9, 'yaw 90 swaps X span to width');
  assert.ok(Math.abs(spanZ(pl90.footprint.aabb) - 3) < 1e-9, 'yaw 90 swaps Z span to length');
});

await area('wall/ceiling kinds report headroom; floorY computes clear against 2.1 m', () => {
  for (const kind of ['sconce', 'chandelier']) {
    const noFloor = furnishingPlacement({ id: kind + '-nofloor', kind, x: 0, y: 2.5, z: 0, yaw: 0 });
    assert.ok(noFloor.headroom, `${kind} reports headroom`);
    assert.equal(noFloor.headroom.floorY, null);
    assert.equal(noFloor.headroom.clear, null);
    assert.equal(noFloor.headroom.required, 2.1);

    const withFloor = furnishingPlacement({ id: kind + '-floor', kind, x: 0, y: 2.5, z: 0, yaw: 0, floorY: 0 });
    assert.equal(withFloor.headroom.floorY, 0);
    const expectedClear = withFloor.headroom.bottomY - 0 >= 2.1;
    assert.equal(withFloor.headroom.clear, expectedClear);

    const tooLow = furnishingPlacement({ id: kind + '-low', kind, x: 0, y: 0.3, z: 0, yaw: 0, floorY: 0 });
    assert.equal(tooLow.headroom.clear, tooLow.headroom.bottomY >= 2.1);
  }
});

await area('furnishingPlacements rejects duplicate ids and concatenates in record order', () => {
  assert.throws(() => furnishingPlacements([
    { id: 'dup', kind: 'table' }, { id: 'dup', kind: 'bench' },
  ]), /duplicate/i);

  const records = [
    { id: 'r1', kind: 'table', x: 0, y: 0, z: 0 },
    { id: 'r2', kind: 'sconce', x: 1, y: 1, z: 0 },
  ];
  const combined = furnishingPlacements(records);
  const p1 = furnishingPlacement(records[0]), p2 = furnishingPlacement(records[1]);
  assert.equal(combined.roots.length, p1.roots.length + p2.roots.length);
  assert.deepEqual(combined.roots, [...p1.roots, ...p2.roots], 'roots concatenate in record order');
  assert.equal(combined.lights.points.length, p1.lights.points.length + p2.lights.points.length);
  assert.deepEqual(combined.lights.points, [...p1.lights.points, ...p2.lights.points]);
  assert.deepEqual(combined.lights.spots, [...p1.lights.spots, ...p2.lights.spots]);
});

// ===========================================================================
// 6. recordFromManifest.

function localQuantized(value) {
  return typeof value === 'number' && Number.isFinite(value) && value > 0
    ? Math.round(value / 0.05) * 0.05 : undefined;
}

await area('recordFromManifest maps a plan-manifest fixture to a furnishing record', () => {
  const cabinetFixture = {
    id: 'fx1', position: [1, 2, 3], yaw: 90, kind: 'cabinet',
    width: 1.234, depth: 0.567, height: 1.89, seed: 7,
  };
  const rec = recordFromManifest(cabinetFixture);
  assert.equal(rec.kind, 'cupboard', 'alias "cabinet" maps to kind cupboard');
  assert.ok(Math.abs(rec.yaw - Math.PI / 2) < 1e-12, 'yaw converts degrees to radians');
  assert.ok(Math.abs(rec.width - localQuantized(1.234)) < 1e-9, 'width quantized to 0.05');
  assert.ok(Math.abs(rec.depth - localQuantized(0.567)) < 1e-9, 'depth quantized to 0.05');
  assert.ok(Math.abs(rec.height - localQuantized(1.89)) < 1e-9, 'height quantized to 0.05');
  const placement = furnishingPlacement(rec);
  assert.equal(placement.module, 'CastleCupboard', 'converted record places via furnishingPlacement');

  const tableFixture = {
    id: 'fx2', position: [0, 0, 0], yaw: 0, kind: 'table',
    width: 2.02, depth: 0.87, height: 0.8, seed: 1,
  };
  const tableRecord = recordFromManifest(tableFixture);
  assert.equal(tableRecord.kind, 'table');
  assert.ok(Math.abs(tableRecord.length - localQuantized(2.02)) < 1e-9, 'table width -> length');
  assert.ok(Math.abs(tableRecord.width - localQuantized(0.87)) < 1e-9, 'table depth -> width');
  assert.doesNotThrow(() => furnishingPlacement(tableRecord));

  assert.throws(() => recordFromManifest({ kind: 'nonsense' }), TypeError, 'unknown kind throws');
});

await area('recordFromManifest against a real courtyard manifest, if present', async () => {
  const manifestPath = '/mnt/d/Shared With Desktop/AI/matter-engine-cpp/build/qa/castle-grid/courtyard-detailed-manifest.json';
  let raw;
  try {
    raw = await readFile(manifestPath, 'utf8');
  } catch {
    console.log('  (skip) courtyard-detailed-manifest.json not present; skipping manifest replay');
    return;
  }
  const doc = JSON.parse(raw);
  function findFixtures(node) {
    if (!node || typeof node !== 'object') return null;
    if (Array.isArray(node.fixtures)) return node.fixtures;
    for (const value of Object.values(node)) {
      if (Array.isArray(value)) continue;
      const found = findFixtures(value);
      if (found) return found;
    }
    return null;
  }
  const fixtures = findFixtures(doc);
  assert.ok(Array.isArray(fixtures), 'manifest has a fixtures array somewhere');
  let count = 0;
  for (const fixture of fixtures) {
    const record = recordFromManifest(fixture);
    const placement = furnishingPlacement(record);
    assert.ok(placement && placement.module, `manifest fixture ${fixture.id ?? count} converts and places`);
    ++count;
  }
  console.log(`  manifest replay: ${count} fixtures converted and placed`);
});

// ===========================================================================
// 7. Wrapper Parts under projects/world_demo/objects/.

const WRAPPER_KINDS = [
  ['CastleTable', 'table'], ['CastleBench', 'bench'], ['CastleChair', 'chair'],
  ['CastleBed', 'bed'], ['CastleChest', 'chest'], ['CastleCupboard', 'cupboard'],
  ['CastleAltar', 'altar'], ['CastleBarrel', 'barrel'], ['CastleSconce', 'sconce'],
  ['CastleChandelier', 'chandelier'], ['CastleWindowGlazing', 'window'],
];

let wrapperImportCounter = 0;
async function loadWrapperClass(className) {
  const source = await requiredText(new URL(`../objects/${className}.js`, import.meta.url), className);
  const rewritten = source.replace("'shared-lib/castle_furnishings'", JSON.stringify(furnishingsUrl));
  assert.notEqual(rewritten, source, `${className} imports shared-lib/castle_furnishings`);
  const withExport = `${rewritten}\nexport default ${className};\n// ${wrapperImportCounter++}\n`;
  const mod = await import(dataUrl(withExport));
  return mod.default;
}

await area('wrapper Parts: params superset, requires() matches furnishingChildren, build() balances', async () => {
  globalThis.Part = RecordingPart;
  try {
    for (const [className, kind] of WRAPPER_KINDS) {
      const ClassRef = await loadWrapperClass(className);
      const defaults = FURNISHING_DEFAULTS[kind];
      for (const key of Object.keys(defaults))
        assert.ok(key in ClassRef.params, `${className}.params has key ${key}`);

      if (typeof ClassRef.requires === 'function') {
        const declared = ClassRef.requires(ClassRef.params);
        const expected = furnishingChildren(kind, ClassRef.params);
        assert.deepEqual(declared, expected, `${className}.requires matches furnishingChildren('${kind}')`);
      }

      const instance = new ClassRef();
      instance.build(ClassRef.params);
      instance.balanced();
      assert.ok(instance.ops.length > 0, `${className}.build emits geometry`);
    }

    const glowClassRef = await loadWrapperClass('CastleFixtureGlow');
    for (const key of Object.keys(FIXTURE_GLOW_DEFAULTS))
      assert.ok(key in glowClassRef.params, `CastleFixtureGlow.params has key ${key}`);
    assert.equal(typeof glowClassRef.requires, 'undefined', 'CastleFixtureGlow has no static requires');
    const glowInstance = new glowClassRef();
    glowInstance.build(glowClassRef.params);
    glowInstance.balanced();
    assert.deepEqual(glowInstance.rayTracedCalls, [false], 'CastleFixtureGlow.build calls rayTraced(false)');
  } finally {
    delete globalThis.Part;
  }
});

// ===========================================================================

console.log(`castle furnishings: PASS - ${areasCovered} assertion areas covered ` +
  `(kinds/determinism, child contract, transform agreement, materials, ` +
  `placement metadata, manifest mapping, wrapper Parts)`);
