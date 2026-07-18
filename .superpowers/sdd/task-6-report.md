# Task 6 Report: Engine-Native Physics Events

## Status

Implemented Box3D movement, contact begin/end, contact hit, and sensor begin/end
conversion into the existing engine-native `PhysicsEvents` contract. Completed
buffers publish immediately after each successful Box3D step and are visible to
`PostPhysics`, `PhysicsPull`, and callers after the fixed tick.

## TDD Evidence

The first RED added controlled movement/impact/sensor fixtures before production
changes. Against the Task 5 implementation, the executable reported the expected
empty-event failures for body movement, contact begin/end, hit data, sensor
begin/end, PostPhysics visibility, and buffer replacement. One additional
generation assertion initially targeted the second allocation rather than the
immediately recycled first allocation; correcting the fixture to the recycled
floor ID left only the seven event-feature failures.

The stale-participant seam was then added as a separate test-first cycle. Before
its implementation the current-source executable failed to link with the expected
missing `tombstone_event_participant_for_test` symbol. The minimal implementation
made the focused suite print `ALL PASS`.

## Implementation

- Every bridge records the exact normalized owning Flecs world and full
  generational entity ID.
- `PhysicsContext::step` copies all Box3D body/contact/sensor arrays immediately
  after `b3World_Step`, while the transient arrays and stable bridge user data are
  valid.
- Body and shape records are accepted only when the stable heap bridge, exact
  owning world, live full ID, current bridge-map identity, Box3D handle, and user
  data all agree. Invalid, tombstoned, or cross-world records are dropped and
  increment `stale_events`.
- Contact and sensor pairs normalize to ascending full IDs. Hit normals are
  inverted whenever Box3D's A/B order is swapped so the normal still points from
  published `first` to `second`.
- Body, contact, hit, and sensor buffers sort deterministically by full IDs.
- A complete local `PhysicsEvents` value replaces the previous published buffer
  before `PhysicsPull` and `PostPhysics`; no pointer into Box3D event storage is
  retained.
- The private one-shot tombstone seam models a participant retired between step
  and conversion without introducing unsafe concurrent structural mutation.
- Event processing performs no ECS structural mutation. A `PostPhysics` reader
  test proves consumers can create entities/components through Flecs deferral.

## Coverage

- exact full generational IDs and ascending normalized pairs;
- awake movement and transition-to-sleep body events;
- contact begin/end and high-speed hit point, normal, and approach speed;
- sensor begin/end;
- per-tick replacement rather than accumulation;
- publication before a fixed-pipeline `PostPhysics` reader;
- safe deferred structural creation by that reader;
- stale-participant dropping and `stale_events` accounting;
- Task 1-5 physics and ECS regression suites.

## Verification

Two absent-at-start MSVC directories rebuilt the complete focused graph:

- `msvc-box3d-task6-verify1`: 49 Box3D C17 objects, Flecs C17, all current
  physics/ECS C++17 sources; physics `ALL PASS`; the controller timeout occurred
  only after compiling `ecs_tests.obj`, which was immediately linked and run
  separately as `ALL PASS`.
- `msvc-box3d-task6-verify2`: uninterrupted fresh build of 49 Box3D C17 objects,
  Flecs C17, all current physics/ECS C++17 sources; both executables `ALL PASS`.

Additional gates:

- Box3D Phase 2 build-contract checker: `PASS`.
- Explicit Task 7+ ray/overlap implementation scan: `PASS` (none found).
- `git diff --check`: clean.

GNU Make/GCC and MinGW product-toolchain gates remain unavailable on this host;
the MSVC evidence is not represented as a substitute for those unavailable gates.

## Independent Review

An independent read-only task review found no Critical, Important, or Minor
issues and returned `PASS`.
