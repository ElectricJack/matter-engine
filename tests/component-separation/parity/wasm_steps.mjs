// WASM side of the parity/lifetime harness. Interprets the same JSON steps as
// native_runner.cpp over the production adapter (bindings/geometry-c/wasm/
// geometry.mjs) and returns outcome lines in the same shape, so run_parity.py
// compares the two backends field by field. Shared verbatim by Node
// (node_wasm.mjs) and Chromium (browser/parity_page.mjs); it never decides
// pass/fail. Every Geometry instance in one run shares one Emscripten module,
// as instances do inside a runtime worker.
import {createGeometry, GeometryError} from '../../../bindings/geometry-c/wasm/geometry.mjs';

const decoder = new TextDecoder('utf-8', {fatal: true});
const json = value => JSON.stringify(value);
const BUDGET_KEYS = new Set(['operations', 'vertices', 'indices', 'output_bytes', 'working_bytes',
  'execution_microseconds', 'queries', 'dependency_depth', 'progress_events']);

class StepError extends Error {}
const fail = message => { throw new StepError(message); };
function text(step, key, fallback) {
  const value = step[key] ?? fallback;
  if (typeof value !== 'string') fail(`missing string field ${key}`);
  return value;
}
function jobOf(step, key) {
  const job = step[key];
  if (!job || typeof job !== 'object') fail(`missing job ${key}`);
  if (!/^[0-9a-f]{16}$/.test(job.generation_epoch ?? '')) fail('generation_epoch must be 16 lowercase hex digits');
  return {request_id: text(job, 'request_id'), source_revision: text(job, 'source_revision'),
    generation_epoch: BigInt(`0x${job.generation_epoch}`)};
}
// Integers arrive as JSON numbers or decimal strings, as for the native runner.
function integer(value, key) {
  if (typeof value === 'string' && /^[0-9]{1,20}$/.test(value)) return BigInt(value);
  if (Number.isSafeInteger(value) && value >= 0) return BigInt(value);
  fail(`${key} must be an exact non-negative integer`);
}
// Adapter misuse (TypeError/RangeError) is reported distinctly from C statuses.
function thrown(error, extra = '') {
  if (error instanceof GeometryError) return `{"status":${error.status}${extra},"diagnostics":${json(error.diagnostics)}}`;
  if (error instanceof TypeError || error instanceof RangeError)
    return `{"status":"${error.name}"${extra},"message":${json(error.message)}}`;
  throw error;
}

export class WasmSteps {
  #module; #instances = new Map(); #results = new Map(); #snapshots = new Map();
  constructor(module) { this.#module = module; }

  async run(step) {
    try { return await this.#run(step); }
    catch (error) {
      if (error instanceof StepError) throw new Error(`wasm_steps: ${error.message}`);
      throw error;
    }
  }
  async dispose() { for (const geometry of this.#instances.values()) geometry.dispose(); this.#instances.clear(); }

  async #run(step) {
    const op = text(step, 'op');
    if (op === 'create') return this.#create(step);
    if (op === 'dispose') {
      try { this.#table(step).dispose(); return '{"status":0}'; } catch (error) { return thrown(error); }
    }
    if (op === 'capabilities') {
      try { return `{"status":0,"wire":${json(this.#table(step).capabilities())}}`; } catch (error) { return thrown(error); }
    }
    if (op === 'evaluate' || op === 'query' || op === 'process') return this.#call(op, step);
    if (op === 'release') {
      try { this.#table(step).release(this.#result(step)); return '{"status":0}'; } catch (error) { return thrown(error); }
    }
    if (op === 'snapshot') return this.#snapshot(step);
    if (op === 'verify_snapshot') return this.#verify(step);
    if (op === 'heap') return `{"status":0,"heap_bytes":${this.#module.HEAPU8.byteLength}}`;
    if (op === 'repeat') return this.#repeat(step);
    fail(`unknown op ${op}`);
  }
  async #create(step) {
    const limits = {};
    for (const [key, value] of Object.entries(step.limits ?? {})) limits[key] = Number(integer(value, key));
    try {
      this.#instances.set(text(step, 'instance'), await createGeometry(() => Promise.resolve(this.#module), {limits}));
      return '{"status":0}';
    } catch (error) { return thrown(error); }
  }
  #table(step) {
    const geometry = this.#instances.get(text(step, 'instance'));
    if (!geometry) fail(`unknown instance ${step.instance}`);
    return geometry;
  }
  #result(step) {
    if (!this.#results.has(text(step, 'result'))) fail(`unknown result ${step.result}`);
    return this.#results.get(step.result);
  }
  #call(op, step) {
    const geometry = this.#table(step), request = text(step, 'request'), job = jobOf(step, 'job');
    const budget = {};
    for (const [key, value] of Object.entries(step.budget ?? {})) {
      if (!BUDGET_KEYS.has(key)) fail(`unknown budget ${key}`);
      budget[key] = integer(value, key);
    }
    // The adapter's only cancellation is the pre-call signal; progress-driven
    // cancellation exists only on the native ABI and is never sent here.
    const cancel = text(step, 'cancel', 'none');
    if (cancel !== 'none' && cancel !== 'pre') fail(`unsupported cancel mode ${cancel}`);
    const options = {job, budget, signal: cancel === 'pre' ? {aborted: true} : undefined};
    const extra = ',"progress_events":null';
    let result;
    try {
      result = op === 'evaluate' ? geometry.evaluate(request, options)
        : geometry[op](this.#results.get(text(step, 'source')) ?? fail(`unknown result ${step.source}`), request, options);
    } catch (error) { return thrown(error, extra); }
    this.#results.set(text(step, 'as'), result);
    if (text(step, 'emit', 'wire') !== 'wire') return `{"status":0${extra},"handle":null}`;
    return `{"status":0${extra},"handle":null,"wire_bytes":${result.wire.byteLength},"wire":${decoder.decode(result.wire)}}`;
  }
  // JS-owned copies must outlive release, disposal and later heap reuse.
  #snapshot(step) {
    const result = this.#result(step);
    const copy = value => value.slice();
    this.#snapshots.set(text(step, 'as'), {result, wire: copy(result.wire), meshes: result.meshes.map(mesh => ({
      positions: copy(mesh.positions), normals: copy(mesh.normals), tangents: copy(mesh.tangents),
      uv_sets: mesh.uv_sets.map(copy), indices: copy(mesh.indices)}))});
    return `{"status":0,"wire_bytes":${result.wire.byteLength},"meshes":${result.meshes.length}}`;
  }
  #verify(step) {
    const saved = this.#snapshots.get(text(step, 'snapshot')) ?? fail(`unknown snapshot ${step.snapshot}`);
    const same = (a, b) => a.length === b.length && a.every((v, i) => Object.is(v, b[i]));
    const {result} = saved;
    const heap = this.#module.HEAPU8.buffer;
    const detached = [result.wire, ...result.meshes.flatMap(m => [m.positions, m.normals, m.tangents, m.indices, ...m.uv_sets])]
      .every(view => view.buffer !== heap);
    const intact = same(result.wire, saved.wire) && result.meshes.length === saved.meshes.length && result.meshes.every((mesh, i) => {
      const s = saved.meshes[i];
      return same(mesh.positions, s.positions) && same(mesh.normals, s.normals) && same(mesh.tangents, s.tangents) &&
        same(mesh.indices, s.indices) && mesh.uv_sets.length === s.uv_sets.length && mesh.uv_sets.every((uv, j) => same(uv, s.uv_sets[j]));
    });
    let parsed = false;
    try { parsed = JSON.stringify(JSON.parse(decoder.decode(result.wire))) === JSON.stringify(result.value); } catch { parsed = false; }
    return `{"status":0,"intact":${intact},"detached_from_heap":${detached},"value_matches_wire":${parsed}}`;
  }
  async #repeat(step) {
    const count = Number(integer(step.count, 'count'));
    if (!Array.isArray(step.steps) || step.steps.length === 0) fail('repeat requires steps');
    const outcomes = {};
    for (let i = 0; i < count; ++i) {
      for (const inner of step.steps) {
        if (inner.op === 'repeat') fail('nested repeat');
        const outcome = JSON.parse(await this.#run(inner));
        const key = `${inner.op}:${outcome.status}`;
        outcomes[key] = (outcomes[key] ?? 0) + 1;
      }
    }
    return `{"status":0,"iterations":${count},"outcomes":${json(sorted(outcomes))},"distinct_handles":null}`;
  }
}
export const sorted = object => Object.fromEntries(Object.entries(object).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0));

// Runs a step list and returns one outcome line per step.
export async function runSteps(module, steps) {
  const runner = new WasmSteps(module), lines = [];
  try { for (const step of steps) lines.push(await runner.run(step)); }
  finally { await runner.dispose(); }
  return lines;
}
