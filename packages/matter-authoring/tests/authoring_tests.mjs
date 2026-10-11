import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { SourceTextModule, createContext } from 'node:vm';
import * as helpers from '../../../libs/MatterContracts/examples/contracts.mjs';
import { AuthoringError, RecipeBuilder, SeededRng, contractServices, parameters, nodeInput, identity,
  multiply, translation, scale, legacyPart, LEGACY_VOXELS, DEFAULT_CAPABILITIES, validateRecipe } from '../index.mjs';
import { orderedCsg, extrudedProp, repeatedAssembly } from '../examples/recipes.mjs';
import { expectedProp, expectedRepeated } from './expected.mjs';

const services = contractServices(helpers, async bytes => new Uint8Array(createHash('sha256').update(bytes).digest()));
const source = { id: 'fixture.authoring', uri: 'fixture://authoring', revision: '1' };
const builder = options => new RecipeBuilder({ source, seed: 1n, services, ...options });
const sourceAt = { uri: 'author://part.js', line: 17, column: 4 };
let checks = 0;
function test(fixture, run) { run(); ++checks; console.log(`PASS ${fixture}`); }
function throws(code, run, inspect = () => {}) {
  assert.throws(run, error => { assert(error instanceof AuthoringError); assert.equal(error.diagnostic.code, code); inspect(error.diagnostic); return true; });
}
const documents = [extrudedProp(services), orderedCsg(services), repeatedAssembly(services)];
const additionalDocuments = [];
const expectations = [expectedProp, JSON.parse(readFileSync(new URL('../../../libs/MatterContracts/examples/ordered-csg.recipe.json', import.meta.url))), expectedRepeated];
const fixtureIds = ['SDK-PROP-v1', 'SDK-CSG-v1', 'SDK-REPEATED-v1'];
for (let i = 0; i < documents.length; ++i) test(fixtureIds[i], () => {
  assert.deepEqual(documents[i].build(), expectations[i]);
  assert.equal(documents[i].canonical(), helpers.canonicalJson(expectations[i]));
  assert(Object.isFrozen(documents[i].build().nodes[0].parameters));
});
test('SDK-RNG-v1', () => {
  const vectors = JSON.parse(readFileSync(new URL('../../../libs/MatterContracts/tests/rng-vectors.json', import.meta.url)));
  for (const vector of vectors.vectors) {
    const words = new SeededRng(vector.seed, services), units = new SeededRng(vector.seed, services);
    assert.deepEqual(vector.words.map(() => words.nextU64().toString(16).padStart(16, '0')), vector.words);
    assert.deepEqual(vector.units.map(() => units.random()), vector.units);
    const saved = words.snapshot(); assert.equal(words.nextU64(), words.restore(saved).nextU64());
    assert.equal(words.clone().nextU64(), words.nextU64());
  }
  const a = new SeededRng(1n, services), b = new SeededRng(1n, services);
  assert.equal(a.int(8), Math.floor(b.random() * 8));
  assert.equal(a.range(-2, 2), -2 + b.random() * 4);
  throws('invalid_input', () => a.int(0)); throws('invalid_input', () => a.range(2, 1));
  throws('invalid_input', () => builder({ seed: 1 })); throws('unsupported_capability', () => new SeededRng(1n));
});
test('SDK-PARAMETERS-v1', () => {
  const specs = { width: { type: 'number', default: 0.1, minimum: 0 }, count: { type: 'integer', default: 2 }, enabled: { type: 'boolean', default: true }, label: { type: 'string', default: 'a' }, mode: { type: 'enum', default: 'box', values: ['box', 'sphere'] } };
  const p = parameters(specs, { width: 0.25, mode: 'sphere' });
  assert.deepEqual(Object.keys(p), ['count', 'enabled', 'label', 'mode', 'width']); assert.equal(p.width, 0.25); assert(Object.isFrozen(p));
  for (const override of [{ unknown: 1 }, { width: NaN }, { enabled: 1 }, { count: 1.5 }, { mode: 'water' }]) throws('invalid_input', () => parameters(specs, override));
  const cyclic = {}; cyclic.self = cyclic; throws('cycle', () => builder({ parameters: cyclic }));
  let calls = 0; const accessor = { get x() { ++calls; return 1; } };
  throws('invalid_input', () => builder({ parameters: accessor })); assert.equal(calls, 0);
  for (const value of [new Uint8Array(4), { x: undefined }, { x: () => 1 }, { x: '\ud800' }]) throws('invalid_input', () => builder({ parameters: value }));
  const unusual = JSON.parse('{"__proto__":{"safe":true},"constructor":1}');
  const doc = builder({ parameters: unusual }); doc.output(doc.box('box', [1, 1, 1]));
  assert(Object.hasOwn(doc.build().parameters, '__proto__')); assert.equal({}.safe, undefined);
});
test('SDK-DIAGNOSTICS-v1', () => {
  const b = builder(); b.at(sourceAt, () => b.place('missing', nodeInput('absent'), identity())); b.output('missing');
  throws('missing_dependency', () => b.build(), d => { assert.deepEqual(d.source, sourceAt); assert.equal(d.node_id, 'missing'); assert.deepEqual(d.dependency_path, ['missing', 'absent']); });
  const cycle = builder(); cycle.at(sourceAt, () => cycle.place('a', nodeInput('b'), identity())); cycle.place('b', nodeInput('a'), identity()); cycle.output('a');
  throws('cycle', () => cycle.build(), d => { assert.deepEqual(d.dependency_path, ['a', 'b', 'a']); assert.deepEqual(d.source, sourceAt); });
  throws('unsupported_capability', () => builder().at(sourceAt, b => b.node({ id: 'water', operation: 'water.flow' })), d => { assert.deepEqual(d.source, sourceAt); assert.equal(d.node_id, 'water'); });
  throws('unsupported_capability', () => builder({ requiredCapabilities: [{ name: 'matter.unknown', version: 1 }] }));
  const old = builder(); old.node({ id: 'old', operation: 'primitive.box', operationVersion: 2, parameters: { size: [1, 1, 1] } }); old.output('old'); throws('unsupported_version', () => old.build());
  const bad = builder(); bad.output(bad.box('box', [1, 0, 1])); throws('invalid_input', () => bad.build());
  const typo = structuredClone(expectedProp); typo.nodes[0].parameters.typo = 1; throws('invalid_input', () => validateRecipe(typo));
  for (const key of ['parameters', 'source_map']) {
    const malformed = structuredClone(expectedProp); malformed[key] = null; throws('invalid_input', () => validateRecipe(malformed));
  }
  const malformedNode = structuredClone(expectedProp); malformedNode.nodes[0].parameters = null; throws('invalid_input', () => validateRecipe(malformedNode));
  const incompleteSource = structuredClone(expectedProp); delete incompleteSource.source.content; throws('invalid_input', () => validateRecipe(incompleteSource));
  const duplicateHash = structuredClone(expectedProp); duplicateHash.source.legacy_hashes = Array.from({ length: 2 }, () => ({ namespace: 'matter.source.v1', value: '0000000000000001' })); throws('invalid_input', () => validateRecipe(duplicateHash));
  const byteBudget = builder({ services: { ...services, canonicalJson: document => helpers.canonicalJson(document, { maxBytes: 32 }) } }); byteBudget.output(byteBudget.box('box', [1, 1, 1])); throws('resource_limit', () => byteBudget.canonical());
  const prototypeName = builder(); prototypeName.output(prototypeName.box('constructor', [1, 0, 1])); throws('invalid_input', () => prototypeName.build(), d => assert.equal(d.source, null));
});
test('SDK-AFFINE-v1', () => {
  const b = builder(), box = b.box('box', [1, 1, 1]); const m = multiply(translation(10, 20, 30), scale(-2, 3, 4));
  b.output(b.place('affine', box, m)); assert.deepEqual(b.build().nodes[1].transform, [-2, 0, 0, 10, 0, 3, 0, 20, 0, 0, 4, 30, 0, 0, 0, 1]);
  const c = builder(), outer = c.box('outer', [2, 2, 2]), mirrored = c.place('mirrored', outer, scale(-1, 1, 1));
  c.at(sourceAt, () => c.output(c.csg('csg', 'subtract', [outer, mirrored])));
  throws('unsupported_capability', () => c.build(), d => assert.deepEqual(d.source, sourceAt));
  const n = builder(), part = n.box('box', [1, 1, 1]), scaled = n.place('scaled', part, scale(1, 2, 1)); n.output(n.csg('bad', 'union', [part, scaled])); throws('unsupported_capability', () => n.build());
  const singular = identity(); singular[0] = 0; throws('invalid_input', () => b.place('singular', box, singular));
});
test('SDK-COMPOSITION-v1', () => {
  const b = builder(); const first = b.compose('first', expectedRepeated), second = b.compose('second', expectedRepeated); b.output(...first, ...second);
  const d = b.build(); assert.equal(d.asset_dependencies.length, 1); assert.equal(d.nodes.length, 6); assert.equal(d.nodes[3].inputs[0].reference, 'shared.pair');
  assert.equal(d.nodes[4].inputs[0].reference, 'second/pair');
  throws('invalid_input', () => b.dependency({ ...expectedRepeated.asset_dependencies[0], content: { algorithm: 'sha256', hex: 'cd'.repeat(32) } }));
  const before = b.canonical(); throws('invalid_input', () => b.compose('first', expectedRepeated)); assert.equal(b.canonical(), before);
  const prop = builder(); prop.output(...prop.compose('child', expectedProp)); assert.equal(prop.build().nodes[1].materials[0], 'child/clay');
  additionalDocuments.push(d, prop.build());
  const named = builder(); named.output(named.box('constructor', [1, 1, 1])); const scoped = builder(); scoped.output(...scoped.compose('scoped', named.build())); assert.equal(scoped.build().nodes[0].id, 'scoped/constructor');
});
test('SDK-MESH-v1', () => {
  const b = builder(), hash = { algorithm: 'sha256', hex: '01'.repeat(32) }, material = b.material('plain');
  b.resource({ id: 'positions', content: hash, scalar_type: 'f32', components: 3, element_count: 3n, byte_length: 36n });
  b.resource({ id: 'indices', content: hash, scalar_type: 'u32', components: 1, element_count: 3n, byte_length: 12n });
  b.output(b.mesh('triangle', { positions: 'positions', indices: 'indices' }, material));
  assert.equal(b.build().resources[0].element_count, '0000000000000003');
  additionalDocuments.push(b.build());
  const invalid = structuredClone(b.build()); invalid.resources[1].byte_length = '0000000000000004'; throws('invalid_input', () => validateRecipe(invalid));
  const customCap = { name: 'matter.operation.mesh.test', version: 1 }, custom = builder({ supportedCapabilities: [...DEFAULT_CAPABILITIES, customCap] });
  custom.output(custom.node({ id: 'custom', operation: 'mesh.test', parameters: { count: 1 } })); assert.equal(custom.build().required_capabilities[1].name, customCap.name);
  const duplicate = structuredClone(expectedProp); duplicate.required_capabilities.push(duplicate.required_capabilities[0]); throws('invalid_input', () => validateRecipe(duplicate));
  const optional = builder(); optional.output(optional.box('box', [1, 1, 1])); optional.extension({ capability: { name: 'matter.annotation.test', version: 1 }, required: false, payload: { note: 'preserved' } });
  throws('invalid_input', () => optional.extension({ capability: { name: 'matter.annotation.test', version: 1 }, required: false, payload: null }));
  additionalDocuments.push(optional.metadata(null).build());
  const textured = builder(); textured.resource({ id: 'pixel', content: hash, scalar_type: 'u8', components: 4, element_count: 1n, byte_length: 4n });
  textured.texture({ id: 'color', resource: 'pixel', width: 1, height: 1, pixel_format: 'rgba8', color_space: 'srgb', wrap_u: 'repeat', wrap_v: 'repeat', min_filter: 'nearest', mag_filter: 'nearest' });
  textured.material('paint', { baseColorTexture: 'color' }); textured.output(textured.box('textured', [1, 1, 1], { materials: ['paint'] })); additionalDocuments.push(textured.build());
  const dataTexture = structuredClone(textured.build()); dataTexture.materials[0].normal_texture = 'color'; throws('invalid_input', () => validateRecipe(dataTexture));
  let calls = 0; throws('invalid_input', () => b.resource({ id: 'getter', get content() { ++calls; return hash; }, scalar_type: 'u8', components: 1, element_count: 1n, byte_length: 1n })); assert.equal(calls, 0);
});
test('SDK-LIMITS-v1', () => {
  const b = builder({ limits: { maxNodes: 2 } }), box = b.box('box', [1, 1, 1]); b.output(box);
  throws('resource_limit', () => b.repeat('too_many', 2, box, identity)); assert.equal(b.build().nodes.length, 1);
  const deep = builder({ limits: { maxDepth: 8 } }); let previous = deep.box('base', [1, 1, 1]);
  for (let i = 0; i < 8; ++i) previous = deep.place(`p${i}`, previous, identity()); deep.output(previous);
  assert.equal(deep.build().nodes.length, 9); previous = deep.place('too_deep', previous, identity()); deep.output(previous);
  throws('resource_limit', () => deep.build(), d => assert.equal(d.dependency_path.length, 10));
  const rollback = builder(); const ref = rollback.box('base', [1, 1, 1]); rollback.output(ref);
  throws('invalid_input', () => rollback.repeat('r', 2, ref, i => i ? [] : identity())); assert.equal(rollback.build().nodes.length, 1);
  const huge = []; huge.length = 0xffffffff; throws('resource_limit', () => builder({ parameters: { huge } }));
  throws('resource_limit', () => builder({ parameters: { largeString: '😀'.repeat(16385) } }));
  const resources = builder({ limits: { maxResourceBytes: 8 } }); resources.resource({ id: 'large', content: { algorithm: 'sha256', hex: 'ab'.repeat(32) }, scalar_type: 'u32', components: 1, element_count: 3n, byte_length: 12n }); resources.output(resources.box('box', [1, 1, 1])); throws('resource_limit', () => resources.build());
});
const legacyDocuments = [];
test('SDK-LEGACY-v1', () => {
  const b = builder({ supportedCapabilities: [...DEFAULT_CAPABILITIES, LEGACY_VOXELS] }); b.material('clay');
  const part = legacyPart(b, { prefix: 'part', materialIds: { 7: 'clay' }, sourceLocation: () => sourceAt });
  part.fill(7); part.translate(10, 0, 0); part.beginVoxels(0.25); part.box([1, 0, 0], [2, 3, 4]);
  part.sphere([0, 0, 0], 1); part.difference(); part.smoothing(0.125); part.endVoxels();
  const d = part.finish(); assert.deepEqual(d.nodes[0].parameters.size, [4, 6, 8]); assert.equal(d.nodes[1].transform[3], 11);
  assert.equal(d.nodes[4].parameters.operator, 'subtract'); assert.deepEqual(d.nodes[4].inputs.map(i => i.reference), ['part.1', 'part.3']);
  assert.deepEqual(d.extensions[0].payload, { sessions: [{ output: 'part.4', smoothing_metres: 0.125, spacing_metres: 0.25 }] });
  assert.deepEqual(d.source_map['part.4'], sourceAt); legacyDocuments.push(d);
  throws('invalid_input', () => part.box([0, 0, 0], [1, 1, 1]));
  const unsupported = legacyPart(builder(), { sourceLocation: () => sourceAt });
  throws('unsupported_capability', () => unsupported.beginVoxels(0.1), d => assert.deepEqual(d.source, sourceAt));
  throws('unsupported_capability', () => unsupported.cylinder([0, 0, 0], [0, 1, 0], 1), d => { assert(d.message.includes('cylinder')); assert.deepEqual(d.source, sourceAt); });
  throws('invalid_input', () => unsupported.difference()); throws('invalid_input', () => unsupported.popMatrix());
  const unbalanced = legacyPart(builder()); unbalanced.pushMatrix(); throws('invalid_input', () => unbalanced.finish());
  const multiple = legacyPart(builder({ supportedCapabilities: [...DEFAULT_CAPABILITIES, LEGACY_VOXELS] }));
  multiple.beginVoxels(0); multiple.box([0, 0, 0], [1, 1, 1]); throws('unsupported_capability', () => multiple.difference()); multiple.endVoxels();
  multiple.smoothing(-2); multiple.beginVoxels(0.5); multiple.sphere([0, 0, 0], 1); multiple.endVoxels();
  const multiDoc = multiple.finish(); assert.equal(multiDoc.extensions.length, 1); assert.equal(multiDoc.extensions[0].payload.sessions.length, 2); assert.equal(multiDoc.extensions[0].payload.sessions[0].spacing_metres, Math.fround(0.1)); legacyDocuments.push(multiDoc);
  assert.equal(multiDoc.extensions[0].payload.sessions[1].smoothing_metres, 0);
  const extensionConsumer = builder({ supportedCapabilities: [...DEFAULT_CAPABILITIES, LEGACY_VOXELS] }); extensionConsumer.output(extensionConsumer.box('base', [1, 1, 1]));
  const beforeExtension = extensionConsumer.canonical(); throws('unsupported_capability', () => extensionConsumer.compose('voxel', multiDoc)); assert.equal(extensionConsumer.canonical(), beforeExtension);
  throws('unsupported_capability', () => validateRecipe(multiDoc));
  const sealedBuilder = builder(), sealed = legacyPart(sealedBuilder); sealed.fill('late'); sealed.box([0, 0, 0], [1, 1, 1]);
  throws('missing_dependency', () => sealed.finish()); throws('invalid_input', () => sealed.translate(1, 0, 0)); sealedBuilder.material('late'); assert.equal(sealed.finish().nodes.length, 2);
  const childBuilder = builder(); const child = legacyPart(childBuilder, { resolveChild: request => { assert(Object.isFrozen(request.parameters)); return expectedRepeated.asset_dependencies[0]; } });
  child.placeChild('Pair', { width: 2 }); child.placeChild('Pair', { width: 2 });
  const children = child.finish(); assert.equal(children.asset_dependencies.length, 1); assert.equal(children.outputs.length, 2); legacyDocuments.push(children);
});
test('SDK-OWNERSHIP-v1', () => {
  const size = [1, 2, 3], params = { nested: { n: 1 } }, b = builder({ parameters: params }); b.output(b.box('box', size));
  size[0] = 5; params.nested.n = 9; assert.equal(b.build().nodes[0].parameters.size[0], 1); assert.equal(b.build().parameters.nested.n, 1);
  assert.throws(() => { b.build().parameters.nested.n = 2; }, TypeError);
});

// The core is loaded by a host that supplies modules and frozen services only.
// Browser/Node/QuickJS I/O and entropy globals are deliberately unavailable.
// An embedded host marshals documents into its own service realm, as a native
// host would serialize values across its VM boundary. The SDK has no such I/O.
const hostServices = { ...services, canonicalJson: document => helpers.canonicalJson(JSON.parse(JSON.stringify(document))) };
const context = createContext({ hostServices, Date: undefined, crypto: undefined, TextEncoder: undefined,
  fetch: undefined, process: undefined, require: undefined, Buffer: undefined, document: undefined, window: undefined, console: undefined });
const modules = new Map();
function load(url) {
  const href = url.href;
  if (!modules.has(href)) {
    assert(href.startsWith(new URL('../', import.meta.url).href), 'portable host rejects external imports');
    const code = readFileSync(url, 'utf8');
    modules.set(href, new SourceTextModule(code, { context, identifier: href }));
  }
  return modules.get(href);
}
const entry = new SourceTextModule(`import { extrudedProp, orderedCsg, repeatedAssembly } from '../examples/recipes.mjs';
  Math.random = () => { throw new Error('ambient entropy forbidden'); };
  export const canonical = [extrudedProp, orderedCsg, repeatedAssembly].map(f => f(hostServices).canonical());`, { context, identifier: import.meta.url });
await entry.link((specifier, referrer) => load(new URL(specifier, referrer.identifier))); await entry.evaluate();
test('SDK-PORTABLE-HOST-v1', () => assert.deepEqual(Array.from(entry.namespace.canonical), expectations.map(helpers.canonicalJson)));

for (const b of documents) {
  const actual = await b.digest(), expected = await helpers.contentDigest(b.build(), async bytes => new Uint8Array(createHash('sha256').update(bytes).digest()));
  assert.deepEqual(actual, expected);
}
console.log('PASS SDK-DIGEST-v1'); ++checks;
const nativeCheck = process.argv[2];
if (nativeCheck) test('SDK-NATIVE-WIRE-v1', () => {
  const all = [...documents.map(b => b.build()), ...additionalDocuments, ...legacyDocuments];
  for (const d of all) {
    const result = spawnSync(nativeCheck, d.extensions.some(e => e.capability.name === LEGACY_VOXELS.name) ? [LEGACY_VOXELS.name] : [], { input: helpers.canonicalJson(d), encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 });
    assert.equal(result.status, 0, result.stderr); assert.equal(result.stdout.trim(), helpers.canonicalJson(d));
  }
  const unsupported = spawnSync(nativeCheck, [], { input: helpers.canonicalJson(legacyDocuments[0]), encoding: 'utf8' }); assert.equal(unsupported.status, 1); assert.match(unsupported.stderr, /unsupported_capability/);
});
else console.log('Native wire check is a separate local CMake gate; executable not supplied.');
console.log(`ALL PASS: ${checks} authoring fixtures`);
