import {ProceduralRuntime} from './runtime/index.mjs';

export class ProceduralError extends Error {
  constructor(code, message, diagnostics = []) {
    super(message); this.name = 'ProceduralError'; this.code = code;
    this.diagnostics = diagnostics.length ? diagnostics.map(d => ({code, message, ...d})) : [{code, message, severity: 'error'}];
  }
}
function failure(error, fallback = 'invalid_input') {
  return error instanceof ProceduralError ? error : new ProceduralError(error?.code ?? error?.diagnostic?.code ?? fallback,
    error?.message ?? String(error), error?.diagnostics ?? (error?.diagnostic ? [error.diagnostic] : []));
}
export function transferables(result) {
  const buffers = new Set();
  for (const level of [result, ...result.lods]) for (const mesh of level.meshes)
    for (const key of ['positions', 'normals', 'uvs', 'indices', 'materialIds', 'partIds']) buffers.add(mesh[key].buffer);
  return [...buffers];
}
export async function createProceduralEngine(options = {}) {
  try {
    let workerFactory;
    if (typeof process !== 'undefined' && process.versions?.node) {
      const {Worker} = await import('node:worker_threads');
      workerFactory = url => {
        const worker = new Worker(url);
        const port = {onmessage: null, onerror: null, onmessageerror: null,
          postMessage: message => worker.postMessage(message), terminate: () => { void worker.terminate(); }};
        worker.on('message', data => port.onmessage?.({data}));
        worker.on('error', () => port.onerror?.({preventDefault() {}}));
        worker.on('messageerror', data => port.onmessageerror?.({data}));
        worker.on('exit', code => { if (code !== 0) port.onerror?.({preventDefault() {}}); });
        return port;
      };
    }
    const runtime = new ProceduralRuntime({
      geometryModuleURL: new URL(options.moduleURL ?? './matter_geometry_wasm.mjs', import.meta.url),
      workerURL: new URL(options.workerURL ?? './worker.mjs', import.meta.url),
      geometryAdapterURL: new URL('./geometry.mjs', import.meta.url),
      concurrency: options.concurrency ?? 1, maxQueued: options.maxQueued ?? 32,
      timeoutMs: options.timeoutMs ?? 10000, workerFactory,
    });
    let sequence = 0;
    return {
      async evaluate(input, options = {}) {
        try {
          if (options.signal?.aborted) throw new ProceduralError('cancelled', 'Job was already cancelled');
          const recipe = typeof input === 'string' ? JSON.parse(input) : typeof input?.build === 'function' ? input.build() : input;
          if (recipe?.document_type !== 'recipe') throw new ProceduralError('invalid_input', 'Expected a recipe document, JSON string or RecipeBuilder');
          const modifiers = options.modifiers ?? [], lodRatios = options.lodRatios ?? [];
          if (!Array.isArray(modifiers) || modifiers.length > 32) throw new ProceduralError('invalid_input', 'At most 32 modifiers are allowed');
          if (!Array.isArray(lodRatios) || lodRatios.length > 8 || lodRatios.some(r => !Number.isFinite(r) || r <= 0 || r >= 1))
            throw new ProceduralError('invalid_input', 'At most eight LOD ratios strictly between zero and one are allowed');
          const response = await runtime.submit({kind: 'evaluate',
            job: {request_id: `evaluate.${++sequence}`, source_revision: 'package.v1', generation_epoch: 1},
            ...(options.budget === undefined ? {} : {budget: options.budget}),
            request: {recipe, modifiers, lodRatios}}, {signal: options.signal, timeoutMs: options.timeoutMs});
          return response.output;
        } catch (error) { throw failure(error); }
      },
      dispose() { runtime.dispose(); },
    };
  } catch (error) { throw failure(error); }
}
