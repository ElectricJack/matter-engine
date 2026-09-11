import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const here = new URL('.', import.meta.url);
const sceneUrl = new URL('LocalLightRtGallery.js', here);
const proxyUrl = new URL('objects/LocalLightRtGlowProxy.js', here);
const fixtureUrl = new URL('objects/LocalLightRtGalleryFixture.js', here);

// Evaluate the real world factory with only its engine-provided material/world
// globals substituted.  A data URL keeps this test package-free, matching the
// repository's other QuickJS-authored Node checks.
globalThis.World = class {};
globalThis.defineCastleMaterials = () => ({
  plaster: 1, limestone: [2, 3, 4, 5], foundation: 6, terracotta: 7,
  iron: 8, gold: 9, clearGlass: 10,
});
let source = await readFile(sceneUrl, 'utf8');
source = source.replace(
  /^import \{ defineCastleMaterials \} from 'shared-lib\/castle_materials';$/m,
  'const defineCastleMaterials = globalThis.defineCastleMaterials;');
const scene = await import('data:text/javascript;base64,' +
  Buffer.from(source).toString('base64'));

const lights = scene.localLightRtGalleryLights(true);
assert.equal(lights.points.length, 5);
assert.equal(lights.spots.length, 1);
for (const light of [...lights.points, ...lights.spots]) {
  assert.equal(light.castsShadow, true);
  assert.ok(light.intensity > 0 && light.range > 0 && light.sourceRadius > 0);
  assert.ok(light.position.every(Number.isFinite));
}
const spotLength = Math.hypot(...lights.spots[0].direction);
assert.ok(Math.abs(spotLength - 1) < 0.001);
assert.ok(lights.spots[0].inner < lights.spots[0].outer);

const zeroLights = scene.localLightRtGalleryLights(false);
assert.deepEqual(zeroLights.points, []);
assert.deepEqual(zeroLights.spots, []);
assert.deepEqual(zeroLights.sun.color, [0, 0, 0]);
assert.deepEqual(zeroLights.sky.color, [0, 0, 0]);

const withProxy = scene.localLightRtGalleryRoots(true);
const withoutProxy = scene.localLightRtGalleryRoots(false);
assert.equal(withProxy.length, withoutProxy.length + 1);
assert.deepEqual(withProxy.slice(0, withoutProxy.length), withoutProxy);
assert.equal(withProxy.at(-1).module, 'LocalLightRtGlowProxy');

const [proxySource, fixtureSource] = await Promise.all([
  readFile(proxyUrl, 'utf8'), readFile(fixtureUrl, 'utf8'),
]);
assert.match(proxySource, /this\.rayTraced\(false\)/);
assert.doesNotMatch(fixtureSource, /this\.rayTraced\(false\)/);
assert.match(fixtureSource, /this\.fill\(p\.gold\)/);
assert.match(fixtureSource, /this\.fill\(p\.clearGlass\)/);

const finiteVector = (value, size) =>
  Array.isArray(value) && value.length === size && value.every(Number.isFinite);
globalThis.Part = class {
  constructor() { this.geometryCalls = 0; this.rt = true; }
  fill(material) { assert.ok(Number.isInteger(material)); }
  box(center, half) {
    assert.ok(finiteVector(center, 3) && finiteVector(half, 3));
    assert.ok(half.every((value) => value > 0));
    ++this.geometryCalls;
  }
  sphere(center, radius) {
    assert.ok(finiteVector(center, 3) && Number.isFinite(radius) && radius > 0);
    ++this.geometryCalls;
  }
  cylinder(a, b, radius) {
    assert.ok(finiteVector(a, 3) && finiteVector(b, 3));
    assert.ok(Number.isFinite(radius) && radius > 0);
    ++this.geometryCalls;
  }
  rayTraced(value) { assert.equal(typeof value, 'boolean'); this.rt = value; }
};
globalThis.MAT = { lightWarmLow: 26 };
const fixtureModule = await import('data:text/javascript;base64,' +
  Buffer.from(fixtureSource + '\nexport { LocalLightRtGalleryFixture };').toString('base64'));
const fixture = new fixtureModule.LocalLightRtGalleryFixture();
fixture.build({
  plaster: 1, limestone: 2, foundation: 6, terracotta: 7,
  iron: 8, gold: 9, clearGlass: 10,
});
assert.ok(fixture.geometryCalls >= 35);
assert.equal(fixture.rt, true);

const proxyModule = await import('data:text/javascript;base64,' +
  Buffer.from(proxySource + '\nexport { LocalLightRtGlowProxy };').toString('base64'));
const proxy = new proxyModule.LocalLightRtGlowProxy();
proxy.build();
assert.equal(proxy.geometryCalls, 6);
assert.equal(proxy.rt, false);

console.log('LocalLightRtGallery fixture tests passed');
