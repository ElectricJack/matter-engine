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
  '../scenes/RiverHydrology/RiverHydrology.js', import.meta.url),
  'RiverHydrology scene')).replace(
  "'shared-lib/river_hydrology_definition'", JSON.stringify(sharedUrl));
const sceneUrl = `data:text/javascript;base64,${
  Buffer.from(sceneSource).toString('base64')}`;
const riverHydrology = await import(sceneUrl);

assert.equal(riverHydrology.buildRiverHydrologyDefinition,
  shared.buildRiverHydrologyDefinition,
  'RiverHydrology consumes the shared pure definition instead of a private copy');
assert.equal(riverHydrology.authorRiverHydrologyNetwork,
  shared.authorRiverHydrologyNetwork,
  'RiverHydrology consumes the shared accepted network inputs');
assert.equal(riverHydrology.buildRiverHydrologyField,
  shared.buildRiverHydrologyField,
  'RiverHydrology consumes the shared accepted terrain inputs');

const scene = shared.buildRiverHydrologyDefinition(0x12345678);
assert.equal(scene.sections.length, 2);
assert.ok(scene.sections[0].length >= 100, 'upper section is at least 100 m');
assert.ok(scene.sections[1].length >= 100, 'lower section is at least 100 m');
assert.ok(Math.abs(scene.waterfall.drop - 12) < 0.25,
  'waterfall has the authored 12 m drop');
assert.ok(scene.spillway.width >= 8 && scene.spillway.width <= 12,
  'first pool spillway is broad enough for the inherited emitter');
assert.ok(scene.roots.length >= 12, 'the river has at least twelve boulders');
assert.ok(scene.roots.every(root => root.id && root.fluidCollider),
  'every boulder has stable identity and matching fluid collision');
assert.ok(scene.roots.every(root => root.components === undefined),
  'the shared river generator does not implicitly author rigid bodies');
assert.equal(scene.entities, undefined,
  'the shared river generator does not implicitly author gameplay entities');
assert.ok(scene.channelProfile.some(point => point.width >= 34),
  'the first pool visibly widens beyond the rapids');
assert.ok(scene.channelProfile.some(point => point.width >= 36),
  'the final pool visibly widens beyond the lower rapids');
assert.match(sharedSource,
  /network\.meshAnimation\(\{\s*framesPerSecond:\s*30,\s*duration:\s*1(?:\.0)?,\s*phaseOffset:\s*0\.5,?\s*\}\)/s,
  'the accepted RiverFloat network opts into the fixed 30 Hz mesh animation profile');

console.log('river_hydrology_scene_tests: PASS');
