# Public service contracts v1

Task `clear-crest-48.1.3` supplies interfaces and boundary checks above the neutral
data schema. It supplies **no production evaluator, renderer, terrain algorithm,
JavaScript host or exported C geometry implementation**. Fake services and
interface-only targets are excluded from extraction, visual and performance
acceptance. The delivered shared contract gate `.1.4` remains prerequisite to tracks.

## Public boundaries

All headers live under `include/matter/contracts/` and compile independently.
`matter::contracts` contains serializable data; `matter::services` contains native
service contracts. Only C++17 and neutral MathLib headers cross this boundary.

| Header | Boundary |
| --- | --- |
| `jobs.h` | Job envelope, immutable results, admission, cancellation, budgets and progress |
| `generation.h` | Owned resolved inputs/outputs, evaluate/query/process, batched queries |
| `hosts.h` | Frozen source snapshot, resolver, authoring host, cache, injected build service |
| `graphics.h` | Owned uploads, ticket polling, scene/camera/lights, observations and release |
| `terrain.h` | Field/region request, portable product, boundary/detail envelopes and admission |
| `abi.h` | Pure C versioned table, statuses, opaque handles and explicit ownership |

The coordinator supplies nonempty bounded request ID and source revision strings
and a nonzero generation epoch. Services echo all three. `admit` compares all
three and leaves the previous consumer owner unchanged on failure or mismatch.
Algorithms do not read live world state. Epochs change on invalidation/retries;
matching only the revision cannot admit a late result.

## Ownership, threads and budgets

`JobResult<T>` publishes a `shared_ptr<const T>` only with a valid envelope and
no error diagnostic. Failure/cancellation holds no product. Builders relinquish
mutable aliases before publication; copying a result retains storage and dropping
the last owner releases it. Success factories enforce envelope/error invariants;
services also validate complete semantic outputs. `finish_asset` supplies the
shared geometry return check.

`AssetProduct` wraps the unchanged `AssetDocument` plus owned payloads. Resource
descriptions must match actual bytes, typed lengths and SHA-256 identities. Typed
payloads use little-endian scalars and finite float32 values. `asset_owner` aliases
the document while retaining the complete product; `upload_request` carries the
bytes to graphics. Query/process consume complete products and caches retain them.
This wrapper does not change the existing asset JSON/schema/identity.

Synchronous inputs are borrowed through return. Instances are confined to the
invoking thread unless an implementation explicitly permits concurrent calls.
Cancellation sources may be signaled from another thread. Async wrappers retain
inputs, control and captures through terminal completion, releasing them on every
success, failure, cancellation and disposal path.

Progress runs inline on the invoking thread, never under service locks; no
re-entry into that service. Async hosts marshal UI notifications separately.
Progress has no committable partial asset. `JobMonitor` bounds event count,
fraction and stage length; callback exceptions become `backend_failure`.
Implementations likewise translate exceptions at service boundaries.

Budgets bound operations, vertices, indices, output/working bytes, dependency
depth, query count, elapsed microseconds and progress events. Numeric ceilings
are nonzero; zero progress events disables notifications. Usage is cumulative
except current working-memory/depth values. Checkpoints surround bounded work
units and precede return. Cancellation/time limits are cooperative, not hard
process deadlines. Worker termination invalidates that worker's handles.
Output accounting includes canonical asset bytes and payload bytes; it is a
portable byte ceiling, not allocator or peak-process-memory instrumentation.

## Capability and consumer obligations

Required capability/algorithm name and version must match exactly; unknown pairs
fail with `unsupported_capability`. Incompatible schema/ABI versions fail
explicitly. Optional metadata cannot change geometry semantics.

| Service | Obligation |
| --- | --- |
| Geometry | Validate recipe/product, exact algorithms, resources and budgets |
| Resolver/host | Frozen transitive snapshot, explicit capabilities and execution limits; no ambient SDK globals |
| Cache/build | Keys include transitive sources/parameters/seed/versions/settings; atomic explicit stores after current-result admission; separate renderer keys |
| Graphics | Validate appearance/representation capabilities, accept imported/synthetic assets, report demand to coordinator |
| Terrain | Require `matter.terrain.generate@1` and declared boundary policy versions; validate field/region/detail and resolved inputs |

Graphics never invokes generation or resolves modules. It retains upload inputs
until completion. Pending uploads have no usable resource; polling makes
completion threading explicit. Scene transactions are atomic with increasing
scene revisions. Removing one instance retains other instances' shared resource.
Resource release drops caller ownership while scene references and device work
retain storage through retirement. Ticket release cancels future publication and
retires inputs safely. Foreign/stale/repeated releases fail. The fake exercises
these ownership rules without GPU execution.

Service `TerrainProduct` is distinct from the existing serialized
`contracts::TerrainProduct`. It echoes field digest, job, region coordinates,
high precision origin, cell counts and detail, and owns geometry plus optional
field/collision/seam products. CPU lifetimes are independent of graphics residency.
V1 boundary envelopes name six axis-aligned neighbor faces, local/neighbor
spacing, exact policy version and optional seam resource. Neighbor int32 arithmetic
is checked. Positive sample domains have uniform spacing, compared against
domain-derived spacing with absolute/relative `1e-5` metre tolerance. Echoed
identities/descriptors match exactly. Products match the requested faces and
carry named seam bytes. `admit_terrain` checks both outer and product envelopes.
This does not prove mesh continuity; algorithm-specific seams/transitions belong
to the terrain track's versioned policy.

## C binding requirements

`abi.h` fixes uint32 statuses, uint64 lengths/handles and size/version-tagged
aggregate requests/control/budgets/results/provider table. Scalar byte-view pairs
belong to that table version. V1 accepts major 1, minor 0, at least the v1 size;
additive tails need declared versions/features and incompatible meanings need
a new major. No STL, exceptions, script values or device types cross the ABI.

Providers expose synchronous evaluate/query/process/read/release and caller-buffer
capability/diagnostic operations. Inputs/callback context are borrowed through
return. Callbacks run inline, never under locks, and must not throw/re-enter.
Providers catch exceptions, return stable statuses and own storage. Handles are
nonzero, instance-scoped, never reused; failure returns zero. Successful wire
results echo the job. Library-owned views are immutable and must be copied before
release, another WASM allocation or memory growth. Only the provider releases
storage. Dispose releases outstanding allocations and invalidates handles.
Custom allocation and C/WASM wire encoding remain binding-track work; this task
defines the interface and tests ownership with a fake, not an exported evaluator.

## Scaffolding and ownership

The [machine-readable manifest](component-ownership.json) fixes local targets,
paths, direct dependencies and central integration owners. Each non-contract
native target is `INTERFACE` with `MATTER_COMPONENT_SCAFFOLD=TRUE`, supplying
registration seams without runtime symbols. Local CMake files configure alone
or through `matter_register_component` in `cmake/ComponentScaffolds.cmake`.
All contracts-build `MATTER_SCAFFOLD_*` options default OFF: `GEOMETRY`, `BUILD`,
`GRAPHICS`, `TERRAIN`, `ASSET_PIPELINE`, `GEOMETRY_C`, `HOST_QUICKJS`.

Graphics/build/host link contracts only; build receives injected services.
Terrain/pipeline/C binding link contracts plus geometry. The host scaffold loads
no QuickJS package; the implemented host later adds its private dependency.
JS SDK/browser paths are reserved, not native targets. Central manifests, live
entrypoints and legacy implementation authority remain with their assigned owners.

```sh
cmake -S libs/MatterContracts -B /tmp/matter-graphics-scaffold \
  -DBUILD_TESTING=OFF -DMATTER_SCAFFOLD_GRAPHICS=ON
```

## Executable fixture IDs

| IDs | Executable and assertion |
| --- | --- |
| `SVC-EVALUATE`, `SVC-QUERY`, `SVC-PROCESS`, `SVC-HOST`, `SVC-BUILD`, `SVC-CACHE` | `matter_contracts_consumer_tests`: public fake producers/consumers, ordered batches, injected services, cache miss/hit |
| `SVC-OWNER`, `SVC-RELEASE`, `SVC-PAYLOAD` | Complete result ownership, generated bytes through process/upload/aliases and final release |
| `SVC-ADMIT`, `SVC-STALE`, `SVC-FAIL`, `SVC-CANCEL` | Each envelope field, no-publication failures and cancellation before/during evaluation |
| `SVC-CAPS`, `SVC-BUDGET`, `SVC-CALLBACK`, `SVC-PROGRESS`, `SVC-USAGE`, `SVC-RESOLVE` | Exact versions, ceilings, callback threading/exception/count, usage rollback and payload corruption |
| `GFX-SUBMIT`, `GFX-PENDING`, `GFX-READY`, `GFX-SCENE`, `GFX-SHARING`, `GFX-RELEASE`, `GFX-CANCEL`, `GFX-TICKET`, `GFX-CAPS` | Input retention, shared scene lifetime, cancellation/disposal and stale/foreign handles |
| `TERRAIN-ENVELOPE`, `TERRAIN-ADMIT`, `TERRAIN-ADJACENT`, `TERRAIN-DETAIL`, `TERRAIN-LATE`, `TERRAIN-BOUNDARY`, `TERRAIN-FIELD`, `TERRAIN-SEAM`, `TERRAIN-OWNER`, `TERRAIN-CAPS`, `TERRAIN-BUDGET`, `TERRAIN-CANCEL` | Fake adjacent/detail products, boundaries, stale/invalid/missing data and resource limits |
| `C-ABI` | `matter_contracts_c_ownership_tests`: 1,000 allocate/copy/release cycles, foreign/released/disposed handles, cancellation/failure/version and staged ownership |
| `MATRIX` | `tests/build_matrix.py`: contracts-only, seven individual components, all scaffolds, seven standalone roots; compiled consumers, exact acyclic edges and CMake File API source/target allowlist |

## Repeatable checks

Supported native MSVC uses the existing repository resolver, two build jobs,
serial CTest, then matrix configurations in the same pinned Developer environment:

```powershell
libs/MatterContracts/build-windows.ps1 -Config RelWithDebInfo -Test -Matrix
```

From WSL invoke this script through native PowerShell as documented in README.
Independent Unix portability checks:

```sh
cmake -S libs/MatterContracts -B /tmp/matter-service-contracts -G Ninja \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/matter-service-contracts -j 2
ctest --test-dir /tmp/matter-service-contracts --output-on-failure -j 1
python3 libs/MatterContracts/tests/build_matrix.py --cmake /usr/bin/cmake --generator Ninja
python3 libs/MatterContracts/tests/schema_tests.py \
  --vectors /tmp/matter-service-contracts/matter_contracts_vectors
```

Build compiles every header alone. CTest includes the new C++/C consumers and
existing data/cross-language vectors. Matrix uses Python standard library only
and fails on unavailable compilers/forbidden targets/sources; missing checks are
not passes. Root packaging/GPU, WASM and real seam/rendering validation remain
with the extraction/integration owners.
