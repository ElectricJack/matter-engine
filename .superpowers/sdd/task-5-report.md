# Task 5 Report: Dynamic Body Command Queue

## Status

Implemented Task 5 on reviewed Task 4 base `e95fbc7`. The public teleport,
velocity, force, impulse, and wake functions now admit only live dynamic bodies
owned by a runtime physics context. Accepted work is value-copied into the
originating context and executes in `PhysicsPush` before the immediately
following Box3D step.

No Task 6+ contact/sensor event conversion or query implementation was added.

## RED Evidence

The command matrix was added to `physics_tests.cpp` before production changes.
It covered:

- last-write-wins teleport and velocity;
- ordered, copied force and impulse payloads;
- idempotent wake;
- execution before the following step;
- context-less, unreconciled, invalid, static, kinematic, dead, stale-generation,
  and nonfinite rejection;
- same-real-world staged entities and independent runtime worlds;
- invalid-tick retention and push-time revalidation;
- `failed_commands` accounting for stale, invalid, changed-type, and removed
  queued targets.

MSVC C++17 compilation against the unchanged Task 4 header failed at the new
expectations because `PhysicsCommandKind`, `PhysicsCommandTraceEntry`, and
`PhysicsContext::last_command_trace` did not exist. The public functions also
had declarations but no definitions at the Task 4 boundary.

The required GNU command could not run because `make` is not installed or
available on this host's PATH.

## Implementation

- Every queued record contains only the originating real-world pointer, full
  generational entity ID, command kind, and copied numeric payload.
- Public admission uses `ecs_get_world` only to normalize a Flecs stage for
  world identity and context lookup. It retains the caller's full entity ID.
- Admission rejects a missing context, dead/stale full ID, non-dynamic ECS
  declaration, `PhysicsError`, missing/invalid bridge, mismatched bridge user
  data, non-dynamic Box3D body, and invalid numeric payload.
- The origin world's private `PhysicsContextRef` must point back to the exact
  receiving context before admission, so colliding full IDs in two runtimes
  cannot cross the context boundary.
- Teleport and velocity use per-context full-ID maps and overwrite only accepted
  work for the same entity. Force and impulse use vectors that preserve lock
  acquisition/enqueue order. Wake uses an identity set.
- Queue mutation and the `PhysicsPush` swap are protected by the context's
  command mutex. Work enqueued after the swap remains for a later push.
- `PhysicsPush` swaps all pending containers before applying work. Teleport and
  velocity maps and the wake set are copied into full-ID-sorted vectors;
  force/impulse vectors retain their exact order.
- Every drained record is revalidated against the runtime real world, full-ID
  liveness, current ECS body type/error, stable bridge identity, live body and
  shape handles, Box3D body type, and both body/shape user-data pointers.
- A rejected drained record increments `PhysicsStats::failed_commands` exactly
  once. Admission rejection does not count as failed queued work.
- Teleport normalizes its copied quaternion before enqueue. Velocity sets both
  vectors. Force and impulse apply at the center and wake the body. Wake sets
  awake state explicitly. Teleport also wakes after setting the transform so a
  formerly sleeping body emits pull-visible movement on the following step.
- A private applied-command trace provides deterministic coverage without
  exposing Box3D handles publicly.

## Debugging Evidence

The first incremental linked run exposed two test/build isolation issues:

- A staged-handle fixture left Flecs configured with two stages before calling
  `Runtime::tick`, which this single-threaded runtime configuration does not
  progress. Restoring one stage after the readonly admission check removed the
  stall without changing production behavior.
- Linking the changed private header against stale Task 4 C++ objects made the
  existing PostPhysics/FixedPostUpdate probes observe old ordering. Rebuilding
  every current header consumer restored both exact Task 4 assertions. This was
  stale-object contamination, not a pipeline regression.

One new assertion initially inspected velocity after five dynamic spheres were
created at the same position. The applied-command trace was correct, while
Box3D collision resolution changed the post-step velocity. Separating the
fixture bodies made the assertion isolate command behavior.

After those fixture/build corrections, the full current-source focused physics
executable printed `ALL PASS`.

## Independent Review Fixes

The first independent review found two Important edge cases. Regressions were
added before either production fix. Against the unchanged implementation, the
focused executable exited 1 with exactly:

```text
FAIL: a context rejects a foreign real world even when the full ID collides
FAIL: sleeping teleport-only command pulls into ECS after the next step
2 FAILURE(S)
```

The first fix verifies that the originating world's private context singleton
owns the receiving context before consulting its same-numbered bridge. An
intermediate run removed only the cross-world failure. The second fix wakes a
successfully teleported body so Box3D publishes movement for `PhysicsPull`.

Both fresh green directories then rebuilt every current C++ header consumer.
Physics and ECS again printed `ALL PASS` in both directories, with 49 fresh
Box3D objects in each. The same reviewer re-checked the fixes and returned
`PASS` with no remaining Critical or Important issue.

## Fresh Verification

Two absent-at-start directories were built independently:

- `MatterEngine3/tests/build/msvc-box3d-task5-green1`
- `MatterEngine3/tests/build/msvc-box3d-task5-green2`

Each verification:

1. compiled all 49 pinned `Libraries/box3d/src/*.c` files as C17;
2. archived a fresh `box3d.lib`;
3. compiled vendored Flecs separately as C17;
4. compiled current `ecs_runtime.cpp`, `physics_context.cpp`,
   `physics_shapes.cpp`, `physics_systems.cpp`, `transform_system.cpp`,
   `physics_tests.cpp`, and `ecs_tests.cpp` as C++17;
5. linked and ran independent physics and ECS executables.

Both runs reported:

```text
BOX3D_OBJECTS=49
ALL PASS
ALL PASS
```

Additional gates:

```text
PASS: Box3D Phase 2 build contract
PASS: no Task 6+ event/query implementation
git diff --check: clean
```

## Coverage

- teleport/velocity last-write-wins behavior and full-ID sorted drain;
- force/impulse payload copying, accumulation, and enqueue order;
- wake idempotence;
- command state remains unchanged until push, then affects the next step;
- nonfinite vectors and invalid teleport quaternion rejection;
- context-less, unreconciled, invalid, static, kinematic, dead, and stale
  generational entity rejection for all five public functions;
- same-real-world staged handle acceptance without replacing its full ID;
- independent real worlds cannot drain each other's pending work;
- invalid runtime ticks retain valid work for the next fixed tick;
- stale generation reuse, invalidated bridge, dynamic-to-kinematic change, and
  body removal are rejected again at push and counted;
- valid work in a mixed accepted/rejected drain still reaches Box3D;
- swapped work drains exactly once;
- all reviewed Task 4 phase, synchronization, and accounting tests remain green.

## Concerns / Blocked Gates

- GNU Make/GCC execution remains blocked because `make` is unavailable.
- MinGW execution remains blocked because the configured MinGW compiler is not
  installed on this host.
- The fresh MSVC C17/C++17 evidence is supplemental and is not represented as a
  substitute for those unavailable product-toolchain gates.
