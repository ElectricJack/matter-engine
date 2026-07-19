// Phase 5 — verifies that authored entity recipes are instantiated as Flecs
// entities when a Ready world-state command carries them through the Runtime.
#include "check.h"
#include "../src/ecs/ecs_runtime.h"
#include "../src/ecs/scene_registry.h"
#include "matter/world_definition.h"
#include "matter/ecs.h"

namespace {

void test_ready_command_instantiates_entities() {
    matter::ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();

    // Enqueue a Ready command with entity recipes.
    std::vector<matter::RawEntityRecipe> recipes = {
        {"floor", "Floor", "", R"({"LocalTransform": {"translation": [0, 0, 0]}})"},
        {"crate-0", "Crate 0", "", R"({"LocalTransform": {"translation": [1, 5, 0]}})"},
        {"crate-1", "Crate 1", "", R"({"LocalTransform": {"translation": [3, 7, 0]}})"},
    };

    matter::ecs_runtime::WorldStateCommand cmd;
    cmd.kind = matter::ecs_runtime::WorldStateCommandKind::Ready;
    cmd.entities = recipes;
    runtime.enqueue_world_state(std::move(cmd));

    // Tick to drain commands.
    matter::TickDesc tick_desc;
    tick_desc.frame_delta_seconds = 1.0f / 60.0f;
    runtime.tick(tick_desc);

    // Verify entities were created.
    int entity_count = 0;
    world.each([&](flecs::entity, const matter::scene::SceneEntityId&) {
        ++entity_count;
    });
    CHECK(entity_count == 3, "three entities should be instantiated");

    // Verify WorldRuntimeState is Ready.
    const auto& state = world.get<matter::ecs::WorldRuntimeState>();
    CHECK(state.status == matter::ecs::WorldStatus::Ready, "world status should be Ready");
    CHECK(state.content_generation == 1, "content generation should be 1");
}

void test_ready_without_entities_does_not_bootstrap() {
    matter::ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();

    // Enqueue Ready with empty entities (normal pre-Phase5 behavior).
    matter::ecs_runtime::WorldStateCommand cmd;
    cmd.kind = matter::ecs_runtime::WorldStateCommandKind::Ready;
    runtime.enqueue_world_state(std::move(cmd));

    matter::TickDesc tick_desc;
    tick_desc.frame_delta_seconds = 1.0f / 60.0f;
    runtime.tick(tick_desc);

    int entity_count = 0;
    world.each([&](flecs::entity, const matter::scene::SceneEntityId&) {
        ++entity_count;
    });
    CHECK(entity_count == 0, "no entities when Ready carries no recipes");
}

void test_reload_replaces_entities() {
    matter::ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();

    // First load: 2 entities.
    {
        std::vector<matter::RawEntityRecipe> recipes = {
            {"a", "Entity A", "", "{}"},
            {"b", "Entity B", "", "{}"},
        };
        matter::ecs_runtime::WorldStateCommand cmd;
        cmd.kind = matter::ecs_runtime::WorldStateCommandKind::Ready;
        cmd.entities = recipes;
        runtime.enqueue_world_state(std::move(cmd));
    }
    matter::TickDesc tick_desc;
    tick_desc.frame_delta_seconds = 1.0f / 60.0f;
    runtime.tick(tick_desc);

    int count1 = 0;
    world.each([&](flecs::entity, const matter::scene::SceneEntityId&) { ++count1; });
    CHECK(count1 == 2, "first load creates 2 entities");

    // Second load (reload): 3 entities.
    {
        std::vector<matter::RawEntityRecipe> recipes = {
            {"x", "Entity X", "", "{}"},
            {"y", "Entity Y", "", "{}"},
            {"z", "Entity Z", "", "{}"},
        };
        matter::ecs_runtime::WorldStateCommand cmd;
        cmd.kind = matter::ecs_runtime::WorldStateCommandKind::Ready;
        cmd.entities = recipes;
        runtime.enqueue_world_state(std::move(cmd));
    }
    runtime.tick(tick_desc);

    int count2 = 0;
    world.each([&](flecs::entity, const matter::scene::SceneEntityId&) { ++count2; });
    CHECK(count2 == 3, "reload replaces old entities with new set");

    const auto& state = world.get<matter::ecs::WorldRuntimeState>();
    CHECK(state.content_generation == 2, "generation incremented twice");
}

void test_entity_with_part_instance_gets_zero_hash() {
    matter::ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();

    std::vector<matter::RawEntityRecipe> recipes = {
        {"obj", "Object", "", R"({"LocalTransform": {"translation": [0,0,0]}, "PartInstance": {"part": "Crate"}})"},
    };
    matter::ecs_runtime::WorldStateCommand cmd;
    cmd.kind = matter::ecs_runtime::WorldStateCommandKind::Ready;
    cmd.entities = recipes;
    runtime.enqueue_world_state(std::move(cmd));

    matter::TickDesc tick_desc;
    tick_desc.frame_delta_seconds = 1.0f / 60.0f;
    runtime.tick(tick_desc);

    int found = 0;
    world.each([&](flecs::entity, const matter::scene::SceneEntityId&,
                   const matter::scene::PartInstance& pi) {
        CHECK(pi.part_hash == 0, "permissive resolver yields hash=0");
        ++found;
    });
    CHECK(found == 1, "entity with PartInstance was created");
}

} // namespace

int main() {
    test_ready_command_instantiates_entities();
    test_ready_without_entities_does_not_bootstrap();
    test_reload_replaces_entities();
    test_entity_with_part_instance_gets_zero_hash();
    return check_summary();
}
