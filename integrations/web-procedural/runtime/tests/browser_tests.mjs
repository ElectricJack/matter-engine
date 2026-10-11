import {ProceduralRuntime, RuntimeError} from '../dist/index.mjs';

const propSource = `export default ({sdk, job, parameters, seed}) => {
  const b = new sdk.RecipeBuilder({source:{id:'web.prop',uri:'fixture://WEB-PROP',revision:job.source_revision},seed,parameters});
  const clay = b.material('clay',{baseColor:[0.75,0.25,0.125,1],roughness:0.8});
  const p = b.polygon('profile',[[0,0],[1,0],[1,1],[0,1]]);
  return b.output(b.extrude('prop',p,parameters.depth ?? 2,{materials:[clay]})).build();
}`;
const assemblySource = `export default ({sdk,job,seed}) => {
  const b = new sdk.RecipeBuilder({source:{id:'web.assembly',uri:'fixture://WEB-ASSEMBLY',revision:job.source_revision},seed});
  const clay = b.material('clay',{roughness:0.8});
  const mesh = b.box('shared.child',[1,2,1],{materials:[clay]});
  const placed = b.repeat('child',16,mesh,i => sdk.multiply(sdk.translation(i*2,0,0), i===1 ? sdk.scale(-2,3,4) : sdk.identity()));
  return b.output(...placed).build();
}`;
const stagedSource = `export default ({sdk,job,seed,evaluate,query,release}) => {
  const build = id => new sdk.RecipeBuilder({source:{id,uri:'fixture://WEB-STAGED',revision:job.source_revision},seed});
  const b = build('stage.one'); b.output(b.box('surface',[2,2,2]));
  const surface = evaluate(b.build());
  const sampled = query(surface,{binding_version:1,samples:[{prototype:surface.meshes[0].id,kind:'ray_surface',position:[0,4,0],direction:[0,-1,0],max_distance_metres:10}]});
  const hit = sampled.value.samples[0];
  if (!hit.hit) throw Error('surface query missed');
  const next = build('stage.two'); const detail = next.box('detail',[0.25,0.25,0.25]);
  next.output(next.place('on.surface',detail,sdk.translation(...hit.position)));
  const result = evaluate(next.build());
  release(surface); release(sampled);
  return result;
}`;
const busySource = `export default async context => {
  const until = performance.now()+350;
  while(performance.now()<until) {};
  return (${propSource.replace('export default ', '')})(context);
}`;
const stuckSource = 'export default () => { while(true) {} }';

function check(condition, message) { if (!condition) throw new Error(message); }
async function rejects(promise, code) {
  try { await promise; throw new Error(`Expected ${code}`); }
  catch (error) { check(error instanceof RuntimeError && error.code === code, `Expected ${code}, got ${error.stack}`); }
}
const job = (id, source = propSource, revision = 'r1', epoch = 1n) => ({kind:'author',
  job:{request_id:id,source_revision:revision,generation_epoch:epoch},snapshot:{source,uri:`fixture://${id}`}, parameters:{depth:2}});
const options = () => ({geometryModuleURL: new URL('/wasm/matter_geometry_wasm.mjs', location.href), concurrency:2, maxQueued:3});
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

export async function runTests() {
  const passed = [], measurements = {};
  const runtime = new ProceduralRuntime(options());
  try {
    check(!('QuickJS' in globalThis), 'Unexpected QuickJS global');
    const prop = await runtime.submit(job('prop'));
    check(prop.value.kind === 'asset' && prop.meshes.length === 1 && prop.meshes[0].indices.length === 36, 'extruded prop topology');
    check(prop.value.asset.materials[0].id === 'clay' && prop.meshes[0].positions instanceof Float32Array, 'material/typed buffer');
    measurements['WEB-PROP'] = prop.measurements;
    passed.push('WEB-PROP');
    const assembly = await runtime.submit(job('assembly', assemblySource));
    check(assembly.meshes.length === 1 && assembly.value.asset.instances.length === 16, 'shared repeated assembly');
    check(assembly.value.asset.instances.every(i=>i.prototype === assembly.meshes[0].id), 'prototype sharing lost');
    check(assembly.value.asset.instances[1].local_transform[0] === -2 && assembly.value.asset.instances[1].local_transform[5] === 3, 'affine placement lost');
    measurements['WEB-ASSEMBLY'] = assembly.measurements;
    passed.push('WEB-ASSEMBLY');
    const stage = await runtime.submit(job('stage', stagedSource));
    check(stage.measurements.stages === 3 && stage.value.asset.instances[0].local_transform[7] === 1, 'evaluate-query-evaluate stage placement');
    measurements['WEB-STAGED'] = stage.measurements;
    passed.push('WEB-STAGED');
    const original = [...prop.meshes[0].positions], heaps = [];
    for (let i = 0; i < 24; ++i) {
      const result = await runtime.submit(job(`repeat.${i}`));
      check(result.meshes[0].positions.every((v,j)=>v===original[j]), 'repeated evaluation changed geometry');
      heaps.push(result.measurements.wasmHeapBytes);
    }
    check(heaps.every(v=>v===heaps[0]), 'WASM grew during repeated fixed-size evaluation');
    const transferred = structuredClone(prop, {transfer:[prop.wire.buffer,prop.meshes[0].positions.buffer]});
    check(prop.wire.byteLength === 0 && prop.meshes[0].positions.byteLength === 0 && transferred.meshes[0].positions[0] === original[0], 'host arrays were not independently transferable');
    measurements['WEB-REPEAT'] = {iterations:24,wasmHeapBytes:heaps[0],lastWasmHeapBytes:heaps.at(-1)};
    passed.push('WEB-REPEAT-OWNERSHIP');

    const already = new AbortController(); already.abort();
    await rejects(runtime.submit(job('already'), {signal:already.signal}), 'cancelled');
    const abort = new AbortController();
    const cancelled = runtime.submit(job('cancel.running', stuckSource), {signal:abort.signal});
    const observedCancel = rejects(cancelled, 'cancelled');
    await sleep(50); abort.abort(); await observedCancel;
    await runtime.submit(job('after.cancel'));
    passed.push('WEB-CANCEL-RESTART');

    const restarted = runtime.submit(job('restart.running', stuckSource));
    const observedRestart = rejects(restarted, 'cancelled');
    await sleep(50); runtime.restart(0); await observedRestart;
    await runtime.submit(job('after.restart'));
    passed.push('WEB-RESTART');
    await rejects(runtime.submit(job('timeout', stuckSource), {timeoutMs:100}), 'resource_limit');
    await runtime.submit(job('after.timeout'));
    passed.push('WEB-DEADLINE');

    // Both private-memory workers are occupied; verify bounded queue/cancellation
    // without scheduling any uncontrolled extra workers.
    const first = runtime.submit(job('bound.1', stuckSource)); const f = rejects(first,'cancelled');
    const second = runtime.submit(job('bound.2', stuckSource)); const s = rejects(second,'cancelled');
    const queue = [3,4,5].map(i=>runtime.submit(job(`bound.${i}`)));
    check(runtime.stats.active === 2 && runtime.stats.queued === 3 && runtime.stats.workers === 2,'concurrency/queue bound');
    await rejects(runtime.submit(job('bound.overflow')), 'resource_limit');
    await rejects(runtime.submit(job('bound.3')), 'invalid_input');
    const q = rejects(queue[0], 'cancelled'); runtime.cancel('bound.3'); await q;
    runtime.cancel('bound.1'); runtime.cancel('bound.2'); await Promise.all([f,s,...queue.slice(1)]);
    passed.push('WEB-BOUNDS-QUEUED-CANCEL');

    const invalidated = runtime.submit(job('invalidated',stuckSource)); const stale = rejects(invalidated,'stale_result');
    await sleep(30); runtime.setCurrent('r2',0xffffffffffffffffn); await stale;
    await rejects(runtime.submit(job('old')), 'stale_result');
    const current = await runtime.submit(job('new',propSource,'r2',0xffffffffffffffffn));
    check(current.value.job.generation_epoch === 'ffffffffffffffff', 'uint64 epoch truncated');
    passed.push('WEB-REVISION-EPOCH');

    const unsupported = job('unsupported',`export default ({sdk,job,seed}) => {
      const b = new sdk.RecipeBuilder({source:{id:'unsupported',uri:'fixture://unsupported',revision:job.source_revision},seed});
      const mesh = b.box('box',[1,1,1]); b.output(mesh);
      return {...b.build(),required_capabilities:[...b.build().required_capabilities,{name:'matter.unknown',version:1}]};
    }`,'r2',0xffffffffffffffffn);
    await rejects(runtime.submit(unsupported), 'unsupported_capability');
    await rejects(runtime.submit(job('dependency.assembly',`export default ({sdk,job,seed}) => {
      const b = new sdk.RecipeBuilder({source:{id:'external.assembly',uri:'fixture://external',revision:job.source_revision},seed});
      const dep=b.dependency({id:'external.child',content:{algorithm:'sha256',hex:'ab'.repeat(32)},schema_version:{major:1,minor:0}});
      return b.output(b.assembly('child',dep)).build();
    }`,'r2',0xffffffffffffffffn)), 'missing_dependency');
    await rejects(runtime.submit({...job('bad','export default () => { throw Error("source failed") }','r2',0xffffffffffffffffn)}), 'backend_failure');
    await rejects(runtime.submit(job('throw.null','export default () => { throw null; }','r2',0xffffffffffffffffn)), 'backend_failure');
    await runtime.submit(job('after.failure',propSource,'r2',0xffffffffffffffffn));
    const limited = {...job('stage.limit',stagedSource,'r2',0xffffffffffffffffn), maxStages:2};
    await rejects(runtime.submit(limited), 'resource_limit');
    const outputLimited = {...job('output.limit',propSource,'r2',0xffffffffffffffffn),budget:{output_bytes:1n}};
    await rejects(runtime.submit(outputLimited), 'resource_limit');
    passed.push('WEB-ERROR-RELEASE-CAPS');

    // Named main-thread responsiveness probe, after initialization. No numerical
    // product performance budget is inferred from this environment.
    const gaps = []; let previous = performance.now();
    const timer = setInterval(()=>{const now=performance.now();gaps.push(now-previous);previous=now;},5);
    const started = performance.now();
    const responsive = await runtime.submit(job('responsiveness',busySource,'r2',0xffffffffffffffffn));
    clearInterval(timer);
    check(gaps.length >= 5, 'main thread did not service timers during worker authoring');
    measurements['WEB-RESPONSIVENESS'] = {elapsedMs:performance.now()-started,timerTicks:gaps.length,
      intervalMs:5,maxTimerGapMs:Math.max(...gaps),worker:responsive.measurements,
      pageUsedJSHeapBytes:performance.memory?.usedJSHeapSize ?? null,
      scope:'Warmed 350ms worker CPU probe; page JS heap excludes worker WASM heaps; heap capacity is not allocator peak'};
    passed.push('WEB-RESPONSIVENESS');
    const last = runtime.submit(job('dispose.running',stuckSource,'r2',0xffffffffffffffffn)); const d = rejects(last,'disposed');
    const queuedDisposal = runtime.submit(job('dispose.second',stuckSource,'r2',0xffffffffffffffffn)); const dd = rejects(queuedDisposal,'disposed');
    const queued = runtime.submit(job('dispose.queued',propSource,'r2',0xffffffffffffffffn)); const dq = rejects(queued,'disposed');
    runtime.dispose(); runtime.dispose(); await Promise.all([d,dd,dq]);
    check(runtime.stats.active === 0 && runtime.stats.queued === 0 && runtime.stats.workers === 0, 'dispose retained jobs/workers');
    await rejects(runtime.submit(job('after.dispose')), 'disposed');
    check(current.meshes[0].positions.byteLength > 0, 'disposed runtime invalidated owned result');
    passed.push('WEB-DISPOSE');
  } finally { runtime.dispose(); }

  const recycle = new ProceduralRuntime({...options(),concurrency:1,maxJobsPerWorker:2});
  try {
    await recycle.submit(job('realm.1')); await recycle.submit(job('realm.2'));
    check(recycle.stats.workers===0 && recycle.stats.restarts===1,'module realm not retired');
    await recycle.submit(job('realm.3')); check(recycle.stats.workers===1,'retired realm did not restart');
    passed.push('WEB-REALM-RECYCLE');
  } finally {recycle.dispose();}

  // Deterministic message delivery races are tested at the actual host admission
  // boundary. Real workers above exercise transport, WASM and termination.
  const ports = [];
  class FakeWorker {
    onmessage=null; onerror=null; onmessageerror=null; terminated=false; messages=[];
    constructor(){ports.push(this);}
    postMessage(m){this.messages.push(m);}
    terminate(){this.terminated=true;}
    result(message, changes={}){
      const e={...message.envelope,...changes};
      this.onmessage?.({data:{protocol:1,type:'result',serial:message.serial,envelope:e,
        result:{wire:new Uint8Array(),meshes:[],value:{binding_version:1,kind:'asset',job:e},measurements:{}}}});
    }
  }
  const fake = new ProceduralRuntime({...options(),concurrency:1,maxQueued:2,workerFactory:()=>new FakeWorker()});
  try {
    await rejects(fake.submit({...job('invalid.epoch'),job:{request_id:'invalid.epoch',source_revision:'r1',generation_epoch:2**60}}),'invalid_input');
    await rejects(fake.submit({kind:'evaluate',job:job('nonfinite').job,request:{value:Infinity}}),'invalid_input');
    const small=new ProceduralRuntime({...options(),maxInputBytes:10,workerFactory:()=>new FakeWorker()});
    await rejects(small.submit(job('large')),'resource_limit'); small.dispose();
    const mutable = job('copy'); const original = mutable.snapshot.source;
    const one = fake.submit(mutable); mutable.snapshot.source='changed';
    check(ports[0].messages[0].input.snapshot.source===original,'source snapshot borrowed');
    const oldCallback=ports[0].onmessage, oldMessage=ports[0].messages[0];
    const cancelled=rejects(one,'cancelled'); fake.cancel('copy'); await cancelled;
    const next=fake.submit(job('copy'));
    const copiedJob=job('copy.queued'); copiedJob.parameters.depth=4;
    const copied=fake.submit(copiedJob); copiedJob.parameters.depth=99;
    oldCallback({data:{protocol:1,type:'result',serial:oldMessage.serial,envelope:oldMessage.envelope,result:{value:{job:oldMessage.envelope}}}});
    check(fake.stats.active===1 && fake.stats.rejectedLateMessages===1,'late terminated worker admitted');
    ports[1].result(ports[1].messages[0]); await next;
    check(ports[1].messages[1].input.parameters.depth===4,'queued parameters borrowed');
    ports[1].result(ports[1].messages[1]); await copied;
    for (const [field,value] of [['request_id','foreign'],['source_revision','foreign'],['generation_epoch','0000000000000002']]) {
      const p=fake.submit(job(`mismatch.${field}`)); const rejected=rejects(p,'stale_result');
      const port=ports.at(-1); port.result(port.messages.at(-1),{[field]:value}); await rejected;
    }
    const nested=fake.submit(job('nested')); const n=rejects(nested,'stale_result');
    const port=ports.at(-1), message=port.messages.at(-1);
    port.onmessage({data:{protocol:1,type:'result',serial:message.serial,envelope:message.envelope,
      result:{value:{job:{...message.envelope,source_revision:'nested.foreign'}}}}});
    await n;
    check(fake.stats.rejectedLateMessages===5,'not all envelope mismatches rejected');
    passed.push('WEB-LATE-MESSAGE-ADMISSION');
    passed.push('WEB-INPUT-SNAPSHOT-LIMITS');
  } finally {fake.dispose();}
  return {passed, measurements};
}
