// Chromium half of the parity harness. Fetches the plan run_parity.py
// materialised (/__parity/plan.json) and records raw observations only:
// WASM step outcomes (the same wasm_steps.mjs the Node run uses), authoring
// outcomes from the portable SDK, and worker-runtime scenario observations.
// Every verdict is decided in run_parity.py against native results and the
// frozen fixtures, never here.
import {runSteps} from '../wasm_steps.mjs';
import {author} from '../authoring/author.mjs';
import * as generators from '../authoring/generators.mjs';
import * as sdk from '../../../../packages/matter-authoring/index.mjs';
import * as contracts from '../../../../libs/MatterContracts/examples/contracts.mjs';
import {ProceduralRuntime, RuntimeError} from '../../../../integrations/web-procedural/runtime/dist/index.mjs';

const MODULE_URL = '/wasm/matter_geometry_wasm.mjs';
const GENERATORS_URL = new URL('../authoring/generators.mjs', import.meta.url).href;
const sha256 = async bytes => new Uint8Array(await crypto.subtle.digest('SHA-256', bytes.slice().buffer));
const decoder = new TextDecoder('utf-8', {fatal: true});
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// Each case gets a fresh Emscripten module, exactly as the Node run does.
async function freshModule() { return (await import(MODULE_URL)).default({}); }

// The runtime loads author modules from blob: URLs, so the generators are
// imported by absolute URL and called with the same authoring context shape
// author.mjs uses (the worker's context has no supportedCapabilities).
const authorSource = name => `import * as G from ${JSON.stringify(GENERATORS_URL)};
export default ({sdk, parameters, seed, services}) =>
  G[${JSON.stringify(name)}](sdk, {parameters, seed, services, supportedCapabilities: sdk.DEFAULT_CAPABILITIES});`;
const stagedSource = query => `import * as G from ${JSON.stringify(GENERATORS_URL)};
export default ({sdk, parameters, seed, services, evaluate, query, release}) => {
  const context = {parameters, seed, services, supportedCapabilities: sdk.DEFAULT_CAPABILITIES};
  const body = evaluate(G.primitive(sdk, context));
  const sampled = query(body, ${JSON.stringify(query)});
  const hit = sampled.value.samples[0];
  if (!hit.hit) throw Error('surface query missed');
  const next = new sdk.RecipeBuilder({...context, source: {id: 'corpus.staged', uri: 'fixture://component-separation/staged',
    revision: '1', content: null, legacy_hashes: []}});
  const detail = next.box('detail', [0.25, 0.25, 0.25]);
  next.output(next.place('on.surface', detail, sdk.translation(...hit.position)));
  const result = evaluate(next.build());
  release(body); release(sampled);
  return result;
}`;
const stuckSource = 'export default () => { while (true) {} }';

const code = async promise => {
  try { await promise; return 'ok'; }
  catch (error) {
    if (error instanceof RuntimeError) return error.code;
    throw error;
  }
};
const wireText = result => decoder.decode(result.wire);
const observed = result => ({wire: wireText(result), stages: result.measurements.stages,
  wasm_heap_bytes: result.measurements.wasmHeapBytes});

async function runRuntime(plan) {
  const {envelope, late, seed, corpus, query, budget} = plan;
  const epoch = text => BigInt(`0x${text}`);
  let serial = 0;
  const job = (name, source, {revision = envelope.source_revision, generation = envelope.generation_epoch, id} = {}) => ({
    kind: 'author', snapshot: {source, uri: `fixture://parity/${name}`}, parameters: {}, seed: epoch(seed), budget,
    job: {request_id: id ?? `${envelope.request_id}.${name}.${++serial}`, source_revision: revision, generation_epoch: epoch(generation)}});
  const options = {geometryModuleURL: new URL(MODULE_URL, location.href), concurrency: 2, maxQueued: 4};
  const out = {};
  const runtime = new ProceduralRuntime(options);
  try {
    // Corpus through the worker, under the exact native envelope so the wires
    // are comparable byte for byte (request ids are only reused sequentially).
    out['R-CORPUS'] = {};
    for (const name of corpus)
      out['R-CORPUS'][name] = observed(await runtime.submit(job(name, authorSource(name), {id: envelope.request_id})));

    const staged = await runtime.submit(job('staged', stagedSource(query)));
    out['R-STAGED'] = {...observed(staged), instances: staged.value.asset.instances.map(i => ({id: i.id, local_transform: i.local_transform}))};

    const already = new AbortController(); already.abort();
    out['R-CANCEL-BEFORE'] = {code: await code(runtime.submit(job('already', authorSource('primitive')), {signal: already.signal})),
      after: observed(await runtime.submit(job('after.already', authorSource('primitive'))))};

    const abort = new AbortController();
    const running = code(runtime.submit(job('cancel.running', stuckSource), {signal: abort.signal}));
    await sleep(50); abort.abort();
    out['R-CANCEL-RUNNING'] = {code: await running, after: observed(await runtime.submit(job('after.cancel', authorSource('primitive'))))};

    const deadline = await code(runtime.submit(job('deadline', stuckSource), {timeoutMs: 100}));
    out['R-DEADLINE'] = {code: deadline, after: observed(await runtime.submit(job('after.deadline', authorSource('primitive'))))};

    const restartsBefore = runtime.stats.restarts;
    const restarted = code(runtime.submit(job('restart.running', stuckSource)));
    await sleep(50); runtime.restart(0);
    out['R-RESTART'] = {code: await restarted, restarts: runtime.stats.restarts - restartsBefore,
      after: observed(await runtime.submit(job('after.restart', authorSource('primitive'))))};

    const invalidated = code(runtime.submit(job('invalidated', stuckSource)));
    await sleep(30); runtime.setCurrent(late.source_revision, epoch(late.generation_epoch));
    const staleRunning = await invalidated;
    const staleNew = await code(runtime.submit(job('old.envelope', authorSource('primitive'))));
    const current = await runtime.submit(job('new.envelope', authorSource('primitive'),
      {revision: late.source_revision, generation: late.generation_epoch}));
    out['R-STALE'] = {running: staleRunning, submitted: staleNew, current: observed(current), current_job: current.value.job};

    const first = await runtime.submit(job('own.0', authorSource('primitive'), {revision: late.source_revision, generation: late.generation_epoch}));
    const original = [...first.meshes[0].positions];
    const heaps = [first.measurements.wasmHeapBytes];
    let identical = true, lastWire = wireText(first);
    for (let i = 1; i < 50; ++i) {
      const result = await runtime.submit(job(`own.${i}`, authorSource('primitive'), {revision: late.source_revision, generation: late.generation_epoch}));
      heaps.push(result.measurements.wasmHeapBytes);
      identical &&= result.meshes[0].positions.every((v, j) => Object.is(v, original[j]));
      lastWire = wireText(result);
    }
    const kept = await runtime.submit(job('own.kept', authorSource('primitive'), {revision: late.source_revision, generation: late.generation_epoch}));
    const transferred = structuredClone(first, {transfer: [first.wire.buffer, first.meshes[0].positions.buffer]});
    out['R-OWNERSHIP'] = {iterations: heaps.length, heaps_equal: heaps.every(h => h === heaps[0]), heap_bytes: heaps[0],
      positions_identical: identical, last_wire: lastWire,
      transfer_detached_source: first.wire.byteLength === 0 && first.meshes[0].positions.byteLength === 0,
      transfer_clone_intact: transferred.meshes[0].positions.every((v, j) => Object.is(v, original[j])),
      kept};

    const unknown = runtime.submit(job('author.unknown', authorSource('unknownCapability'), {revision: late.source_revision, generation: late.generation_epoch}));
    const assembly = runtime.submit(job('author.assembly', authorSource('repeatedAssembly'), {revision: late.source_revision, generation: late.generation_epoch}));
    out['R-AUTHOR-ERRORS'] = {unknownCapability: await code(unknown), repeatedAssembly: await code(assembly)};

    const pendingDisposal = code(runtime.submit(job('dispose.running', stuckSource, {revision: late.source_revision, generation: late.generation_epoch})));
    runtime.dispose(); runtime.dispose();
    const afterDispose = await code(runtime.submit(job('after.dispose', authorSource('primitive'), {revision: late.source_revision, generation: late.generation_epoch})));
    const keptPositions = out['R-OWNERSHIP'].kept.meshes[0].positions;
    out['R-OWNERSHIP'].kept_survives_dispose = keptPositions.byteLength > 0 && keptPositions.every((v, j) => Object.is(v, original[j]));
    delete out['R-OWNERSHIP'].kept;
    out['R-DISPOSE'] = {running: await pendingDisposal, submitted: afterDispose, stats: {...runtime.stats}};
  } finally { runtime.dispose(); }
  return out;
}

export async function runPlan() {
  const plan = await (await fetch('/__parity/plan.json')).json();
  const report = {runs: {}, authoring: {}, runtime: null,
    environment: {userAgent: navigator.userAgent, crossOriginIsolated: globalThis.crossOriginIsolated ?? false}};
  for (const run of plan.runs) report.runs[run.id] = await runSteps(await freshModule(), run.steps);
  for (const request of plan.authoring) report.authoring[request.id] = await author({sdk, contracts, generators, sha256}, request);
  report.runtime = await runRuntime(plan.runtime);
  return report;
}
