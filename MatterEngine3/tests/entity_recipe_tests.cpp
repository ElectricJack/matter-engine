// entity_recipe_tests.cpp — Phase 4 Task 5: recipe normalization and
// transactional scene bootstrap.

#include "check.h"
#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/scene.h"
#include "matter/streaming.h"
#include "matter/world_definition.h"
#include "ecs/scene_registry.h"

#include "flecs.h"

#include <string>
#include <unordered_map>
#include <vector>

using namespace matter;
using namespace matter::scene;

// ---------------------------------------------------------------------------
// Shared part-resolver fixtures.
// ---------------------------------------------------------------------------

static PartResolver make_resolver(const std::unordered_map<std::string, uint64_t>& table) {
    return [table](const std::string& module_name, uint64_t& out_hash) -> bool {
        auto it = table.find(module_name);
        if (it == table.end()) return false;
        out_hash = it->second;
        return true;
    };
}

static flecs::world make_world() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<streaming::StreamingModule>();
    world.import<SceneModule>();
    return world;
}

// ---------------------------------------------------------------------------
// Declarative vs. DSL-authored normalization parity.
// ---------------------------------------------------------------------------

static void test_declarative_and_dsl_normalize_identically() {
    // "Declarative" and "DSL" authoring both funnel into RawEntityRecipe;
    // normalization must not depend on how the raw recipe was produced —
    // only on its (authored_id, display_name, parent_authored_id,
    // components_json) contents.
    RawEntityRecipe declarative;
    declarative.authored_id = "crate_1";
    declarative.display_name = "Crate";
    declarative.components_json = R"({"RigidBody": {}, "BoxCollider": {}})";

    RawEntityRecipe dsl_authored;
    dsl_authored.authored_id = "crate_1";
    dsl_authored.display_name = "Crate";
    dsl_authored.components_json = R"({"RigidBody": {}, "BoxCollider": {}})";

    EntityRecipe out_a, out_b;
    RecipeError err_a, err_b;
    CHECK(validate(declarative, out_a, err_a), "declarative recipe should validate");
    CHECK(validate(dsl_authored, out_b, err_b), "dsl recipe should validate");

    CHECK(out_a.authored_id == out_b.authored_id, "authored_id should match across forms");
    CHECK(out_a.display_name == out_b.display_name, "display_name should match across forms");
    CHECK(out_a.components_json == out_b.components_json, "components_json should match across forms");
    CHECK(out_a.part_hash == out_b.part_hash, "part_hash should match across forms");
    CHECK(out_a.valid && out_b.valid, "both recipes should be marked valid");
}

// ---------------------------------------------------------------------------
// Stable ordering / identity.
// ---------------------------------------------------------------------------

static void test_stable_ordering_and_ids() {
    std::vector<RawEntityRecipe> recipes = {
        {"alpha", "Alpha", "", "{}"},
        {"beta", "Beta", "", "{}"},
        {"gamma", "Gamma", "", "{}"},
    };

    std::vector<EntityRecipe> out;
    RecipeError err;
    CHECK(validate_batch(recipes, out, err), "batch should validate");
    CHECK(out.size() == 3, "should have 3 validated recipes");
    CHECK(out[0].authored_id == "alpha", "order preserved: alpha first");
    CHECK(out[1].authored_id == "beta", "order preserved: beta second");
    CHECK(out[2].authored_id == "gamma", "order preserved: gamma third");

    // Re-running validate_batch on the same input should produce identical
    // ids/order — normalization is deterministic.
    std::vector<EntityRecipe> out2;
    RecipeError err2;
    CHECK(validate_batch(recipes, out2, err2), "second batch validation should succeed");
    for (size_t i = 0; i < out.size(); ++i) {
        CHECK(out[i].authored_id == out2[i].authored_id, "authored_id stable across runs");
    }
}

// ---------------------------------------------------------------------------
// Duplicate IDs across "forms" (declarative + DSL) rejected.
// ---------------------------------------------------------------------------

static void test_duplicate_ids_across_forms_rejected() {
    std::vector<RawEntityRecipe> recipes = {
        {"shared_id", "FromDeclarative", "", "{}"},
        {"shared_id", "FromDsl", "", R"({"RigidBody": {}})"},
    };

    std::vector<EntityRecipe> out;
    RecipeError err;
    CHECK(!validate_batch(recipes, out, err), "duplicate id across forms should be rejected");
    CHECK(err.message.find("duplicate") != std::string::npos, "error should mention duplicate");
    CHECK(err.authored_id == "shared_id", "error should identify the offending id");
}

// ---------------------------------------------------------------------------
// Bad parent rejected.
// ---------------------------------------------------------------------------

static void test_bad_parent_rejected() {
    std::vector<RawEntityRecipe> recipes = {
        {"orphan", "Orphan", "does_not_exist", "{}"},
    };

    std::vector<EntityRecipe> out;
    RecipeError err;
    CHECK(!validate_batch(recipes, out, err), "recipe with bad parent should be rejected");
    CHECK(err.message.find("missing parent") != std::string::npos, "error should mention missing parent");
}

// ---------------------------------------------------------------------------
// Missing part -> error.
// ---------------------------------------------------------------------------

static void test_missing_part_errors() {
    RawEntityRecipe raw;
    raw.authored_id = "prop_1";
    raw.components_json = R"({"PartInstance": {"part": "props/does_not_exist"}})";

    PartResolver resolver = make_resolver({{"props/crate", 12345ULL}});

    EntityRecipe out;
    RecipeError err;
    CHECK(!validate(raw, out, err, resolver), "unresolvable part should be rejected");
    CHECK(err.message.find("missing part") != std::string::npos, "error should mention missing part");
    CHECK(err.authored_id == "prop_1", "error should identify the offending recipe");
}

static void test_missing_part_errors_without_resolver() {
    // No resolver at all -> any authored "part" reference must fail closed
    // rather than silently resolving to hash 0.
    RawEntityRecipe raw;
    raw.authored_id = "prop_2";
    raw.components_json = R"({"PartInstance": {"part": "props/crate"}})";

    EntityRecipe out;
    RecipeError err;
    CHECK(!validate(raw, out, err), "part reference without resolver should be rejected");
}

// ---------------------------------------------------------------------------
// Part dependency recorded without placement (part_hash resolved, entity
// still normalizes as a dependency-only reference).
// ---------------------------------------------------------------------------

static void test_part_dependency_recorded_without_placement() {
    RawEntityRecipe raw;
    raw.authored_id = "crate_prop";
    raw.components_json = R"({"PartInstance": {"part": "props/crate", "visible": false}})";

    PartResolver resolver = make_resolver({{"props/crate", 0xABCDEF01ULL}});

    EntityRecipe out;
    RecipeError err;
    CHECK(validate(raw, out, err, resolver), "part reference with valid resolver should validate");
    CHECK(out.part_hash == 0xABCDEF01ULL, "part_hash should be resolved");
    CHECK(out.valid, "recipe should be marked valid");
}

static void test_part_instance_without_part_field_has_zero_hash() {
    // A PartInstance component with no authored "part" module reference is
    // legal (e.g. a placeholder/instance slot) and should not require a
    // resolver at all.
    RawEntityRecipe raw;
    raw.authored_id = "empty_slot";
    raw.components_json = R"({"PartInstance": {"visible": true}})";

    EntityRecipe out;
    RecipeError err;
    CHECK(validate(raw, out, err), "PartInstance without part field should validate without a resolver");
    CHECK(out.part_hash == 0, "part_hash should default to 0");
}

// ---------------------------------------------------------------------------
// Transactional bootstrap.
// ---------------------------------------------------------------------------

static void test_bootstrap_transactional_success() {
    flecs::world world = make_world();

    std::vector<RawEntityRecipe> recipes = {
        {"hero", "Hero", "", R"({"RigidBody": {}, "SphereCollider": {}})"},
        {"ground", "Ground", "", R"({"BoxCollider": {}})"},
    };

    SceneGeneration gen;
    RecipeError err;
    CHECK(bootstrap_transactional(world, recipes, gen, nullptr, err),
          "initial bootstrap should succeed");
    CHECK(gen.value == 1, "generation should be 1 after first bootstrap");

    int scene_entities = 0;
    world.each([&](flecs::entity, const SceneEntityId&) { ++scene_entities; });
    CHECK(scene_entities == 2, "should have 2 scene entities after bootstrap");
}

static void test_failed_reload_retains_prior_generation_and_entities() {
    flecs::world world = make_world();

    std::vector<RawEntityRecipe> good_recipes = {
        {"npc_1", "Npc1", "", "{}"},
        {"npc_2", "Npc2", "", "{}"},
    };

    SceneGeneration gen;
    RecipeError err;
    CHECK(bootstrap_transactional(world, good_recipes, gen, nullptr, err),
          "first bootstrap should succeed");
    CHECK(gen.value == 1, "generation should be 1 after first bootstrap");

    int entities_before = 0;
    std::vector<std::string> names_before;
    world.each([&](flecs::entity e, const SceneEntityId&) {
        ++entities_before;
        if (e.name().c_str()) names_before.push_back(e.name().c_str());
    });
    CHECK(entities_before == 2, "should have 2 entities before failed reload");

    // A batch with a duplicate id must fail validation and must not touch
    // the previously-instantiated scene.
    std::vector<RawEntityRecipe> bad_recipes = {
        {"dup", "Dup1", "", "{}"},
        {"dup", "Dup2", "", "{}"},
    };

    RecipeError err2;
    CHECK(!bootstrap_transactional(world, bad_recipes, gen, nullptr, err2),
          "reload with duplicate ids should fail");
    CHECK(gen.value == 1, "generation should remain unchanged after failed reload");

    int entities_after = 0;
    std::vector<std::string> names_after;
    world.each([&](flecs::entity e, const SceneEntityId&) {
        ++entities_after;
        if (e.name().c_str()) names_after.push_back(e.name().c_str());
    });
    CHECK(entities_after == 2, "prior entity count should be retained after failed reload");
    CHECK(names_after == names_before, "prior entity set should be unchanged after failed reload");
}

static void test_failed_reload_due_to_missing_part_retains_scene() {
    flecs::world world = make_world();

    std::vector<RawEntityRecipe> good_recipes = {
        {"anchor", "Anchor", "", "{}"},
    };

    SceneGeneration gen;
    RecipeError err;
    CHECK(bootstrap_transactional(world, good_recipes, gen, nullptr, err),
          "first bootstrap should succeed");
    CHECK(gen.value == 1, "generation should be 1 after first bootstrap");

    std::vector<RawEntityRecipe> bad_recipes = {
        {"prop", "Prop", "", R"({"PartInstance": {"part": "props/missing"}})"},
    };

    PartResolver resolver = make_resolver({{"props/other", 999ULL}});
    RecipeError err2;
    CHECK(!bootstrap_transactional(world, bad_recipes, gen, resolver, err2),
          "reload referencing a missing part should fail");
    CHECK(gen.value == 1, "generation should remain unchanged after missing-part failure");

    int scene_entities = 0;
    world.each([&](flecs::entity, const SceneEntityId&) { ++scene_entities; });
    CHECK(scene_entities == 1, "prior single entity should remain after failed reload");
}

static void test_bootstrap_transactional_replaces_prior_scene() {
    flecs::world world = make_world();

    std::vector<RawEntityRecipe> first_recipes = {
        {"old_1", "Old1", "", "{}"},
        {"old_2", "Old2", "", "{}"},
        {"old_3", "Old3", "", "{}"},
    };

    SceneGeneration gen;
    RecipeError err;
    CHECK(bootstrap_transactional(world, first_recipes, gen, nullptr, err),
          "first bootstrap should succeed");
    CHECK(gen.value == 1, "generation should be 1");

    std::vector<RawEntityRecipe> second_recipes = {
        {"new_1", "New1", "", "{}"},
    };

    CHECK(bootstrap_transactional(world, second_recipes, gen, nullptr, err),
          "second bootstrap should succeed");
    CHECK(gen.value == 2, "generation should bump to 2 on successful reload");

    int scene_entities = 0;
    bool found_old = false;
    world.each([&](flecs::entity e, const SceneEntityId&) {
        ++scene_entities;
        if (e.name().c_str() && std::string(e.name().c_str()).rfind("Old", 0) == 0)
            found_old = true;
    });
    CHECK(scene_entities == 1, "old scene entities should be destroyed on successful reload");
    CHECK(!found_old, "no entity from the prior generation should remain");
}

// ---------------------------------------------------------------------------
// normalize() packaging.
// ---------------------------------------------------------------------------

static void test_normalize_success_candidate() {
    std::vector<RawEntityRecipe> recipes = {
        {"a", "A", "", "{}"},
        {"b", "B", "a", "{}"},
    };

    SceneGeneration target{7};
    RecipeError err;
    SceneBootstrapCandidate candidate = normalize(recipes, target, nullptr, err);

    CHECK(candidate.success, "normalize should succeed for a valid batch");
    CHECK(candidate.recipes.size() == 2, "candidate should carry both recipes");
    CHECK(candidate.target_generation.value == 7, "candidate should carry the requested target generation");
}

static void test_normalize_failure_candidate_is_empty() {
    std::vector<RawEntityRecipe> recipes = {
        {"x", "X", "missing_parent", "{}"},
    };

    SceneGeneration target{3};
    RecipeError err;
    SceneBootstrapCandidate candidate = normalize(recipes, target, nullptr, err);

    CHECK(!candidate.success, "normalize should fail for a batch with a missing parent");
    CHECK(candidate.recipes.empty(), "failed candidate should carry no recipes");
}

// ---------------------------------------------------------------------------
// Main.
// ---------------------------------------------------------------------------

int main() {
    test_declarative_and_dsl_normalize_identically();

    test_stable_ordering_and_ids();

    test_duplicate_ids_across_forms_rejected();

    test_bad_parent_rejected();

    test_missing_part_errors();
    test_missing_part_errors_without_resolver();

    test_part_dependency_recorded_without_placement();
    test_part_instance_without_part_field_has_zero_hash();

    test_bootstrap_transactional_success();
    test_failed_reload_retains_prior_generation_and_entities();
    test_failed_reload_due_to_missing_part_retains_scene();
    test_bootstrap_transactional_replaces_prior_scene();

    test_normalize_success_candidate();
    test_normalize_failure_candidate_is_empty();

    return check_summary();
}
