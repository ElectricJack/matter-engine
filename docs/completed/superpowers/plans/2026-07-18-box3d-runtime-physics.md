# Box3D Runtime Physics — Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add one persistent Box3D world to every MatterEngine3 `WorldSession` and expose opt-in ECS rigid bodies, four convex collider kinds, fixed-step synchronization, commands, events, queries, and recoverable diagnostics.

**Architecture:** Public reflected Flecs components describe physics intent without exposing Box3D. A private session-owned `PhysicsContext` owns the `b3WorldId`, stable entity bridge records, opaque body/shape IDs, commands, events, and queries. Engine systems reconcile and push state before one Box3D step per fixed tick, then pull dynamic state before public `PostPhysics` systems run.

**Tech Stack:** C++17, C17, Flecs v4.1.6, vendored Box3D, GNU Make/MinGW product builds, MSVC supplemental headless verification.

## Global Constraints

- Reuse only `Libraries/box3d/`; do not fetch, fork, or vendor another physics library.
- Exactly one persistent Box3D world belongs to each `WorldSession`/`ecs_runtime::Runtime`.
- Public headers expose no Box3D types or headers.
- A rigid-body entity is a hierarchy root with finite unit local scale and exactly one collider.
- Phase 2 supports sphere, capsule, box, and convex hull with at most 32 input points.
- Static/kinematic transforms are ECS-authored; dynamic transforms are Box3D-authored after creation.
- Box3D steps exactly once per ECS fixed step and never in `FrameUpdate`.
- Use one Box3D worker; do not add multithreaded physics.
- Invalid entities fail closed with reflected, recoverable `PhysicsError`.
- Stable bridge/user-data records never point into Flecs component storage.
- Public events and query hits contain full generational Flecs entity IDs, never Box3D IDs.
- Preserve Phase 1 invalid-tick, reload/regenerate, hierarchy, and fixed-accumulator contracts.
- No rendering/UI, sector ECS migration, joints, characters, meshes/heightfields, scripting, or networking.
- Required GNU/MinGW/GPU gates must be reported truthfully if this host still cannot execute them; supplemental MSVC evidence is not a substitute.

## File Structure

- `MatterEngine3/include/matter/physics.h` — public reflected configuration/state, events, commands, queries, and stats.
- `MatterEngine3/src/ecs/physics_context.h` — private context and stable bridge interface.
- `MatterEngine3/src/ecs/physics_context.cpp` — Box3D world ownership, commands, stepping, event conversion, and queries.
- `MatterEngine3/src/ecs/physics_shapes.h` — private validated desired-body/shape representation.
- `MatterEngine3/src/ecs/physics_shapes.cpp` — component validation and Box3D body/shape construction.
- `MatterEngine3/src/ecs/physics_systems.cpp` — Flecs registration, reconciliation, phase systems, and transform synchronization.
- `MatterEngine3/src/ecs/ecs_runtime.h/.cpp` — value ownership and construction/destruction ordering.
- `MatterEngine3/tests/physics_tests.cpp` — focused headless Phase 2 behavior suite.
- `MatterEngine3/tests/Makefile` — `run-physics` target and standard-test delegation.
- Product/engine Makefiles and `build-all.sh` — source/link closure for the new runtime dependency.

---

### Task 1: Public Physics Contract and Reflection

**Files:**
- Create: `MatterEngine3/include/matter/physics.h`
- Create: `MatterEngine3/tests/physics_tests.cpp`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: `matter::ecs::{LocalTransform, TransformDirty, PrePhysics, Physics, PostPhysics, FixedPipelineSystem}`.
- Produces: all public Phase 2 types and `PhysicsModule`; later tasks implement the declared functions.

- [ ] **Step 1: Add a dedicated failing reflection test target**

Create `physics_tests.cpp` with the existing `CHECK`/failure-counter harness. Import
`CoreModule` and the new `PhysicsModule`, then assert component liveness, enum
constants, field names/types, fieldless phase tags, default values, and that
`matter/physics.h` compiles without any Box3D include path.

```cpp
#include "matter/physics.h"

static void test_physics_contract_and_reflection() {
    flecs::world world;
    world.import<matter::ecs::CoreModule>();
    world.import<matter::physics::PhysicsModule>();

    CHECK(world.component<matter::physics::RigidBody>().is_alive(),
          "RigidBody registered");
    CHECK(world.component<matter::physics::SphereCollider>().is_alive(),
          "SphereCollider registered");
    CHECK(world.component<matter::physics::PhysicsReconcile>().has(flecs::Phase),
          "PhysicsReconcile is a phase");
    CHECK(world.component<matter::physics::PhysicsPush>().has(flecs::Phase),
          "PhysicsPush is a phase");
    CHECK(world.component<matter::physics::PhysicsPull>().has(flecs::Phase),
          "PhysicsPull is a phase");
}
```

Add `physics-contract-tests` and `run-physics-contract` recipes that compile only
Flecs, `ecs_runtime.cpp`, `transform_system.cpp`, and the new test. Do not link
Box3D yet.

- [ ] **Step 2: Run the contract test to verify RED**

Run: `make -C MatterEngine3/tests run-physics-contract`

Expected: compile failure because `matter/physics.h` and `PhysicsModule` do not exist.

- [ ] **Step 3: Define the complete public contract**

Create `matter/physics.h` with the exact types from the design:

```cpp
#pragma once

#include <cstdint>
#include <vector>
#include "matter/ecs.h"

namespace matter::physics {
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
struct PhysicsVelocity { Float3 linear{}; Float3 angular{}; };
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
struct SphereCollider { ColliderProperties properties{}; Float3 center{}; float radius = 0.5f; };
struct CapsuleCollider {
    ColliderProperties properties{};
    Float3 point_a{0.0f, -0.5f, 0.0f};
    Float3 point_b{0.0f, 0.5f, 0.0f};
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
struct PhysicsSettings { Float3 gravity{0.0f, -9.81f, 0.0f}; uint32_t substeps = 4; };
enum class PhysicsErrorCode : uint8_t {
    None, MissingTransform, HasParent, NonUnitScale, MissingCollider,
    MultipleColliders, InvalidBody, InvalidCollider, HullBuildFailed
};
struct PhysicsError { PhysicsErrorCode code = PhysicsErrorCode::None; };
struct PhysicsReconcile {};
struct PhysicsPush {};
struct PhysicsPull {};

struct PhysicsBodyEvent { flecs::entity_t entity = 0; bool awake = false; };
struct PhysicsPairEvent { flecs::entity_t first = 0; flecs::entity_t second = 0; };
struct PhysicsHitEvent {
    flecs::entity_t first = 0; flecs::entity_t second = 0;
    Float3 position{}; Float3 normal{}; float approach_speed = 0.0f;
};
struct PhysicsEvents {
    std::vector<PhysicsBodyEvent> body;
    std::vector<PhysicsPairEvent> contact_begin, contact_end;
    std::vector<PhysicsHitEvent> contact_hit;
    std::vector<PhysicsPairEvent> sensor_begin, sensor_end;
};
struct PhysicsStats {
    uint64_t steps = 0, bodies_created = 0, bodies_destroyed = 0;
    uint64_t rejected_configurations = 0, failed_commands = 0, stale_events = 0;
    uint32_t live_bodies = 0;
};
struct PhysicsRayHit {
    flecs::entity_t entity = 0; Float3 position{}; Float3 normal{}; float fraction = 0.0f;
};

struct PhysicsModule { explicit PhysicsModule(flecs::world&); };

const PhysicsEvents& physics_events(const flecs::world&);
PhysicsStats physics_stats(const flecs::world&);
bool physics_teleport(flecs::entity, Float3, Quaternion);
bool physics_set_velocity(flecs::entity, Float3, Float3);
bool physics_apply_force(flecs::entity, Float3);
bool physics_apply_impulse(flecs::entity, Float3);
bool physics_wake(flecs::entity);
bool physics_ray_cast(flecs::world&, Float3, Float3, uint64_t, PhysicsRayHit&);
std::vector<flecs::entity_t> physics_overlap_sphere(
    flecs::world&, Float3, float, uint64_t);
} // namespace matter::physics
```

Register all inspectable fields and enum constants. Register `PhysicsReconcile`,
`PhysicsPush`, and `PhysicsPull` as phases with dependency order:

```text
PrePhysics -> PhysicsReconcile -> PhysicsPush -> Physics -> PhysicsPull -> PostPhysics
```

Keep the Phase 1 edges; the new edges refine them without changing the outer order.
Set the `PhysicsSettings` singleton during module import.

- [ ] **Step 4: Run contract and Phase 1 tests GREEN**

Run: `make -C MatterEngine3/tests run-physics-contract run-ecs`

Expected: both suites print `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/include/matter/physics.h MatterEngine3/src/ecs/ecs_runtime.cpp \
  MatterEngine3/tests/physics_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat(physics): define reflected ECS physics contract"
```

---

### Task 2: Session-Owned Box3D Context

**Files:**
- Create: `MatterEngine3/src/ecs/physics_context.h`
- Create: `MatterEngine3/src/ecs/physics_context.cpp`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/tests/physics_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Modify: `ExplorerDemo/Makefile`
- Create: `.superpowers/sdd/box3d-phase2-static-check.ps1`

**Interfaces:**
- Consumes: `PhysicsModule`, `PhysicsSettings`, Box3D C API.
- Produces: `physics::detail::PhysicsContext`, context lookup, stable bridge ownership, stats, and one world per runtime.

- [ ] **Step 1: Write failing context lifetime tests**

Add tests that construct two runtimes, assert independent live contexts with zero
bodies, destroy one without affecting the other, and verify `PhysicsStats` starts at
zero. Add a Box3D world-validity test seam only in `physics_context.h`; do not expose
the `b3WorldId` publicly.

```cpp
static void test_one_physics_world_per_runtime() {
    matter::ecs_runtime::Runtime first;
    matter::ecs_runtime::Runtime second;
    CHECK(matter::physics::physics_stats(first.world()).live_bodies == 0,
          "first runtime has empty physics world");
    CHECK(matter::physics::physics_stats(second.world()).live_bodies == 0,
          "second runtime has independent empty physics world");
    CHECK(matter::physics::detail::context_world_is_valid(first.world()),
          "first Box3D world valid");
    CHECK(matter::physics::detail::context_world_is_valid(second.world()),
          "second Box3D world valid");
}
```

- [ ] **Step 2: Run RED with the real Box3D link**

Replace the contract-only target with `physics-tests`/`run-physics`. Link
`../src/ecs/physics_context.cpp`, all required ECS sources, and exactly one
`$(BOX3D_DIR)/libbox3d.a`.

Because `Runtime` will own `PhysicsContext` after this task, also add
`physics_context.cpp` and exactly one platform-appropriate Box3D archive to every
engine/test/Viewer/Explorer target that already contains `ecs_runtime.cpp`. Create
the initial static checker to enforce that closure and shared Box3D include paths.

Run: `make -C MatterEngine3/tests run-physics`

Expected: compile/link failure because `PhysicsContext` and lookup do not exist.

- [ ] **Step 3: Implement context ownership and Runtime ordering**

In `physics_context.h`, declare a noncopyable context and a private singleton ref:

```cpp
namespace matter::physics::detail {
class PhysicsContext;
struct PhysicsContextRef { PhysicsContext* value = nullptr; };
PhysicsContext& context(flecs::world&);
const PhysicsContext& context(const flecs::world&);
bool context_world_is_valid(const flecs::world&);
}
```

`PhysicsContext` creates a `b3WorldId` from `b3DefaultWorldDef`, sets one worker,
applies default gravity, and throws `std::runtime_error` if the returned ID is not
valid. Its destructor destroys the Box3D world exactly once.

Change `Runtime` to declare `world_` before
`std::unique_ptr<physics::detail::PhysicsContext> physics_`, add an out-of-line
destructor, import `PhysicsModule`, construct the context, and set `PhysicsContextRef`.
The declaration order guarantees context destruction before Flecs world destruction.

- [ ] **Step 4: Implement stats/event accessors and run GREEN twice**

`physics_stats` and `physics_events` look up the private singleton and return copied
stats/const event buffers. A world without the module fails closed with zero/empty
static results; it never dereferences null.

Run twice: `make -C MatterEngine3/tests run-physics`

Expected twice: `ALL PASS` and no Box3D leak/assert output.

Run: `& .\.superpowers\sdd\box3d-phase2-static-check.ps1`

Expected: `PASS: Box3D Phase 2 build contract`.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/ecs/physics_context.* MatterEngine3/src/ecs/ecs_runtime.* \
  MatterEngine3/tests/physics_tests.cpp MatterEngine3/tests/Makefile \
  MatterEngine3/Makefile MatterViewer/Makefile ExplorerDemo/Makefile \
  .superpowers/sdd/box3d-phase2-static-check.ps1
git commit -m "feat(physics): own one Box3D world per ECS runtime"
```

---

### Task 3: Body Validation, Shapes, and Stable Bridge Lifecycle

**Files:**
- Create: `MatterEngine3/src/ecs/physics_shapes.h`
- Create: `MatterEngine3/src/ecs/physics_shapes.cpp`
- Create: `MatterEngine3/src/ecs/physics_systems.cpp`
- Modify: `MatterEngine3/src/ecs/physics_context.h/.cpp`
- Modify: `MatterEngine3/tests/physics_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Modify: `ExplorerDemo/Makefile`
- Modify: `.superpowers/sdd/box3d-phase2-static-check.ps1`

**Interfaces:**
- Consumes: public body/collider components and private context.
- Produces: deterministic reconcile system, valid Box3D bodies/shapes, `PhysicsError`, and stable bridges.

- [ ] **Step 1: Write the failing validation/lifecycle matrix**

Add table-driven tests for missing transform, parent, nonunit/nonfinite scale,
missing/multiple collider, invalid body numbers, invalid primitive dimensions,
invalid material, too-small/coplanar/over-32 hulls, and Box3D hull failure. Assert the
exact `PhysicsErrorCode`, zero live bodies, then correct each entity and assert the
error is removed and one body exists.

Add successful static/dynamic creation tests for sphere, capsule, oriented box, and
32-point hull. Delete/remove/invalidate each and assert body/shape validity becomes
false at the next fixed reconciliation boundary.

Add an archetype-move test: create a body, add/remove unrelated components, step,
and prove Box3D user data still resolves the full original entity ID.

- [ ] **Step 2: Run RED**

Run: `make -C MatterEngine3/tests run-physics`

Expected: validation, body-count, and bridge checks fail because reconciliation does
not exist.

- [ ] **Step 3: Implement pure desired-shape validation**

In `physics_shapes.h`, define `DesiredBody` with a tagged shape union/value data and:

```cpp
ValidationResult validate_desired_body(flecs::entity entity);
b3ShapeId create_shape(b3BodyId body, const DesiredBody& desired,
                       b3HullData*& temporary_hull);
```

Validation counts the four collider components, checks the root/unit-scale contract,
normalizes quaternions, validates finite values/material/filter data, and calls
`b3CreateHull(points, count, 32)` only after pure checks pass. Use RAII to call
`b3DestroyHull` on every success/failure path.

- [ ] **Step 4: Implement stable bridges and deterministic reconciliation**

Store bridges as `std::unordered_map<flecs::entity_t,
std::unique_ptr<BridgeRecord>>`; the pointed-to record remains stable across map
rehashes. It contains full entity ID, `b3BodyId`, `b3ShapeId`, and live state.

At `PhysicsReconcile`, collect candidates and existing bridge IDs, sort/unique full
IDs, then reconcile in ascending order. On valid creation, set body/shape user data
to the bridge pointer. On removal/failure, clear user data, destroy the body, retire
the bridge, update stats, and attach the exact error. Never retain a Flecs component
pointer across a structural mutation.

Any body/collider/transform/`ChildOf` change marks the entity for reconcile. A cheap
hash of the full desired configuration prevents rebuilding unchanged bodies. When
replacing a valid dynamic body, snapshot pose, velocities, and awake state; publish
the replacement before destroying the old body; then restore state.

Add `physics_shapes.cpp` and `physics_systems.cpp` to every runtime-bearing source
union at the same time they are created. Strengthen the static checker to require all
three physics implementation files, preventing an intermediate broken archive or
product link graph.

- [ ] **Step 5: Run GREEN plus sanitizer/static lifetime checks**

Run twice: `make -C MatterEngine3/tests run-physics`

Run: `git diff --check`

Expected: all validation/lifecycle tests `ALL PASS`; no pointer stored in user data
originates from Flecs component/query memory.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/src/ecs/physics_shapes.* MatterEngine3/src/ecs/physics_systems.cpp \
  MatterEngine3/src/ecs/physics_context.* MatterEngine3/tests/physics_tests.cpp \
  MatterEngine3/tests/Makefile MatterEngine3/Makefile MatterViewer/Makefile \
  ExplorerDemo/Makefile .superpowers/sdd/box3d-phase2-static-check.ps1
git commit -m "feat(physics): reconcile ECS bodies and convex colliders"
```

---

### Task 4: Fixed-Step Simulation and Transform Synchronization

**Files:**
- Modify: `MatterEngine3/src/ecs/physics_context.h/.cpp`
- Modify: `MatterEngine3/src/ecs/physics_systems.cpp`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/tests/physics_tests.cpp`

**Interfaces:**
- Consumes: reconciled bodies, fixed pipeline delta, transform system.
- Produces: one step per fixed tick, settings updates, static/kinematic push, dynamic pull.

- [ ] **Step 1: Write failing simulation/order tests**

Add tests for:

- dynamic sphere falling onto a static floor;
- static edits reaching Box3D before step;
- kinematic target reaching Box3D during the fixed step;
- dynamic translation/rotation/velocity reaching ECS after step;
- dynamic descendant world transform current after `FixedPostUpdate`;
- settings gravity/substeps update before the next step;
- one step per fixed step, including a three-step catch-up tick;
- no step for valid zero-fixed-step or every invalid `TickDesc` case;
- explicit phase trace exactly matches the design order.

- [ ] **Step 2: Run RED**

Run: `make -C MatterEngine3/tests run-physics run-ecs`

Expected: simulation stays still, stats steps remain zero, and detailed phase order
is absent.

- [ ] **Step 3: Implement ordered physics systems**

Register three fixed-pipeline systems:

```cpp
world.system("MatterPhysicsReconcile").kind<PhysicsReconcile>()...;
world.system("MatterPhysicsPush").kind<PhysicsPush>()...;
world.system("MatterPhysicsStep").kind<matter::ecs::Physics>()...;
world.system("MatterPhysicsPull").kind<PhysicsPull>()...;
```

Reconcile validates/builds. Push applies settings, static transforms, kinematic
targets, and commands. Step calls `b3World_Step(context.world_id(), delta,
clamped_substeps)` exactly once. Pull consumes movement events, writes dynamic
`LocalTransform` translation/rotation with unit scale, writes `PhysicsVelocity`, and
adds `TransformDirty`. Box3D IDs stay private.

Make all engine systems carry `FixedPipelineSystem`. Do not add them to
`FramePipelineSystem`.

- [ ] **Step 4: Run focused and Phase 1 suites GREEN twice**

Run twice: `make -C MatterEngine3/tests run-physics run-ecs`

Expected: both suites print `ALL PASS`; physics step counters match fixed-step
counters exactly.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/ecs/physics_context.* MatterEngine3/src/ecs/physics_systems.cpp \
  MatterEngine3/src/ecs/ecs_runtime.cpp MatterEngine3/tests/physics_tests.cpp
git commit -m "feat(physics): step Box3D in the fixed ECS pipeline"
```

---

### Task 5: Dynamic Body Command Queue

**Files:**
- Modify: `MatterEngine3/src/ecs/physics_context.h/.cpp`
- Modify: `MatterEngine3/tests/physics_tests.cpp`

**Interfaces:**
- Consumes: public command functions and live bridge lookup.
- Produces: deterministic teleport, velocity, force, impulse, and wake behavior.

- [ ] **Step 1: Write failing command tests**

Verify teleport and velocity are last-write-wins per entity, wake is idempotent,
forces/impulses accumulate in enqueue order, and all commands execute before the next
step. Assert calls reject dead, stale-generation, cross-world, static, kinematic,
invalid, and context-less entities without draining commands on invalid ticks.

- [ ] **Step 2: Run RED**

Run: `make -C MatterEngine3/tests run-physics`

Expected: public functions return false or do not affect simulation.

- [ ] **Step 3: Implement entity-safe queued commands**

Commands store only the originating real-world identity, full entity ID, command
kind, and copied numeric payload. Normalize stages with `ecs_get_world` for identity
checks but retain the caller's valid entity ID. Teleport/velocity maps overwrite per
entity; force/impulse vectors preserve enqueue order; wake uses a set.

At `PhysicsPush`, swap queues, sort per-entity last-write-wins commands by full ID,
validate the bridge/entity/body type again, apply them, and increment
`failed_commands` for rejected work. Invalid runtime ticks do not enter the fixed
pipeline and therefore retain queued commands.

- [ ] **Step 4: Run GREEN twice**

Run twice: `make -C MatterEngine3/tests run-physics`

Expected: `ALL PASS`; no command crosses worlds or stale generations.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/ecs/physics_context.* MatterEngine3/tests/physics_tests.cpp
git commit -m "feat(physics): queue deterministic rigid-body commands"
```

---

### Task 6: Engine-Native Movement, Contact, and Sensor Events

**Files:**
- Modify: `MatterEngine3/src/ecs/physics_context.h/.cpp`
- Modify: `MatterEngine3/src/ecs/physics_systems.cpp`
- Modify: `MatterEngine3/tests/physics_tests.cpp`

**Interfaces:**
- Consumes: Box3D body/contact/sensor event arrays and stable bridge records.
- Produces: sorted `PhysicsEvents` visible during `PostPhysics` and after the tick.

- [ ] **Step 1: Write failing event tests**

Build controlled fixtures that produce body movement/sleep state, contact begin/end,
contact hit, and sensor begin/end. Assert both mapped full entity IDs, normalized
pair order, hit position/normal/speed, sorted buffers, and buffer replacement on the
next completed fixed step.

Destroy one participant before conversion through a test seam and assert the stale
event is dropped and `stale_events` increments. Add a `PostPhysics` system that reads
events and defers entity/component creation safely.

- [ ] **Step 2: Run RED**

Run: `make -C MatterEngine3/tests run-physics`

Expected: event buffers remain empty and the PostPhysics observer sees nothing.

- [ ] **Step 3: Convert and sort events after each step**

Immediately after `b3World_Step`, copy Box3D event arrays while valid. Resolve each
shape/body user-data bridge, confirm live full entity IDs with `ecs_is_alive`, reject
cross-world/tombstone entries, normalize pairs, then sort by kind and entity IDs.

Publish the completed buffer atomically before `PhysicsPull`/`PostPhysics`. Never
retain pointers into Box3D event arrays. Do not emit structural changes from the C
callbacks or while Box3D is stepping.

- [ ] **Step 4: Run GREEN twice**

Run twice: `make -C MatterEngine3/tests run-physics`

Expected: all event and deferral cases print `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/ecs/physics_context.* MatterEngine3/src/ecs/physics_systems.cpp \
  MatterEngine3/tests/physics_tests.cpp
git commit -m "feat(physics): publish ECS-native physics events"
```

---

### Task 7: Ray Cast and Sphere Overlap Queries

**Files:**
- Modify: `MatterEngine3/src/ecs/physics_context.h/.cpp`
- Modify: `MatterEngine3/tests/physics_tests.cpp`

**Interfaces:**
- Consumes: public query functions, Box3D callbacks, live bridge map.
- Produces: closest ray hit and sorted/deduplicated overlap entity IDs.

- [ ] **Step 1: Write failing query tests**

Test closest ray hit among two bodies, miss, category filtering, nonfinite/zero
translation rejection, stale bridge rejection, and cross-world safety. Test sphere
overlap filtering, no-hit, deduplication, and ascending full-ID order.

- [ ] **Step 2: Run RED**

Run: `make -C MatterEngine3/tests run-physics`

Expected: ray function returns false and overlap returns empty for valid fixtures.

- [ ] **Step 3: Implement synchronous guarded queries**

Use a context `stepping_` guard. Reject queries during step, invalid numeric input,
or worlds without a live context. Box3D callbacks resolve stable bridge user data,
validate entity liveness, and copy engine-native results immediately.

Ray cast keeps the minimum fraction and returns the associated position/normal.
Sphere overlap collects full IDs, then `sort`/`unique`. Never expose callback pointers
or Box3D handles.

- [ ] **Step 4: Run GREEN twice**

Run twice: `make -C MatterEngine3/tests run-physics`

Expected: all query cases print `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/ecs/physics_context.* MatterEngine3/tests/physics_tests.cpp
git commit -m "feat(physics): add ECS-safe physics queries"
```

---

### Task 8: WorldSession, Build Closure, and Final Regression Gates

**Files:**
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEngine3/tests/Makefile`
- Modify: `MatterViewer/Makefile`
- Modify: `ExplorerDemo/Makefile`
- Modify: `build-all.sh`
- Modify: `MatterEngine3/tests/world_stream_tests.cpp`
- Modify: `.superpowers/sdd/progress.md`
- Modify: `.superpowers/sdd/box3d-phase2-static-check.ps1`

**Interfaces:**
- Consumes: all Phase 2 sources and public APIs.
- Produces: closed archive/product/test build graph and WorldSession lifecycle proof.

- [ ] **Step 1: Write failing WorldSession and static build-contract tests**

Extend `world_stream_tests.cpp` with a runtime body that survives `reload()` and
`regenerate()`, preserves its physics stats/context, and disappears with session
replacement. Do not require rendering or GPU initialization beyond the existing
fixture.

Strengthen the existing PowerShell static checker so its final form parses
engine/test/Viewer/Explorer Makefiles and asserts:

- all three physics C++ implementation files are in every runtime-bearing target;
- exactly one Box3D archive is linked by every final executable/test that contains
  `ecs_runtime.cpp`;
- shared include paths publish `Libraries/box3d/include`;
- `run-physics` is independently invocable and delegated exactly once from the
  standard MatterEngine3 `test` target before legacy suites;
- `build-all.sh` includes `run-physics`;
- no basename collisions exist in flattened Windows object rules.

- [ ] **Step 2: Run static/WorldSession RED**

Run:

```powershell
& .\.superpowers\sdd\box3d-phase2-static-check.ps1
```

Run: `make -C MatterEngine3/tests run-worldstream`

Expected: static checker reports missing physics source/link closure; WorldSession
physics persistence assertions fail or do not compile.

- [ ] **Step 3: Close engine, tests, and temporary product build graphs**

Add `physics_context.cpp`, `physics_shapes.cpp`, and `physics_systems.cpp` to the
MatterEngine3 archive. Ensure every test/application source union that contains
`ecs_runtime.cpp` also contains those implementations and links one matching Box3D
archive. Preserve C17 Box3D vs C++17 bridge compilation. Add `run-physics` to normal
tests/build-all without removing existing gates.

ExplorerDemo remains buildable only as a temporary migration consumer; do not add
features to it or retire it in Phase 2.

- [ ] **Step 4: Run all available final gates**

Required commands:

```bash
make -C MatterEngine3/tests run-physics run-ecs run-tilesetphysics run-worldstream
make -C MatterEngine3 test
./build-all.sh
make -C MatterViewer windows -j2
make -C ExplorerDemo windows -j2
```

Also run the static checker, `git diff --check`, forbidden-scope symbol scan, and two
fresh C17/C++17 focused physics compile-link-runs. If GNU/MinGW/GPU gates are still
unavailable on this host, record the exact failure and do not mark them passed.

- [ ] **Step 5: Final scope/status documentation**

Update the top SDD ledger with per-task commits/reviews and verification caveats.
Update the Phase 2 design status to implemented only if every mandatory gate passes;
otherwise say implementation complete with mandatory external verification pending.
Record the one non-blocking Phase 1 reflection-metadata assertion gap separately; do
not misstate it as Phase 2 work.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/Makefile MatterEngine3/tests/Makefile MatterViewer/Makefile \
  ExplorerDemo/Makefile build-all.sh MatterEngine3/tests/world_stream_tests.cpp \
  .superpowers/sdd/box3d-phase2-static-check.ps1 .superpowers/sdd/progress.md \
  docs/superpowers/specs/2026-07-18-box3d-runtime-physics-design.md
git commit -m "build(physics): gate Box3D runtime integration"
```

---

## Final Review

After Task 8, generate a whole-branch diff from the plan/spec commit to `HEAD` and
dispatch an independent senior reviewer. The reviewer must check public API leakage,
context destruction order, bridge pointer stability, body replacement failure paths,
deferred Flecs mutation, phase ordering, one-step accounting, event lifetime,
query callback safety, build closure, and Phase 2 scope. Fix every Critical and
Important finding in one test-first fix round and re-review before handoff.
