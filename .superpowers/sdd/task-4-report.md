# Task 4 Report: Fixed-Step Simulation and Transform Synchronization

## Status

Implemented Task 4 on reviewed Task 3 base `26ff737`. Each valid ECS fixed
step now runs the private physics sequence `Reconcile -> Push -> Step -> Pull`
exactly once. Static and kinematic transforms push from ECS, Box3D steps with
the current reflected settings, and moved dynamic bodies pull pose and velocity
back into ECS before fixed-post transform propagation.

No public command queue, contact/sensor event publication, or physics query
behavior was added.

## RED Evidence

The simulation and accounting tests were added before production changes and
compiled/linked against the unchanged Task 3 objects. The executable exited 1
with 11 expected failures:

```text
FAIL: dynamic sphere falls and settles on the static floor
FAIL: settled dynamic sphere publishes its resting velocity
FAIL: falling-sphere simulation steps Box3D once per fixed tick
FAIL: static ECS transform reaches Box3D during push
FAIL: kinematic target reaches Box3D in the same fixed step
FAIL: dynamic pose pulls into ECS with unit scale after step
FAIL: dynamic linear and angular velocity pull into ECS after step
FAIL: dynamic descendant world transform is current after FixedPostUpdate
FAIL: updated gravity applies before the next Box3D step
FAIL: settings update still performs exactly one Box3D step
FAIL: three-step catch-up performs exactly three Box3D steps
11 FAILURE(S)
```

The explicit substep and internal phase-trace assertions were then added before
their seams. MSVC compilation failed specifically because
`PhysicsContext::last_step_substeps`, `PhysicsSystemStage`, and
`PhysicsContext::fixed_step_trace` did not exist.

The required GNU RED command could not execute because `make` is not installed.

## Implementation

- Registered `MatterPhysicsReconcile`, `MatterPhysicsPush`,
  `MatterPhysicsStep`, and `MatterPhysicsPull` in their refined fixed phases.
  Every system carries `FixedPipelineSystem`; none carries
  `FramePipelineSystem`.
- Push copies `PhysicsSettings`, applies finite gravity, clamps substeps to
  `[1, 16]`, and visits private bridges in ascending full generational entity
  ID order.
- Static bodies receive ECS transforms directly. Static and kinematic push
  normalize the current finite, nonzero ECS quaternion before any Box3D call;
  kinematic bodies then receive a `b3Body_SetTargetTransform` for the current
  fixed delta before stepping.
- Step calls `b3World_Step` exactly once and increments `PhysicsStats::steps`
  only after that call.
- Pull consumes only Box3D's private movement-event buffer. It validates each
  stable heap bridge pointer and full private body ID, copies all Box3D values,
  then writes dynamic `LocalTransform`, `PhysicsVelocity`, and
  `TransformDirty`. The Flecs system explicitly declares all three writes so
  PostPhysics and FixedPostUpdate scheduling sees the mutations. No Flecs
  component/query pointer survives a structural mutation.
- Dynamic local transforms use unit scale. The existing `FixedPostUpdate`
  propagation system consumes the dirty root/subtree before the fixed pipeline
  ends, keeping descendants current.
- The private trace records the actual four engine systems, not merely phase
  metadata, and resets at each reconciliation boundary.

## GREEN and Root-Cause Evidence

The first implementation run exposed test-layer assumptions rather than missing
production behavior:

- Task 3's `reconcile` helper used a full runtime tick. Once Task 4 added real
  stepping, that helper advanced gravity and velocity while testing replacement
  state. It now calls the private reconciliation operation directly so Task 3
  lifecycle tests continue to isolate reconciliation.
- Runtime caps one frame's accumulator contribution at 0.25 seconds. Initial
  0.5-second settings and 0.3-second catch-up cases therefore ran zero and two
  fixed steps, respectively. The cases now use unclamped deltas while testing
  the same one-step and three-step contracts.
- Pinned Box3D documents kinematic target results as close rather than exact;
  captured evidence showed exact translation and a small quaternion difference.
  The rotation assertion uses an appropriate tolerance.
- The descendant expectation originally assumed the seeded rotation stayed
  fixed despite nonzero angular velocity. It now composes the child's local
  translation with the actual fixed-post root matrix.

After those isolation corrections, the focused physics executable and existing
ECS executable both printed `ALL PASS`.

## Independent Review Fixes

Three review regressions were added before their production fixes:

- substeps `0` and `17` exercise the approved `[1, 16]` boundaries;
- static and kinematic finite non-unit ECS quaternions exercise push safety;
- a PostPhysics reader and a custom phase after `FixedPostUpdate` exercise
  visibility of pull writes and descendant propagation while the root's old
  `WorldTransform` is deliberately stale.

Against unchanged production objects, the boundary and visibility executable
exited 1 with:

```text
FAIL: requested substeps above the approved range clamp to sixteen
FAIL: PostPhysics observes pull pose, velocity, and dirtiness
FAIL: phase after FixedPostUpdate observes descendant from pulled local pose
3 FAILURE(S)
```

With the quaternion regression enabled, the unchanged push passed the raw
non-unit value into Box3D and triggered Box3D's quaternion-validity assertion,
stalling the combined debug test process. This was isolated before editing
production code.

The fixes changed the substep ceiling to 16, normalize a copied current ECS
rotation before static or kinematic Box3D calls, and add explicit Flecs write
terms for `LocalTransform`, `PhysicsVelocity`, and `TransformDirty`. The
expanded focused physics suite and the neighboring ECS suite then both printed
`ALL PASS`.

For final review verification, current `physics_context.cpp`,
`physics_systems.cpp`, `physics_tests.cpp`, and `ecs_tests.cpp` were compiled as
C++17 in an absent-at-start directory and linked against the second fresh Task
4 Box3D/Flecs build. Both executables printed `ALL PASS`. The review build
directory and generated PDB were removed afterward.

## Fresh Verification

Two independent absent-at-start directories were built:

- `MatterEngine3/tests/build/msvc-box3d-task4-green1`
- `MatterEngine3/tests/build/msvc-box3d-task4-green2`

Each build:

1. compiled exactly all 49 pinned `Libraries/box3d/src/*.c` files as C17;
2. archived a fresh `box3d.lib`;
3. compiled vendored Flecs separately as C17;
4. compiled `ecs_runtime.cpp`, `physics_context.cpp`, `physics_shapes.cpp`,
   `physics_systems.cpp`, `transform_system.cpp`, `physics_tests.cpp`, and
   `ecs_tests.cpp` as C++17;
5. linked and ran independent physics and ECS executables.

Both builds reported:

```text
BOX3D_OBJECTS=49
ALL PASS
ALL PASS
```

Final focused gates:

```text
PASS: Box3D Phase 2 build contract
PASS: no Task 5+ command/contact/sensor/query behavior
git diff --check: clean
```

The generated `vc140.pdb` was removed after verification.

## Coverage

- dynamic sphere settles on a static floor;
- static edits and kinematic targets reach Box3D in the fixed step;
- dynamic translation, quaternion, linear velocity, and angular velocity pull
  back to ECS with unit scale;
- a moved dynamic root's descendant world transform is current after
  `FixedPostUpdate`;
- gravity and substeps update before the next step;
- substeps clamp at both approved boundaries, including `0 -> 1` and
  `17 -> 16`;
- finite non-unit static and kinematic ECS rotations are normalized before
  Box3D receives them;
- PostPhysics observes pulled pose, velocity, and dirtiness, and a phase after
  `FixedPostUpdate` observes the resulting descendant world transform even
  when the root world transform was stale;
- one-to-one step accounting includes a three-step catch-up;
- valid zero-fixed-step and all invalid `TickDesc` cases perform no Box3D step;
- explicit engine trace is exactly `Reconcile, Push, Step, Pull`.

## Concerns / Blocked Gates

- GNU Make/GCC execution remains blocked because `make` is unavailable.
- MinGW execution remains blocked because
  `x86_64-w64-mingw32-g++-posix` is unavailable.
- The fresh MSVC C17/C++17 evidence is supplemental and is not represented as a
  substitute for those unavailable product-toolchain gates.
