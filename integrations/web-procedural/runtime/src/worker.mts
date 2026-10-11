import {PROTOCOL, RuntimeError, envelope, freeze, same} from './protocol.mjs';
import type {Reply, Start} from './protocol.mjs';
import type {AuthoringContext, ErrorCode, GeometryResult, RuntimeResult} from './types.mjs';
import type {Geometry} from '../../../../bindings/geometry-c/wasm/geometry.js';
import type {RecipeDocument} from '../../../../packages/matter-authoring/index.mjs';

interface WasmModule { HEAPU8: Uint8Array }
const host = globalThis as unknown as {
  onmessage: (event: MessageEvent<Start>) => void;
  postMessage: (reply: Reply, transfer?: ArrayBuffer[]) => void;
};
let modulePromise: Promise<WasmModule> | undefined;
let busy = false;

async function run(message: Start): Promise<void> {
  const start = performance.now();
  let geometry: Geometry | undefined;
  const live = new Set<GeometryResult>();
  let sourceURL: string | undefined;
  try {
    if (message.protocol !== PROTOCOL || message.type !== 'run' || !same(message.envelope, envelope(message.input.job)))
      throw new RuntimeError('invalid_input', 'Invalid worker protocol or envelope');
    const adapter = await import(message.config.geometryAdapterURL) as {createGeometry: (factory: () => Promise<WasmModule>) => Promise<Geometry>};
    // A worker owns one private module. Each job owns a fresh native instance.
    // Failed initialization is retryable on the next job; no foreign handles.
    modulePromise ??= import(message.config.geometryModuleURL).then(m => m.default({}) as Promise<WasmModule>);
    const module = await modulePromise;
    geometry = await adapter.createGeometry(() => Promise.resolve(module));
    const instance = geometry;
    const options = {job: message.input.job, budget: message.input.budget};
    let stages = 0;
    const call = (fn: () => GeometryResult): GeometryResult => {
      if (++stages > message.input.maxStages!) throw new RuntimeError('resource_limit', 'Stage count exceeded maxStages');
      const result = fn();
      live.add(result); return result;
    };
    const release = (result: GeometryResult) => {
      if (!live.has(result)) throw new RuntimeError('invalid_input', 'Released or foreign stage result');
      instance.release(result); live.delete(result);
    };
    let result: GeometryResult;
    if (message.input.kind === 'evaluate') result = call(() => instance.evaluate(message.input.kind === 'evaluate' ? message.input.request : {}, options));
    else {
      const input = message.input;
      const [sdk, contracts] = await Promise.all([import(message.config.sdkURL), import(message.config.contractsURL)]);
      const context: AuthoringContext = Object.freeze({
        sdk, job: freeze({...input.job}), parameters: freeze(input.parameters ?? {}), seed: input.seed ?? 1n,
        capabilities: freeze(instance.capabilities()),
        services: sdk.contractServices(contracts, async (bytes: Uint8Array) => new Uint8Array(await crypto.subtle.digest('SHA-256', bytes.slice().buffer))),
        evaluate: (recipe: RecipeDocument, resources: readonly object[] = []) => call(() => instance.evaluate({binding_version: 1, recipe, resources}, options)),
        query: (asset: GeometryResult, request: object | string) => call(() => instance.query(asset, request, options)),
        process: (asset: GeometryResult, request: object | string) => call(() => instance.process(asset, request, options)),
        release,
      });
      // Execute the exact submitted module bytes in browser JavaScript. Source
      // discovery is host-owned; modules use the injected SDK, with no ambient
      // QuickJS bindings or dependency fetch inside geometry evaluation.
      sourceURL = URL.createObjectURL(new Blob([input.snapshot.source], {type: 'text/javascript'}));
      const author = await import(sourceURL);
      if (typeof author.default !== 'function') throw new RuntimeError('invalid_input', `No default authoring function in ${input.snapshot.uri}`);
      const authored: unknown = await author.default(context);
      if (live.has(authored as GeometryResult)) result = authored as GeometryResult;
      else if (authored && typeof authored === 'object' && (authored as RecipeDocument).document_type === 'recipe')
        result = context.evaluate(authored as RecipeDocument);
      else throw new RuntimeError('invalid_input', 'Authoring must return a recipe or a live result from this job');
    }
    if (!same(message.envelope, result.value.job)) throw new RuntimeError('stale_result', 'Geometry did not echo the complete envelope');
    // Binding arrays already copy WASM storage. Release all native ownership
    // before transfer. Never transfer a module heap or an intermediate result.
    const buffers = new Set<ArrayBuffer>([result.wire.buffer as ArrayBuffer]);
    for (const mesh of result.meshes) for (const view of [mesh.positions, mesh.normals, mesh.tangents, mesh.indices, ...mesh.uv_sets])
      buffers.add(view.buffer as ArrayBuffer);
    const output: RuntimeResult = {...result, measurements: {elapsedMs: performance.now() - start,
      stages, wasmHeapBytes: module.HEAPU8.byteLength, transferredBytes: [...buffers].reduce((total, b) => total + b.byteLength, 0)}};
    for (const owned of [...live]) release(owned);
    instance.dispose(); geometry = undefined;
    host.postMessage({protocol: PROTOCOL, type: 'result', serial: message.serial, envelope: message.envelope, result: output}, [...buffers]);
  } catch (error) {
    if (!geometry) modulePromise = undefined;
    const failure = (error !== null && (typeof error === 'object' || typeof error === 'function') ? error : {message: String(error)}) as
      {code?: string; status?: number; message?: string; diagnostics?: unknown[]; diagnostic?: {code?: string}};
    const statuses: Record<number, ErrorCode> = {1: 'invalid_input', 2: 'unsupported_capability', 3: 'unsupported_version',
      4: 'missing_dependency', 5: 'cycle', 6: 'resource_limit', 7: 'cancelled', 8: 'stale_result', 9: 'backend_failure', 10: 'invalid_handle'};
    const codes: ErrorCode[] = [...Object.values(statuses), 'disposed'];
    const suppliedCode = failure.code ?? failure.diagnostic?.code;
    const code = failure.status !== undefined ? statuses[failure.status] ?? 'backend_failure' :
      codes.includes(suppliedCode as ErrorCode) ? suppliedCode as ErrorCode : 'backend_failure';
    host.postMessage({protocol: PROTOCOL, type: 'error', serial: message.serial, envelope: message.envelope,
      code, status: failure.status, message: failure.message ?? String(error),
      diagnostics: failure.diagnostics ?? (failure.diagnostic ? [failure.diagnostic] : [])});
  } finally {
    if (sourceURL) URL.revokeObjectURL(sourceURL);
    // Disposal releases every intermediate, even if authoring or a native call
    // throws. Hard termination reclaims the entire worker/module instead.
    geometry?.dispose();
  }
}
host.onmessage = event => {
  if (busy) throw new Error('Concurrent dispatch to a single worker');
  busy = true;
  void run(event.data).finally(() => { busy = false; });
};
