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
