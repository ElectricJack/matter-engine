# Task 7 Report: ECS-Safe Physics Queries

## Status

Implemented the existing public closest-ray and sphere-overlap APIs as synchronous,
guarded queries over the runtime-owned Box3D world. Results contain only copied
engine-native values and full Flecs entity IDs.

## TDD Evidence

Tests were added before production changes for closest/miss/filter rays, invalid
numeric inputs, stale bridge rejection, exact-world/context-less safety, the
while-stepping guard, and filtered/sorted/deduplicated sphere overlaps containing a
recycled non-zero-generation entity ID.

The RED compile failed for the expected missing query implementation surface only:
MSVC C2039 errors reported absent `ray_cast`, `overlap_sphere`,
`tombstone_query_participant_for_test`, and `set_stepping_for_test` members on
`PhysicsContext`. After the minimal implementation, the focused suite passed.

## Implementation

- `PhysicsContext` owns a `stepping` guard set around every `b3World_Step`; both
  query types fail closed while it is set.
- Public entry points normalize a staged Flecs world to its real world and require
  its private `PhysicsContextRef`; private entry points additionally prove that the
  supplied real world owns exactly this context.
- Inputs reject nonfinite vectors, zero ray translations, and nonfinite or
  nonpositive overlap radii.
- Query filters map the public category mask to Box3D's query mask without exposing
  Box3D types.
- Callbacks validate the stable bridge pointer, exact owning world, full live entity
  ID, current bridge-map identity, matching live shape/body handles, and body user
  data before copying any result.
- Ray callbacks ignore stale records and retain/clip to the minimum fraction while
  copying entity, point, normal, and fraction immediately.
- Overlap callbacks copy only entity IDs; the completed list is sorted and uniqued.
  Allocation failure is contained inside the C callback boundary and returns an
  empty result rather than unwinding through Box3D.
- Private test seams model a stale query participant and the active-step state; no
  Box3D handle or pointer enters the public engine header. A one-shot overlap seam
  duplicates one resolved full ID so the sort/unique behavior is observable.

## Verification

Two absent-at-start MSVC build directories independently compiled and linked:

1. exactly all 49 pinned Box3D C sources as C17 into a fresh archive;
2. vendored Flecs separately as C17;
3. current `ecs_runtime.cpp`, `physics_context.cpp`, `physics_shapes.cpp`,
   `physics_systems.cpp`, `transform_system.cpp`, `physics_tests.cpp`, and
   `ecs_tests.cpp` as C++17;
4. independent physics and ECS executables.

Both runs printed:

```text
BOX3D_OBJECTS=49
ALL PASS
ALL PASS
```

Additional gates passed:

- Box3D Phase 2 static build-contract checker;
- public-header scan for Box3D symbols;
- explicit no-Task8/product-file scope scan;
- `git diff --check`.

GNU Make/GCC and MinGW product gates remain unavailable on this host and are not
claimed verified.

## Independent Review

The first review found one Important test-quality gap: the overlap test asserted
uniqueness without producing a duplicate candidate. The fix began with an expected
MSVC C2039 RED for the missing duplicate-participant seam, then added a private
one-shot callback seam and retained the existing assertion that the final list
contains each ID once. A fresh covering build again compiled all 49 Box3D C17
sources and Flecs C17, then both the current physics and ECS C++17 suites printed
`ALL PASS`. Removing only the `unique` operation produced exactly the expected
`sphere overlap returns unique ascending full generational IDs` failure; restoring
it returned the focused physics suite to `ALL PASS`. Re-review is recorded below.
