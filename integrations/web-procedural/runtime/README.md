# Browser procedural worker runtime

TypeScript module-worker scheduling around the delivered
[geometry WASM adapter](../../../bindings/geometry-c/README.md) and
[authoring SDK](../../../packages/matter-authoring/README.md). Geometry, queries,
processing and JavaScript authoring execute in private workers. This module owns
no renderer, live world, QuickJS runtime or central build manifest.

## API

Build with `tsc -p integrations/web-procedural/runtime/tsconfig.json`, then import
`dist/index.mjs`. The generated declarations remain beside the JavaScript.
Serve the repository and compiled WASM over HTTP(S), with `.wasm` served as
`application/wasm`. Worker and source module loading must be allowed by the
application's CSP. The runtime's default SDK/adapter URLs follow the repository
layout; supply explicit URLs when integrating another deployment layout.

```ts
import {ProceduralRuntime} from './dist/index.mjs';
const runtime = new ProceduralRuntime({
  geometryModuleURL: new URL('/wasm/matter_geometry_wasm.mjs', location.href),
  concurrency: 2,
  maxQueued: 32,
});
runtime.setCurrent('source.r1', 1n);
const controller = new AbortController();
const result = await runtime.submit({
  kind: 'author',
  job: {request_id: 'crate.1', source_revision: 'source.r1', generation_epoch: 1n},
  seed: 1n,
  parameters: {width: 2},
  snapshot: {
    uri: 'author://crate.mjs',
    source: `export default ({sdk, job, seed, parameters}) => {
      const b = new sdk.RecipeBuilder({
        source: {id:'crate',uri:'author://crate.mjs',revision:job.source_revision},
        seed, parameters,
      });
      return b.output(b.box('crate',[parameters.width,1,1])).build();
    }`,
  },
}, {signal: controller.signal});
// Consume JS-owned result.meshes and result.value.asset in a renderer adapter.
runtime.dispose();
// Result buffers remain valid after native release and runtime disposal.
```

`submit()` returns a promise and accepts either an authoring job or
`{kind:'evaluate', job, request:{binding_version:1, recipe, resources:[]}}`.
Inputs are copied when submitted, including queued jobs. Input arrays are never
transferred away from the caller. Request IDs must be unique among active and
queued jobs. Epochs are exact uint64 BigInt/safe integers and echo as sixteen
lowercase hex digits. All three identity fields, the internal dispatch serial
and the worker incarnation are checked before resolving a result. `setCurrent`
rejects old queued/running work with `stale_result`; call it when the application's
source snapshot changes. Previously delivered buffers retain their ownership.

An authoring snapshot is one immutable self-contained ES module exporting
`default(context)`. The host resolves sources/dependencies before submission and
uses the injected SDK to compose already authored recipes. The module receives
the SDK, explicit seed, frozen parameters/job/capabilities and contract services
with browser SHA-256. It returns a recipe or its own live geometry result.
External module discovery is outside this v1 runtime API. Authoring executes
browser JavaScript under the application's worker permissions, without a separate
JavaScript VM or sandbox. Syntax/runtime failures reject the job.

`context.evaluate(recipe, resources?)`, `query(asset, request)`, and
`process(asset, request)` run synchronously inside that worker. A generator can
evaluate a surface, batch query it, construct another recipe, and evaluate again.
The [WEB-STAGED fixture](tests/browser_tests.mjs) does exactly this. Intermediate
results are local to that job; `context.release()` can release them early. Each
job owns a fresh native geometry instance and releases all intermediate handles
on success/failure, then disposes the instance. Final wire/mesh arrays already
copy native memory; they transfer to the caller after native release. Raw native
handles and borrowed WASM heap views never cross the worker boundary.

## Limits and lifecycle

| Setting | Default | Accepted range/meaning |
| --- | --- | --- |
| concurrency | 2 | 1–8 private-memory workers; created lazily |
| maxQueued | 32 | 0–1024 pending jobs, additional to active workers |
| maxInputBytes | 8 MiB | 1 byte–64 MiB per copied job |
| timeoutMs | 10,000 | 1–60,000 ms after dispatch, including initialization; per-submit override |
| maxStages (job) | 32 | 1–512 evaluate/query/process calls |
| maxJobsPerWorker | 32 | 1–128 completions before retiring a worker realm |

The native adapter also enforces its existing finite geometry budgets. A job's
`budget` applies to each stage; the host deadline bounds the whole execution.
Queue capacity bounds retained input storage. WASM uses the pinned 32 MiB initial /
256 MiB maximum private-memory profile for each worker.

`cancel(requestId)` and an `AbortSignal` reject queued work without starting a
worker. Running cancellation, timeout, `restart(index)`, invalidation and disposal
terminate the active worker, reclaiming its module memory. Private-memory
synchronous WASM cannot process incoming cancellation messages during a call;
there is no false cooperative preemption promise. The next job lazily creates a
new worker. Normal completions reuse the module with a fresh native instance.
Periodic realm retirement bounds retention of imported Blob modules even after
URL revocation. Worker errors and message-deserialization failures also retire
the worker. `dispose()` is idempotent and rejects all pending jobs.

`RuntimeError` preserves the native status/diagnostics and stable contract error
code. Unknown required capabilities fail. The repeated-child assembly fixture
uses one shared mesh and sixteen distinct placements, including mirroring and
nonuniform scale. Pinned external `assembly.reference` dependencies remain
unsupported by the underlying geometry service; the runtime supplies no silent
substitution or dependency resolver. Processing/query capabilities match the
delivered binding, without advertising unsupported native features.

## Builds and verification

Required tools are TypeScript, the pinned Emscripten SDK for WASM builds, and
Python with Playwright plus Chromium for browser tests. Missing tools fail.
There are no new runtime npm dependencies and no public package publication.
Use the repository binding's supported clean build to verify the toolchain:

```sh
python3 bindings/geometry-c/wasm/build-smoke.py --emsdk /home/jkern/emsdk \
  --build-dir /tmp/matter-web-wasm
tsc -p integrations/web-procedural/runtime/tsconfig.json
tsc -p integrations/web-procedural/runtime/tests/tsconfig.json
cmake -S integrations/web-procedural/runtime -B /tmp/matter-web-runtime -G Ninja \
  -DMATTER_WEB_WASM_MODULE=/tmp/matter-web-wasm/matter_geometry_wasm.mjs \
  -DMATTER_WEB_TEST_PYTHON=/usr/bin/python3.12
cmake --build /tmp/matter-web-runtime --parallel 1
ctest --test-dir /tmp/matter-web-runtime --output-on-failure --parallel 1
```

The local CMake root also builds the actual WASM binding when configured with
the pinned Emscripten toolchain. Set `EM_CONFIG` to that SDK's `.emscripten`,
put its Node/Emscripten on PATH, supply
`-DCMAKE_TOOLCHAIN_FILE=/path/to/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`,
and omit `MATTER_WEB_WASM_MODULE`. This edits no central manifest.

The browser runner serves production modules and WASM from a local HTTP server,
executes real Chromium module workers and fails on page errors. Transport-race
tests additionally inject controlled worker ports into the same host admission
code. Fixture IDs cover prop/materials, shared repeated-child assembly/affine
placement, staged queries, 24 repeated ownership cycles, cancellation/restart,
deadline, queue/concurrency bounds, stale revisions/full uint64 epochs, explicit
errors/capabilities, main-thread responsiveness, disposal, realm recycling and
late-message envelope checks. Type checks exercise the emitted public API.

`browser-evidence.json` records fixture IDs, environment, elapsed time, actual
transferred buffer bytes, worker WASM heap capacity, a sampled page JS heap,
main-thread timer gaps, and SHA-256/raw/gzip sizes for production modules actually
downloaded. Gzip sizes are calculated per file; the test server sends raw bytes.
WASM capacity is allocated linear memory, not allocator peak or total browser
resident memory. The page heap sample excludes worker JS heaps. No agreed product
performance budget exists, so measurements establish a baseline rather than
claiming a performance gate. The checked-in [evidence](evidence/) records this run.
