import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

globalThis.World = class {};
globalThis.Part = class {};
globalThis.MAT = { bark: 101, plaster: 202 };
const materialCalls = [];
globalThis.defineMaterial = (name, spec) => {
  materialCalls.push({ name, spec });
  return 303;
};

const collisionCalls = [];
globalThis.terrainCollision = settings => {
  const call = { settings, regions: [], builds: 0, operations: [] };
  collisionCalls.push(call);
  return {
    region: (id, bounds) => {
      call.operations.push('region');
      call.regions.push({ id, bounds });
    },
    build: () => {
      call.operations.push('build');
      call.builds += 1;
    },
  };
};

async function requiredText(url, label) {
  try {
    return await readFile(url, 'utf8');
  } catch (error) {
    assert.fail(`${label} must exist: ${error.message}`);
  }
}

function reconstructedTransform(local) {
  const [x, y, z, w] = local.rotation;
  return [
    1 - 2 * (y * y + z * z), 2 * (x * y - z * w),
    2 * (x * z + y * w), local.translation[0],
    2 * (x * y + z * w), 1 - 2 * (x * x + z * z),
    2 * (y * z - x * w), local.translation[1],
    2 * (x * z - y * w), 2 * (y * z + x * w),
    1 - 2 * (x * x + y * y), local.translation[2],
    0, 0, 0, 1,
  ];
}

function assertArrayNear(actual, expected, tolerance, label) {
  assert.equal(actual.length, expected.length, `${label} length`);
  for (let i = 0; i < expected.length; ++i) {
    assert.ok(Math.abs(actual[i] - expected[i]) <= tolerance,
      `${label}[${i}] expected ${expected[i]}, got ${actual[i]}`);
  }
}

const riverCurveSource = await requiredText(new URL(
  '../../../MatterEngine3/shared-lib/river_curve.js', import.meta.url),
  'river curve module');
const riverCurveUrl = `data:text/javascript;base64,${
  Buffer.from(riverCurveSource).toString('base64')}`;
const sharedSource = (await requiredText(new URL(
  '../shared-lib/river_hydrology_definition.js', import.meta.url),
  'shared river hydrology definition')).replace(
  "'shared-lib/river_curve'", JSON.stringify(riverCurveUrl));
const sharedUrl = `data:text/javascript;base64,${
  Buffer.from(sharedSource).toString('base64')}`;
const shared = await import(sharedUrl);

const sceneSource = (await requiredText(new URL(
  '../scenes/RiverFloatLab/RiverFloatLab.js', import.meta.url),
  'RiverFloatLab scene')).replace(
  "'shared-lib/river_hydrology_definition'", JSON.stringify(sharedUrl)) +
  '\nexport { RiverFloatLab };';
const sceneUrl = `data:text/javascript;base64,${
  Buffer.from(sceneSource).toString('base64')}`;
const riverFloatLab = await import(sceneUrl);

assert.deepEqual(materialCalls, [{
  name: 'RiverFloatLabWater',
  spec: {
    albedo: [0.05, 0.14, 0.18], roughness: 0.06,
    transmission: 0.98, ior: 1.333,
    volumeBoundary: true, waterSurface: true,
  },
}], 'RiverFloatLab authors one dedicated water-domain material');

const scene = new riverFloatLab.RiverFloatLab();
scene.collision();
assert.equal(collisionCalls.length, 1,
  'RiverFloatLab authors exactly one terrain-collision builder');
assert.deepEqual(collisionCalls[0].settings, {
  cellSize: 0.5,
  friction: 0.72,
  restitution: 0.02,
}, 'RiverFloatLab uses the accepted terrain-collision resolution and material');
assert.deepEqual(collisionCalls[0].regions, [{
  id: 'river-gameplay',
  bounds: {
    min: [-64, -64, -64],
    max: [384, 128, 64],
  },
}], 'RiverFloatLab bounds collision to the sector-aligned ravine union');
assert.equal(collisionCalls[0].builds, 1,
  'RiverFloatLab completes its terrain-collision builder exactly once');
assert.deepEqual(collisionCalls[0].operations, ['region', 'build'],
  'RiverFloatLab authors its region before completing the builder');

assert.equal(riverFloatLab.buildRiverHydrologyDefinition,
  shared.buildRiverHydrologyDefinition,
  'RiverFloatLab consumes the same pure definition as RiverHydrology');
assert.equal(riverFloatLab.authorRiverHydrologyNetwork,
  shared.authorRiverHydrologyNetwork,
  'RiverFloatLab consumes the same accepted network inputs as RiverHydrology');
assert.equal(riverFloatLab.buildRiverHydrologyField,
  shared.buildRiverHydrologyField,
  'RiverFloatLab consumes the same accepted terrain inputs as RiverHydrology');

const definition = riverFloatLab.buildRiverFloatLabDefinition(0x12345678);
const accepted = shared.buildRiverHydrologyDefinition(0x12345678);
assert.deepEqual(definition.river, accepted,
  'RiverFloatLab retains the accepted curve, profiles, sections, features, and roots');
assert.deepEqual(definition.roots, accepted.roots,
  'RiverFloatLab renders the exact accepted boulder roots');

const ids = definition.entities.map(entity => entity.id);
assert.equal(new Set(ids).size, ids.length, 'all scene entity ids are stable and unique');
const dynamicBodies = definition.entities.filter(entity =>
  entity.components.RigidBody?.type === 'dynamic');
assert.ok(dynamicBodies.length >= 24, 'RiverFloatLab authors at least 24 dynamic bodies');
for (const body of dynamicBodies) {
  for (const component of [
    'LocalTransform', 'PartInstance', 'RigidBody', 'BoxCollider', 'RiverFloatBody',
  ]) {
    assert.ok(body.components[component], `${body.id} carries ${component}`);
  }
  assert.equal(body.components.RigidBody.gravityScale, 1,
    `${body.id} retains ordinary gravity`);
  assert.equal(body.components.BoxCollider.density,
    body.components.RiverFloatBody.effectiveDensityKgM3,
    `${body.id} Box3D mass density matches its float-force density`);
  assert.ok(body.components.LocalTransform.translation.every(Number.isFinite),
    `${body.id} starts at a finite authored position`);
}

const referenceCrate = dynamicBodies.find(body => body.id === 'reference-crate');
const referenceRaft = dynamicBodies.find(body => body.id === 'reference-raft');
assert.ok(referenceCrate, 'reference-crate exists');
assert.ok(referenceRaft, 'reference-raft exists');
assert.equal(referenceCrate.components.PartInstance.part, 'RiverCrate');
assert.deepEqual(referenceCrate.components.BoxCollider.halfExtents, [0.75, 0.75, 0.75]);
assert.equal(referenceCrate.components.RiverFloatBody.effectiveDensityKgM3, 620);
assert.deepEqual([
  referenceCrate.components.RiverFloatBody.probesX,
  referenceCrate.components.RiverFloatBody.probesY,
  referenceCrate.components.RiverFloatBody.probesZ,
], [2, 2, 2]);
assert.equal(referenceCrate.components.RiverFloatBody.probeInset, 0.075,
  'the smaller crate halves the probe inset with its linear scale');
assert.equal(referenceCrate.components.RiverFloatBody.maxForcePerProbeN, 5250,
  'the smaller crate scales its per-probe force cap by displaced volume');
assert.equal(referenceCrate.components.RiverFloatBody.maxTotalForceN, 35000,
  'the smaller crate scales its total force cap by displaced volume');
assert.equal(referenceRaft.components.PartInstance.part, 'RiverRaft');
assert.deepEqual(referenceRaft.components.BoxCollider.halfExtents, [2.4, 0.35, 1.5]);
assert.equal(referenceRaft.components.RiverFloatBody.effectiveDensityKgM3, 420);
assert.deepEqual([
  referenceRaft.components.RiverFloatBody.probesX,
  referenceRaft.components.RiverFloatBody.probesY,
  referenceRaft.components.RiverFloatBody.probesZ,
], [3, 2, 3]);
assert.equal(referenceRaft.components.RiverFloatBody.probeInset, 0.15,
  'raft probe inset remains unchanged');
assert.equal(referenceRaft.components.RiverFloatBody.maxForcePerProbeN, 24000,
  'raft per-probe force cap remains unchanged');
assert.equal(referenceRaft.components.RiverFloatBody.maxTotalForceN, 150000,
  'raft total force cap remains unchanged');
for (const body of dynamicBodies) {
  const placement = definition.bodyPlacements.find(row => row.id === body.id);
  assert.ok(placement, `${body.id} retains its authored placement recipe`);
  assert.equal(body.components.BoxCollider.density, placement.densityKgM3,
    `${body.id} preserves its authored Box3D density`);
  assert.equal(body.components.RiverFloatBody.effectiveDensityKgM3,
    placement.densityKgM3, `${body.id} preserves its authored float density`);
  const isRaft = placement.part === 'RiverRaft';
  if (isRaft) {
    assert.equal(body.components.PartInstance.part, 'RiverRaft',
      `${body.id} resolves to the scene-local RiverRaft part`);
    assert.deepEqual(body.components.BoxCollider.halfExtents, [2.4, 0.35, 1.5],
      `${body.id} retains the accepted raft collider`);
    continue;
  }
  assert.equal(body.components.PartInstance.part, 'RiverCrate',
    `${body.id} resolves to the scene-local RiverCrate part`);
  assert.deepEqual(body.components.BoxCollider.halfExtents, [0.75, 0.75, 0.75],
    `${body.id} uses the 1.5 m scene-local crate collider`);
  assert.deepEqual([
    body.components.RiverFloatBody.probesX,
    body.components.RiverFloatBody.probesY,
    body.components.RiverFloatBody.probesZ,
  ], [2, 2, 2], `${body.id} retains the minimum stable box probe topology`);
  assert.equal(body.components.RiverFloatBody.probeInset, 0.075,
    `${body.id} scales its probe inset with crate height`);
  assert.equal(body.components.RiverFloatBody.maxForcePerProbeN, 5250,
    `${body.id} scales its per-probe cap with crate volume`);
  assert.equal(body.components.RiverFloatBody.maxTotalForceN, 35000,
    `${body.id} scales its total cap with crate volume`);
}
for (const reference of [referenceCrate, referenceRaft]) {
  assert.equal(reference.components.RigidBody.continuous, true,
    `${reference.id} uses continuous collision`);
  assert.equal(reference.components.RigidBody.enableSleep, false,
    `${reference.id} cannot sleep during the traversal playtest`);
}
const cratePlacement = definition.bodyPlacements.find(row => row.id === 'reference-crate');
const raftPlacement = definition.bodyPlacements.find(row => row.id === 'reference-raft');
assert.equal(cratePlacement.lateralM, 0, 'reference crate starts in the centre lane');
assert.equal(raftPlacement.lateralM, 0, 'reference raft starts in the centre lane');
assert.equal(cratePlacement.riverDistanceM - raftPlacement.riverDistanceM, 10,
  'reference raft starts exactly 10 m behind the reference crate');
const crateLane = shared.sampleRiverHydrologyLane(
  accepted, cratePlacement.riverDistanceM, cratePlacement.lateralM);
const crateSurfaceY = crateLane.position[1] + crateLane.channel.depth;
const expectedCrateY = crateSurfaceY + 0.75 - (620 / 1000) * 1.5;
const oldCrateY = crateSurfaceY + 1.5 - (620 / 1000) * 3;
assert.ok(Math.abs(referenceCrate.components.LocalTransform.translation[1] -
  expectedCrateY) <= 1e-12,
  'reference crate equilibrium is recomputed from its 1.5 m height and density');
assert.notEqual(referenceCrate.components.LocalTransform.translation[1], oldCrateY,
  'reference crate does not retain the old 3 m equilibrium result');

const authoredZones = new Set(definition.bodyPlacements.map(row => row.zone));
for (const zone of ['upper-rapids', 'boulder-wakes', 'waterfall-approach', 'first-spillway']) {
  assert.ok(authoredZones.has(zone), `body variants cover ${zone}`);
}

const colliderEntities = definition.entities.filter(entity =>
  entity.components.RigidBody?.type === 'static' && entity.components.SphereCollider);
assert.equal(colliderEntities.length, accepted.roots.length,
  'every accepted visual boulder has one explicit static collider entity');
for (const root of accepted.roots) {
  const collider = colliderEntities.find(entity =>
    entity.id === `boulder-collider-${root.id}`);
  assert.ok(collider, `${root.id} has an explicit static rigid body collider`);
  assertArrayNear(reconstructedTransform(collider.components.LocalTransform),
    root.transform, 1e-9, `${root.id} collider root transform`);
  assert.deepEqual(collider.components.SphereCollider.center,
    root.fluidCollider.center, `${root.id} collider centre matches fluid collision`);
  assert.equal(collider.components.SphereCollider.radius,
    root.fluidCollider.radius, `${root.id} collider radius matches fluid collision`);
}
assert.ok(accepted.roots.every(root => root.components === undefined),
  'the river generator itself still creates no boulder rigid body');

const sharedCrateSource = await requiredText(new URL(
  '../objects/Crate.js', import.meta.url), 'shared Crate part');
assert.match(sharedCrateSource, /this\.box\(\[0, 0, 0\], \[1\.5, 1\.5, 1\.5\]\)/,
  'the shared Crate remains a centred 3 m box for other worlds');

const riverCrateSource = await requiredText(new URL(
  '../scenes/RiverFloatLab/objects/RiverCrate.js', import.meta.url),
  'RiverCrate part');
assert.match(riverCrateSource, /^class RiverCrate extends Part/m,
  'RiverCrate uses the runtime-discoverable class declaration convention');
assert.doesNotMatch(riverCrateSource, /export\s+class\s+RiverCrate/,
  'RiverCrate is a runtime part script, not an ESM export');
const riverCrateModule = await import(`data:text/javascript;base64,${
  Buffer.from(`${riverCrateSource}\nexport { RiverCrate };`).toString('base64')}`);
const crateCalls = [];
const riverCratePart = new riverCrateModule.RiverCrate();
for (const method of ['fill', 'box']) {
  riverCratePart[method] = (...args) => crateCalls.push([method, ...args]);
}
riverCratePart.build({});
assert.deepEqual(crateCalls.find(call => call[0] === 'fill'),
  ['fill', MAT.plaster], 'RiverCrate keeps the shared plaster-like convention');
assert.deepEqual(crateCalls.find(call => call[0] === 'box'),
  ['box', [0, 0, 0], [0.75, 0.75, 0.75]],
  'RiverCrate visual is centred and exactly 1.5 x 1.5 x 1.5 m');

const raftSource = await requiredText(new URL(
  '../scenes/RiverFloatLab/objects/RiverRaft.js', import.meta.url),
  'RiverRaft part');
assert.match(raftSource, /^class RiverRaft extends Part/m,
  'scene-local parts use the runtime-discoverable class declaration convention');
assert.doesNotMatch(raftSource, /export\s+class\s+RiverRaft/,
  'scene-local parts are scripts, not ESM exports');
const raftModule = await import(`data:text/javascript;base64,${
  Buffer.from(`${raftSource}\nexport { RiverRaft };`).toString('base64')}`);
assert.equal(raftModule.RiverRaft.noImpostor, true,
  'dynamic river rafts keep a real mesh at every distance');
const calls = [];
const raftPart = new raftModule.RiverRaft();
for (const method of ['beginVoxels', 'fill', 'smoothing', 'box', 'endVoxels']) {
  raftPart[method] = (...args) => calls.push([method, ...args]);
}
raftPart.build({});
assert.deepEqual(calls.find(call => call[0] === 'fill'), ['fill', MAT.bark],
  'raft uses the existing bark/wood-like material');
assert.deepEqual(calls.find(call => call[0] === 'box'),
  ['box', [0, 0, 0], [2.4, 0.35, 1.5]],
  'raft visual is centred and exactly 4.8 x 0.7 x 3.0 m');
assert.ok(calls.some(call => call[0] === 'smoothing' && call[1] > 0),
  'raft visual rounds the flattened box');

console.log('river_float_lab_scene_tests: PASS');
