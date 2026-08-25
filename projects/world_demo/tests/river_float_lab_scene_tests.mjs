import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

globalThis.World = class {};
globalThis.Part = class {};
globalThis.MAT = { bark: 101 };

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
  "'shared-lib/river_hydrology_definition'", JSON.stringify(sharedUrl));
const sceneUrl = `data:text/javascript;base64,${
  Buffer.from(sceneSource).toString('base64')}`;
const riverFloatLab = await import(sceneUrl);

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
assert.equal(referenceCrate.components.PartInstance.part, 'Crate');
assert.deepEqual(referenceCrate.components.BoxCollider.halfExtents, [1.5, 1.5, 1.5]);
assert.equal(referenceCrate.components.RiverFloatBody.effectiveDensityKgM3, 620);
assert.deepEqual([
  referenceCrate.components.RiverFloatBody.probesX,
  referenceCrate.components.RiverFloatBody.probesY,
  referenceCrate.components.RiverFloatBody.probesZ,
], [2, 2, 2]);
assert.equal(referenceRaft.components.PartInstance.part, 'RiverRaft');
assert.deepEqual(referenceRaft.components.BoxCollider.halfExtents, [2.4, 0.35, 1.5]);
assert.equal(referenceRaft.components.RiverFloatBody.effectiveDensityKgM3, 420);
assert.deepEqual([
  referenceRaft.components.RiverFloatBody.probesX,
  referenceRaft.components.RiverFloatBody.probesY,
  referenceRaft.components.RiverFloatBody.probesZ,
], [3, 2, 3]);
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

const raftSource = await requiredText(new URL(
  '../scenes/RiverFloatLab/objects/RiverRaft.js', import.meta.url),
  'RiverRaft part');
assert.match(raftSource, /^class RiverRaft extends Part/m,
  'scene-local parts use the runtime-discoverable class declaration convention');
assert.doesNotMatch(raftSource, /export\s+class\s+RiverRaft/,
  'scene-local parts are scripts, not ESM exports');
const raftModule = await import(`data:text/javascript;base64,${
  Buffer.from(`${raftSource}\nexport { RiverRaft };`).toString('base64')}`);
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
