### Task 2: Add the Public ECS Contract and Headless Build Target

**Files:**

- Modify: `MatterEngine3/include/matter/math_types.h`
- Create: `MatterEngine3/include/matter/ecs.h`
- Create: `MatterEngine3/tests/ecs_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

- [ ] **Step 1: Write the failing public-contract test**

Create `MatterEngine3/tests/ecs_tests.cpp` with the existing `check.h` harness style and these first cases:

```cpp
#include "check.h"
#include "matter/ecs.h"

using namespace matter;

static void test_entity_lifecycle_and_components() {
    flecs::world world;
    world.import_<ecs::CoreModule>();

    const flecs::entity entity = world.entity("RuntimeObject")
        .set<ecs::LocalTransform>({{1, 2, 3}, {}, {1, 1, 1}});
    CHECK(entity.is_alive());
    CHECK(entity.has<ecs::LocalTransform>());
    entity.add<ecs::TransformDirty>();
    CHECK(entity.has<ecs::TransformDirty>());
    entity.remove<ecs::TransformDirty>();
    CHECK(!entity.has<ecs::TransformDirty>());

    const flecs::entity_t id = entity.id();
    entity.destruct();
    CHECK(!world.is_alive(id));
}

static void test_deferred_structural_mutation() {
    flecs::world world;
    world.import_<ecs::CoreModule>();
    world.entity().set<ecs::LocalTransform>({});

    world.defer_begin();
    world.each<ecs::LocalTransform>([](flecs::entity e, ecs::LocalTransform&) {
        e.add<ecs::TransformDirty>();
    });
    world.defer_end();

    CHECK(world.count<ecs::TransformDirty>() == 1);
}
```

Add a `main()` that calls both tests and follows the pass/fail reporting convention used by `sector_streamer_tests.cpp`.

- [ ] **Step 2: Add the test target before adding the header**

In `MatterEngine3/tests/Makefile` add:

```make
FLECS_DIR = ../../Libraries/flecs
FLECS_C = $(FLECS_DIR)/flecs.c

ECS_TARGET = ecs_tests
ECS_CPP = ecs_tests.cpp ../src/ecs/ecs_runtime.cpp ../src/ecs/transform_system.cpp
ECS_C = $(FLECS_C)
```

Add a dedicated `flecsc` C flavor compiled with `gcc -std=c99 -O2 -I$(FLECS_DIR)`. Add `ECS_CPP` to the `def` source union, `ECS_C` to the new C flavor union, and add:

```make
ECS_OBJS = $(call obj_list,def,$(ECS_CPP)) $(call obj_list,flecsc,$(ECS_C))

$(ECS_TARGET): $(ECS_OBJS)
	$(CC) $(ECS_OBJS) -o $@ $(FLAVOR_def_FLAGS) $(LDFLAGS)

run-ecs: $(ECS_TARGET)
	./$(ECS_TARGET)
```

Also add `run-ecs` to `.PHONY`, `flecsc_C_OBJS` to `ALL_OBJS`, and `$(ECS_TARGET)` to `clean`.

- [ ] **Step 3: Run the test to prove the API is missing**

Run:

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && make -C MatterEngine3/tests run-ecs'
```

Expected: compilation fails because `matter/ecs.h` does not exist.

- [ ] **Step 4: Add the public PODs and module declarations**

Append to `MatterEngine3/include/matter/math_types.h`:

```cpp
struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};
```

Create `MatterEngine3/include/matter/ecs.h`:

```cpp
#pragma once
#include <cstdint>
#include "flecs.h"
#include "matter/math_types.h"

namespace matter::ecs {

struct LocalTransform {
    Float3 translation{};
    Quaternion rotation{};
    Float3 scale{1.0f, 1.0f, 1.0f};
};

struct WorldTransform { Mat4f matrix{}; };
struct TransformDirty {};

enum class WorldStatus : uint8_t { Loading, Ready, Failed };
struct WorldRuntimeState {
    WorldStatus status = WorldStatus::Loading;
    uint64_t content_generation = 0;
};

struct FixedPreUpdate {};
struct FixedUpdate {};
struct PrePhysics {};
struct Physics {};
struct PostPhysics {};
struct FixedPostUpdate {};
struct FrameUpdate {};
struct FixedPipelineSystem {};
struct FramePipelineSystem {};

struct CoreModule {
    explicit CoreModule(flecs::world& world);
};

bool reparent(flecs::entity child, flecs::entity parent);
void clear_parent(flecs::entity child);

} // namespace matter::ecs
```

- [ ] **Step 5: Add the minimum module registration needed by the tests**

Create `MatterEngine3/src/ecs/ecs_runtime.cpp` and define `CoreModule` with `world.module<CoreModule>()`, component registrations, and a `WorldRuntimeState` singleton. Create empty `ecs_runtime.h` and `transform_system.cpp` translation units temporarily so the target links. Register components under the `matter.ecs` module scope, then restore the previous scope.

- [ ] **Step 6: Run the public-contract tests**

Run the `run-ecs` command from Step 3.

Expected: `ecs_tests: all checks passed`.

- [ ] **Step 7: Commit the public foundation**

```powershell
git add MatterEngine3/include/matter/math_types.h MatterEngine3/include/matter/ecs.h MatterEngine3/src/ecs MatterEngine3/tests/ecs_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat(ecs): add core MatterEngine ECS module"
```

---

