import assert from 'node:assert/strict';
import { test } from 'node:test';
import { EvaluationController, type EvaluationState, type EvaluationResult, type ProceduralEngine } from '../src/evaluation.ts';
import { recipeById } from '../src/recipes/index.ts';

const recipe = recipeById.get('rock')!.build();
const result = (elapsedMs: number): EvaluationResult => ({
  meshes: [], instances: [], bounds: { min: [0, 0, 0], max: [0, 0, 0] },
  materials: [], parts: [], lods: [], diagnostics: [], measurements: { elapsedMs, wasmHeapBytes: 0, transferredBytes: 0 },
});

test('superseded worker completion cannot publish, even when an engine ignores abort', async () => {
  const jobs: { resolve: (value: EvaluationResult) => void; signal: AbortSignal }[] = [];
  const states: EvaluationState[] = [];
  const engine: ProceduralEngine = { dispose() {}, evaluate(_recipe, options) {
    return new Promise(resolve => jobs.push({ resolve, signal: options!.signal! }));
  } };
  const controller = new EvaluationController(engine, state => states.push(state));
  const first = controller.evaluate(0, recipe), second = controller.evaluate(1, recipe);
  assert.equal(jobs[0].signal.aborted, true);
  jobs[1].resolve(result(2)); await second;
  jobs[0].resolve(result(1)); await first;
  assert.deepEqual(states.filter(state => state.kind === 'ready').map(state => state.result.measurements.elapsedMs), [2]);
  controller.dispose();
});

test('parameter invalidation clears stage cache; disposal cancels jobs and prevents publication', async () => {
  let calls = 0, disposed = 0;
  const states: EvaluationState[] = [];
  const engine: ProceduralEngine = { dispose() { disposed++; }, async evaluate() { return result(++calls); } };
  const controller = new EvaluationController(engine, state => states.push(state));
  await controller.evaluate(0, recipe); await controller.evaluate(0, recipe);
  assert.equal(calls, 1);
  controller.invalidate(); await controller.evaluate(0, recipe);
  assert.equal(calls, 2);
  for (let step = 1; step <= 4; step++) await controller.evaluate(step, recipe);
  await controller.evaluate(0, recipe); assert.equal(calls, 7, 'oldest stage evicted after four retained results');
  controller.dispose(); controller.dispose(); await controller.evaluate(1, recipe);
  assert.equal(disposed, 1); assert.equal(calls, 7);
  assert.equal(states.at(-1)!.kind, 'ready');
});

test('heavy placements request one native LOD, reuse the engine and cache the complete result', async () => {
  const heavy = result(3);
  heavy.meshes = [{ id: 'shared', indices: new Uint32Array(2400) } as EvaluationResult['meshes'][number]];
  heavy.instances = [{ meshId: 'shared' }, { meshId: 'shared' }] as EvaluationResult['instances'];
  const calls: Parameters<ProceduralEngine['evaluate']>[1][] = [];
  const states: EvaluationState[] = [];
  const engine: ProceduralEngine = { dispose() {}, async evaluate(_recipe, options) {
    calls.push(options);
    return calls.length === 1 ? heavy : { ...heavy, lods: [{ meshes: [], instances: [], bounds: heavy.bounds, ratio: 0.35 }], measurements: { ...heavy.measurements, elapsedMs: 5 } };
  } };
  const controller = new EvaluationController(engine, state => states.push(state));
  await controller.evaluate(0, recipe); await controller.evaluate(0, recipe);
  assert.equal(calls.length, 2);
  assert.deepEqual(calls[1]!.lodRatios, [0.35]);
  assert.equal(calls[0]!.signal, calls[1]!.signal);
  const last = states.at(-1)!;
  assert.equal(last.kind, 'ready');
  if (last.kind === 'ready') {
    assert.equal(last.result.measurements.elapsedMs, 8);
    assert.equal(last.result.lods.length, 1); assert.equal(last.cached, true);
  }
  controller.dispose();
});

test('a late heavy result cannot schedule LOD work after selection changes', async () => {
  const jobs: ((value: EvaluationResult) => void)[] = [];
  const engine: ProceduralEngine = { dispose() {}, evaluate() { return new Promise(resolve => jobs.push(resolve)); } };
  const controller = new EvaluationController(engine, () => {});
  const old = controller.evaluate(0, recipe);
  controller.invalidate();
  const heavy = result(1);
  heavy.meshes = [{ id: 'shared', indices: new Uint32Array(6000) } as EvaluationResult['meshes'][number]];
  heavy.instances = [{ meshId: 'shared' }] as EvaluationResult['instances'];
  jobs[0](heavy); await old;
  assert.equal(jobs.length, 1);
  controller.dispose();
});
