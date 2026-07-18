# Task 3 Report: Body Validation, Shapes, and Stable Bridge Lifecycle

## Status

Implemented on reviewed base `ba8e4c5`. The runtime now reconciles declarative
Flecs rigid bodies into one private Box3D body and shape per valid entity for
spheres, capsules, oriented boxes, and convex hulls. Invalid configurations fail
closed with recoverable, exact `PhysicsErrorCode` values.

No stepping, commands, event extraction, or query behavior was implemented.

## RED Evidence

The validation/lifecycle matrix was added before production code. An MSVC C++17
build linked successfully against the unchanged Task 2 runtime and exited 1 with
`43 FAILURE(S)`: exact errors, correction/recovery body counts, four-shape
creation/retirement, and archetype stability all failed because reconciliation
did not exist.

Private-handle tests were then added before their seams. Compilation failed on
the absent `body_is_valid`, `shape_is_valid`, `user_data_entity`,
`PhysicsBodyState`, and body-state accessors.

A final regression was written before the hash correction. It exited 1 with two
failures because an ordinary valid dynamic ECS pose edit rebuilt the body. Pose
was removed from the configuration hash so later physics-pull writes only trigger
validation; body/collider configuration changes still replace transactionally.

The specified GNU command could not run because `make`/MinGW are unavailable in
this Windows environment. It is not reported as passing.

## Implementation

- `physics_shapes.*` copies and validates the complete desired configuration,
  normalizes quaternions, enforces root/unit-scale and exactly-one-collider rules,
  validates body/material/geometry values, distinguishes pure invalid hulls from
  Box3D hull-build failure, and owns every temporary hull with RAII.
- `PhysicsContext` owns a full-generational-ID map of `unique_ptr<BridgeRecord>`.
  Body and shape user data point only to the stable heap record, never Flecs
  component/query storage.
- Reconciliation unions dirty/current/existing IDs, sorts and uniquifies raw full
  IDs, creates all four shapes, clears both user-data pointers before destruction,
  retires deleted/removed/invalid bridges, and removes errors after correction.
- Dynamic replacement snapshots Box3D pose, velocities, and awake state; publishes
  the replacement bridge before destroying the old body; then restores state.
- Separate observers mark body, collider, transform, and `ChildOf` changes. The
  private context-ref type is registered before the hierarchy observer so Flecs
  path creation cannot recursively re-enter component registration.
- Runtime teardown nulls the private context ref before destroying Box3D, making
  Flecs finalization-time `OnRemove` callbacks fail closed.
- Every runtime-bearing source union now contains context, shapes, and systems.
  The engine object list also includes all three objects, and the static checker
  enforces exactly-once closure.

## GREEN Evidence

The focused debug build compiled Flecs as C17 and the runtime/context/shapes/
systems/transform/tests as C++17, then printed `ALL PASS`. The optimized focused
physics build and the existing ECS suite also each printed `ALL PASS`.

Two independent absent-at-start directories were then built:

- `MatterEngine3/tests/build/msvc-box3d-task3-green1`
- `MatterEngine3/tests/build/msvc-box3d-task3-green2`

Each compiled exactly all 49 pinned `Libraries/box3d/src/*.c` files as C17 into a
fresh `box3d.lib`, compiled Flecs separately as C17, compiled the six focused
C++17 translation units plus both test files, linked independently, and ran both
executables. Both runs produced:

```text
ALL PASS
ALL PASS
```

After the dynamic-pose hash regression, the changed C++ translation units were
rebuilt and both executables were relinked and rerun against each independently
built 49-source library; all four executions again printed `ALL PASS`.

Final static verification:

```text
PASS: Box3D Phase 2 build contract
 - every Runtime source graph includes context, shapes, and systems exactly once
```

`git diff --check` is clean. A source scan confirms the only pointers assigned to
Box3D user data are `unique_ptr<BridgeRecord>::get()` results; no Flecs component
or query pointer crosses the mutation boundary.

## Review-Finding Fix Evidence

### Collider validation precedence

The combined invalid-material plus Box3D-rejected-hull regression was added before
the fix. Recompiling and linking the focused executable, then running:

```powershell
MatterEngine3\tests\build\msvc-box3d-task3-green2\physics_tests_review_precedence_red.exe
```

exited 1 with the expected behavioral RED:

```text
FAIL: validation case invalid material on Box3D-rejected hull reports the exact error
1 FAILURE(S)
```

After common collider properties were moved ahead of hull construction, the same
focused build printed `ALL PASS`. A private atomic attempt-count seam was then
requested from the test before it existed; MSVC failed with:

```text
error C2039: 'hull_build_attempt_count': is not a member of 'matter::physics::detail'
```

After the seam wrapped both internal hull-build call sites, the combined invalid
case proved the count does not change and the focused executable printed:

```text
ALL PASS
```

### Collision-safe configuration equality

The forced-collision regression first failed to compile on the wished-for seam:

```text
error C2039: 'force_configuration_hash_for_test': is not a member of
'matter::physics::detail::PhysicsContext'
```

Adding only that seam produced the intended behavioral RED:

```text
FAIL: complete desired comparison replaces colliding configurations
1 FAILURE(S)
```

Bridge records now retain the value-owned validated desired configuration. The hash
remains a fast path, followed by explicit equality over every hashed body, material,
filter, shape-kind, and geometry field. With the stored hash forcibly aliased to a
changed sphere configuration, the focused executable rebuilt the body and printed:

```text
ALL PASS
```

### Final review verification

A new absent-at-start directory,
`MatterEngine3/tests/build/msvc-box3d-task3-review-final`, was built with:

```powershell
cl /nologo /std:c17 /O2 /I Libraries\box3d\include /I Libraries\box3d\src /c Libraries\box3d\src\*.c
lib /nologo /OUT:box3d.lib box3d\*.obj
cl /nologo /std:c17 /O2 /I Libraries\flecs /c Libraries\flecs\flecs.c
cl /nologo /std:c++17 /EHsc /O2 /c MatterEngine3\src\ecs\ecs_runtime.cpp MatterEngine3\src\ecs\physics_context.cpp MatterEngine3\src\ecs\physics_shapes.cpp MatterEngine3\src\ecs\physics_systems.cpp MatterEngine3\src\ecs\transform_system.cpp MatterEngine3\tests\physics_tests.cpp MatterEngine3\tests\ecs_tests.cpp
link /nologo /OUT:physics_tests.exe physics_tests.obj ecs_runtime.obj physics_context.obj physics_shapes.obj physics_systems.obj transform_system.obj flecs.obj box3d.lib
link /nologo /OUT:ecs_tests.exe ecs_tests.obj ecs_runtime.obj physics_context.obj physics_shapes.obj physics_systems.obj transform_system.obj flecs.obj box3d.lib
```

All 49 pinned Box3D C files were compiled. Running both executables produced:

```text
ALL PASS
ALL PASS
```

Final commands and results:

```powershell
& .superpowers\sdd\box3d-phase2-static-check.ps1
# PASS: Box3D Phase 2 build contract

git diff --check ba8e4c5
# no output; exit 0
```

## Self-Review

- Opaque Box3D handles remain entirely private; no Box3D include reaches the
  public physics header or private context header.
- Validation copies every component before any `PhysicsError` structural mutation.
- Candidate order uses the entire 64-bit generational entity ID.
- Hull ownership is guarded on all null/success/failure paths, and box hull values
  are never passed to `b3DestroyHull`.
- Replacement failure destroys the prior live body and exposes an error rather
  than allowing stale physics to continue.
- Body deletion, body/collider removal, parenting/nonunit scale, correction,
  archetype moves, user-data identity, and state preservation have direct tests.

## Concerns / Blocked Gates

- GNU Make, GCC/G++, and MinGW execution remain unavailable. Fresh MSVC C17/C++17
  builds plus the strengthened source/link static checker are the available gate.
- Full Viewer/Explorer product links were not run because their required GNU/
  MinGW toolchains are absent; their literal runtime source closures are checked.
