import {createGeometry} from './geometry.mjs';
import {PROTOCOL, same} from './runtime/protocol.mjs';
import {transferables} from './index.mjs';

const node = typeof process !== 'undefined' && !!process.versions?.node;
const port = node ? (await import('node:worker_threads')).parentPort : globalThis;
let modulePromise;
const statuses = {1: 'invalid_input', 2: 'unsupported_capability', 3: 'unsupported_version',
  4: 'missing_dependency', 5: 'cycle', 6: 'resource_limit', 7: 'cancelled',
  8: 'stale_result', 9: 'backend_failure', 10: 'invalid_handle'};

function level(result, parts, materials) {
  const asset = result.value.asset;
  const metadata = new Map(asset.meshes.map(m => [m.id, m]));
  return {
    bounds: asset.bounds,
    instances: asset.instances.map(i => ({id: i.id, partId: i.id, meshId: i.prototype, transform: i.local_transform})),
    meshes: result.meshes.map(m => {
      const meta = metadata.get(m.id);
      const materialIds = new Uint32Array(m.indices.length / 3);
      const groups = meta.submeshes.map(s => {
        const materialIndex = materials.findIndex(material => material.id === s.material);
        if (materialIndex < 0) throw new Error(`Unresolved material ${s.material}`);
        materialIds.fill(materialIndex, s.first_index / 3, (s.first_index + s.index_count) / 3);
        return {start: s.first_index, count: s.index_count, materialIndex};
      });
      const partIndex = parts.indexOf(m.id);
      if (partIndex < 0) throw new Error(`Unresolved part ${m.id}`);
      return {id: m.id, positions: m.positions, normals: m.normals,
        uvs: m.uv_sets[0] ?? new Float32Array(), indices: m.indices,
        materialIds, partIds: new Uint32Array(m.indices.length / 3).fill(partIndex), groups, bounds: meta.bounds};
    }),
  };
}
function processRequest(asset, modifiers) {
  return {binding_version: 1,
    required_capabilities: [...new Set(modifiers.map(m => `matter.operation.${m.operation}`))].map(name => ({name, version: 1})),
    settings: {regions: [{prototypes: asset.meshes.map(m => m.id), modifiers}]}};
}
async function run(message) {
  const start = performance.now();
  let geometry;
  try {
    if (message.protocol !== PROTOCOL || message.type !== 'run' || message.input.kind !== 'evaluate')
      throw new TypeError('Invalid package worker request');
    modulePromise ??= import(message.config.geometryModuleURL).then(m => m.default({}));
    const module = await modulePromise;
    geometry = await createGeometry(() => Promise.resolve(module));
    const options = {job: message.input.job, budget: message.input.budget};
    const {recipe, modifiers, lodRatios} = message.input.request;
    let base = geometry.evaluate({binding_version: 1, recipe}, options);
    if (modifiers.length) {
      const processed = geometry.process(base, processRequest(base, modifiers), options);
      geometry.release(base); base = processed;
    }
    if (!same(message.envelope, base.value.job)) throw new Error('Geometry returned a mismatched job envelope');
    const parts = base.meshes.map(m => m.id), materials = base.value.asset.materials;
    const output = {...level(base, parts, materials), parts, materials, lods: [], diagnostics: [],
      measurements: {elapsedMs: 0, wasmHeapBytes: 0, transferredBytes: 0}};
    for (const ratio of lodRatios) {
      const lod = geometry.process(base, processRequest(base, [{operation: 'mesh.simplify', ratio}]), options);
      output.lods.push({...level(lod, parts, materials), ratio});
      geometry.release(lod);
    }
    const job = base.value.job;
    geometry.dispose(); geometry = undefined;
    const buffers = transferables(output);
    output.measurements = {elapsedMs: performance.now() - start, wasmHeapBytes: module.HEAPU8.byteLength,
      transferredBytes: buffers.reduce((sum, b) => sum + b.byteLength, 0)};
    // The reused scheduler validates the native job envelope. Native handles,
    // JSON geometry copies and module memory never leave this worker.
    port.postMessage({protocol: PROTOCOL, type: 'result', serial: message.serial, envelope: message.envelope,
      result: {value: {kind: 'asset', job}, output}}, buffers);
  } catch (error) {
    if (!geometry) modulePromise = undefined;
    const code = statuses[error.status] ?? (error instanceof TypeError || error instanceof RangeError ? 'invalid_input' : 'backend_failure');
    port.postMessage({protocol: PROTOCOL, type: 'error', serial: message.serial, envelope: message.envelope,
      code, message: error.message ?? String(error), status: error.status, diagnostics: error.diagnostics ?? []});
  } finally { geometry?.dispose(); }
}
if (node) port.on('message', message => { void run(message); });
else port.onmessage = event => { void run(event.data); };
