# Portable QuickJS authoring host

`matter_host_quickjs` implements the delivered `matter::services::AuthoringHost`.
Its public factory is in `include/matter/portable_host/quickjs_host.h`. The only
link dependencies are `matter_contracts` and the existing vendored QuickJS.
Each synchronous invocation owns a fresh runtime and returns an immutable,
validated `RecipeDocument` or diagnostics. Public headers expose no VM values.
The result survives destruction of the runtime, host and input snapshot.

This is the facade for the later ScriptHost/build-coordinator integration. No
central source manifest, ScriptHost/DslState, world, bake, export, cache or
renderer entrypoint is changed. The host does not evaluate geometry, write
artifacts, create graphics resources, discover modules, or access files/network.

## Invocation

Construct the adapter with `make_quickjs_host()` and call `author(envelope,
snapshot, request, budget, control)` through the neutral service interface.
The snapshot entry must default-export a function accepting the context below:

```js
import { RecipeBuilder } from 'matter:authoring';
export default context => {
  const b = new RecipeBuilder(context);
  b.output(b.box('crate', [context.parameters.width, 1, 1]));
  return b.build();
};
```

The context owns deeply frozen `source`, `parameters`, `limits` and
`supportedCapabilities`, the requested uint64 `seed` as BigInt, and explicit
`services` for the shared canonical serializer, SplitMix64 and SHA-256. It is
compatible with the SDK's `RecipeBuilder` options. Parameter schema/default
normalization remains the generator's responsibility. The request's required
capabilities are checked before execution; the returned recipe is checked again
against the configured consumer capabilities. Unknown required capabilities fail.

`QuickJsOptions.validation` selects the recipe capabilities the downstream
consumer actually supports. The default has only the core contract capabilities.
Legacy voxel CSG must explicitly opt into `matter.legacy.voxel-meshing@1`; its
spacing/smoothing envelope is preserved. This declaration does not implement a
voxel evaluator. The SDK's existing `legacyPart()` maps selected verbs, matrices,
material IDs and pinned child dependencies. The integration owner supplies any
legacy syntax wrapper and discovery step; unsupported verbs remain explicit.
`SourceSnapshot.resources` stays with the coordinator/geometry consumer: this
host executes source and returns resource references, without consuming asset or
buffer payloads.

## Frozen sources and identity

- Modules use unique canonical semantic IDs, e.g. `parts/tree/main.mjs`, and
  separate source URIs/revisions for diagnostics. IDs cannot contain empty, `.`
  or `..` path segments. Supplied source must be UTF-8 without NUL bytes.
- Exact-ID imports and relative `./`/`../` imports resolve in that namespace.
  Every supplied import target must appear in the importing module's
  `dependencies`. Undeclared, missing and escaping imports fail with a dependency
  path; a caught missing import still invalidates the invocation. There is no
  extension search, filesystem fallback, URL fetch or Node package resolver.
- `matter:authoring` is an implicit SDK import. `matter-internal/` is reserved.
  CMake embeds the source-of-truth SDK and shared contract helper with explicit
  regeneration dependencies. No copied algorithm implementation is maintained.
- Discovery must provide an acyclic module graph. The entire snapshot is checked,
  including unused declarations; the host bounds module count, total source bytes
  (including the embedded SDK), and longest dependency path before VM execution.
- Optional per-module content digests must equal SHA-256 of the supplied UTF-8
  source. The snapshot digest is a validated opaque coordinator identity; this
  host does not invent its aggregate encoding. Include `quickjs_sdk_content()`
  and the host version (`matter.host.quickjs.v1`) in coordinator cache keys as
  well as transitive sources, request parameters/seed and processing versions.

Top-level await, async entry functions and dynamic imports work when they settle
using the supplied modules and VM job queue. A pending promise with no queued
work fails explicitly: no timer, network or external event loop is installed.
Uncaught jobs and unhandled rejections fail the invocation. Each runtime has its
own globals and modules. Clock, ambient randomness, blocking Atomics, Node and
browser facilities are absent; RNG comes from the explicit SDK services.

## Execution controls and diagnostics

`JobControl` cancellation is checked before work, at VM interrupt polls, between
jobs/module stages and before success. The steady-clock deadline includes setup,
execution, serialization and native validation. Infinite source initialization,
generator loops and self-scheduling promise chains are interrupted. A final
cancelled/failed result never exposes a partial recipe. Progress is synchronous
and bounded; callback exceptions become `backend_failure`.

`ResourceBudget.operations` counts host checkpoints and QuickJS interrupt polls,
not individual JavaScript instructions. It also lowers recipe node/value ceilings.
`working_bytes` bounds admitted source bytes plus VM allocation requests and
allocator headers; `stack_bytes` separately bounds the VM's native stack.
Native conversion/validation storage has bounded document/value/string limits
but is not an aggregate operating-system RSS cap. `output_bytes` bounds canonical
wire output; vertex/index/resource ceilings constrain recipe declarations.
These are cooperative, in-process controls, not a hostile-code security boundary
or a preemptive process deadline. Large native built-ins/decoders may finish a
bounded unit before observing cancellation; use process isolation when required.

OOM is tracked by the host allocator even if QuickJS cannot allocate an Error.
The pinned VM also needs an exception ownership fix: a second OOM during
`build_backtrace` can free the pending error that the function still borrows.
`quickjs-compat.cmake` retains/releases that value in a generated build-directory
translation unit, after checking the exact vendor source SHA-256. No vendor file
is edited by that transformation. The vendor also carries the parser and compiler bytecode
OOM fixes documented in `third_party/quickjs-ng/MATTER_PATCHES.md`; its emitter
regression injects allocation failures during parsing and checks
cleanup and runtime reuse. The public compiler regression also sweeps later
passes and function initialization, including malloc/calloc failures, and compares
successful bytecode with the fault-free baseline. The backtrace regression exercises array and Error
allocation pressure.
SDK diagnostic codes, node IDs, source locations and dependency paths survive the
boundary. Syntax/runtime stacks map module IDs back to supplied URIs and their
line/column when available; missing VM locations are reported as zero, not guessed.
A strict plain-data copy rejects accessors, holes, undefined, closures, invalid
Unicode, nonfinite numbers and cycles before canonical encoding/native decoding.

The vendored QuickJS 0.10.0 has two BigInt defects exercised by the RNG corpus:
`BigInt.asUintN(64, 0xffffffffffffffffn)` yields `-1n`, and positive binary-radix
formatting can retain a leading zero sign limb. `setup.js` corrects these two
runtime intrinsics locally. The portable SDK, RNG algorithm, contracts and vendor
sources remain unchanged. Frozen native words/unit values and Node parity test
the resulting behavior, including high-bit seeds and RNG snapshot/restore.

## Build and verify

The local Windows wrapper uses the same supported repository resolver and MSVC
environment as the product wrapper, while configuring only this owned module:

```powershell
MatterEngine3/src/script/portable_host/build-windows.ps1 -Config RelWithDebInfo -Test
```

From WSL, at the repository root:

```sh
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
  -NoProfile -ExecutionPolicy Bypass \
  -File "$(wslpath -w "$PWD/MatterEngine3/src/script/portable_host/build-windows.ps1")" \
  -Config RelWithDebInfo -Test
```

The independent Unix portability check is:

```sh
cmake -S MatterEngine3/src/script/portable_host -B /tmp/matter-portable-host \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/matter-portable-host --parallel 2
ctest --test-dir /tmp/matter-portable-host --output-on-failure -j 1
```

For allocation-failure and lifetime regressions, configure a separate build with
`-DCMAKE_C_FLAGS='-fsanitize=address -fno-omit-frame-pointer'` and
`-DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer'`, then run the same
build and CTest commands. CTest runs the controls, parity, parser OOM and compiler OOM suites.
Enable leak checking with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`.

`BUILD_TESTING=ON` requires Node; no packages are installed. Library-only builds
need no Node. A parent can use `add_subdirectory` and supply existing
`matter_contracts`/`matter_quickjs` targets. A supplied QuickJS target must carry
the equivalent backtrace retention fix and explicitly set target property
`MATTER_QUICKJS_BACKTRACE_OWNERSHIP_FIXED` to true; configuration fails otherwise.
The later central build owner should adopt this two-line fix (or an audited
vendor update) before linking the facade with the live ScriptHost. This avoids
silently linking the unsafe shared VM or a second copy of its symbols.
Tests are local, CPU-only and serial.

| Fixture | Evidence |
| --- | --- |
| `HOST-SDK-legacyProp`, `legacyCsg`, `legacyChildren` | Exact native/Node recipe equality: materials, mirrored/nonuniform placement, ordered subtraction and meshing settings, repeated pinned child sharing |
| `HOST-SDK-seeded` | All 32 frozen uint64 RNG words and unit draws, four seeds, native/Node recipe equality |
| `HOST-SDK-digest` | Async digest service and exact canonical output across hosts |
| `HOST-SDK-extrudedProp/orderedCsg/repeatedAssembly-frozen-v1` | Independent existing SDK/contract document expectations |
| `HOST-OWNERSHIP-v1` | Results and envelope survive runtime destruction; repeat success/failure cleanup |
| `HOST-SNAPSHOT-IMPORTS-v1` | Frozen edge enforcement, missing imports, cycles, namespace escape, duplicate IDs, source digest mismatch |
| `HOST-SOURCE-DIAGNOSTICS-v1` | Syntax/runtime/SDK locations, unknown capabilities, malformed values |
| `HOST-ASYNC-v1` | Top-level await, async digest, dynamic import failure, pending promise, unhandled rejection |
| `HOST-INTERRUPT-DEADLINE-v1` | Pre-cancel, active cancellation, module/generator/promise loop deadlines, final cancellation, callback failure |
| `HOST-RESOURCE-LIMITS-v1` | Source/heap/output/work/stack ceilings and OOM classification |
| `HOST-OOM-BACKTRACE-OWNERSHIP-v1` | Repeated allocation failure while constructing errors at multiple heap limits; run under AddressSanitizer |
| `matter_host_quickjs_parser_oom` | Parser allocation failures, partial bytecode cleanup, and reuse of the same runtime after failure; run under AddressSanitizer |
| `matter_host_quickjs_compiler_oom` | Public compile-only allocation sweeps, executable bytecode equivalence, runtime recovery, and allocator balance; run under AddressSanitizer |
| `HOST-NO-AMBIENT-SERVICES-v1` | Fresh globals, no file/network/browser/clock interfaces, explicit RNG |
| `HOST-BIGINT-CONFORMANCE-v1` | Unsigned truncation at 0/1/32/63/64/65/128 bits, hexadecimal formatting, single width conversion and invalid-width rejection |

This proves authoring-only recipe equivalence. Geometry, rendering, live legacy
ScriptHost integration and performance qualification belong to their later tasks.
