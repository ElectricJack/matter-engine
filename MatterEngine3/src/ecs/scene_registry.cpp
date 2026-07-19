#include "scene_registry.h"
#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/streaming.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

// Minimal JSON field extraction — operates on the canonical components_json
// string from RawEntityRecipe. Full JSON parsing is NOT needed here; the
// world_definition_loader already validated syntax. We only need to match
// top-level component keys and their field values for type/range checking.
#include <sstream>

namespace matter::scene {

// ---------------------------------------------------------------------------
// Field descriptors for each component kind.
// ---------------------------------------------------------------------------

static const FieldDescriptor s_transform_fields[] = {
    {"translation", FieldType::Float3, 0, 0, false},
    {"rotation", FieldType::Quaternion, 0, 0, false},
    {"scale", FieldType::Float3, 0, 0, false},
};

static const FieldDescriptor s_rigid_body_fields[] = {
    {"type", FieldType::Enum, 0, 2, true},
    {"linear_damping", FieldType::Float, 0.0f, 100.0f, true},
    {"angular_damping", FieldType::Float, 0.0f, 100.0f, true},
    {"gravity_scale", FieldType::Float, -10.0f, 10.0f, true},
    {"sleep_threshold", FieldType::Float, 0.0f, 10.0f, true},
    {"enable_sleep", FieldType::Bool, 0, 0, false},
    {"continuous", FieldType::Bool, 0, 0, false},
};

static const FieldDescriptor s_velocity_fields[] = {
    {"linear", FieldType::Float3, 0, 0, false},
    {"angular", FieldType::Float3, 0, 0, false},
};

static const FieldDescriptor s_sphere_collider_fields[] = {
    {"center", FieldType::Float3, 0, 0, false},
    {"radius", FieldType::Float, 0.001f, 1000.0f, true},
    {"density", FieldType::Float, 0.0f, 100.0f, true},
    {"friction", FieldType::Float, 0.0f, 1.0f, true},
    {"restitution", FieldType::Float, 0.0f, 1.0f, true},
    {"sensor", FieldType::Bool, 0, 0, false},
};

static const FieldDescriptor s_capsule_collider_fields[] = {
    {"point_a", FieldType::Float3, 0, 0, false},
    {"point_b", FieldType::Float3, 0, 0, false},
    {"radius", FieldType::Float, 0.001f, 1000.0f, true},
    {"density", FieldType::Float, 0.0f, 100.0f, true},
    {"friction", FieldType::Float, 0.0f, 1.0f, true},
    {"restitution", FieldType::Float, 0.0f, 1.0f, true},
    {"sensor", FieldType::Bool, 0, 0, false},
};

static const FieldDescriptor s_box_collider_fields[] = {
    {"center", FieldType::Float3, 0, 0, false},
    {"rotation", FieldType::Quaternion, 0, 0, false},
    {"half_extents", FieldType::Float3, 0, 0, false},
    {"density", FieldType::Float, 0.0f, 100.0f, true},
    {"friction", FieldType::Float, 0.0f, 1.0f, true},
    {"restitution", FieldType::Float, 0.0f, 1.0f, true},
    {"sensor", FieldType::Bool, 0, 0, false},
};

static const FieldDescriptor s_convex_hull_fields[] = {
    {"point_count", FieldType::UInt, 3, 32, true},
    {"density", FieldType::Float, 0.0f, 100.0f, true},
    {"friction", FieldType::Float, 0.0f, 1.0f, true},
    {"restitution", FieldType::Float, 0.0f, 1.0f, true},
    {"sensor", FieldType::Bool, 0, 0, false},
};

static const FieldDescriptor s_part_instance_fields[] = {
    {"part_hash", FieldType::UInt, 0, 0, false},
    {"visible", FieldType::Bool, 0, 0, false},
    {"casts_shadow", FieldType::Bool, 0, 0, false},
};

static const FieldDescriptor s_sector_streaming_fields[] = {};

// ---------------------------------------------------------------------------
// Component descriptor table.
// ---------------------------------------------------------------------------

static const ComponentDescriptor s_descriptors[] = {
    {ComponentKind::Transform, "LocalTransform", s_transform_fields, 3, false},
    {ComponentKind::RigidBody, "RigidBody", s_rigid_body_fields, 7, false},
    {ComponentKind::Velocity, "PhysicsVelocity", s_velocity_fields, 2, false},
    {ComponentKind::SphereCollider, "SphereCollider", s_sphere_collider_fields, 6, false},
    {ComponentKind::CapsuleCollider, "CapsuleCollider", s_capsule_collider_fields, 7, false},
    {ComponentKind::BoxCollider, "BoxCollider", s_box_collider_fields, 7, false},
    {ComponentKind::ConvexHullCollider, "ConvexHullCollider", s_convex_hull_fields, 5, false},
    {ComponentKind::PartInstance, "PartInstance", s_part_instance_fields, 3, false},
    {ComponentKind::SectorStreaming, "SectorStreaming", s_sector_streaming_fields, 0, false},
};

static constexpr uint32_t s_descriptor_count = sizeof(s_descriptors) / sizeof(s_descriptors[0]);

const ComponentDescriptor* find_component(const char* name) {
    for (uint32_t i = 0; i < s_descriptor_count; ++i) {
        if (std::strcmp(s_descriptors[i].name, name) == 0)
            return &s_descriptors[i];
    }
    return nullptr;
}

uint32_t component_count() { return s_descriptor_count; }

const ComponentDescriptor* component_at(uint32_t index) {
    return index < s_descriptor_count ? &s_descriptors[index] : nullptr;
}

// ---------------------------------------------------------------------------
// Minimal JSON key extraction for validation.
// Extracts top-level string keys from a JSON object like:
//   {"LocalTransform": {...}, "RigidBody": {...}}
// ---------------------------------------------------------------------------

// Extracts the raw JSON text of a top-level component's value object, e.g.
// given `{"PartInstance": {"part": "props/crate"}}` and key "PartInstance",
// returns `{"part": "props/crate"}`. Returns "" if the key/value is not an
// object.
static std::string extract_component_value_json(const std::string& json,
                                                 const std::string& component_key) {
    size_t pos = json.find("\"" + component_key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '{') return "";

    size_t start = pos;
    int depth = 1;
    ++pos;
    while (pos < json.size() && depth > 0) {
        if (json[pos] == '{') ++depth;
        else if (json[pos] == '}') --depth;
        ++pos;
    }
    return json.substr(start, pos - start);
}

// Extracts a top-level string field's value from a JSON object fragment.
// Returns false when the field is absent or not a string.
static bool extract_string_field(const std::string& obj_json, const std::string& field,
                                 std::string& out_value) {
    size_t pos = obj_json.find("\"" + field + "\"");
    if (pos == std::string::npos) return false;
    pos = obj_json.find(':', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < obj_json.size() && (obj_json[pos] == ' ' || obj_json[pos] == '\t')) ++pos;
    if (pos >= obj_json.size() || obj_json[pos] != '"') return false;
    ++pos;
    size_t start = pos;
    while (pos < obj_json.size() && obj_json[pos] != '"') {
        if (obj_json[pos] == '\\') ++pos;
        ++pos;
    }
    if (pos > obj_json.size()) return false;
    out_value = obj_json.substr(start, pos - start);
    return true;
}

static bool is_collider_kind(ComponentKind k) {
    return k == ComponentKind::SphereCollider ||
           k == ComponentKind::CapsuleCollider ||
           k == ComponentKind::BoxCollider ||
           k == ComponentKind::ConvexHullCollider;
}

static std::vector<std::string> extract_top_keys(const std::string& json) {
    std::vector<std::string> keys;
    size_t i = 0;
    while (i < json.size() && json[i] != '{') ++i;
    if (i >= json.size()) return keys;
    ++i;

    int depth = 0;
    while (i < json.size()) {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\n' ||
               json[i] == '\r' || json[i] == '\t' || json[i] == ','))
            ++i;
        if (i >= json.size() || json[i] == '}') break;
        if (depth == 0 && json[i] == '"') {
            ++i;
            size_t start = i;
            while (i < json.size() && json[i] != '"') ++i;
            keys.emplace_back(json.substr(start, i - start));
            if (i < json.size()) ++i; // closing quote
            // skip colon and value
            while (i < json.size() && json[i] != ':') ++i;
            if (i < json.size()) ++i;
            // skip the value (could be object, array, string, number, bool, null)
            while (i < json.size() && json[i] == ' ') ++i;
            if (i < json.size()) {
                if (json[i] == '{') {
                    int d = 1; ++i;
                    while (i < json.size() && d > 0) {
                        if (json[i] == '{') ++d;
                        else if (json[i] == '}') --d;
                        ++i;
                    }
                } else if (json[i] == '[') {
                    int d = 1; ++i;
                    while (i < json.size() && d > 0) {
                        if (json[i] == '[') ++d;
                        else if (json[i] == ']') --d;
                        ++i;
                    }
                } else if (json[i] == '"') {
                    ++i;
                    while (i < json.size() && json[i] != '"') {
                        if (json[i] == '\\') ++i;
                        ++i;
                    }
                    if (i < json.size()) ++i;
                } else {
                    while (i < json.size() && json[i] != ',' && json[i] != '}')
                        ++i;
                }
            }
        } else {
            ++i;
        }
    }
    return keys;
}

// ---------------------------------------------------------------------------
// Identity hashing. Authored IDs use FNV-1a over the string bytes.
// Session-created IDs use the high bit set with a monotonic counter.
// ---------------------------------------------------------------------------

static uint64_t hash_authored_id(const std::string& id) {
    uint64_t h = 14695981039346656037ULL;
    for (char c : id) {
        h ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
        h *= 1099511628211ULL;
    }
    return h & 0x7FFFFFFFFFFFFFFFULL; // clear high bit for authored IDs
}

// ---------------------------------------------------------------------------
// validate — checks a single RawEntityRecipe.
// ---------------------------------------------------------------------------

bool validate(const RawEntityRecipe& raw, EntityRecipe& out, RecipeError& err,
             const PartResolver& resolve_part) {
    if (raw.authored_id.empty()) {
        err.message = "empty authored_id";
        err.authored_id = raw.authored_id;
        return false;
    }

    auto keys = extract_top_keys(raw.components_json);
    int collider_count = 0;
    uint64_t resolved_part_hash = 0;

    for (const auto& key : keys) {
        const ComponentDescriptor* desc = find_component(key.c_str());
        if (!desc) {
            err.message = "unknown component: " + key;
            err.authored_id = raw.authored_id;
            err.field_path = key;
            return false;
        }
        if (is_collider_kind(desc->kind)) {
            ++collider_count;
            if (collider_count > 1) {
                err.message = "multiple colliders not allowed";
                err.authored_id = raw.authored_id;
                err.field_path = key;
                return false;
            }
        }
        if (desc->kind == ComponentKind::PartInstance) {
            std::string comp_json = extract_component_value_json(raw.components_json, key);
            std::string module_name;
            if (extract_string_field(comp_json, "part", module_name) && !module_name.empty()) {
                uint64_t hash = 0;
                if (!resolve_part || !resolve_part(module_name, hash)) {
                    err.message = "missing part: " + module_name;
                    err.authored_id = raw.authored_id;
                    err.field_path = "PartInstance.part";
                    return false;
                }
                resolved_part_hash = hash;
            }
        }
    }

    out.authored_id = raw.authored_id;
    out.display_name = raw.display_name;
    out.parent_authored_id = raw.parent_authored_id;
    out.components_json = raw.components_json;
    out.part_hash = resolved_part_hash;
    out.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// validate_batch — validates a set of recipes including cross-references.
// ---------------------------------------------------------------------------

bool validate_batch(const std::vector<RawEntityRecipe>& recipes,
                    std::vector<EntityRecipe>& out,
                    RecipeError& err,
                    const PartResolver& resolve_part) {
    out.clear();
    out.reserve(recipes.size());

    std::unordered_set<std::string> ids;
    std::unordered_map<std::string, std::string> parent_map;

    for (const auto& raw : recipes) {
        EntityRecipe validated;
        if (!validate(raw, validated, err, resolve_part))
            return false;

        if (ids.count(raw.authored_id)) {
            err.message = "duplicate authored_id: " + raw.authored_id;
            err.authored_id = raw.authored_id;
            return false;
        }
        ids.insert(raw.authored_id);
        if (!raw.parent_authored_id.empty())
            parent_map[raw.authored_id] = raw.parent_authored_id;

        out.push_back(std::move(validated));
    }

    // Check missing parents.
    for (const auto& [child, parent] : parent_map) {
        if (!ids.count(parent)) {
            err.message = "missing parent: " + parent;
            err.authored_id = child;
            return false;
        }
    }

    // Check cycles: walk from each node to root; if we revisit, cycle.
    for (const auto& [start, _] : parent_map) {
        std::unordered_set<std::string> visited;
        std::string cur = start;
        while (!cur.empty() && parent_map.count(cur)) {
            if (visited.count(cur)) {
                err.message = "parent cycle detected at: " + cur;
                err.authored_id = start;
                return false;
            }
            visited.insert(cur);
            cur = parent_map[cur];
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// instantiate — creates Flecs entities from validated recipes.
// ---------------------------------------------------------------------------

bool instantiate(flecs::world& world,
                 const EntityRecipe* recipes, uint32_t count,
                 SceneGeneration& gen, RecipeError& err) {
    if (count == 0) return true;

    std::unordered_map<std::string, flecs::entity> id_to_entity;
    std::unordered_set<uint64_t> used_hashes;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& recipe = recipes[i];
        uint64_t hash = hash_authored_id(recipe.authored_id);

        if (used_hashes.count(hash)) {
            err.message = "hash collision for authored_id: " + recipe.authored_id;
            err.authored_id = recipe.authored_id;
            return false;
        }
        used_hashes.insert(hash);

        flecs::entity e = world.entity();
        e.set<SceneEntityId>({hash});

        if (!recipe.display_name.empty())
            e.set_name(recipe.display_name.c_str());

        // Set transform by default (every scene entity has one).
        e.set<ecs::LocalTransform>({});

        auto keys = extract_top_keys(recipe.components_json);
        for (const auto& key : keys) {
            const ComponentDescriptor* desc = find_component(key.c_str());
            if (!desc) {
                err.message = "unknown component: " + key;
                err.authored_id = recipe.authored_id;
                return false;
            }
            switch (desc->kind) {
            case ComponentKind::Transform:
                // Already set above; authored values would override.
                break;
            case ComponentKind::RigidBody:
                e.set<physics::RigidBody>({});
                break;
            case ComponentKind::Velocity:
                e.set<physics::PhysicsVelocity>({});
                break;
            case ComponentKind::SphereCollider:
                e.set<physics::SphereCollider>({});
                break;
            case ComponentKind::CapsuleCollider:
                e.set<physics::CapsuleCollider>({});
                break;
            case ComponentKind::BoxCollider:
                e.set<physics::BoxCollider>({});
                break;
            case ComponentKind::ConvexHullCollider:
                e.set<physics::ConvexHullCollider>({});
                break;
            case ComponentKind::PartInstance: {
                PartInstance pi{};
                pi.part_hash = recipe.part_hash;
                e.set<PartInstance>(pi);
                break;
            }
            case ComponentKind::SectorStreaming:
                e.add<streaming::SectorStreaming>();
                break;
            }
        }

        id_to_entity[recipe.authored_id] = e;
    }

    // Wire parent relationships.
    for (uint32_t i = 0; i < count; ++i) {
        const auto& recipe = recipes[i];
        if (!recipe.parent_authored_id.empty()) {
            auto child_it = id_to_entity.find(recipe.authored_id);
            auto parent_it = id_to_entity.find(recipe.parent_authored_id);
            if (child_it != id_to_entity.end() && parent_it != id_to_entity.end()) {
                child_it->second.child_of(parent_it->second);
            }
        }
    }

    gen.value++;
    return true;
}

// ---------------------------------------------------------------------------
// normalize — validates raw recipes and packages a bootstrap candidate.
// Performs no world mutation; callers apply the candidate separately.
// ---------------------------------------------------------------------------

SceneBootstrapCandidate normalize(const std::vector<RawEntityRecipe>& raw_recipes,
                                  SceneGeneration target_generation,
                                  const PartResolver& resolve_part,
                                  RecipeError& err) {
    SceneBootstrapCandidate candidate;
    candidate.target_generation = target_generation;
    candidate.success = validate_batch(raw_recipes, candidate.recipes, err, resolve_part);
    if (!candidate.success)
        candidate.recipes.clear();
    return candidate;
}

// ---------------------------------------------------------------------------
// bootstrap_transactional — atomically replaces the scene.
// Validation happens before any world mutation, so a failed reload leaves
// the prior scene entities and generation counter untouched.
// ---------------------------------------------------------------------------

bool bootstrap_transactional(flecs::world& world,
                             const std::vector<RawEntityRecipe>& raw_recipes,
                             SceneGeneration& gen,
                             const PartResolver& resolve_part,
                             RecipeError& err) {
    SceneGeneration target{gen.value + 1};
    SceneBootstrapCandidate candidate = normalize(raw_recipes, target, resolve_part, err);
    if (!candidate.success)
        return false; // prior scene and gen retained untouched.

    // Only mutate the world once validation of the entire batch succeeded.
    std::vector<flecs::entity> prior_entities;
    world.each([&](flecs::entity e, const SceneEntityId&) { prior_entities.push_back(e); });
    for (auto& e : prior_entities) e.destruct();

    SceneGeneration new_gen = gen;
    if (!instantiate(world, candidate.recipes.data(),
                     (uint32_t)candidate.recipes.size(), new_gen, err)) {
        // validate_batch already checked duplicate ids/unknown components, so
        // this path should be unreachable in practice; still propagate the
        // failure rather than silently leaving a half-built scene.
        return false;
    }

    gen = new_gen;
    return true;
}

// ---------------------------------------------------------------------------
// SceneModule — Flecs module registration with reflection metadata.
// ---------------------------------------------------------------------------

SceneModule::SceneModule(flecs::world& world) {
    world.module<SceneModule>();

    world.component<SceneEntityId>()
        .member("value", &SceneEntityId::value);

    world.component<PartInstance>()
        .member("part_hash", &PartInstance::part_hash)
        .member("visible", &PartInstance::visible)
        .member("casts_shadow", &PartInstance::casts_shadow);

    world.component<PartInstanceErrorCode>()
        .constant("None", PartInstanceErrorCode::None)
        .constant("MissingPart", PartInstanceErrorCode::MissingPart)
        .constant("PartUnavailable", PartInstanceErrorCode::PartUnavailable)
        .constant("RendererCapacity", PartInstanceErrorCode::RendererCapacity);

    world.component<PartInstanceError>()
        .member("code", &PartInstanceError::code)
        .member("part_hash", &PartInstanceError::part_hash);

    world.component<SceneGeneration>()
        .member("value", &SceneGeneration::value);
}

} // namespace matter::scene
