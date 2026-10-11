import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
import {spawnSync} from 'node:child_process';
import {Worker, isMainThread, workerData, parentPort} from 'node:worker_threads';
import {createGeometry, GeometryError} from '../wasm/geometry.mjs';

if (isMainThread) {
  const [modulePath, native] = process.argv.slice(2);
  assert(modulePath, 'pass compiled module path and optionally native C consumer');
  let expected;
  if (native) {
    const result = spawnSync(native, ['--vectors'], {encoding: 'utf8', maxBuffer: 32 * 1024 * 1024});
    assert.equal(result.status, 0, result.stderr); expected = JSON.parse(result.stdout);
  }
  await new Promise((resolve, reject) => {
    let received = false;
    const worker = new Worker(new URL(import.meta.url), {workerData: {modulePath, expected}});
    worker.on('error', reject);
    worker.on('message', message => {
      if (message.error) reject(new Error(message.error));
      else { received = true; console.log(message); }
    });
    worker.on('exit', code => code === 0 && received ? resolve() : reject(new Error(`worker exit ${code}, result=${received}`)));
  });
} else {
  try {
    const requests = JSON.parse(readFileSync(new URL('requests.json', import.meta.url)));
    const factory = (await import(pathToFileURL(workerData.modulePath))).default;
    const wasmBinary = readFileSync(workerData.modulePath.replace(/\.mjs$/, '.wasm'));
    let module;
    const create = () => createGeometry(async options => { module = await factory(options); return module; }, {moduleOptions: {wasmBinary}});
    const geometry = await create(); const primaryModule = module;
    assert(primaryModule.HEAPU8.buffer instanceof ArrayBuffer);
    const job = {request_id: 'fixture.request', source_revision: 'revision.1', generation_epoch: 0xffffffffffffffffn};
    const options = {job};
    const fail = (fn, status) => assert.throws(fn, e => e instanceof GeometryError && e.status === status);
    const results = {};
    for (const name of ['box', 'extrude', 'sphere', 'placement', 'csg']) results[name] = geometry.evaluate(requests[name], options);
    results.query = geometry.query(results.box, requests.query, options);
    results.process = geometry.process(results.sphere, requests.process, options);
    assert.equal(results.box.meshes[0].indices.length, 36);
    assert.equal(results.extrude.meshes[0].indices.length, 36);
    assert.equal(results.placement.value.asset.meshes.length, 1);
    assert.equal(results.placement.value.asset.instances.length, 2);
    assert.deepEqual(results.box.value.asset.bounds, {min: [-1, -2, -3], max: [1, 2, 3]});
    assert.deepEqual(results.query.value.samples.map(x => x.hit), [true, false]);
    assert.equal(results.query.value.samples[0].distance_metres, 8);
    assert.deepEqual(results.query.value.samples[0].normal, [0, 1, 0]);
    assert(results.csg.meshes[0].indices.length > 0);
    assert(results.process.meshes[0].indices.length < results.sphere.meshes[0].indices.length);
    assert(results.process.value.asset.metadata['matter.geometry.modifiers'].length === 1);
    for (const result of Object.values(results)) assert.equal(result.value.job.generation_epoch, 'ffffffffffffffff');
    const caps = geometry.capabilities();
    for (const name of ['matter.geometry.csg.cpu.v1', 'matter.operation.mesh.smooth', 'matter.operation.mesh.simplify'])
      assert(caps.some(c => c.name === name && c.version === 1));
    assert(!caps.some(c => /retopo/.test(c.name)));

    // Established geometry tolerance, not recalibrated against observed output.
    function compare(actual, expected, path = '') {
      if (typeof expected === 'number') {
        const geometryField = /\.(positions|normals|tangents|uv_sets|bounds|position|normal|distance_metres)(\.|$)/.test(path);
        if (!geometryField) assert.equal(actual, expected, path);
        else {
          const absolute = path.startsWith('csg.') && path.includes('.normals.') ? 2e-3 : 1e-6;
          assert(Math.abs(actual - expected) <= absolute + 1e-6 * Math.abs(expected), `${path}: ${actual} != ${expected}`);
        }
      } else if (expected && typeof expected === 'object') {
        assert.deepEqual(Object.keys(actual), Object.keys(expected), path);
        for (const key of Object.keys(expected)) compare(actual[key], expected[key], `${path}.${key}`);
      } else assert.equal(actual, expected, path);
    }
    if (workerData.expected) for (const name of Object.keys(results)) compare(results[name].value, workerData.expected[name], name);

    fail(() => geometry.evaluate('{', options), 1);
    fail(() => geometry.evaluate({binding_version: 2}, options), 3);
    const unsupported = structuredClone(requests.box);
    unsupported.recipe.required_capabilities.push({name: 'matter.unknown', version: 1});
    fail(() => geometry.evaluate(unsupported, options), 2);
    const unknownAlgorithm = {...requests.box, algorithms: [{name: 'unknown', version: 1}]};
    fail(() => geometry.evaluate(unknownAlgorithm, options), 2);
    fail(() => geometry.evaluate(requests.box, {...options, budget: {vertices: 1}}), 6);
    fail(() => geometry.evaluate(requests.box, {...options, budget: {working_bytes: 1024}}), 6);
    fail(() => geometry.evaluate(requests.box, {...options, budget: {output_bytes: 32}}), 6);
    fail(() => geometry.query(results.query, requests.query, options), 1);
    const abort = new AbortController(); abort.abort();
    for (let i = 0; i < 20; ++i) fail(() => geometry.evaluate(requests.box, {...options, signal: abort.signal}), 7);
    assert.throws(() => geometry.evaluate(requests.box, {...options, budget: {vertices: -1}}), RangeError);
    assert.throws(() => geometry.evaluate(requests.box, {job: {...job, generation_epoch: 2 ** 64}}), TypeError);
    const other = await create();
    fail(() => other.query(results.box, requests.query, options), 10);
    other.dispose(); other.dispose();
    fail(() => other.evaluate(requests.box, options), 10);

    const held = results.box;
    const copy = held.meshes[0].positions.slice(); const wire = held.wire.slice();
    const oldHeap = primaryModule.HEAPU8.buffer;
    const large = primaryModule._malloc(oldHeap.byteLength + 1024 * 1024);
    assert(large, 'forced heap growth allocation');
    assert.notEqual(primaryModule.HEAPU8.buffer, oldHeap, 'memory grew');
    assert.equal(oldHeap.byteLength, 0, 'borrowed views on old memory are detached');
    assert.deepEqual(held.meshes[0].positions, copy); assert.deepEqual(held.wire, wire);
    primaryModule._free(large);
    // Handles remain valid across growth; only raw borrowed views are invalid.
    const afterGrowth = geometry.query(held, requests.query, options); geometry.release(afterGrowth);
    geometry.release(held); fail(() => geometry.release(held), 10);
    fail(() => geometry.query(held, requests.query, options), 10);
    assert.deepEqual(held.meshes[0].positions, copy); assert.deepEqual(held.wire, wire);

    const retainedHeap = primaryModule.HEAPU8.buffer.byteLength;
    for (let i = 0; i < 100; ++i) {
      const result = geometry.evaluate(requests.box, options); geometry.release(result);
      fail(() => geometry.evaluate('{', options), 1);
    }
    assert.equal(primaryModule.HEAPU8.buffer.byteLength, retainedHeap, 'bounded repeated evaluate/failure/release');
    // Disposal frees outstanding results while copied data remains transferable.
    geometry.dispose(); geometry.dispose();
    fail(() => geometry.process(results.sphere, requests.process, options), 10);
    const transfer = structuredClone(held.meshes[0].positions, {transfer: [held.meshes[0].positions.buffer]});
    assert.deepEqual(transfer, copy);
    parentPort.postMessage(`BIND-WASM: real primitives/extrusion/CSG/placement/query/modifiers, errors, cancellation, instances, growth and disposal passed; native equivalence=${!!workerData.expected}`);
  } catch (error) { parentPort.postMessage({error: error.stack}); process.exitCode = 1; }
}
