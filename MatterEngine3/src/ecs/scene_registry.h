#pragma once

#include "matter/scene.h"
#include "matter/world_definition.h"
#include "flecs.h"

#include <cstdint>
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

const ComponentDescriptor* find_component(const char* name);
uint32_t component_count();
const ComponentDescriptor* component_at(uint32_t index);

bool validate(const RawEntityRecipe& raw, EntityRecipe& out, RecipeError& err);

bool validate_batch(const std::vector<RawEntityRecipe>& recipes,
                    std::vector<EntityRecipe>& out,
                    RecipeError& err);

bool instantiate(flecs::world& world,
                 const EntityRecipe* recipes, uint32_t count,
                 SceneGeneration& gen, RecipeError& err);

struct SceneModule {
    explicit SceneModule(flecs::world& world);
};

} // namespace matter::scene
