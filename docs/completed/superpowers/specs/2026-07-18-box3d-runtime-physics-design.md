# Box3D Runtime Physics — Phase 2 Design

**Date:** 2026-07-18  
**Status:** Implementation complete; mandatory GNU/MinGW/GPU verification pending
**Depends on:** `2026-07-17-flecs-ecs-foundation-design.md`

## Summary

MatterEngine3 will integrate the repository's existing vendored Box3D library with
the Flecs runtime established in Phase 1. Every `WorldSession` owns exactly one
persistent Box3D world through its existing ECS runtime. Physics remains optional
per entity: adding a rigid-body component and exactly one supported collider makes
an entity physical; removing those components removes its Box3D objects.

Phase 2 is an engine/headless milestone. It includes body and shape lifecycle,
fixed-step simulation, transform and velocity synchronization, contact and sensor
events, force/impulse/teleport commands, ray queries, sphere-overlap queries,
reflection, recoverable diagnostics, and headless tests. It does not add rendering,
MatterViewer UI, sector streaming, gameplay scripting, networking, joints,
character controllers, triangle meshes, heightfields, or compound colliders.

## Existing Context

- Flecs v4.1.6 is vendored and every `WorldSession` owns one `ecs_runtime::Runtime`.
- The fixed pipeline already reserves `PrePhysics`, `Physics`, and `PostPhysics`
  between `FixedUpdate` and `FixedPostUpdate`.
- Local/world transforms, hierarchy validation, fixed-step accumulation, and
  deterministic pipeline ordering are already implemented.
- Box3D is already vendored under `Libraries/box3d/` and used by the offline
  tileset-settling pipeline. Phase 2 reuses that exact dependency; it does not add
  or fork another physics library.
- Box3D is C17 and MatterEngine3 remains C++17.

## Decisions of Record

| Question | Decision |
|---|---|
| Product scope | MatterEngine3/headless integration only |
| Physics ownership | One persistent Box3D world per `WorldSession` |
| Activation | Opt-in per entity through dynamically attached ECS components |
| ECS/physics boundary | Public declarative components; private opaque Box3D handles |
| Body hierarchy | An entity with `RigidBody` must be a Flecs hierarchy root |
| Entity scale | A rigid-body entity must have finite unit `LocalTransform::scale`; scaled visuals belong on children |
| Collider count | Exactly one collider component per rigid-body entity in Phase 2 |
| Collider kinds | Sphere, capsule, box, and convex hull (maximum 32 input points) |
| Authority | Static/kinematic transforms are ECS-authored; dynamic transforms are Box3D-authored after creation |
| Simulation | One `b3World_Step` per ECS fixed tick; no physics work in `FrameUpdate` |
| Parallelism | Box3D uses one worker in Phase 2; multithreading requires a later profiling/design pass |
| Invalid configuration | Fail the entity closed with reflected `PhysicsError`; retry after correction |
| Events | Engine-native, generational Flecs entity IDs; no Box3D handles escape publicly |
| Queries | Synchronous tick-thread ray cast and sphere overlap |
| Reload | Runtime bodies survive authored-content `reload()` and `regenerate()` with their ECS entities |

## Public Component Model

Physics declarations live in a new public `matter/physics.h`. The header exposes
engine-native types and Flecs-facing functions but does not include Box3D headers.

### Body configuration

```cpp
enum class RigidBodyType : uint8_t { Static, Kinematic, Dynamic };

struct RigidBody {
    RigidBodyType type = RigidBodyType::Static;
    float linear_damping = 0.0f;
    float angular_damping = 0.0f;
    float gravity_scale = 1.0f;
    float sleep_threshold = 0.05f;
    bool enable_sleep = true;
    bool continuous = false;
};

struct PhysicsVelocity {
    Float3 linear{};
    Float3 angular{};
};
```

`PhysicsVelocity` is updated from Box3D for dynamic bodies after each step. It is
also initialized at body creation. Later direct velocity changes use the explicit
command API so ordinary component inspection cannot accidentally fight the solver.

### Shared collider properties

```cpp
struct ColliderProperties {
    float density = 1.0f;
    float friction = 0.6f;
    float restitution = 0.0f;
    uint64_t category_bits = 1;
    uint64_t mask_bits = UINT64_MAX;
    bool sensor = false;
    bool contact_events = true;
    bool hit_events = false;
};
```

Density must be positive for dynamic bodies and nonnegative for static/kinematic
bodies. Friction and restitution must be finite and nonnegative. Category and mask
bits map directly to Box3D filtering.

### Collider components

An entity with `RigidBody` must have exactly one of:

```cpp
struct SphereCollider {
    ColliderProperties properties{};
    Float3 center{};
    float radius = 0.5f;
};

struct CapsuleCollider {
    ColliderProperties properties{};
    Float3 point_a{0.0f, -0.5f, 0.0f};
    Float3 point_b{0.0f,  0.5f, 0.0f};
    float radius = 0.5f;
};

struct BoxCollider {
    ColliderProperties properties{};
    Float3 center{};
    Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    Float3 half_extents{0.5f, 0.5f, 0.5f};
};

struct ConvexHullCollider {
    ColliderProperties properties{};
    uint32_t point_count = 0;
    Float3 points[32]{};
};
```

Primitive dimensions, points, rotations, and offsets must be finite. Radii and
half-extents must be positive. A hull needs at least four non-coplanar points and
must be accepted by `b3CreateHull`. Hull point storage is deliberately bounded and
value-owned so Flecs component moves cannot invalidate external allocations.

### World settings

One reflected singleton configures each session's physics world:

```cpp
struct PhysicsSettings {
    Float3 gravity{0.0f, -9.81f, 0.0f};
    uint32_t substeps = 4;
};
```

Gravity must be finite. Substeps are clamped to `[1, 16]`. Settings changes are
applied before the next fixed step. Phase 2 does not expose Box3D worker callbacks
and always configures one worker.

### Recoverable diagnostics

```cpp
enum class PhysicsErrorCode : uint8_t {
    None,
    MissingTransform,
    HasParent,
    NonUnitScale,
    MissingCollider,
    MultipleColliders,
    InvalidBody,
    InvalidCollider,
    HullBuildFailed
};

struct PhysicsError {
    PhysicsErrorCode code = PhysicsErrorCode::None;
};
```

Invalid entities get no live Box3D body. `PhysicsError` is replaced when the
failure changes and removed automatically after a valid body is created. Adding,
removing, or modifying relevant components schedules reconciliation, so fixing an
entity recovers without recreating it or the world.

## Private Runtime Boundary

`ecs_runtime::Runtime` value-owns a private `PhysicsContext`. The context creates
one `b3WorldId` during runtime construction and destroys it during runtime teardown.
It owns:

- the Box3D world;
- stable heap-allocated bridge records;
- entity-to-body and body-to-bridge lookup tables;
- a private category-filtered dynamic-tree query index;
- the latest engine-native event buffer;
- queued force, impulse, velocity, and teleport commands;
- synchronous query scratch storage.

Opaque `b3BodyId` and `b3ShapeId` values live only in private components/source
files. Box3D `userData` points to a stable bridge record, never to Flecs component
storage. A bridge record contains the full generational `flecs::entity_t`, current
opaque IDs, its private query proxy, a physics-transform-pending bit, and a
live/tombstone state. Destroying a body removes its query proxy and clears its
Box3D user data before retiring the bridge record.

Bodies are reconciled in ascending full Flecs entity-ID order. This makes creation,
destruction, validation, and conflict behavior independent of archetype iteration
or hash-map order. Phase 2 targets deterministic ordering and repeatability for a
fixed platform/build, not cross-platform bitwise-identical floating-point output.
Normal fixed ticks reconcile only entities dirtied by relevant Flecs observers.
If recording a dirty entity cannot allocate, the context sets a non-allocating
fail-closed flag and audits all declarative bodies and live bridges on the next
reconciliation boundary.

## Transform and State Authority

All rigid-body entities are hierarchy roots and must have a finite unit local scale.
This gives Box3D an unambiguous world-space pose. Render or logical children may
still inherit the body's transform and may carry arbitrary local scale.

- **Static:** creation and later explicit ECS transform edits are pushed to Box3D
  before stepping. Box3D never writes the transform back.
- **Kinematic:** the current ECS pose becomes the Box3D target for the fixed step.
  Box3D never becomes authoritative over the ECS pose.
- **Dynamic:** creation uses the ECS pose and initial velocity. After that, Box3D
  writes translation, rotation, and velocity back after every step. Direct ordinary
  edits to `LocalTransform` are not treated as teleports and are overwritten.

Dynamic transform writes preserve unit scale, add `TransformDirty`, and are followed
by the existing `FixedPostUpdate` hierarchy propagation. This makes descendants and
the derived `WorldTransform` current before the fixed pipeline ends.
The stable bridge's pending bit distinguishes the deferred physics-authored
`LocalTransform` write without allocating per moved body; the observer still
pose-compares before suppression so a later user edit remains visible.

Explicit commands provide intentional dynamic-body mutation:

```cpp
bool physics_teleport(flecs::entity, Float3 position, Quaternion rotation);
bool physics_set_velocity(flecs::entity, Float3 linear, Float3 angular);
bool physics_apply_force(flecs::entity, Float3 force);
bool physics_apply_impulse(flecs::entity, Float3 impulse);
bool physics_wake(flecs::entity);
```

Commands validate that the entity and session still exist and execute before the
next Box3D step. Teleport and velocity commands are last-write-wins per entity;
wake is idempotent; force and impulse commands accumulate in enqueue order. Calls
for dead, cross-world, non-dynamic, or invalid physics entities return `false`.
Structural reactions requested by event consumers use ordinary Flecs deferral and
cannot mutate Box3D during `b3World_Step`.

## Fixed-Pipeline Ordering

The reserved coarse phases remain public. Engine-owned subphases make the detailed
order explicit rather than relying on system registration order:

```text
FixedUpdate
PrePhysics                 application prepares ECS state / commands
PhysicsReconcile           validate, create, update, destroy bodies and shapes
PhysicsPush                settings, static/kinematic poses, queued commands
Physics                    exactly one b3World_Step(fixed_delta, substeps)
PhysicsPull                snapshot events; pull dynamic poses and velocities
PostPhysics                application consumes stable ECS state and event buffer
FixedPostUpdate            propagate WorldTransform and clear dirty tags
```

`PhysicsReconcile`, `PhysicsPush`, and `PhysicsPull` are engine phase tags registered
in the core module. `Physics` contains one engine step system. `PostPhysics` is the
supported phase for gameplay systems that react to physics. No physics phase is
part of the variable `FrameUpdate` pipeline.

If a frame runs multiple fixed steps, Box3D steps once per fixed step and the public
event buffer contains events from the most recently completed fixed step. A valid
tick that runs no fixed steps leaves the last completed-step events available.
Invalid ticks drain no physics commands and perform no reconciliation or stepping.

## Events

After each step the context consumes Box3D's contiguous body, contact, and sensor
event arrays into engine-native buffers. Public records contain full generational
Flecs entity IDs and no pointers or Box3D IDs:

- moved/awoke/fell-asleep body events;
- contact begin and end;
- contact hit position, normal, and approach speed;
- sensor begin and end.

Events involving a destroyed, stale, or cross-world bridge are discarded. Pairwise
events are normalized to ascending entity-ID order and the complete buffers are
sorted by event kind and entity IDs for deterministic consumption. The buffers are
read-only and remain valid until the next completed fixed step.

`physics_events(flecs::world&)` returns a const view of the current buffers. Event
consumers may schedule ECS structural changes, but body creation/destruction waits
for the next reconciliation boundary.

## Queries

Phase 2 exposes synchronous tick-thread queries:

```cpp
struct PhysicsRayHit {
    flecs::entity_t entity = 0;
    Float3 position{};
    Float3 normal{};
    float fraction = 0.0f;
};

bool physics_ray_cast(
    flecs::world&, Float3 origin, Float3 translation,
    uint64_t category_mask, PhysicsRayHit& closest_hit);

std::vector<flecs::entity_t> physics_overlap_sphere(
    flecs::world&, Float3 center, float radius, uint64_t category_mask);
```

Queries reject nonfinite inputs, cannot run during `b3World_Step`, discard stale
bridges, and return deterministic entity ordering. Ray cast reports the closest
accepted hit, breaking equal-fraction ties by the smaller full entity ID. The
query `category_mask` selects collider category bits independently of the
collider's simulation `mask_bits`; a collider with `mask_bits == 0` remains
queryable. A private dynamic tree indexes all live shapes by category and AABB, so
queries precise-test only broadphase candidates rather than scanning all bridges.
Proxies publish and retire transactionally with bridges and track static pushes,
kinematic/dynamic body events, and teleports. Sphere overlap deduplicates entities
before sorting because one body may later own more than one Box3D shape even though
Phase 2 creates only one.

## Configuration Changes and Lifecycle

Flecs observers mark affected entities for reconciliation when body, collider,
transform, or hierarchy state changes. Reconciliation is transactional per entity:

1. validate the complete desired configuration;
2. build any temporary hull data;
3. create a replacement Box3D body and shape;
4. preserve dynamic pose, velocity, and awake state when rebuilding an existing
   dynamic body unless an explicit teleport/velocity command supersedes them;
5. publish the new private handles;
6. destroy the old body and retire its bridge.

If validation or replacement creation fails, the previous live body is removed and
the entity fails closed with `PhysicsError`; stale physics must not continue under a
configuration the ECS no longer represents.

Removing `RigidBody`, removing its only collider, adding a second collider, deleting
the entity, or parenting it destroys the live body at the next reconciliation
boundary. Destroying the session destroys the entire Box3D world after ECS systems
stop. Reload and regenerate leave the runtime, ECS entities, and Box3D world intact.

## Reflection and Tooling

Body types, settings, primitive colliders, shared properties, velocity, and error
codes are registered with Flecs metadata. Hull point storage may be registered as a
bounded opaque array while `point_count` and common properties remain inspectable.
Private handles, bridge records, pending commands, and event buffers are not
reflected or serialized.

Reflection remains an editor/debug contract, not a persistence or networking wire
format. Phase 2 adds no MatterViewer inspector.

## Error Handling

- Per-entity configuration failures use `PhysicsError` and do not stop the world.
- Invalid public commands and queries return `false` or an empty result.
- Box3D world creation failure is a fatal `WorldSession` construction failure.
- No exception or callback escapes through Box3D C callbacks.
- Event conversion and queries ignore stale bridge records rather than dereferencing
  invalid Flecs entities.
- The context records cumulative diagnostic counters for rejected configurations,
  stale events, failed commands, body creations/destructions, and completed steps.

## Build Integration

The existing vendored Box3D source/archive and MinGW archive remain the sole Box3D
dependency. MatterEngine3's archive gains the physics bridge C++ implementation.
Every executable or test that links the ECS runtime must also link exactly one
Box3D archive. Linux and Windows include/link lists are checked statically, matching
the one-C-Flecs build-contract checks from Phase 1.

Phase 2 adds a dedicated headless `run-physics` target and includes it in the normal
MatterEngine3 `test` target and repository build-all gate. The focused target builds
Box3D as C17 and the bridge/tests as C++17.

## Test Strategy

### Component and lifecycle tests

- module registration and reflection metadata;
- exactly one persistent physics context/world per runtime/session;
- empty physics world behavior;
- creation/removal/recreation for each body type and all four collider kinds;
- entity deletion and session teardown leave no valid body or bridge;
- ECS entities and bodies survive reload/regenerate;
- component moves/archetype changes do not invalidate Box3D user data;
- configuration rebuild preserves dynamic state where specified.

### Validation and recovery tests

- missing/multiple colliders, parented body, nonunit scale, invalid dimensions,
  invalid material, and failed hull construction each produce the correct error;
- invalid entities have no live Box3D body;
- correcting each configuration removes the error and creates a body;
- parenting or invalidating a formerly valid body removes it;
- dead, stale-generation, and cross-world commands fail safely.

### Simulation and synchronization tests

- gravity moves a dynamic sphere onto a static floor;
- static transform edits and kinematic targets reach Box3D before the step;
- dynamic poses and velocities reach ECS after the step;
- descendants receive the dynamic parent's updated world transform by the end of
  `FixedPostUpdate`;
- teleport, velocity, force, impulse, and wake commands have the specified effects;
- exactly one physics step occurs per fixed step, including catch-up frames;
- zero-fixed-step and invalid ticks do not step physics;
- body reconciliation and event ordering are deterministic by full entity ID.

### Event and query tests

- contact begin/end and hit events map both bodies correctly;
- sensor begin/end events map sensor and visitor correctly;
- sleep/wake/movement events map the body correctly;
- event records contain no stale IDs after destruction;
- post-physics systems can read events and safely defer structural changes;
- closest ray hit, filtering, misses, and stale-bridge rejection;
- sphere overlap filtering, deduplication, and sorted output.

### Regression gates

- Phase 1 ECS tests remain green;
- existing tileset Box3D physics tests remain green;
- MatterEngine3 normal tests and build-all pass on Linux;
- clean MinGW MatterViewer and temporary ExplorerDemo builds link the new runtime;
- no rendering, sector-streaming ECS, editor UI, joints, character controller, or
  networking code appears in the Phase 2 diff.

## Out of Scope

- MatterViewer physics UI, debug drawing, gizmos, or sandbox scenes;
- sector streaming or ExplorerDemo retirement;
- render proxies and dynamic-object scene synchronization;
- triangle meshes, heightfields, compound shapes, or multiple colliders per entity;
- joints, motors, vehicles, ragdolls, and character controllers;
- persistent scripting and authored-world physics instantiation;
- networking, prediction, rollback, and replication;
- Box3D multithreading or cross-platform bitwise determinism.

## Success Criteria

Phase 2 is complete when:

1. Every `WorldSession` owns one persistent Box3D world through its ECS runtime.
2. Adding valid rigid-body and collider components creates a body; removing or
   invalidating them destroys it without stale handles.
3. Static/kinematic input and dynamic output obey the documented authority rules.
4. Box3D steps exactly once per ECS fixed tick in deterministic phase order.
5. Four collider kinds, commands, events, ray casts, and sphere overlaps work through
   engine-native APIs with no public Box3D handles.
6. Invalid entities fail closed with reflected, recoverable diagnostics.
7. Headless physics, ECS regression, existing tileset physics, Linux build-all, and
   clean MinGW product gates pass.
8. No Phase 3+ streaming, rendering/editor bridge, scripting, or networking work is
   included.
