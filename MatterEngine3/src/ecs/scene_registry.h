#pragma once

#include "matter/scene.h"
#include "matter/world_definition.h"
#include "flecs.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace matter::scene {

enum class ComponentKind : uint8_t {
    Transform,
    RigidBody,
    Velocity,
    SphereCollider,
    CapsuleCollider,
    BoxCollider,
    ConvexHullCollider,
    PartInstance,
    SectorStreaming
};

enum class FieldType : uint8_t {
    Float,
    Int,
    UInt,
    Bool,
    Enum,
    Float3,
    Quaternion
};

struct FieldDescriptor {
    const char* name = nullptr;
    FieldType type = FieldType::Float;
    float range_min = 0.0f;
    float range_max = 0.0f;
    bool has_range = false;
};

struct ComponentDescriptor {
    ComponentKind kind{};
    const char* name = nullptr;
    const FieldDescriptor* fields = nullptr;
    uint32_t field_count = 0;
    bool allow_multiple = false;
};

struct RecipeError {
    std::string message;
    std::string authored_id;
    std::string field_path;
};

struct SceneGeneration {
    uint64_t value = 0;
};

// Resolves an authored part module name (from a PartInstance component's
// "part" field) to its content-addressed part_hash. Returns false when the
// module cannot be resolved (missing part).
using PartResolver = std::function<bool(const std::string& module_name, uint64_t& out_hash)>;

// Result of normalizing a batch of raw recipes: either a fully validated set
// of EntityRecipes targeting a generation, or a failure (recipes/success
// reflect the failed attempt only — callers must not apply it).
struct SceneBootstrapCandidate {
    std::vector<EntityRecipe> recipes;
    SceneGeneration target_generation;
    bool success = false;
};

const ComponentDescriptor* find_component(const char* name);
uint32_t component_count();
const ComponentDescriptor* component_at(uint32_t index);

bool validate(const RawEntityRecipe& raw, EntityRecipe& out, RecipeError& err,
             const PartResolver& resolve_part = nullptr);

bool validate_batch(const std::vector<RawEntityRecipe>& recipes,
                    std::vector<EntityRecipe>& out,
                    RecipeError& err,
                    const PartResolver& resolve_part = nullptr);

// Validates raw_recipes and, on success, packages them as a bootstrap
// candidate targeting target_generation. Performs no world mutation.
SceneBootstrapCandidate normalize(const std::vector<RawEntityRecipe>& raw_recipes,
                                  SceneGeneration target_generation,
                                  const PartResolver& resolve_part,
                                  RecipeError& err);

bool instantiate(flecs::world& world,
                 const EntityRecipe* recipes, uint32_t count,
                 SceneGeneration& gen, RecipeError& err);

// Transactionally replaces the scene: validates raw_recipes first (no world
// mutation on failure — the prior scene and gen are retained unchanged), then
// destroys the previous generation's scene entities and instantiates the
// validated set, bumping gen on success.
bool bootstrap_transactional(flecs::world& world,
                             const std::vector<RawEntityRecipe>& raw_recipes,
                             SceneGeneration& gen,
                             const PartResolver& resolve_part,
                             RecipeError& err);

struct SceneModule {
    explicit SceneModule(flecs::world& world);
};

} // namespace matter::scene
