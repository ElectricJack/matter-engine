import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createProceduralEngine, ProceduralError, transferables} from '../dist/index.mjs';
import {RecipeBuilder, translation} from '../dist/authoring/index.mjs';

function builder(id) {
  return new RecipeBuilder({source: {id, uri: `test://${id}`, revision: '1'}, seed: 1n});
}
function samples() {
  const box = builder('box'); box.output(box.box('box', [2, 4, 6]));
  const sphere = builder('sphere'); sphere.output(sphere.sphere('sphere', 1, 12));
  const extrude = builder('extrude');
  extrude.output(extrude.extrude('extrude', extrude.polygon('outline', [[0, 0], [1, 0], [1, 1], [0, 1]]), 2));
  return [box, sphere, extrude];
}
function checkMesh(mesh) {
  assert(mesh.positions instanceof Float32Array);
  assert(mesh.indices instanceof Uint32Array);
  assert(mesh.normals.length === 0 || mesh.normals.length === mesh.positions.length);
  assert(mesh.uvs.length === 0 || mesh.uvs.length === mesh.positions.length / 3 * 2);
  assert.equal(mesh.materialIds.length, mesh.indices.length / 3);
  assert.equal(mesh.partIds.length, mesh.indices.length / 3);
  for (const i of mesh.indices) assert(i < mesh.positions.length / 3);
  for (let i = 0; i < mesh.positions.length; i++) {
    assert(Number.isFinite(mesh.positions[i]));
    assert(mesh.positions[i] >= mesh.bounds.min[i % 3] - 1e-6);
    assert(mesh.positions[i] <= mesh.bounds.max[i % 3] + 1e-6);
  }
}
const rejects = (promise, code) => assert.rejects(promise,
  e => e instanceof ProceduralError && e.code === code && e.diagnostics.length > 0);

test('three SDK recipes: real WASM counts, bounds, transferable independent buffers', async () => {
  const engine = await createProceduralEngine();
  try {
    const [box, sphere, extrude] = samples();
    const outputs = await Promise.all([
      engine.evaluate(box), engine.evaluate(sphere.build()), engine.evaluate(JSON.stringify(extrude.build())),
    ]);
    assert.deepEqual(outputs.map(r => r.meshes[0].indices.length / 3), [12, 264, 12]);
    // Canonical emitter emits one vertex per triangle corner.
    assert.deepEqual(outputs.map(r => r.meshes[0].positions.length / 3), [36, 792, 36]);
    assert.deepEqual(outputs.map(r => r.bounds), [
      {min: [-1, -2, -3], max: [1, 2, 3]},
      {min: [-1, -1, -1], max: [1, 1, 1]},
      // Canonical extrusion frame for +Z: u = cross(+Z,+Y) = -X, v = -Y.
      {min: [-1, -1, 0], max: [0, 0, 2]},
    ]);
    for (const result of outputs) {
      result.meshes.forEach(checkMesh);
      assert.equal(result.instances.length, 1);
      assert.equal(result.instances[0].meshId, result.meshes[0].id);
      assert.deepEqual(result.parts, [result.meshes[0].id]);
      assert(result.measurements.wasmHeapBytes >= 33554432);
      assert.equal(result.measurements.transferredBytes, transferables(result).reduce((n, b) => n + b.byteLength, 0));
    }
    engine.dispose();
    const before = outputs[0].meshes[0].positions.slice();
    const moved = structuredClone(outputs[0], {transfer: transferables(outputs[0])});
    assert.equal(outputs[0].meshes[0].positions.byteLength, 0);
    assert.deepEqual(moved.meshes[0].positions, before);
    console.log(JSON.stringify(outputs.slice(1).map(r => ({parts: r.parts, bounds: r.bounds, measurements: r.measurements}))));
  } finally { engine.dispose(); }
});

test('CSG, material/part identity, placements, modifiers and independent LODs', async () => {
  const engine = await createProceduralEngine();
  try {
    const b = builder('csg');
    const clay = b.material('clay', {baseColor: [1, 0, 0, 1]});
    // Small physical extent keeps the native default-spacing CSG lattice fast.
    const ball = b.sphere('ball', 0.2, 12, {materials: [clay]});
    const cut = b.place('cut', b.box('cut-shape', [0.2, 0.6, 0.6]), translation(0.16, 0, 0));
    b.output(b.csg('difference', 'subtract', [ball, cut], {materials: [clay]}));
    const csg = await engine.evaluate(b);
    assert(csg.meshes.find(m => m.id === csg.instances[0].meshId).indices.length > 0); csg.meshes.forEach(checkMesh);
    const placed = builder('placed');
    const red = placed.material('red', {baseColor: [1, 0, 0, 1]});
    const green = placed.material('green', {baseColor: [0, 1, 0, 1]});
    const shape = placed.sphere('shape', 1, 12, {materials: [red]});
    placed.output(placed.place('left', shape, translation(-2, 0, 0)), placed.place('right', shape, translation(2, 0, 0)),
      placed.box('other', [1, 1, 1], {materials: [green]}));
    const result = await engine.evaluate(placed, {modifiers: [{operation: 'mesh.smooth', iterations: 1}], lodRatios: [0.6, 0.3]});
    assert.equal(result.meshes.length, 2); assert.equal(result.instances.length, 3);
    assert.deepEqual(result.instances.slice(0, 2).map(i => [i.id, i.meshId, i.transform[3]]), [['left', 'shape', -2], ['right', 'shape', 2]]);
    for (const level of [result, ...result.lods]) {
      level.meshes.forEach(checkMesh);
      for (const mesh of level.meshes) {
        assert([...mesh.partIds].every(i => result.parts[i] === mesh.id));
        assert([...mesh.materialIds].every(i => result.materials[i].id === (mesh.id === 'shape' ? 'red' : 'green')));
      }
    }
    const count = level => level.meshes.find(m => m.id === 'shape').indices.length;
    assert(count(result.lods[0]) < count(result));
    assert(count(result.lods[1]) < count(result.lods[0]));
  } finally { engine.dispose(); }
});

test('structured errors, resource limits, queued/running cancellation and recovery', async () => {
  const engine = await createProceduralEngine();
  const [box] = samples();
  try {
    await rejects(engine.evaluate('{'), 'invalid_input');
    const bad = structuredClone(box.build()); bad.nodes[0].parameters.size = [-1, 1, 1];
    await rejects(engine.evaluate(bad), 'invalid_input');
    await rejects(engine.evaluate(box, {budget: {vertices: 1}}), 'resource_limit');
    await rejects(engine.evaluate(box, {lodRatios: [0]}), 'invalid_input');
    await rejects(engine.evaluate(box, {modifiers: [{operation: 'mesh.unknown'}]}), 'unsupported_capability');
    const unsupported = structuredClone(box.build()); unsupported.required_capabilities.push({name: 'matter.unknown', version: 1});
    await rejects(engine.evaluate(unsupported), 'unsupported_capability');
    const pre = new AbortController(); pre.abort();
    await rejects(engine.evaluate(box, {signal: pre.signal}), 'cancelled');
    const abort = new AbortController(), queued = new AbortController();
    const first = rejects(engine.evaluate(box, {signal: abort.signal}), 'cancelled');
    const second = rejects(engine.evaluate(box, {signal: queued.signal}), 'cancelled');
    queued.abort(); abort.abort(); await Promise.all([first, second]);
    assert.equal((await engine.evaluate(box)).meshes[0].indices.length, 36);
    const pending = rejects(engine.evaluate(box), 'disposed'); engine.dispose(); await pending;
    await rejects(engine.evaluate(box), 'disposed');
  } finally { engine.dispose(); }
});
