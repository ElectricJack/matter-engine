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
