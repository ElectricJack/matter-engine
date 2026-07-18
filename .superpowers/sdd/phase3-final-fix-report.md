# Phase 3 Final Fix Report: Transactional Eviction Ownership

## Root Cause

Both eviction handoffs drained their source before the destination owned a
durable copy. `SectorStreamer::take_evictions()` swapped out the streamer's
queue before the coordinator reserved tagged storage, and
`Coordinator::take_evictions()` did the same before
`PendingEvictionBatch::append()` performed potentially allocating pushes.
Allocation failure could therefore destroy the only retry ownership for one or
more resource-release tags. The idle timeout path also called the streaming
step outside an exception boundary, so an unexpected allocation exception
could escape the worker thread.

## Approved Design and Implementation

- `SectorStreamer` now exposes a non-draining `peek_evictions()` view and a
  `noexcept` prefix commit. Commit validates both the exact source vector and
  the requested count before erasing. Coordinator collection reserves the
  complete tagged destination first, copies only trivially/nothrow-copyable
  tags, commits the source prefix, and only then retires resident ledger rows.
- `PendingEvictionBatch::append()` accepts the coordinator's const batch,
  computes the exact unique addition count, checks overflow, performs one
  reserve, and then copies nothrow tags without further allocation. The new
  `Coordinator::transfer_evictions()` clears its source only after append
  reports complete success. Failed append leaves both source and destination
  unchanged and retryable.
- The idle streaming timeout invokes `run_idle_worker_step_noexcept()`. It
  classifies allocation failures as `OutOfMemory`, other failures as
  `Internal`, invokes the existing stream `BakeError` path, and separately
  contains any exception thrown while reporting the failure. The worker loop
  can therefore continue to service later commands and shutdown.

## TDD Evidence

The coordinator regressions were added before production changes. The first
MSVC compile exited 1 on the exact wished contracts:

```text
error C2039: EvictionTransferStage is not a member of detail
error C2039: IdleWorkerFailure is not a member of detail
error C3861: run_idle_worker_step_noexcept identifier not found
error C2660: Coordinator::worker_step does not take 2 arguments
error C2039: transfer_evictions is not a member of Coordinator
```

The deterministic tests inject `std::bad_alloc` before destination reserve at
both ownership boundaries. Each starts with two resident resource tags, proves
the failed destination remains empty, retries without injection, cleans both
resources exactly once, and proves no committed tag is available for a
duplicate retry. The idle wrapper regression covers allocation classification,
internal-exception classification, later successful progress, and an exception
thrown by the failure handler itself.

## Fresh Current-Source Verification

All lightweight gates were rebuilt or rerun from the final source. No flight,
long-flight, screenshot, performance, or GPU runtime automation was run.

```text
phase3-final-fix-coord.exe   -> ALL PASS
phase3-final-fix-async.exe   -> ALL PASS
phase3-final-fix-ecs.exe     -> ALL PASS
phase3-final-fix-physics.exe -> ALL PASS
phase3-task-6 focused Viewer controller/ImGuizmo executable -> ALL PASS

Flecs Task 7 build-contract checker          -> PASS
Box3D Phase 2 build-contract checker         -> PASS
Sector streaming Phase 3 closure checker     -> PASS

matter_engine.cpp MSVC C++20 translation unit      -> exit 0
world_stream_tests.cpp MSVC C++20 translation unit -> exit 0
git diff --check                                  -> exit 0
```

The ECS and physics executables used freshly compiled current C++ sources,
fresh MSVC C17 Flecs, and a fresh archive built from all 49 pinned Box3D C
sources. The product translation units used C++20 because the existing product
source contains designated-initializer syntax that MSVC rejects in C++17.

GNU Make/WSL and supported GPU-linked world-stream execution remain
unavailable on this Windows host. No result is claimed for those gates. The
pre-existing `.superpowers/sdd/progress.md` modification was preserved and is
excluded from this fix.

## Concerns

No remaining lifecycle concern was found in the two reviewed transfer paths.
The legacy draining `take_evictions()` methods remain for existing focused
callers, but production session ownership now uses only the transactional
peek/commit and transfer APIs.
