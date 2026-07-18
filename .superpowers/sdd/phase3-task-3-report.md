# Phase 3 Task 3 Report: Flecs Ownership and Post-Transform Bridge

## Status

Implemented the private Flecs-to-coordinator bridge and Runtime ownership. One
full generational ECS owner is arbitrated per Runtime, rejected duplicates remain
explicit retries, the frame pipeline samples only resolved `WorldTransform` X/Z,
and teardown nulls the private context before coordinator destruction.

## TDD RED evidence

The nine focused Runtime/Flecs behaviors were added before the Runtime seam,
private context, observers, system, or coordinator ownership existed. After
supplying the repository's normal diagnostic Vulkan/MSL include roots, MSVC
reached the tests and failed for the expected missing feature:

```text
ecs_tests.cpp
MatterEngine3\tests\ecs_tests.cpp(1435): error C2039:
  'streaming_coordinator': is not a member of
  'matter::ecs_runtime::Runtime'
```

Command shape:

```powershell
cl /nologo /std:c++17 /EHsc /c MatterEngine3\tests\ecs_tests.cpp `
  /IMatterEngine3\include /IMatterEngine3\src /ILibraries\flecs `
  /ILibraries\Vulkan-Headers\include /IMatterSurfaceLib\include
```

The first linked implementation run then exposed a real Flecs ordering defect:

```text
FAIL: resolved child translation submits initial X/Z sector
FAIL: camera-independent anchor submits its resolved transform
FAIL: changing an unrelated CameraDesc has no coordinator effect
3 FAILURE(S)
```

Diagnostic evidence showed `WorldTransform` existed after the first pipeline but
the coordinator remained `PendingTransform`; the next tick emitted the delayed
request. The root cause was the missing Flecs read dependency between the
`FrameUpdate` transform writer and `StreamingUpdate` sampler. Adding
`.read<ecs::WorldTransform>()` forced the required pipeline merge. No test
expectation was weakened.

## Implementation

- `streaming_systems.h/.cpp` publishes the private `StreamingContextRef`, private
  full-ID arbitration bookkeeping, intent-only add/set/remove observers, snapshot
  publication, and a `StreamingUpdate`/`FramePipelineSystem` sampler.
- Duplicate adds preserve `SectorStreaming`, report `OwnerAlreadyClaimed` with
  the active full ID, and never become standby owners. Removing/re-adding is the
  only retry path.
- Sampling validates the active full ID and reads only
  `WorldTransform.matrix.m[3]` and `.m[11]`. Missing derived transforms stay
  pending without error; no LocalTransform, camera, streamer, render, store, GPU,
  or worker state is consulted.
- Snapshot publication mutates status/error components only for alive matching
  full IDs, tracks the previously published owner for cleanup, and returns
  immediately when the context is null.
- Runtime member order is `world_`, `physics_`, `streaming_`, then pipeline
  handles. The constructor imports public modules, creates/publishes the
  coordinator context, then builds pipelines. The destructor nulls and destroys
  streaming before preserving the existing null-before-destroy physics teardown.

## GREEN evidence

Vendored dependencies were built natively: Flecs `flecs.c` as C17 and all 49
Box3D library C sources as C17; Task 3 and Runtime sources used MSVC C++17.

Fresh linked focused ECS run:

```text
ecs_tests.cpp ... streaming_systems.cpp ... sector_streaming_coordinator.cpp
Generating Code...
ALL PASS
```

Fresh linked Runtime physics regression:

```text
physics_tests.cpp ... streaming_systems.cpp ... sector_streaming_coordinator.cpp
Generating Code...
ALL PASS
```

Fresh standalone coordinator and pure streamer regressions:

```text
sector_streaming_coordinator_tests.cpp ...
ALL PASS

sector_streamer_tests.cpp ...
  long flight: peak=7997 end=7993
ALL PASS
```

The Phase 2 static checker exited 0 with `PASS: Box3D Phase 2 build contract`.
`git diff --check` exited 0.

## Source graphs

The following Runtime-bearing unions each contain `streaming_systems.cpp` and
`sector_streaming_coordinator.cpp` exactly once:

- `MatterEngine3/Makefile:ME3_CPP`
- `MatterEngine3/tests/Makefile:ECS_CPP`
- `MatterEngine3/tests/Makefile:PHYSICS_CPP`
- `MatterEngine3/tests/Makefile:GPU_ALL_CPP`
- `MatterViewer/Makefile:WIN_ME3_CPP`

The engine object union also includes `streaming_systems.o` exactly once; existing
sector-streamer closures were reused without duplication.

## Self-review

- The ECS singleton bookkeeping exists only to make same-frame claim arbitration
  deterministic; coordinator attachment/snapshot state remains the lifecycle and
  published-status authority.
- `OnAdd` plus `OnSet` is idempotent for the active entity, so `set` cannot reject
  its own immediately successful `OnAdd`.
- Recycled entity indices cannot receive cleanup or status because every mutation
  checks the full generational ID with `world.is_alive`.
- The explicit `WorldTransform` read term proves transform writes are visible in
  `StreamingUpdate`; the parent-only test verifies resolved row-major translation
  changes sectors from `(0,0)` to `(10,1)` in one tick.
- The camera test changes only a plain `CameraDesc` and proves no coordinator
  request appears; no ECS camera component was introduced.
- Runtime destruction with an attached component completes without a callback to
  the released coordinator because observers fail closed on the nulled context.

## Concerns

GNU Make was attempted with:

```powershell
make -C MatterEngine3/tests run-ecs run-physics run-sectorcoord run-sectorstream
```

The host reports that `make` is not recognized. CMake is also unavailable, so the
real Box3D C17 archive was built directly with MSVC `cl`/`lib`. No GNU recipe
success is claimed; native linked behavior and the repository static build checker
are the available gates.
