import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
import {createGeometry, GeometryError} from '../wasm/geometry.mjs';

const factory = (await import(pathToFileURL(process.argv[2]).href)).default;
const box = (await import(pathToFileURL(process.argv[3]).href)).default;
let state = 0x5741534d;
function next() { state ^= state << 13; state ^= state >>> 17; state ^= state << 5; return state >>> 0; }
function failure(call, status) {
  assert.throws(call, error => error instanceof GeometryError && error.status === status);
}
const module = await factory();
const geometry = await createGeometry(async () => module, {limits: {max_results: 2}});
const other = await createGeometry(factory);
try {
  for (let iteration = 0; iteration < 24; ++iteration) {
    console.error(`seed=0x5741534d iteration=${iteration}`);
    const epoch = BigInt(next()) << 32n | BigInt(next());
    const options = {job: {request_id: `seed.${iteration}`, source_revision: 'property', generation_epoch: epoch}};
    const request = structuredClone(box);
    const dimensions = Array.from({length: 3}, () => (1 + next() % 255) / 8);
    request.recipe.nodes[0].parameters.size = dimensions;
    const first = geometry.evaluate(request, options);
    const second = geometry.evaluate(request, options);
    assert.deepEqual(first.wire, second.wire, 'deterministic output');
    assert.equal(first.value.job.generation_epoch, epoch.toString(16).padStart(16, '0'));
    assert.equal(first.value.job.request_id, options.job.request_id);
    assert.equal(first.value.job.source_revision, options.job.source_revision);
    for (let axis = 0; axis < 3; ++axis) {
      const values = Array.from(first.meshes[0].positions).filter((_, i) => i % 3 === axis);
      assert.equal(Math.max(...values) - Math.min(...values), dimensions[axis]);
    }
    failure(() => geometry.evaluate(request, options), 6); // bounded live handles
    failure(() => other.release(first), 10);
    const savedWire = first.wire.slice(), savedPositions = first.meshes[0].positions.slice();
    geometry.release(second);
    const query = {binding_version: 1, samples: [{prototype: first.value.asset.meshes[0].id,
      kind: 'closest_surface', position: [0, 0, 0]}]};
    const before = geometry.query(first, query, options);
    const expectedSamples = structuredClone(before.value.samples);
    geometry.release(before);
    // Public copies can be mutated without changing the retained native asset.
    first.meshes[0].positions.fill(999);
    first.value.asset.meshes[0].positions.forEach(position => position.fill(999));
    const queried = geometry.query(first, query, options);
    assert.deepEqual(queried.value.samples, expectedSamples, 'native query ignores mutable public copies');
    geometry.release(queried);
    geometry.release(first);
    failure(() => geometry.release(first), 10);
    assert.deepEqual(first.wire, savedWire, 'wire survives release');
    assert(savedPositions.every(Number.isFinite));
    // Guaranteed malformed JSON with varied truncation, UTF-16 and numbers.
    const malformed = ['{', '{"binding_version":1,"binding_version":1}',
      '{"binding_version":1,"recipe":"\\ud800"}', '{"binding_version":1e999}',
      JSON.stringify(request).slice(0, 1 + next() % 32)];
    failure(() => geometry.evaluate(malformed[iteration % malformed.length], options), 1);
    failure(() => geometry.evaluate(request, {...options, budget: {output_bytes: 1 + next() % 31}}), 6);
    const cancelled = new AbortController(); cancelled.abort();
    failure(() => geometry.evaluate(request, {...options, signal: cancelled.signal}), 7);
    const bad = [NaN, Infinity, -1, 1.5, 2 ** 64, '1'];
    assert.throws(() => geometry.evaluate(request, {...options, budget: {vertices: bad[iteration % bad.length]}}),
      error => error instanceof TypeError || error instanceof RangeError);
    // Recover both slots after every failure path; a leaked native handle
    // would exhaust this two-result instance on the next iteration.
    const recovered = geometry.evaluate(request, options); geometry.release(recovered);
  }
  // Raw wasm32 transport: request views are checked before touching the heap.
  const out = module._malloc(8), limits = module._malloc(72), id = module._malloc(1);
  try {
    assert.equal(module._mgw_create(2, 0, 0, out), 0);
    const token = new DataView(module.HEAPU8.buffer).getUint32(out, true);
    try {
      for (let i = 0; i < 32; ++i) {
        module._mgw_budget(limits); module.HEAPU8[id] = 0x61;
        const heap = module.HEAPU8.length;
        const ptr = heap - next() % 64, length = 65 + next() % 256;
        const status = module._mgw_call(token, 0, 0n, ptr, length, id, 1, id, 1, BigInt(next()), limits, 0, out);
        assert.equal(status, 1, 'view extending past linear memory is rejected');
        assert.equal(new DataView(module.HEAPU8.buffer).getBigUint64(out, true), 0n, 'failure publishes no handle');
      }
    } finally { assert.equal(module._mgw_dispose(token), 0); }
  } finally { module._free(out); module._free(limits); module._free(id); }
} finally { geometry.dispose(); other.dispose(); }
console.log('ALL PASS: seeded WASM adapter and raw transport properties');
