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
