import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const here = new URL('.', import.meta.url);
const sceneUrl = new URL('LocalLightRtGallery.js', here);
const proxyUrl = new URL('objects/LocalLightRtGlowProxy.js', here);
const fixtureUrl = new URL('objects/LocalLightRtGalleryFixture.js', here);
const captureUrl = new URL('capture-reopen.ps1', here);

// Evaluate the real world factory with only its engine-provided material/world
// globals substituted.  A data URL keeps this test package-free, matching the
// repository's other QuickJS-authored Node checks.
globalThis.World = class {};
globalThis.defineCastleMaterials = () => ({
  plaster: 1, limestone: [2, 3, 4, 5], foundation: 6, terracotta: 7,
  iron: 8, gold: 9, clearGlass: 10,
});
globalThis.defineMaterial = (_name, definition) => {
  assert.equal(definition.detail, 'ForestFloor');
  return 11;
};
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

const [proxySource, fixtureSource, captureSource] = await Promise.all([
  readFile(proxyUrl, 'utf8'), readFile(fixtureUrl, 'utf8'),
  readFile(captureUrl, 'utf8'),
]);
assert.match(proxySource, /this\.rayTraced\(false\)/);
assert.doesNotMatch(fixtureSource, /this\.rayTraced\(false\)/);
assert.match(fixtureSource, /this\.fill\(p\.gold\)/);
assert.match(fixtureSource, /this\.fill\(p\.clearGlass\)/);
assert.match(fixtureSource, /this\.fill\(p\.pomGround\)/);

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
  Buffer.from(fixtureSource + '\nexport { LocalLightRtGalleryFixture, LOCAL_LIGHT_RT_CORNER_FIXTURE, LOCAL_LIGHT_RT_HOUSING_FIXTURE };').toString('base64'));
const fixture = new fixtureModule.LocalLightRtGalleryFixture();
fixture.build({
  plaster: 1, limestone: 2, foundation: 6, terracotta: 7,
  iron: 8, gold: 9, clearGlass: 10,
  pomGround: 11,
});
assert.ok(fixture.geometryCalls >= 35);
assert.equal(fixture.rt, true);

// Analytic emitters must sit wholly outside their RT-visible iron housings.
// A tangent/intersecting housing turns the four fixed area-light samples into
// a stable black-pepper pattern because the raw direct lane is not filtered.
const housing = fixtureModule.LOCAL_LIGHT_RT_HOUSING_FIXTURE;
for (const light of lights.points) {
  assert.ok(housing.pointOffset > housing.pointRadius + light.sourceRadius);
}
assert.ok(housing.spotOffset >
          housing.spotRadius + lights.spots[0].sourceRadius);

// The colored-bounce witness must encode a real two-segment visibility test:
// neutral source -> pale receiver is blocked by the return, while lit colored
// card -> receiver clears its positive-z end.  This catches fixture edits that
// can make an attractive screenshot without exercising indirect lighting.
const corner = fixtureModule.LOCAL_LIGHT_RT_CORNER_FIXTURE;
const zWhereSegmentCrossesX = (a, b, x) => {
  const t = (x - a[0]) / (b[0] - a[0]);
  assert.ok(t > 0 && t < 1);
  return a[2] + t * (b[2] - a[2]);
};
for (const receiver of [corner.wallReceiver, corner.sphereReceiver]) {
  assert.ok(Math.abs(zWhereSegmentCrossesX(corner.source, receiver,
                                           corner.returnX)) < corner.returnMaxZ,
            'the return must block local direct from the neutral source');
  assert.ok(zWhereSegmentCrossesX(corner.card, receiver, corner.returnX) >
              corner.returnMaxZ + 0.10,
            'the colored card must be visible around the end of the return');
}
assert.ok(Math.hypot(...corner.card.map((value, i) => value - corner.source[i])) <
          lights.points[1].range,
          'the neutral point light must reach the terracotta card');

const proxyModule = await import('data:text/javascript;base64,' +
  Buffer.from(proxySource + '\nexport { LocalLightRtGlowProxy };').toString('base64'));
const proxy = new proxyModule.LocalLightRtGlowProxy();
proxy.build();
assert.equal(proxy.geometryCalls, 6);
assert.equal(proxy.rt, false);

// Pin the reopened native evidence protocol.  These exact contiguous blocks
// guarantee one fixed camera per pair, one intended property delta, explicit
// history invalidation, and the same 96 successfully presented settle frames.
const timelineMatch = captureSource.match(/\$timeline = @"\r?\n([\s\S]*?)\r?\n"@/);
assert.ok(timelineMatch, 'capture-reopen.ps1 must contain a literal timeline');
const timeline = timelineMatch[1].replaceAll('\r\n', '\n');
for (const fixedSetting of [
  'render_path native_rt',
  'set render.gi.diffuse_multiplier 1',
  'set render.lighting.emission_multiplier 1',
  'set render.lighting.exposure_ev 0',
]) assert.ok(timeline.includes(fixedSetting));

const requireExactPair = (lines) => {
  const block = lines.join('\n');
  assert.ok(timeline.includes(block), `missing exact capture block:\n${block}`);
  assert.equal(lines.filter((line) => line.startsWith('cam ')).length, 1);
  assert.equal(lines.filter((line) => line === 'history_reset').length, 2);
  assert.equal(lines.filter((line) => line === 'wait_frames 96').length, 2);
};
requireExactPair([
  'cam 5.8 3.2 7.4 1.5 1.30 1.05',
  'set render.gi.enabled false',
  'history_reset',
  'wait_frames 96',
  'shot $shots/corner-gi-off.png',
  'set render.gi.enabled true',
  'history_reset',
  'wait_frames 96',
  'shot $shots/corner-gi-on.png',
]);
requireExactPair([
  'cam 14.0 2.85 6.2 13.5 1.45 0.1',
  'set render.gi.enabled false',
  'history_reset',
  'wait_frames 96',
  'shot $shots/materials-gi-off.png',
  'set render.gi.enabled true',
  'history_reset',
  'wait_frames 96',
  'shot $shots/materials-gi-on.png',
]);
requireExactPair([
  'cam 12.8 3.1 5.5 11.5 1.8 1.0',
  'set render.gi.enabled true',
  'set render.lighting.emission_multiplier 1',
  'history_reset',
  'wait_frames 96',
  'shot $shots/proxy-visible.png',
  'set render.lighting.emission_multiplier 0',
  'history_reset',
  'wait_frames 96',
  'shot $shots/proxy-hidden.png',
]);
assert.match(timeline, /set render\.lighting\.emission_multiplier 1\nquit$/);
assert.match(captureSource, /--world LocalLightRtGallery --timeline/);
for (const rejectedRunMarker of [
  'native_rt unavailable', 'set: unknown property', 'set: cannot parse',
  'event: bake\\.finished timeout', 'idle: timeout',
  'wait_frames: dispatch failed', 'validation errors: [1-9]',
]) assert.ok(captureSource.includes(rejectedRunMarker));

console.log('LocalLightRtGallery fixture tests passed');
