import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

globalThis.World = class {};

const sceneUrl = new URL('../scenes/RiverHydrology/RiverHydrology.js', import.meta.url);
const riverCurveSource = await readFile(new URL(
  '../../../MatterEngine3/shared-lib/river_curve.js', import.meta.url), 'utf8');
const riverCurveUrl = `data:text/javascript;base64,${
  Buffer.from(riverCurveSource).toString('base64')}`;
const source = (await readFile(sceneUrl, 'utf8')).replace(
  "'shared-lib/river_curve'", JSON.stringify(riverCurveUrl));
const moduleUrl = `data:text/javascript;base64,${Buffer.from(source).toString('base64')}`;
const { buildRiverHydrologyDefinition } = await import(moduleUrl);

assert.equal(typeof buildRiverHydrologyDefinition, 'function');
const scene = buildRiverHydrologyDefinition(0x12345678);

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
assert.ok(scene.channelProfile.some(point => point.width >= 34),
  'the first pool visibly widens beyond the rapids');
assert.ok(scene.channelProfile.some(point => point.width >= 36),
  'the final pool visibly widens beyond the lower rapids');

console.log('river_hydrology_scene_tests: PASS');
