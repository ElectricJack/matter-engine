# Terrain demand and lifetime coordinator

`matter_terrain_coordinator` connects the frozen terrain, generation and graphics
service contracts through an application lane. Its only linked dependency is
`matter_contracts`; it builds without terrain/geometry implementations, graphics,
QuickJS, world state or the editor. This is the coordination adapter owned by
`clear-crest-48.5.3`. Live world/scene placement and central target registration
remain with the ordered terrain integration task (`clear-crest-48.9.3`).

## Calling and ownership contract

Create `Coordinator` with a required SHA-256 implementation identity, the selected
generator's capabilities, finite limits, and optional cache/graphics services.
Include all generator, field and algorithm implementation versions in that
identity. The services outlive the coordinator. Call every coordinator, cache and
graphics method on one application thread; services must not re-enter it.

1. `define(request)` validates and retains a frozen field/dependency definition.
   It supplies a unique request ID and increasing generation epoch; the caller
   supplies `job.source_revision`. Changing an input, detail or boundary policy
   requires another definition. Invalid input leaves the previous definition
   intact. Same-input replacement is still an invalidation.
2. `demand(region, {cpu, collision, graphics})` coalesces consumers' needs.
   `observe_graphics()` (or `observe(batch)`) accepts bounded graphics observations
   whose instance IDs name known regions. Unknown regions/required representations
   fail explicitly. Representation changes outside a frozen request go back to
   world policy for a new definition. Graphics never receives a generator.
3. `take(work)` admits validated cache hits or returns one generation work item.
   An empty successful return means there is no work or running capacity is full.
   Every dispatch advances the epoch, including demand after a CPU product was
   dropped without editing the field.
   The application runs `execute(work, terrain_service)` on its chosen bounded
   executor, using a service instance whose concurrency contract permits it.
4. Marshal **both** the work receipt and result back to the application thread and
   call `complete(work, result)` exactly once, even for cancelled/failed work.
   Receipts keep requests and cancellation tokens alive without retaining the
   coordinator. A malformed backend envelope still returns that receipt's running
   credit. Duplicate/foreign receipts cannot return someone else's credit.
5. `pump_graphics()` starts up to the configured upload limit and polls each
   outstanding ticket once. `gpu(region)` exposes a handle only after successful
   completion and ticket release. World integration places/removes scene instances
   using the graphics scene interface. The coordinator owns its resource reference;
   callers must not release that reference themselves. Graphics retires device
   work and any independent scene references through its own contract.

Generation completion compares request ID, source revision and epoch, then
validates the complete terrain product. Only current successful results publish
CPU/collision owners or reach cache storage. Cache hits validate the original
request and product, recompute the key, then bind the immutable data to the new
job envelope. Upload submission and completion envelopes are checked separately.
Late generation after edit, retry, removal/recreation or demand off/on is rejected.

Inputs and results obey the public immutable-ownership rule: a producer must
relinquish mutable aliases before handing over shared owners. `execute()` checks
cancellation before and after the generator call. Cooperative cancellation does
not forcibly terminate a worker. A cancelled job retains its running credit and
input owners until its completion arrives; retries cannot create unlimited
abandoned work. Coordinator destruction cancels workers and releases its graphics
ownership; the host still owns/drains work already taken.

Failures are returned as diagnostics. A generation failure stops automatic
generation retries, and an upload failure stops automatic uploads for that
definition. `retry(region)` creates a new epoch and retries from cache/generation.
Cache read/identity failures publish nothing. Cache write failures are reported
after valid CPU publication and leave that independent product usable. Valid
graphics releases normally succeed; if a service reports a release error, the
coordinator retains the handle for explicit retry/eviction. Destruction is best
effort, so integrations requiring teardown diagnostics should evict before
destroying services.

## Separate lifetimes and bounds

`definition()` retains frozen field inputs independently of generated products.
`cpu()` and `collision()` are independent immutable product leases. Collision can
reference mesh and extension resources, so its lease retains the complete product
rather than pretending those shared bytes can be freed. External consumers' leases
remain valid even after the coordinator removes a region.

`evict_gpu()` cancels/releases uploads/resources and clears graphics demand,
retaining the definition and independently demanded CPU/collision state.
`drop_cpu()` clears the coordinator's CPU lease and CPU demand. A pending upload
owns its inputs independently. If graphics still needs an upload and no remaining
owner can supply it, CPU eviction queues regeneration within the normal limit.
`evict()` removes the whole region and cancels its generation. Graphics eviction
while collision generation is pending does not cancel that independent demand.

Limits cover region definitions, queued/running jobs, pending uploads, observation
batch size, and per-region input/metadata/output plus per-job working budgets.
Queue/region overflow returns `resource_limit` without replacing the prior
definition/demand. Running/upload saturation leaves demand waiting for capacity.
No unbounded retry queue or internal thread pool is created. Bounds apply to
coordinator ownership and declared service budgets, not external consumers' leases
or an allocator/RSS guarantee.

The injectable `Cache` stores terrain products, not GPU resources. `MemoryCache`
is a bounded FIFO implementation. Each entry reserves its declared metadata,
input-resource and output byte limits. It rejects oversized entries before any
eviction; consumers' immutable owners survive replacement/eviction. The portable
SHA-256 namespace is `matter.terrain.coordinator.cache.v1`; keys include the
implementation digest, source revision, field identity, dependencies and resolved
content, region/origin/domain/detail, boundaries, capabilities, algorithms and
evaluation/validation limits. Only the transient request ID and epoch are omitted.
It does not reinterpret legacy cache hashes or implicitly persist to disk.

## Focused verification

The local wrapper uses the repository's supported MSVC toolchain resolver, two
build jobs, serial tests, and the ignored `MatterEngine3/build/` output directory:

```powershell
MatterEngine3/src/component_adapters/terrain_coordinator/build-windows.ps1 -Config RelWithDebInfo -Test
```

Equivalent WSL invocation from the repository root:

```bash
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
  -NoProfile -ExecutionPolicy Bypass \
  -File "$(wslpath -w "$PWD/MatterEngine3/src/component_adapters/terrain_coordinator/build-windows.ps1")" \
  -Config RelWithDebInfo -Test
```

Independent Unix build:

```bash
cmake -S MatterEngine3/src/component_adapters/terrain_coordinator \
  -B /tmp/matter-terrain-coordinator -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/matter-terrain-coordinator -j 2
ctest --test-dir /tmp/matter-terrain-coordinator --output-on-failure -j 1
```

| CTest suffix | Fixture IDs and behavior |
| --- | --- |
| `epochs` | `TC-EPOCH-EDIT`, `TC-EPOCH-EVICT`, `TC-EPOCH-RETRY`, `TC-DEMAND-OFF-ON`: newer publication survives late results, repeated completion, cache reuse and region recreation |
| `lifetimes` | `TC-LIFETIME-GPU`, `TC-LIFETIME-COLLISION`, `TC-DISPOSE`: pending/resident GPU eviction, collision generation/leases, weak-owner release, destructor cancellation |
| `cache` | `TC-CACHE-IDENTITY`, `TC-CACHE-ADMISSION`, `TC-CACHE-BOUNDS`: changed input/resource/dependency/domain/algorithm keys, corrupt/stale lookup rejection, reservation eviction and surviving leases |
| `pressure` | `TC-BACKPRESSURE`, `TC-OBSERVATION-BOUNDS`, `TC-UPLOAD-BOUNDS`: saturated queue/running/uploads, repeated invalidation, atomic observation preflight and duplicate coalescing |
| `failures` | `TC-FAILURE-RETRY`, `TC-INVALID-RESULT`, `TC-CAPABILITIES`: generator failure/exception, malformed envelope/product, explicit retry and budget/unknown-capability rejection |
| `graphics` | `TC-GRAPHICS-DEMAND`, `TC-GRAPHICS-LATE`, `TC-GRAPHICS-RETRY`: observation cannot call generator, pending upload cancellation, stale submissions/completions, upload/poll failures/exceptions and resource cleanup |
| `thread` | `TC-WORKER-CANCEL`, `TC-WORKER-OWNERSHIP`: promise-controlled non-cooperative worker returns after a newer revision publishes, without sleeps or a GPU |

These fixtures verify coordination with fake services; they do not claim terrain
meshing, rendered views, native scene integration or performance qualification.
