# Procedural build coordinator

`matter_build` is a C++17 library depending only on `matter_contracts` and the
standard library. Its public header is `matter/build/coordinator.h`. Source
resolution, authoring, geometry and optional cache lookup are injected through
the frozen v1 service interfaces. There is no filesystem, renderer, QuickJS,
thread pool or live Matter world implementation in this target.

The implementation reuses the separation established by
`MatterEngine3/src/part_graph.h`: resolve a dependency DAG, memoize shared
prototypes and evaluate children before parents through injected services.
The old engine graph remains in place until the ordered integration/retirement
tasks migrate its consumers. This target neither compiles nor copies that
engine's legacy hash, scripting, bake or cache implementation.

## Invocation and graph semantics

```cpp
matter::build::CoordinatorOptions options;
options.implementation = service_bundle_digest;
options.host_modules = {{"matter:authoring"}}; // Only if the injected host supplies it.
matter::build::Coordinator coordinator(resolver, host, geometry, options, &cache);
auto valid = coordinator.activate(job); // request ID, source revision, generation epoch
if (!valid.ok()) { /* report valid.diagnostics */ }
auto result = coordinator.build(job, request, control);
```

`build()` implements the public single-root `BuildService`. `build_graph()` takes
a frozen `GraphRequest` with explicit prototype nodes, child bindings and roots.
The host/application discovery adapter constructs that graph before calling it;
this module does not inspect JavaScript `static requires` or implicitly discover
additional prototypes during generation. A child binding names a graph node and
the asset dependency ID made available to its parent. The parent authoring host
receives child documents in `SourceSnapshot.resources.assets`, with their
verified content digests and immutable owners. Child payload resources accompany
those documents. Equal resource IDs must describe identical content and layout;
ambiguous resource IDs or asset bindings fail explicitly.

Source import graphs are separate from prototype graphs. Every source module
is validated and ordered children first, but only the snapshot entry is passed
to an authoring invocation. Utility imports need no generator entry point.
Unresolved, cyclic, duplicate, mismatched or corrupt inputs fail before geometry.
Host-provided imports must be listed explicitly in `options.host_modules`.
Their implementation bytes/version belong in `options.implementation`.

Graphs are checked with iterative three-colour DFS and a longest-path depth
check, including shared children reached by a longer later path. All declared
nodes are structurally validated, including unreachable nodes. Only nodes
reachable from requested roots are resolved/generated. Repeated roots retain
their order and graph IDs. Equivalent prototype requests share one immutable
asset owner; instance IDs and transforms inside assets are left intact.

The coordinator runs exactly one synchronous service invocation at a time.
Services are thread-confined under the v1 contract. For parallel independent
builds, the application supplies separate service/coordinator instances to its
own bounded executor. Node, source, edge, depth, operation, stage, progress,
output and cooperative time budgets bound each invocation. Per-node output
budgets are checked for generated and cached assets; the graph budget also
accounts aggregate unique output vertices, indices and encoded/payload bytes.
Source working-byte accounting is a high-water source payload bound, not a
measurement or hard limit on STL allocator overhead. Services must enforce their
own working-memory and execution budgets. Cooperative checkpoints cannot stop a
blocked or noncooperative injected service.

## Admission and lifetime

Call `activate()` before building. Every activation supersedes the previous
invocation, even when the caller reuses the same envelope. `invalidate()` and
`activate()` are thread-safe and cancel the old service token. Call `build` /
`build_graph` from only one thread per coordinator; borrowed services, requests
and callbacks must remain alive through return. `published()` is thread-safe
and returns the last successfully admitted graph owner.

Every service result must echo all three envelope fields. Checks between stages
and under the final publication lock reject obsolete completions, including a
backend that ignores cancellation. Failure, cancellation and invalidation leave
the previous published owner untouched. Caller cancellation is checked at stage
boundaries and admission; activation invalidation is additionally observable by
cooperative services during their call. External cancellation during a long
service call can therefore delay termination until that call returns, but cannot
publish a cancelled result. Consumers admitting a returned result later must
still compare it to their current envelope via `services::admit`.

Progress belongs to the coordinator: service progress callbacks are muted, and
one bounded notification is issued at each service stage plus completion. No
callback/service invocation occurs under the publication lock. Callback/backend
exceptions become `backend_failure`; an unknown required capability fails
explicitly. A failed build returns no partial graph. Immutable shared owners
retain all child/result data after the invocation; mutable aliases are forbidden
by the shared service contract.

## Cache identity and persistence

Portable keys use `matter.portable.asset.v1` and SHA-256 of the shared exact JSON
canonical profile. Identity covers:

- Actual source bytes and identities for every transitive snapshot module,
  dependency edges, resolver identity and resolved resource/asset descriptions.
- Canonical parameters, the complete uint64 seed, RNG and schema versions,
  requested capabilities, algorithm versions and processing settings.
- Child prototype keys, validation/acceptance limits and per-node budgets.
- A required `options.implementation` digest covering the injected authoring
  SDK/host, geometry and processing implementation bundle. Integrators must
  change it when those semantics change; include the QuickJS adapter's
  `quickjs_sdk_content()` when using that host.

Prototype graph IDs and job envelopes are excluded, allowing repeated requests
to reuse geometry while preserving their separate identities. Source semantic
IDs/URIs/revisions remain part of the key: renaming a source can change authored
identity and is not assumed equivalent merely because source bytes match.
Declared source hashes are verified; an incomplete resolver snapshot hash
cannot conceal a transitive byte change. Resource lengths/types/digests and
resolved assets are checked before lookup, and cached outputs are validated
against current limits before admission.

`legacy_cache_key()` accepts only `matter.source.v1` and `matter.resolved.v1`.
It hashes the exact uint64 with its legacy namespace into separate
`matter.legacy.*` cache namespaces. It never promotes old Matter hashes into
portable content digests or accesses renderer resource caches.

Builds perform cache lookup only. `GraphProduct.nodes` exposes immutable assets
and keys for an explicit persistence/export consumer after successful admission.
No child is persisted early: a cancelled or failed parent cannot leave partial
build writes. The persistence adapter owns atomic storage and current-result
checks with its application's invalidation policy; a caller must not assume an
old successful result remains current indefinitely. This library does not supply
disk persistence, transactions or an automatic cache writer.

## Build and verification

Supported Windows MSVC wrapper (uses the repository toolchain resolver):

```powershell
libs/ProceduralBuild/build-windows.ps1 -Config RelWithDebInfo -Test
```

From WSL:

```bash
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
  -NoProfile -ExecutionPolicy Bypass \
  -File "$(wslpath -w "$PWD/libs/ProceduralBuild/build-windows.ps1")" \
  -Config RelWithDebInfo -Test
```

Standalone GNU/Linux, without editor or script/geometry implementations:

```bash
cmake -S libs/ProceduralBuild -B /tmp/matter-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build /tmp/matter-build -j 2
ctest --test-dir /tmp/matter-build --output-on-failure -j 1
```

Use `BUILD_TESTING=OFF` for library-only consumption. No Node/Python/graphics SDK
is needed for this target's tests. Root manifest and live-world registration
remain owned by the ordered integration tasks.

| CTest | Fixture IDs and checks |
| --- | --- |
| `matter_build_graph` | `BUILD-GRAPH`, `BUILD-SHARED-CHILD`, `BUILD-MISSING`, `BUILD-CYCLE`, `BUILD-PROTOTYPE`: children-first diamond graph, missing/cyclic graphs, owner reuse and distinct instances |
| `matter_build_source_graph` | `BUILD-SOURCE-DAG`, `BUILD-SOURCE-MISSING`, `BUILD-SOURCE-CYCLE`, `BUILD-FROZEN-INPUT`: utility imports, explicit host imports, source identity/digest validation |
| `matter_build_cache_keys` | `BUILD-CANONICAL-KEY`, `BUILD-TRANSITIVE-KEY`, `BUILD-LEGACY-NAMESPACE`: deterministic keys, invalidation by dependencies/settings/seed/versions and namespaced legacy uint64 values |
| `matter_build_cache` | `BUILD-CACHE-HIT`, `BUILD-CACHE-VALIDATION`, `BUILD-CAPABILITY`: explicit cache store by consumer, lookup reuse, malformed/over-budget hits and unknown required capabilities |
| `matter_build_controls` | `BUILD-STALE-STAGES`, `BUILD-ENVELOPE`, `BUILD-CANCEL`, `BUILD-LATE`, `BUILD-EXCEPTIONS`: wrong envelopes at all five service stages, cancellation, deterministic concurrent invalidation, prior owner retention and exceptions |
| `matter_build_budgets` | `BUILD-OPERATIONS`, `BUILD-STAGES`, `BUILD-OUTPUT-BUDGET`, `BUILD-PROGRESS`, `BUILD-DAG-DEPTH`: bounded traversal/stages/output/progress and longest shared dependency path |

Geometry is deliberately fake in these fixtures. They establish coordinator
behavior, not production geometry, native/WASM geometry equivalence, legacy
script compatibility, renderer integration or performance acceptance.
