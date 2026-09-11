import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';

// Minimal runtime globals the CastleMasonry scene scripts assume (mirrors
// river_float_lab_scene_tests.mjs's stubbing approach).
globalThis.Part = class {};
globalThis.World = class {};
let nextMaterialHandle = 100;
const materialCalls = [];
globalThis.defineMaterial = (name, spec) => {
  materialCalls.push({ name, spec });
  return nextMaterialHandle++;
};
globalThis.SHAPE = { triangles: 0, strip: 1, fan: 2, polygon: 3 };

// castle_masonry.js and the scene scripts use the engine's bare
// `shared-lib/...` specifiers; rewrite every one (recursively -- castle_
// masonry.js itself imports castle_primitives, castle_masonry_fixture.js
// declares none) to data: URLs so Node loads the exact same source the
// QuickJS host does. Reuses the trick from castle_masonry_tests.mjs, made
// recursive since the scene files chain through more than one shared module.
function dataUrl(source) {
  return `data:text/javascript;base64,${Buffer.from(source).toString('base64')}`;
}

const sharedLibCache = new Map();
function loadSharedLib(name) {
  if (sharedLibCache.has(name)) return sharedLibCache.get(name);
  const url = new URL(`../shared-lib/${name}.js`, import.meta.url);
  if (!existsSync(url)) assert.fail(`shared-lib/${name} must exist at ${url}`);
  const rewritten = rewriteSharedImports(readFileSync(url, 'utf8'));
  const result = dataUrl(rewritten);
  sharedLibCache.set(name, result);
  return result;
}

function rewriteSharedImports(source) {
  return source.replace(/(['"])shared-lib\/([\w.-]+)\1/g,
    (match, quote, name) => JSON.stringify(loadSharedLib(name)));
}

async function loadRuntimeScript(url, className, label) {
  if (!existsSync(url)) assert.fail(`${label} must exist at ${url}`);
  const source = readFileSync(url, 'utf8');
  assert.match(source, new RegExp(`^class ${className} extends `, 'm'),
    `${label} uses the runtime-discoverable class declaration convention`);
  assert.doesNotMatch(source, new RegExp(`export\\s+class\\s+${className}`),
    `${label} is a runtime script, not an ESM export`);
  const rewritten = rewriteSharedImports(source) + `\nexport { ${className} };`;
  return (await import(dataUrl(rewritten)))[className];
}

const castlePlan = await import(loadSharedLib('castle_plan'));
const castleMasonryFixtureLib = await import(loadSharedLib('castle_masonry_fixture'));
const castleMasonry = await import(loadSharedLib('castle_masonry'));

const CastleMasonryFixture = await loadRuntimeScript(
  new URL('../scenes/CastleMasonry/objects/CastleMasonryFixture.js', import.meta.url),
  'CastleMasonryFixture', 'CastleMasonryFixture part');
const CastleMasonryGround = await loadRuntimeScript(
  new URL('../scenes/CastleMasonry/objects/CastleMasonryGround.js', import.meta.url),
  'CastleMasonryGround', 'CastleMasonryGround part');
const CastleMasonry = await loadRuntimeScript(
  new URL('../scenes/CastleMasonry/CastleMasonry.js', import.meta.url),
  'CastleMasonry', 'CastleMasonry world');

// ------------------------------------------------------------ World roots

assert.ok(Array.isArray(CastleMasonry.roots) && CastleMasonry.roots.length > 0,
  'CastleMasonry declares roots');
const groundRoot = CastleMasonry.roots.find(root => root.module === 'CastleMasonryGround');
assert.ok(groundRoot, 'CastleMasonry roots include the CastleMasonryGround slab');
const fixtureRoot = CastleMasonry.roots.find(root => root.module === 'CastleMasonryFixture');
assert.ok(fixtureRoot, 'CastleMasonry roots include CastleMasonryFixture');
assert.equal(fixtureRoot.expand, true,
  'the fixture root uses expand: true (masonry is child-only, per castle_masonry.js)');
assert.ok(fixtureRoot.params && typeof fixtureRoot.params === 'object', 'fixture root has params');
for (const [key, value] of Object.entries(fixtureRoot.params))
  assert.ok(typeof value === 'number' && Number.isFinite(value),
    `fixture root param ${key} is a flat finite scalar`);
assert.ok(Array.isArray(fixtureRoot.transform) && fixtureRoot.transform.length === 16 &&
  fixtureRoot.transform.every(Number.isFinite),
  'fixture root carries a flat 16-float row-major transform');

// ------------------------------------------------------------ requires()

const manifest = castlePlan.compilePlan(castleMasonryFixtureLib.CASTLE_MASONRY_FIXTURE_PLAN);
// Simulate the engine's static-defaults + authored-params merge.
const mergedParams = { ...CastleMasonryFixture.params, ...fixtureRoot.params };
const expectedRequires = castleMasonry.masonryChildVariants(
  manifest, castleMasonry.masonryOptions(mergedParams));
const actualRequires = CastleMasonryFixture.requires(mergedParams);
assert.ok(Array.isArray(actualRequires) && actualRequires.length > 0,
  'CastleMasonryFixture.requires(params) is non-empty');
assert.deepEqual(actualRequires, expectedRequires,
  'CastleMasonryFixture.requires(params) equals masonryChildVariants(compilePlan(PLAN), masonryOptions(params))');

// ------------------------------------------------------------ build()

const IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
function mul(a, b) {
  const out = new Array(16).fill(0);
  for (let r = 0; r < 4; ++r) for (let c = 0; c < 4; ++c)
    for (let k = 0; k < 4; ++k) out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
  return out;
}

const fixtureInstance = new CastleMasonryFixture();
fixtureInstance.stack = [IDENTITY];
fixtureInstance.placements = [];
fixtureInstance.pushMatrix = function pushMatrix() { this.stack.push(this.stack[this.stack.length - 1]); };
fixtureInstance.popMatrix = function popMatrix() {
  assert.ok(this.stack.length > 1, 'matrix stack underflow');
  this.stack.pop();
};
fixtureInstance.applyMatrix = function applyMatrix(m) {
  assert.equal(m.length, 16);
  assert.ok(m.every(Number.isFinite), 'finite placement matrix');
  this.stack[this.stack.length - 1] = mul(this.stack[this.stack.length - 1], m);
};
fixtureInstance.placeChild = function placeChild(module, params) {
  this.placements.push({ module, params });
};
fixtureInstance.build(mergedParams);

assert.ok(fixtureInstance.placements.length > 2000,
  `CastleMasonryFixture.build() places > 2000 children (got ${fixtureInstance.placements.length})`);

const declaredKeys = new Set(actualRequires.map(item => `${item.module}:${JSON.stringify(item.params)}`));
for (const placement of fixtureInstance.placements) {
  assert.ok(Object.values(placement.params).every(value => typeof value === 'number' && Number.isFinite(value)),
    `placed ${placement.module} has flat finite params`);
  assert.ok(declaredKeys.has(`${placement.module}:${JSON.stringify(placement.params)}`),
    `placed ${placement.module}:${JSON.stringify(placement.params)} was declared by requires()`);
}
// build() places only what requires() declared -- no extra module/param combos.
const placedKeys = new Set(fixtureInstance.placements.map(
  item => `${item.module}:${JSON.stringify(item.params)}`));
for (const key of placedKeys) assert.ok(declaredKeys.has(key), `${key} is declared`);

// ------------------------------------------------------------ module resolution

const objectsDir = new URL('../objects/', import.meta.url);
const placedModules = new Set(fixtureInstance.placements.map(item => item.module));
assert.ok(placedModules.size > 0, 'the fixture placed at least one module kind');
for (const module of placedModules)
  assert.ok(existsSync(new URL(`${module}.js`, objectsDir)),
    `placed module ${module} resolves to projects/world_demo/objects/${module}.js`);

console.log(`castle masonry scene: PASS - ${fixtureInstance.placements.length} placements, ` +
  `${placedModules.size} module kinds (${[...placedModules].sort().join(', ')}), ` +
  `${actualRequires.length} declared variants, ${materialCalls.length} materials defined`);
