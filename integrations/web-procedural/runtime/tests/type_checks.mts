import {ProceduralRuntime, RuntimeError} from '../dist/index.mjs';
import type {AuthoringContext, RuntimeJob} from '../dist/index.mjs';
const runtime = new ProceduralRuntime({geometryModuleURL: new URL('https://example.invalid/geometry.mjs'), concurrency: 2});
const job: RuntimeJob = {kind: 'author', job: {request_id: 'typed', source_revision: 'r1', generation_epoch: 1n},
  snapshot: {source: 'export default () => ({})', uri: 'author://typed'}, seed: 1n, budget: {output_bytes: 1024n}};
const promise = runtime.submit(job, {signal: new AbortController().signal});
promise.then(result => { const positions: Float32Array = result.meshes[0].positions; const epoch: string = result.value.job.generation_epoch; void positions; void epoch; });
function staged(context: AuthoringContext) {
  const b = new context.sdk.RecipeBuilder({source:{id:'typed',uri:'author://typed',revision:'r1'},seed:context.seed,services:context.services});
  b.output(b.box('box',[1,1,1]));
  const geometry = context.evaluate(b.build());
  context.query(geometry,{binding_version:1,samples:[]}); context.release(geometry);
  // @ts-expect-error worker-local geometry objects cannot be replaced by handles
  context.release(123n);
  // @ts-expect-error authoring input parameters are immutable
  context.parameters.width = 3;
}
// @ts-expect-error every job carries a source revision
runtime.submit({kind:'evaluate',job:{request_id:'missing',generation_epoch:1n},request:{}});
// @ts-expect-error job kinds are explicit
runtime.submit({...job,kind:'query'});
const error: RuntimeError = new RuntimeError('cancelled','cancelled'); void error; void staged;
runtime.dispose();
