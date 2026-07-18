# Phase 3 Task 4 Report: WorldSession Streaming Lifecycle

## Result

Implemented Runtime-coordinator-driven sector streaming in `WorldSession`.
Procedural profiles remain dormant until an ECS `SectorStreaming` owner with a
resolved transform exists; closed worlds remain usable and expose recoverable
`UnsupportedWorld`. Status and `FrameStats::resident_sectors` use one copied
coordinator snapshot.

The available CPU and compile gates pass. GNU Make and the GPU-linked world
suite remain unavailable on this host and are not claimed.

## RED Evidence

Tests were changed before production integration. The focused coordinator seam
failed to compile because `TaggedEviction` lacked an issuance token and
`Coordinator` lacked an app-publication reservation API. A static source check
also failed with:

```text
RED: WorldSession::Impl still owns and focus-updates the automatic SectorStreamer
```

The updated world-stream translation unit then failed on the missing public
`WorldSession::streaming_status()` seam. These failures established both the old
automatic behavior and the missing publication/status boundaries.

## Implementation and Lifecycle Mapping

- `WorldSession::Impl` no longer owns or invokes `SectorStreamer`; only the
  Runtime-owned coordinator does so. The worker is always given a bounded timed
  wait so ECS attachment changes can progress without another bake command.
- Successful procedural installation copies the existing production sector
  size/rings/rungs/default hysteresis/cooldown and inflight policy into the
  coordinator. Closed-world and closed-world resolve-cache paths install no
  profile and publish `UnsupportedWorld` without failing authored content.
- Every request, app publication reservation, acknowledgement, and eviction
  carries owner, generation, issuance, and sector tuple. App resources are fully
  mutated before true acknowledgement; failures roll back then acknowledge
  false. Coordinator residency is therefore never early or phantom.
- One app-thread `apply_sector_evictions` helper handles movement, detach,
  reload, regenerate, shutdown, and partial-publication rollback. It removes the
  WorldState instance, query state, culler/Vulkan resources and caches,
  PartStore/transient artifacts, and the matching ledger entry. Full-tag
  mismatch is a fail-closed no-op.
- Reload/regenerate null and step the old profile, queue FIFO evictions, wait on
  the app/GPU barrier, then replace field/profile state. ECS entities and
  components survive. The same attached owner starts one fresh generation;
  removal during the barrier leaves the coordinator detached and prevents a
  restart.
- Shutdown invalidates the owner first, cancels the command worker, pumps the
  app/GPU queue until worker clear has posted its FIFO evictions, drains them,
  applies a final same-helper fail-closed sweep, and only then tears down the GPU
  queue and render resources.
- `set_bake_focus()` remains unchanged for closed-world ordering/refinement and
  is absent from infinite streaming. Tick/pump publish only copied snapshots to
  ECS/public status and `FrameStats`.

## GREEN Evidence

Fresh/current-source MSVC C17/C++17 verification:

```text
phase3-task4-coordinator-green.exe  -> ALL PASS
phase3-task4-ecs.exe                -> ALL PASS
phase3-task4-physics.exe            -> ALL PASS
sector_streamer_tests.exe           -> long flight peak=7997 end=7993; ALL PASS
```

The product-flavor MSVC C++17 compile of `matter_engine.cpp` passed with the
normal script-host, OpenGL 4.3, tileset, Flecs, Box3D, MSL, QuickJS, raylib, and
Vulkan-header include seam. The updated `world_stream_tests.cpp` translation
unit passed under the same seam. Only existing-style MSVC `getenv` C4996 and
double-to-float C4244 warnings were emitted.

The Box3D Phase 2 build-contract checker printed
`PASS: Box3D Phase 2 build contract`. `git diff --check` passed. A static audit
found no session-owned/direct streamer calls and exactly one app eviction helper.

## Test Coverage Added

The CPU fake publication ledger covers resource-before-true-ack ordering,
rollback plus false acknowledgement, no phantom residency, publication-in-flight
detach cleanup, stale tagged eviction rejection, one fresh reload generation,
and removal during reload. The GPU world-stream suite now asserts no activation
means zero, ECS transform activation starts, bake focus does not drive streaming,
reload/regenerate preserve the owner/component, removal prevents restart, and a
closed-world activation reports `UnsupportedWorld` while authored state remains
Ready.

## Unavailable Gates and Self-Review

Canonical capable-host gates remain:

```text
make -C MatterEngine3/tests run-sectorcoord run-ecs run-worldstream
```

This Windows environment has no GNU Make/WSL distribution and no supported GPU
world-stream execution path, so no GNU, linked GPU, or screenshot result is
claimed. Scope is limited to the public copied-status seam, private coordinator
integration seams proven by tests, lifecycle integration, focused tests, and the
closed-world fixture. The unrelated modified `.superpowers/sdd/progress.md` was
preserved and excluded from this task's commit.

## Fix Round 1: Failure-Path Hardening

### Review Findings and Root Causes

The initial integration had four related early-release boundaries. Coordinator
evictions were moved directly into a one-shot GPU job, so a mid-batch failure
lost the only cleanup tags. Publication used several ad-hoc exits instead of one
transaction, recorded successful registrations rather than attempts, and loaded
PartStore before collision rejection. Procedural install published its profile
before authored app/GPU finalization. Finally, the generic ECS snapshot publisher
removed every error before the session re-added `UnsupportedWorld`, causing
observable remove/add churn on every tick.

### RED Evidence

Focused tests were added before the hardening implementation. The coordinator
test translation unit failed on the absent production seams:

```text
error C2039: 'PendingEvictionBatch': is not a member of detail
error C2039: 'PublicationTransaction': is not a member of detail
error C2039: 'ProfileActivationGate': is not a member of detail
```

The ECS regression also targeted the absent
`set_profile(nullptr, UnsupportedWorld)` overload. The updated GPU integration
test specifies a `-1` authored-finalize fault and a deterministic same-app-thread
component-removal latch through the existing fault hook.

### Durable Eviction Barrier

`PendingEvictionBatch` is the shared, mutex-protected production/test seam. It
deduplicates full tags, applies a value snapshot without holding its mutex, and
erases only the successfully completed snapshot. A one-shot injected endpoint
failure after its first item proves the complete two-tag batch survives and the
retry is idempotent. `WorldSession` now retains every coordinator eviction in
this batch. Its single app helper clears individual resource-attempt bits only
after release succeeds; a retained full batch can therefore safely revisit
already-clean entries.

Profile clear now uses a blocking FIFO app job, retries transient failures, and
requires both the pending batch and the entire app resident ledger to be empty
before field/provider replacement. Shutdown uses the same worker barrier and a
final durable fail-closed check before GPU queue/resource teardown.

### Publication Transaction

One `PublicationTransaction` is created immediately after a successful
`begin_publication`. It owns rollback-before-false-ack ordering and provides a
destructor fallback. Same-sector collision is rejected before PartStore access.
The app ledger is inserted before external publication calls, and shared
`PublicationResources` bits are set before store load, WorldState insertion,
culler registration, and Vulkan registration; transient ownership is recorded
at insertion. Thus a call that partially mutates and then fails is still released
by the same eviction helper. True acknowledgement occurs only at explicit commit
after all required mutations. CPU stage-fault cases cover transient, store,
WorldState, culler, and Vulkan attempt rollback plus the success-only commit path.

### Profile Readiness and Stable UnsupportedWorld

`ProfileActivationGate` privately stages the procedural config during
`install_world` and publishes it only at the successful end of authored
finalize/Ready and fatal tail work. Failure/cancellation leaves the coordinator
with no profile, no requests, and zero residency. The world-stream regression
injects a finalize failure and asserts `PendingProfile`, zero resident/inflight,
and zero `FrameStats` residency.

Coordinator snapshots now carry the stable recoverable profile error. The one
generic ECS publisher sets or preserves it centrally and removes it only when a
real profile publishes (or ownership disappears); the session no longer performs
a second error mutation. A five-tick Runtime regression observes continuous
`UnsupportedWorld`, zero OnRemove callbacks, then exactly one removal when a
procedural profile replaces it.

The reload-removal integration test no longer races the worker. Its existing
fault hook removes `SectorStreaming` on the app thread at the finalize barrier;
the subsequent profile commit sees no owner and cannot restart residency.

### Fresh GREEN Evidence

All binaries and translation units below were rebuilt or rerun from the final
fix-round sources:

```text
phase3-task4-fix1-coordinator.exe -> ALL PASS
phase3-task4-fix1-final-ecs.exe   -> ALL PASS
phase3-task4-fix1-final-physics.exe -> ALL PASS
sector_streamer_tests.exe -> long flight peak=7997 end=7993; ALL PASS
matter_engine.cpp MSVC product TU -> exit 0
world_stream_tests.cpp MSVC product TU -> exit 0
Box3D Phase 2 checker -> PASS
```

The product TU emitted only the previously recorded C4996 `getenv` and C4244
conversion warnings. GNU Make and the GPU-linked world-stream executable remain
unavailable on this host, so the new world integration cases are compile-proven
but not claimed executed here.

## Fix Round 2: FIFO Ownership, Durable Completion, and Terminal Progress

### RED Evidence

The second review round first extended the focused coordinator tests. A fresh
MSVC C++17 compile failed on the intended missing contracts:

```text
static assertion failed: publication reservation must contain allocation failure
static assertion failed: publication acknowledgement must report durable enqueue failure
static assertion failed: publication acknowledgement must not throw through the GPU pump
error C2039: apply_tag / abandon_noexcept
error C2661: PublicationTransaction: no overload takes 3 arguments
error C2039: active / begin_clear / finish_clear / abort_clear
```

After the first transaction implementation, the focused executable produced a
second behavioral RED:

```text
FAIL: scope teardown retries true ack without changing its intent
```

That regression prevented a failed true acknowledgement from being converted
by destructor fallback into rollback plus false acknowledgement.

### FIFO and Publication Completion

Inline publication rollback now calls `PendingEvictionBatch::apply_tag` with
only its own full tag. It never snapshots, applies, or retires the lifecycle
FIFO retained by the worker. The queue-order regression leaves movement and
detach tags in their original issuance order after an inline publication
rollback completes.

Every issued sector reserves one of 32 fixed session completion slots before
bake. A successful bake transfers its transient artifact into that durable slot
before job construction or posting. The app job constructs a non-allocating
function-pointer `PublicationTransaction` before `begin_publication`; begin is
`noexcept`, acknowledgement is `noexcept` and reports enqueue failure, and the
slot retains orphan cleanup plus true/false acknowledgement state until a later
app pump completes it. Completed rollback is not repeated while a failed false
acknowledgement retries. A failed true acknowledgement retains commit intent,
including during destructor fallback. The shared GPU queue pump now contains
throwing jobs so later FIFO cleanup work still executes.

### Bounded Reload and Shutdown

Profile clear is provisional. A reload makes one eviction-barrier attempt; on
persistent failure it restores the prior profile, retains the old field and app
state, reports one stream error, and aborts replacement. There is no worker
cleanup retry loop.

Shutdown invalidates attachment and gives cancellation/FIFO work 64 bounded
full-drain passes. Queue shutdown then releases any blocking waiter. After join,
one no-throw whole-owner fallback best-effort releases every resident entry and
orphan transient, then clears the sector ledger, completion slots, lifecycle
batch, activation gate, and coordinator intent. Persistent release failure can
therefore no longer keep session destruction in an unbounded loop.

### Fresh Round-2 GREEN Evidence

All of the following used the final round-two sources:

```text
phase3-task4-round2-coord.exe    -> ALL PASS
phase3-task4-round2-async.exe    -> ALL PASS
phase3-task4-round2-ecs.exe      -> ALL PASS
phase3-task4-round2-physics.exe  -> ALL PASS
phase3-task4-round2-streamer.exe -> long flight peak=7997 end=7993; ALL PASS
matter_engine.cpp product MSVC TU -> exit 0
world_stream_tests.cpp product MSVC TU -> exit 0
Box3D Phase 2 build-contract checker -> PASS
Task 4 lifecycle audit -> PASS
git diff --check -> PASS
```

The product compile emitted only the existing MSVC C4996 warnings for `getenv`
and C4244 double-to-float conversion warnings. GNU Make/WSL and a supported
GPU-linked world-stream execution path remain unavailable on this Windows host;
therefore the expanded persistent reload/shutdown world cases are compile-proven
but are not claimed executed here.

## Fix Round 3: Completion Capacity Admission

The remaining exhaustion path called fallible `acknowledge(request, false)`
after `next_request` had already allocated a coordinator request, then ignored a
failed enqueue. Repeated generations could retain all 32 durable completion
slots and leave the next issued request without terminal completion ownership.

### RED and Invariant

The focused regression was written first and failed to compile because the
wished production seam did not exist:

```text
error C2039: 'PublicationCompletionCapacity' is not a member of detail
```

The regression retains one completion claim through each of 32 restarted
generations. A 33rd admission must fail before calling `next_request`, with zero
coordinator inflight requests. It then durably enqueues false acknowledgements
for every retained tag, releases every claim, processes them on the worker, and
proves a new current-generation request can be admitted and drained.

`PublicationCompletionCapacity` is a fixed `std::array<bool, 32>` plus a count;
reserve, release, clear, and queries are bounded and `noexcept`. `WorldSession`
claims capacity under its existing completion mutex before `next_request`. Full
capacity is backpressure: the worker allocates no coordinator request and emits
no fallible acknowledgement. The claim is released only when no request was
available, after durable true/false acknowledgement completion, or during
terminal teardown. A stack guard turns any exception after a request is issued
but before artifact handoff into retained false-ack completion work.

### Fresh Round-3 GREEN Evidence

```text
phase3-task4-round3-coord.exe    -> ALL PASS
phase3-task4-round3-async.exe    -> ALL PASS
phase3-task4-round3-ecs.exe      -> ALL PASS
phase3-task4-round3-physics.exe  -> ALL PASS
phase3-task4-round3-streamer.exe -> long flight peak=7997 end=7993; ALL PASS
matter_engine.cpp product MSVC TU -> exit 0
world_stream_tests.cpp product MSVC TU -> exit 0
Box3D Phase 2 build-contract checker -> PASS
round-3 admission-order audit -> PASS
git diff --check -> PASS
```

The product compile again emitted only the existing MSVC C4996 `getenv` and
C4244 double-to-float warnings. GNU Make/WSL and the GPU-linked world-stream
runtime remain unavailable, so no new GNU or GPU execution result is claimed.

## Fix Round 4: Strong Request-Tracking Rollback

`Coordinator::next_request` previously let `SectorStreamer::next_request`
mark a sector inflight before the coordinator appended the corresponding
issued-request and publication-candidate records. Allocation failure at either
append could therefore leave an inflight sector without coordinator ownership.
The session could then release its preclaimed publication-completion slot even
though the request had not been rolled back.

### RED and Exact Rollback

The deterministic regression was written first and failed to compile at all
three wished contract points:

```text
error: RequestTrackingStage was absent
error: no matching three-argument Coordinator::next_request overload
static assertion failed: Coordinator::next_request must be noexcept
```

The regression injects failure independently at `IssuedRequest` and
`PublicationCandidate`. For each stage it proves the baseline streamer
inflight count is restored, neither an issued record nor publication candidate
survives, the preclaimed completion slot is reusable after the call returns,
and a later request for the same sector completes normally. A stale false
acknowledgement is also processed before the later request, proving that a
partially appended issued record cannot add cooldown during rollback.

Both coordinator vectors now reserve their required capacity before the
streamer mutates inflight state. After mutation, either tracking append failure
erases the exact request from both vectors and calls the streamer's dedicated
exact-request cancellation path. Cancellation clears only the matching
inflight rung and does not apply cooldown. `Coordinator::next_request` is
`noexcept` and returns `false` only after that rollback completes; the session
therefore releases its completion-capacity claim exactly once, after the
rollback boundary.

### Fresh Round-4 GREEN Evidence

All of the following used the final round-four sources:

```text
phase3-task4-round4-coord.exe    -> ALL PASS
phase3-task4-round4-async.exe    -> ALL PASS
phase3-task4-round4-ecs.exe      -> ALL PASS
phase3-task4-round4-physics.exe  -> ALL PASS
phase3-task4-round4-streamer.exe -> long flight peak=7997 end=7993; ALL PASS
matter_engine.cpp product MSVC TU -> exit 0
world_stream_tests.cpp product MSVC TU -> exit 0
Box3D Phase 2 build-contract checker -> PASS
round-4 strong-exception audit -> PASS
git diff --check -> PASS
```

The product compile emitted only the existing MSVC C4996 `getenv` and C4244
double-to-float warnings. GNU Make/WSL and a supported GPU-linked world-stream
execution path remain unavailable on this Windows host, so no GNU or
GPU-linked runtime result is claimed.
