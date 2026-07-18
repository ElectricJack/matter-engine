#include "check.h"
#include "matter/ecs.h"

#include <cstring>

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

static void test_core_component_reflection() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::component<ecs::LocalTransform> local_transform =
        world.component<ecs::LocalTransform>();
    const flecs::Type* local_transform_meta =
        local_transform.try_get<flecs::Type>();
    CHECK(local_transform_meta != nullptr,
          "local transform has reflection type metadata");
    CHECK(local_transform_meta != nullptr &&
              local_transform_meta->kind == flecs::meta::StructType,
          "local transform metadata describes a struct");
    const ecs_member_t* translation_member =
        ecs_struct_get_member(world, local_transform, "translation");
    const ecs_member_t* rotation_member =
        ecs_struct_get_member(world, local_transform, "rotation");
    const ecs_member_t* scale_member =
        ecs_struct_get_member(world, local_transform, "scale");
    CHECK(translation_member != nullptr,
          "local transform reflects translation");
    CHECK(rotation_member != nullptr,
          "local transform reflects rotation");
    CHECK(scale_member != nullptr,
          "local transform reflects scale");

    ecs::LocalTransform value{};
    if (translation_member != nullptr && rotation_member != nullptr &&
        scale_member != nullptr) {
        flecs::cursor cursor(world, local_transform, &value);
        CHECK(cursor.push() == 0, "reflection cursor enters local transform");
        CHECK(cursor.member("translation") == 0,
              "reflection cursor selects translation");
        CHECK(cursor.push() == 0, "reflection cursor enters translation");
        CHECK(cursor.member("x") == 0,
              "reflection cursor selects translation.x");
        CHECK(cursor.set_float(12.0) == 0,
              "reflection cursor writes translation.x");
        CHECK(cursor.pop() == 0, "reflection cursor leaves translation");
        CHECK(cursor.pop() == 0, "reflection cursor leaves local transform");
    }
    CHECK(value.translation.x == 12.0f,
          "reflection cursor changes the typed local transform value");

    const flecs::string json = world.to_json(&value);
    CHECK(std::strstr(json.c_str(), "\"translation\"") != nullptr,
          "local transform JSON contains named translation field");
    CHECK(std::strstr(json.c_str(), "\"rotation\"") != nullptr,
          "local transform JSON contains named rotation field");
    CHECK(std::strstr(json.c_str(), "\"scale\"") != nullptr,
          "local transform JSON contains named scale field");

    const flecs::component<ecs::WorldRuntimeState> runtime_state =
        world.component<ecs::WorldRuntimeState>();
    CHECK(ecs_struct_get_member(world, runtime_state, "status") != nullptr,
          "world runtime state reflects status");
    CHECK(ecs_struct_get_member(world, runtime_state, "content_generation") != nullptr,
          "world runtime state reflects content generation");
}

int main() {
    test_entity_lifecycle_and_components();
    test_deferred_structural_mutation();
    test_core_component_reflection();
    return check_summary();
}
