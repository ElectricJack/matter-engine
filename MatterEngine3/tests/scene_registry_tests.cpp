// scene_registry_tests.cpp — Phase 4 Task 4: reflected ECS scene registry.

#include "check.h"
#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/scene.h"
#include "matter/streaming.h"
#include "matter/world_definition.h"
#include "ecs/scene_registry.h"

#include "flecs.h"

#include <string>
#include <vector>

using namespace matter;
using namespace matter::scene;

// ---------------------------------------------------------------------------
// Reflection registration tests.
// ---------------------------------------------------------------------------

static void test_scene_module_registers_scene_entity_id() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<SceneModule>();

    auto comp = world.component<SceneEntityId>();
    CHECK(comp.id() != 0, "SceneEntityId not registered");
}

static void test_scene_module_registers_part_instance() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<SceneModule>();

    auto comp = world.component<PartInstance>();
    CHECK(comp.id() != 0, "PartInstance not registered");
}

static void test_scene_module_registers_part_instance_error() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<SceneModule>();

    auto comp = world.component<PartInstanceError>();
    CHECK(comp.id() != 0, "PartInstanceError not registered");
}

static void test_core_module_reflects_transform() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    auto comp = world.component<ecs::LocalTransform>();
    CHECK(comp.id() != 0, "LocalTransform not registered");
}

static void test_physics_module_reflects_rigid_body() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::RigidBody>();
    CHECK(comp.id() != 0, "RigidBody not registered");
}

static void test_physics_module_reflects_velocity() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::PhysicsVelocity>();
    CHECK(comp.id() != 0, "PhysicsVelocity not registered");
}

static void test_physics_module_reflects_sphere_collider() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::SphereCollider>();
    CHECK(comp.id() != 0, "SphereCollider not registered");
}

static void test_physics_module_reflects_capsule_collider() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::CapsuleCollider>();
    CHECK(comp.id() != 0, "CapsuleCollider not registered");
}

static void test_physics_module_reflects_box_collider() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::BoxCollider>();
    CHECK(comp.id() != 0, "BoxCollider not registered");
}

static void test_physics_module_reflects_convex_hull_collider() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::ConvexHullCollider>();
    CHECK(comp.id() != 0, "ConvexHullCollider not registered");
}

static void test_streaming_module_reflects_sector_streaming() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<streaming::StreamingModule>();

    auto comp = world.component<streaming::SectorStreaming>();
    CHECK(comp.id() != 0, "SectorStreaming not registered");
}

// ---------------------------------------------------------------------------
// Component descriptor lookup tests.
// ---------------------------------------------------------------------------

static void test_find_component_known() {
    CHECK(find_component("LocalTransform") != nullptr, "LocalTransform not found");
    CHECK(find_component("RigidBody") != nullptr, "RigidBody not found");
    CHECK(find_component("PhysicsVelocity") != nullptr, "PhysicsVelocity not found");
    CHECK(find_component("SphereCollider") != nullptr, "SphereCollider not found");
    CHECK(find_component("CapsuleCollider") != nullptr, "CapsuleCollider not found");
    CHECK(find_component("BoxCollider") != nullptr, "BoxCollider not found");
    CHECK(find_component("ConvexHullCollider") != nullptr, "ConvexHullCollider not found");
    CHECK(find_component("PartInstance") != nullptr, "PartInstance not found");
    CHECK(find_component("SectorStreaming") != nullptr, "SectorStreaming not found");
}

static void test_find_component_unknown() {
    CHECK(find_component("Nonexistent") == nullptr, "Nonexistent should be null");
    CHECK(find_component("MeshRenderer") == nullptr, "MeshRenderer should be null");
}

static void test_component_count() {
    CHECK(component_count() == 9, "expected 9 registered components");
}

// ---------------------------------------------------------------------------
// Single-recipe validation tests.
// ---------------------------------------------------------------------------

static void test_validate_empty_id_rejected() {
    RawEntityRecipe raw;
    raw.authored_id = "";
    raw.components_json = "{}";

    EntityRecipe out;
    RecipeError err;
    CHECK(!validate(raw, out, err), "empty id should be rejected");
    CHECK(err.message.find("empty") != std::string::npos, "error should mention empty");
}

static void test_validate_unknown_component_rejected() {
    RawEntityRecipe raw;
    raw.authored_id = "entity_1";
    raw.components_json = R"({"UnknownWidget": {}})";

    EntityRecipe out;
    RecipeError err;
    CHECK(!validate(raw, out, err), "unknown component should be rejected");
    CHECK(err.message.find("unknown component") != std::string::npos, "error should mention unknown");
    CHECK(err.field_path == "UnknownWidget", "field_path should be the unknown key");
}

static void test_validate_multiple_colliders_rejected() {
    RawEntityRecipe raw;
    raw.authored_id = "entity_1";
    raw.components_json = R"({"SphereCollider": {}, "BoxCollider": {}})";

    EntityRecipe out;
    RecipeError err;
    CHECK(!validate(raw, out, err), "multiple colliders should be rejected");
    CHECK(err.message.find("multiple colliders") != std::string::npos, "error should mention multiple colliders");
}

static void test_validate_valid_recipe() {
    RawEntityRecipe raw;
    raw.authored_id = "ball_1";
    raw.display_name = "Ball";
    raw.components_json = R"({"RigidBody": {}, "SphereCollider": {}})";

    EntityRecipe out;
    RecipeError err;
    CHECK(validate(raw, out, err), "valid recipe should pass");
    CHECK(out.authored_id == "ball_1", "authored_id preserved");
}

static void test_validate_empty_components() {
    RawEntityRecipe raw;
    raw.authored_id = "empty_entity";
    raw.components_json = "{}";

    EntityRecipe out;
    RecipeError err;
    CHECK(validate(raw, out, err), "empty components should pass");
}

// ---------------------------------------------------------------------------
// Batch validation tests.
// ---------------------------------------------------------------------------

static void test_batch_duplicate_ids_rejected() {
    std::vector<RawEntityRecipe> recipes = {
        {"dup_id", "First", "", "{}"},
        {"dup_id", "Second", "", "{}"},
    };

    std::vector<EntityRecipe> out;
    RecipeError err;
    CHECK(!validate_batch(recipes, out, err), "duplicate ids should be rejected");
    CHECK(err.message.find("duplicate") != std::string::npos, "error should mention duplicate");
}

static void test_batch_missing_parent_rejected() {
    std::vector<RawEntityRecipe> recipes = {
        {"child_1", "Child", "nonexistent_parent", "{}"},
    };

    std::vector<EntityRecipe> out;
    RecipeError err;
    CHECK(!validate_batch(recipes, out, err), "missing parent should be rejected");
    CHECK(err.message.find("missing parent") != std::string::npos, "error should mention missing parent");
}

static void test_batch_cycle_rejected() {
    std::vector<RawEntityRecipe> recipes = {
        {"a", "A", "b", "{}"},
        {"b", "B", "c", "{}"},
        {"c", "C", "a", "{}"},
    };

    std::vector<EntityRecipe> out;
    RecipeError err;
    CHECK(!validate_batch(recipes, out, err), "cycle should be rejected");
    CHECK(err.message.find("cycle") != std::string::npos, "error should mention cycle");
}

static void test_batch_valid_hierarchy() {
    std::vector<RawEntityRecipe> recipes = {
        {"root", "Root", "", "{}"},
        {"child", "Child", "root", "{}"},
        {"grandchild", "GrandChild", "child", "{}"},
    };

    std::vector<EntityRecipe> out;
    RecipeError err;
    CHECK(validate_batch(recipes, out, err), "valid hierarchy should pass");
    CHECK(out.size() == 3, "should have 3 validated recipes");
}

// ---------------------------------------------------------------------------
// Instantiation tests.
// ---------------------------------------------------------------------------

static void test_instantiate_creates_entities() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<streaming::StreamingModule>();
    world.import<SceneModule>();

    std::vector<EntityRecipe> recipes = {
        {"hero", "Hero", "", R"({"RigidBody": {}, "SphereCollider": {}})"},
        {"ground", "Ground", "", R"({"BoxCollider": {}})"},
    };

    SceneGeneration gen;
    RecipeError err;
    CHECK(instantiate(world, recipes.data(), (uint32_t)recipes.size(), gen, err),
          "instantiate should succeed");
    CHECK(gen.value == 1, "generation should increment");

    int scene_entities = 0;
    world.each([&](flecs::entity, const SceneEntityId&) { ++scene_entities; });
    CHECK(scene_entities == 2, "should create 2 entities");
}

static void test_instantiate_wires_parent() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<streaming::StreamingModule>();
    world.import<SceneModule>();

    std::vector<EntityRecipe> recipes = {
        {"parent_e", "Parent", "", "{}"},
        {"child_e", "Child", "parent_e", "{}"},
    };

    SceneGeneration gen;
    RecipeError err;
    CHECK(instantiate(world, recipes.data(), (uint32_t)recipes.size(), gen, err),
          "instantiate with hierarchy should succeed");

    flecs::entity child;
    world.each([&](flecs::entity e, const SceneEntityId&) {
        if (e.parent().is_valid() && e.parent().has<SceneEntityId>())
            child = e;
    });
    CHECK(child.is_valid(), "child entity should exist");
    CHECK(child.parent().is_valid(), "child should have parent");
    CHECK(child.parent().has<SceneEntityId>(), "parent should have SceneEntityId");
}

static void test_instantiate_adds_components() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<streaming::StreamingModule>();
    world.import<SceneModule>();

    std::vector<EntityRecipe> recipes = {
        {"phys_ball", "Ball", "", R"({"RigidBody": {}, "SphereCollider": {}, "PhysicsVelocity": {}})"},
    };

    SceneGeneration gen;
    RecipeError err;
    CHECK(instantiate(world, recipes.data(), (uint32_t)recipes.size(), gen, err),
          "instantiate with components should succeed");

    flecs::entity ball;
    world.each([&](flecs::entity e, const SceneEntityId&) { ball = e; });
    CHECK(ball.is_valid(), "ball entity should exist");
    CHECK(ball.has<physics::RigidBody>(), "should have RigidBody");
    CHECK(ball.has<physics::SphereCollider>(), "should have SphereCollider");
    CHECK(ball.has<physics::PhysicsVelocity>(), "should have PhysicsVelocity");
    CHECK(ball.has<ecs::LocalTransform>(), "should have LocalTransform");
}

static void test_instantiate_empty_is_noop() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<streaming::StreamingModule>();
    world.import<SceneModule>();

    SceneGeneration gen;
    RecipeError err;
    CHECK(instantiate(world, nullptr, 0, gen, err), "empty instantiate should succeed");
    CHECK(gen.value == 0, "generation should not increment on empty");
}

// ---------------------------------------------------------------------------
// Identity tests.
// ---------------------------------------------------------------------------

static void test_authored_ids_produce_stable_hashes() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<streaming::StreamingModule>();
    world.import<SceneModule>();

    std::vector<EntityRecipe> batch = {{"stable_id", "Test", "", "{}"}};
    SceneGeneration gen;
    RecipeError err;
    CHECK(instantiate(world, batch.data(), 1, gen, err), "first instantiate");

    uint64_t hash1 = 0;
    world.each([&](flecs::entity, const SceneEntityId& id) { hash1 = id.value; });

    flecs::world world2;
    world2.import<ecs::CoreModule>();
    world2.import<physics::PhysicsModule>();
    world2.import<streaming::StreamingModule>();
    world2.import<SceneModule>();

    SceneGeneration gen2;
    RecipeError err2;
    CHECK(instantiate(world2, batch.data(), 1, gen2, err2), "second instantiate");

    uint64_t hash2 = 0;
    world2.each([&](flecs::entity, const SceneEntityId& id) { hash2 = id.value; });

    CHECK(hash1 == hash2, "same authored_id should produce same hash");
    CHECK(hash1 != 0, "hash should be non-zero");
}

// ---------------------------------------------------------------------------
// Main.
// ---------------------------------------------------------------------------

int main() {
    test_scene_module_registers_scene_entity_id();
    test_scene_module_registers_part_instance();
    test_scene_module_registers_part_instance_error();
    test_core_module_reflects_transform();
    test_physics_module_reflects_rigid_body();
    test_physics_module_reflects_velocity();
    test_physics_module_reflects_sphere_collider();
    test_physics_module_reflects_capsule_collider();
    test_physics_module_reflects_box_collider();
    test_physics_module_reflects_convex_hull_collider();
    test_streaming_module_reflects_sector_streaming();

    test_find_component_known();
    test_find_component_unknown();
    test_component_count();

    test_validate_empty_id_rejected();
    test_validate_unknown_component_rejected();
    test_validate_multiple_colliders_rejected();
    test_validate_valid_recipe();
    test_validate_empty_components();

    test_batch_duplicate_ids_rejected();
    test_batch_missing_parent_rejected();
    test_batch_cycle_rejected();
    test_batch_valid_hierarchy();

    test_instantiate_creates_entities();
    test_instantiate_wires_parent();
    test_instantiate_adds_components();
    test_instantiate_empty_is_noop();

    test_authored_ids_produce_stable_hashes();

    return check_summary();
}
