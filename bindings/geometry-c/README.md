# Native C and WebAssembly geometry

`matter_geometry_c` implements the frozen [C ownership contract](../../libs/MatterContracts/include/matter/contracts/abi.h)
over the real [ProceduralGeometry](../../libs/ProceduralGeometry/README.md) service.
It evaluates primitives, profiles/extrusion, ordered CSG and affine placements,
queries surfaces, and processes simplify/smooth stacks. Native and WASM use the
same sources, request codec, validation, status codes and result format. No
QuickJS, renderer, worker pool, filesystem or live world is linked.

## Native API

Include `<matter/geometry_c.h>` and link `matter_geometry_c`. The only public
types are C11 types from that header and `matter/contracts/abi.h`. Initialize
versioned structures with `{sizeof(Type), MATTER_ABI_MAJOR, MATTER_ABI_MINOR}`.
`matter_geometry_default_config` and `matter_geometry_default_budget` fill
caller-owned initialized structures. `matter_geometry_create(NULL, &api)` uses
the default config and publishes a `matter_geometry_api` function table.

```c
matter_geometry_api api = {{sizeof(api), MATTER_ABI_MAJOR, MATTER_ABI_MINOR}};
matter_budget budget = {{sizeof(budget), MATTER_ABI_MAJOR, MATTER_ABI_MINOR}};
matter_status status = matter_geometry_default_budget(&budget);
if (status == MATTER_OK) status = matter_geometry_create(NULL, &api);
/* Supply UTF-8 request bytes and a versioned matter_job_envelope. */
/* matter_call_result r = api.evaluate(api.instance, request, &job, &budget, NULL); */
/* On success: api.read(...), copy/use bytes, then api.release(...). */
if (status == MATTER_OK) matter_geometry_dispose(&api);
```

See [the standalone C consumer](tests/consumer.c) for complete calls, validation,
diagnostics, query, process and disposal. All functions translate exceptions;
failed calls return a zero result handle and publish no partial asset. Invalid
ABI sizes/versions, malformed lengths, nonfinite values, missing resources and
unknown required capabilities are explicit failures. Native callers must supply
accessible memory for structures, pointers and their claimed buffer lengths;
the ABI cannot make arbitrary native pointers safe.

Instances are thread-confined, with concurrent calls/re-entry rejected.
Independent instances may run concurrently. The opaque `api.instance` value is
a registry token, never a dereferenceable pointer. Result handles combine an
instance identity and a monotonically increasing serial; neither is reused in
that loaded library. Unknown, foreign, released and disposed handles are
rejected. Disposing via a copied function table invalidates every copy. Dispose
frees all outstanding results; a second C dispose returns `INVALID_HANDLE`.
Separate loaded libraries/WASM modules never exchange raw handles.

`read` returns immutable **borrowed** UTF-8 bytes, without a NUL terminator.
On native platforms they live until result release or instance disposal. On
WASM copy immediately before any further allocation/call, memory growth,
release, disposal or worker termination. Allocations can detach a previously
obtained heap buffer. Handles themselves remain valid across heap growth.
Never free the returned pointer. `capabilities` and `diagnostics` copy into
caller storage: `{NULL,0}` returns `RESOURCE_LIMIT` and the required byte count;
allocate that many bytes and retry. A short output buffer is left untouched.
Diagnostics describe the last evaluation/error on the instance; successful
evaluation clears them. A fixed fallback remains readable during allocation
failure. No custom allocator is offered in v1.

## Wire version 1

Requests are strict UTF-8 JSON objects. Unknown fields and duplicate keys fail.
Each request requires `binding_version: 1`. Recipes/assets use the exact
contract codec and `matter-exact-json-v1` profile; the binding does not introduce
a second geometry format. Full runnable requests are in [requests.json](tests/requests.json).

| Function | Fields besides `binding_version` |
| --- | --- |
| evaluate | `recipe`: full RecipeDocument; optional `resources`, `algorithms` |
| query | `samples`: array of `{prototype, kind, position, direction?, max_distance_metres?}`; optional `algorithms` |
| process | `required_capabilities`: capability array; `settings`: geometry modifier settings; optional `algorithms` |

Capabilities/algorithms are `{name, version}` values. Query kinds are
`closest_surface` and `ray_surface`; defaults and coordinate conventions match
the geometry service. Each resolved resource is `{id, bytes_hex}` with lowercase
hexadecimal bytes. Its length, typed payload and SHA-256 identity must match the
recipe's resource description. There is no implicit filesystem/network access.
Assembly dependencies and imported mesh operations remain unsupported by the
underlying geometry service; they are not advertised or silently substituted.

The job is supplied through the C `matter_job_envelope`, separate from the JSON
payload. Its request ID, source revision and full uint64 epoch are always echoed
in successful result wire data. Epochs use sixteen lowercase hexadecimal digits
on the wire, preserving values beyond JavaScript's exact integer range.

An asset result is `{binding_version:1, kind:"asset", job, asset, resources}`;
a query result is `{binding_version:1, kind:"query", job, samples}`. Only asset
handles are accepted by query/process. Outputs contain all material, prototype,
instance, bounds and metadata fields of the portable asset. Processing retains
the source result; the caller releases both independently. Handles retain only
the owned encoded result: query/process decode that immutable storage for their
call rather than pinning duplicate native geometry arrays indefinitely.

`capabilities` returns a JSON capability array from the geometry service plus
`matter.binding.json@1`. Retopology is unavailable; unknown algorithms,
required capabilities or versions fail explicitly. `diagnostics` returns a JSON
array with stable code, severity and message; geometry diagnostics also retain
source, node ID and dependency path.

## Limits and cancellation

Defaults are 64 live results, 64 MiB retained string capacity, 8 MiB per request,
CSG spacing 0.1 m and reference radius 0. Config limits are positive and finite;
max_results cannot exceed 65,536. The loaded library admits at most 1,024 live
instances and fails on identity/serial exhaustion. Result-count and byte limits
are independent; release recovers both. Retained-byte accounting counts string
capacity; bounded registry/map bookkeeping is controlled by instance/result
counts. This is not an allocator-wide resident-set ceiling.

The job budget retains the frozen defaults (including 5 seconds, 64 MiB working
and output storage) and may be lowered by the caller. The binding checks input
lengths before parsing and reserves `64 * wire_bytes + 65536` working bytes for
each input/request/result decode. Geometry gets the remaining working budget.
It admits a conservative JSON serialization allowance before constructing the
output tree: 256 bytes per scalar/container unit plus fixed/document overhead,
or 4096 bytes per query sample. Encoded output must separately fit output_bytes
and remaining retained capacity. This conservative v1 copy path may reject large
documents that would fit as packed binary geometry; no packed-buffer performance
claim is made. Geometry enforces its own operation/depth/vertex/index/query and
working-memory limits; there is no unbounded fallback.

Native callbacks run synchronously on the invoking thread with no registry
lock held. The cancellation callback is polled before evaluation, at geometry
progress reports (before and after the user's callback), and before publication.
Cancellation is carried to geometry's atomic token; geometry also checks its
deadline at internal checkpoints. With progress_events zero, callback polling
is limited to the call boundary. Callbacks must not throw or re-enter. Hard
interrupts are host-owned; this interface does not promise preemption.

## JavaScript worker adapter

```js
import factory from './matter_geometry_wasm.mjs'; // compiled artifact
import {createGeometry} from './geometry.mjs';    // supplied adapter
const geometry = await createGeometry(factory);
try {
  const result = geometry.evaluate(request, {
    job: {request_id: 'build.1', source_revision: 'r1', generation_epoch: 1n},
  });
  geometry.release(result);
  // result.value is parsed asset/query wire data. Typed arrays are independent
  // copies and can be transferred even after release/dispose:
  const positions = result.meshes[0].positions;
  postMessage({positions}, [positions.buffer]);
} finally { geometry.dispose(); }
```

Run the synchronous calls in an ordinary host-created worker. Initialization is
async; this module does not schedule jobs or implement a browser message
protocol. The browser integration task owns those. `AbortSignal` is checked
before a synchronous call; messages cannot interrupt that call in this private
memory profile. Terminating the worker is the host's last-resort cancellation.

`result.wire` is a JS-owned Uint8Array, `result.value` is the parsed result and
`result.meshes` holds JS-owned Float32Array positions/normals/tangents/UVs and
Uint32Array indices. These copies remain valid after native release, memory
growth and instance disposal. Editing them cannot change the retained geometry
used by query/process. Result identity is private to its wrapper instance;
foreign/released objects are rejected. JS disposal is idempotent. Explicit
release or disposal is required; dropping a result object does not free its
native handle. [geometry.d.ts](wasm/geometry.d.ts) describes the adapter API.

## Builds and verification

The local Windows wrapper uses the repository's supported MSVC resolver:

```powershell
./bindings/geometry-c/build-windows.ps1 -Config RelWithDebInfo -Test
```

Independent Linux/WSL native consumer:

```bash
cmake -S bindings/geometry-c -B /tmp/matter-geometry-c -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/matter-geometry-c -j 2
ctest --test-dir /tmp/matter-geometry-c --output-on-failure -j 1
```

The pinned WASM baseline remains Emscripten 5.0.2, revision
`dc80f645ee70178c11666de0c3860d9e064d50e4`, SDK Node 22.16.0, CMake 3.28.3,
Ninja 1.11.1 and Python 3.12.3 ([lock](wasm/toolchain.json)). Install/activate
that specific SDK outside the repository, then reproduce in an empty directory:

```bash
python3 bindings/geometry-c/wasm/build-smoke.py --emsdk /path/to/emsdk \
  --build-dir /tmp/matter-geometry-wasm \
  --native-consumer /tmp/matter-geometry-c/matter_geometry_c_tests
```

This validates tool versions, selects private EM_CACHE/SDK Node, builds serially
and runs the real binding plus four frozen contract smoke fixtures. It records
commands, exit codes and every WASM hash in `smoke-evidence.json`. Passing
`--native-consumer` enables numerical equivalence; without it the worker tests
check functional behavior but do not claim cross-target comparison. Native
tests do not use Agent Queue's Python/pytest runner.

| Fixture | Verification |
| --- | --- |
| BIND-C-OWNERSHIP | C11 version/length/status validation; foreign/stale/double release/dispose; unsupported capability; bounded output; cancellation/re-entry; repeated evaluate/release |
| BIND-ALLOCATION | Fail every allocation position in a successful evaluate/encode/publish call; zero handles on failure, resource diagnostics, final recovery |
| BIND-WASM | Native/WASM box, extrusion, sphere, shared mirrored/nonuniform placements, ordered CSG, surface query, simplify/smooth; typed arrays, cancellation/errors, cross-instance objects, forced heap growth, transfers after release/disposal |
| Frozen WASM contract fixtures | 208 value, 155 service, 7,011 fake ABI checks; numeric/SHA/RNG vectors and independent public headers |

Topology, material/prototype/instance identity, transforms and metadata compare
exactly. Geometry uses the existing `1e-6 + 1e-6 * abs(expected)` tolerance
(positions in metres); CSG normals use `2e-3 + 1e-6 * abs(expected)`. These were
selected before the binding comparison, not adjusted to observed errors.
The worker host for these tests is Node worker_threads; browser renderer,
download/latency/copy-cost budgets and live editor integration are separate tasks.
The [binding verification record](wasm/evidence/2026-10-10-bindings.json) includes
the clean WASM build, GNU/MSVC comparisons, allocation failures and sanitizers.

## Scalar portability and scope

`matter-wasm32-worker-v1` uses private wasm32 memory, no pthreads/shared memory,
SIMD, memory64, filesystem, fast math or FP contraction. Internal exceptions
remain enabled and caught at the ABI. Memory starts at 32 MiB, grows to 256 MiB,
and has a 1 MiB stack. Heap-growth failure or hard termination may invalidate
the instance. Production exports use BigInt for uint64, no main, and a persistent
runtime; smoke executables retain callMain/exit behavior. See
[Emscripten's heap-view guidance](https://emscripten.org/docs/porting/connecting_cpp_and_javascript/Interacting-with-code.html#access-memory-from-javascript).

The private [scalar adapters](wasm/ScalarHeaders.cmake) derive headers at
configure time from canonical sources: omit the unused x86 intrinsics include,
replace SIMD-unioned Tri with scalar positions/padding of the same 64-byte
layout, preserve TriEx and scalar math/matrix implementations, and cut unused
VariationRecorder/cache declarations from the emitter header. Layout assertions
and exact seam guards fail if upstream structure changes. No fake __m128,
algorithm copy, shared-header edit or SIMD enablement is used. The unchanged
geometry target compiles canonical primitives, triangulation, CSG math and MSL
indexed/simplify/smooth/transform sources. BLAS traversal, full legacy mesher,
retopology, animation, storage, renderer and script runtimes remain excluded.
The original [contract-only audit](wasm/portability-baseline.md) records why.

This is an internal module, not public SDK publication. Preserve first-party
attributions; the repository has no top-level license granting public SDK
redistribution. Excluded third-party modules retain their existing licenses;
Emscripten's toolchain is MIT/NCSA with separate component notices. The original
license inventory and smoke evidence remain in the portability baseline and
[historical smoke record](wasm/evidence/2026-10-10-smoke.json).
