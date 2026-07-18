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
