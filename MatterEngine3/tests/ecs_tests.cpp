#include "check.h"
#include "matter/ecs.h"

using namespace matter;

static void test_entity_lifecycle_and_components() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity entity = world.entity("RuntimeObject")
        .set<ecs::LocalTransform>({{1, 2, 3}, {}, {1, 1, 1}});
    CHECK(entity.is_alive(), "entity is alive after creation");
    CHECK(entity.has<ecs::LocalTransform>(), "entity has local transform");
    entity.add<ecs::TransformDirty>();
    CHECK(entity.has<ecs::TransformDirty>(), "dirty tag can be added");
    entity.remove<ecs::TransformDirty>();
    CHECK(!entity.has<ecs::TransformDirty>(), "dirty tag can be removed");

    const flecs::entity_t id = entity.id();
    entity.destruct();
    CHECK(!world.is_alive(id), "entity is dead after destruction");
}

static void test_deferred_structural_mutation() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.entity().set<ecs::LocalTransform>({});

    world.defer_begin();
    world.each<ecs::LocalTransform>([](flecs::entity entity, ecs::LocalTransform&) {
        entity.add<ecs::TransformDirty>();
    });
    world.defer_end();

    CHECK(world.count<ecs::TransformDirty>() == 1,
          "deferred structural mutation adds one dirty tag");
}

int main() {
    test_entity_lifecycle_and_components();
    test_deferred_structural_mutation();
    return check_summary();
}
