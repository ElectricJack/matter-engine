// Synchronous adapter intended to run in an ordinary host-created worker.
// Public data is copied; raw WASM pointers and borrowed heap views never escape.
const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', {fatal: true});
const budgetKeys = ['operations', 'vertices', 'indices', 'output_bytes', 'working_bytes',
  'execution_microseconds', 'queries', 'dependency_depth', 'progress_events'];
const maxU64 = (1n << 64n) - 1n;
function integer(value, max, name) {
  if (typeof value === 'number' && !Number.isSafeInteger(value)) throw new TypeError(`${name} must be an exact integer`);
  if (typeof value !== 'number' && typeof value !== 'bigint') throw new TypeError(`${name} must be an integer`);
  const n = BigInt(value);
  if (n < 0n || n > max) throw new RangeError(`${name} is out of range`);
  return n;
}
export class GeometryError extends Error {
  constructor(status, diagnostics = []) {
    super(diagnostics[0]?.message ?? `Geometry status ${status}`);
    this.name = 'GeometryError'; this.status = status; this.diagnostics = diagnostics;
  }
}
/** Instantiate a fresh modularized Emscripten module, then own its C instance.
 * moduleOptions may supply wasmBinary/locateFile. No browser/Node globals are
 * required by this adapter. Async cancellation/worker scheduling is host-owned.
 */
export async function createGeometry(factory, {moduleOptions = {}, limits = {}} = {}) {
  const module = await factory(moduleOptions);
  return new Geometry(module, limits);
}
class Geometry {
  #module; #token = 0; #handles = new Set(); #owned = new WeakMap(); #maxRequest;
  constructor(module, limits) {
    this.#module = module;
    const names = ['max_results', 'max_retained_bytes', 'max_request_bytes'];
    for (const name of Object.keys(limits)) if (!names.includes(name)) throw new TypeError(`Unknown limit: ${name}`);
    const values = names.map(name => limits[name] === undefined ? 0 : Number(integer(limits[name], 0xffffffffn, name)));
    for (let i = 0; i < names.length; ++i) if (limits[names[i]] !== undefined && values[i] === 0) throw new RangeError('Limits must be positive');
    this.#maxRequest = values[2] || 8 * 1024 * 1024;
    this.#scope(allocate => {
      const out = allocate(4);
      this.#check(module._mgw_create(...values, out));
      this.#token = this.#data().getUint32(out, true);
    });
  }
  #data() { return new DataView(this.#module.HEAPU8.buffer); }
  #scope(fn) {
    const pointers = [];
    try {
      return fn(size => {
        const p = this.#module._malloc(Math.max(1, size));
        if (!p) throw new GeometryError(6, [{message: 'WASM allocation failed'}]);
        pointers.push(p); return p;
      });
    } finally { for (const p of pointers) this.#module._free(p); }
  }
  #active() { if (!this.#token) throw new GeometryError(10, [{message: 'Geometry instance is disposed'}]); }
  #check(status) {
    if (status) {
      let diagnostics = [];
      if (this.#token) { try { diagnostics = this.#info(true); } catch {} }
      throw new GeometryError(status, diagnostics);
    }
  }
  #info(diagnostic) {
    return this.#scope(allocate => {
      const required = allocate(4);
      const first = this.#module._mgw_info(this.#token, +diagnostic, 0, 0, required);
      if (first !== 6 && first !== 0) throw new GeometryError(first);
      const length = this.#data().getUint32(required, true);
      const out = allocate(length);
      const code = this.#module._mgw_info(this.#token, +diagnostic, out, length, required);
      if (code) throw new GeometryError(code);
      return JSON.parse(decoder.decode(this.#module.HEAPU8.subarray(out, out + length)));
    });
  }
  capabilities() { this.#active(); return this.#info(false); }
  #handle(result) {
    const handle = this.#owned.get(result);
    if (handle === undefined || !this.#handles.has(handle)) throw new GeometryError(10, [{message: 'Released or foreign result'}]);
    return handle;
  }
  #call(operation, source, request, {job, budget = {}, signal} = {}) {
    this.#active();
    if (!job || typeof job.request_id !== 'string' || typeof job.source_revision !== 'string') throw new TypeError('A job envelope is required');
    const epoch = integer(job.generation_epoch, maxU64, 'generation_epoch');
    for (const key of Object.keys(budget)) if (!budgetKeys.includes(key)) throw new TypeError(`Unknown budget: ${key}`);
    const sourceHandle = operation ? this.#handle(source) : 0n;
    // Reject lossy JSON values rather than converting NaN/Infinity to null or
    // silently dropping undefined fields. BigInt belongs only in job/budgets.
    const text = typeof request === 'string' ? request : JSON.stringify(request, (_key, value) => {
      if (value === undefined || typeof value === 'function' || typeof value === 'symbol' ||
          (typeof value === 'number' && !Number.isFinite(value))) throw new TypeError('Request is not finite JSON');
      return value;
    });
    if (typeof text !== 'string') throw new TypeError('A JSON request is required');
    const bytes = encoder.encode(text);
    if (bytes.length > this.#maxRequest) throw new GeometryError(6, [{message: 'Request bytes exceed limit'}]);
    return this.#scope(allocate => {
      const put = bytes => { const p = allocate(bytes.length); this.#module.HEAPU8.set(bytes, p); return p; };
      const id = encoder.encode(job.request_id), revision = encoder.encode(job.source_revision);
      if (id.length > 65536 || revision.length > 65536) throw new GeometryError(6);
      const input = put(bytes), idPtr = put(id), revisionPtr = put(revision);
      const limits = allocate(72), out = allocate(8), view = allocate(8);
      this.#module._mgw_budget(limits);
      for (let i = 0; i < budgetKeys.length; ++i) {
        const key = budgetKeys[i];
        if (budget[key] !== undefined) this.#data().setBigUint64(limits + i * 8,
          integer(budget[key], i < 7 ? maxU64 : 0xffffffffn, key), true);
      }
      this.#check(this.#module._mgw_call(this.#token, operation, sourceHandle, input, bytes.length,
        idPtr, id.length, revisionPtr, revision.length, epoch, limits, +(signal?.aborted ?? false), out));
      const handle = this.#data().getBigUint64(out, true);
      try {
        this.#check(this.#module._mgw_read(this.#token, handle, view));
        const ptr = this.#data().getUint32(view, true), length = this.#data().getUint32(view + 4, true);
        // No WASM call/allocation between obtaining the view and copying it.
        const wire = this.#module.HEAPU8.slice(ptr, ptr + length);
        const value = JSON.parse(decoder.decode(wire));
        const meshes = (value.asset?.meshes ?? []).map(mesh => ({
          id: mesh.id, positions: new Float32Array(mesh.positions.flat()),
          normals: new Float32Array(mesh.normals.flat()), tangents: new Float32Array(mesh.tangents.flat()),
          uv_sets: mesh.uv_sets.map(uv => new Float32Array(uv.flat())), indices: new Uint32Array(mesh.indices),
        }));
        const result = {wire, value, meshes};
        this.#owned.set(result, handle); this.#handles.add(handle);
        return result;
      } catch (error) { this.#module._mgw_release(this.#token, handle); throw error; }
    });
  }
  evaluate(request, options) { return this.#call(0, null, request, options); }
  query(asset, request, options) { return this.#call(1, asset, request, options); }
  process(asset, request, options) { return this.#call(2, asset, request, options); }
  release(result) {
    this.#active(); const handle = this.#handle(result);
    this.#check(this.#module._mgw_release(this.#token, handle));
    this.#handles.delete(handle); this.#owned.delete(result);
    // JS-owned wire/typed arrays remain valid and transferable after release.
  }
  dispose() {
    if (!this.#token) return; // Idempotent host disposal; C double-dispose is an error.
    this.#check(this.#module._mgw_dispose(this.#token));
    this.#token = 0; this.#handles.clear(); this.#owned = new WeakMap();
  }
}
