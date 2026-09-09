// MatterEngine3/src/ecs/scene_registry.h
//
// The dynamic scene's reflection schema and its recipe pipeline.
//
// SCHEMA. `ComponentDescriptor` / `FieldDescriptor` describe every ECS
// component the editor is allowed to inspect, down to each member's byte offset
// inside the struct. That offset is what makes generic editing possible: the
// inspector never names a component type, it walks `component_at` /
// `find_field` and calls the `field_get_*` / `field_set_*` accessors below. The
// table itself lives in scene_registry.cpp, where every offset comes from
// `offsetof` on the real struct.
//
// RECIPE PIPELINE. `validate` -> `validate_batch` -> `normalize` ->
// `instantiate`, with `bootstrap_transactional` as the safe front door that
// validates before mutating. Inputs are the `RawEntityRecipe`s the
// world-definition loader produces (matter/world_definition.h).
//
// Relationship to the property system: this is the ECS-side sibling of
// `matter::props` (spec S7). `to_props_desc` bridges one field descriptor over
// where a props helper is wanted; the two shapes props cannot represent
// (Quaternion members, and integer/enum members that are not 4 bytes) stay
// ECS-only.
//
// Threading: app-thread affine. The descriptor tables are static const data and
// safe to read from anywhere, but every function that takes a `flecs::world&`
// mutates it and must run on the thread that pumps the world.

#pragma once

#include "matter/math_types.h"
#include "matter/character.h"
#include "matter/props.h"
#include "matter/scene.h"
#include "matter/world_definition.h"
#include "flecs.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace matter::scene {

// Which ECS component a ComponentDescriptor describes. Used to switch on a
// descriptor without string comparison — notably in `instantiate`, which needs
// per-component JSON decoding. The order matches `s_descriptors` in
// scene_registry.cpp; the numeric values are not persisted anywhere.
enum class ComponentKind : uint8_t {
    Transform,
    RigidBody,
    Velocity,
    SphereCollider,
    CapsuleCollider,
    BoxCollider,
    ConvexHullCollider,
    PartInstance,
    SectorStreaming,
    RiverFloatBody,
    CharacterController
};

// The interpretation of the bytes at a FieldDescriptor's offset, and therefore
// which accessor pair is legal for it. Note this is the LOGICAL type: for Int,
// UInt and Enum the physical width is `FieldDescriptor::storage_size`, which is
// often not 4.
enum class FieldType : uint8_t {
    Float,
    Int,
    UInt,
    Bool,
    Enum,
    Float3,
    Quaternion
};

enum FieldFlags : uint32_t {
    FieldFlagNone = 0,
    // Displayed but never written. Maps to props::ReadOnly. Used for fields
    // whose storage the generic path cannot safely write (PartInstance's
    // 64-bit part_hash, whose editor route is the part picker) or whose value
    // is only meaningful together with sibling data the schema does not cover
    // (ConvexHullCollider::point_count and its points[] array).
    FieldReadOnly = 1u << 0,
};

// A field of an ECS component. `offset` is the byte offset of the member in
// the owning component struct, which is what makes generic (schema-driven)
// field access possible — see the field_get_*/field_set_* accessors below.
// This is the ECS-side sibling of matter::props::Desc (property-system spec
// S7); to_props_desc() converts one where a props helper is wanted. It is not
// simply props::Desc because ECS components carry two shapes props does not:
// Quaternion members, and integer/enum members that are not 4 bytes wide.
struct FieldDescriptor {
    const char* name = nullptr;
    FieldType type = FieldType::Float;
    float range_min = 0.0f;
    float range_max = 0.0f;
    bool has_range = false;
    uint32_t offset = 0;        // byte offset into the component struct
    // Byte width of the underlying storage for Int/UInt/Enum fields. Enum
    // members are frequently uint8_t-backed (physics::RigidBodyType) and
    // PartInstance::part_hash is a uint64_t exposed as a UInt field, so the
    // accessors cannot assume 4 bytes. Ignored for the other field types.
    uint8_t storage_size = 4;
    uint32_t flags = 0;         // FieldFlags
    const char* const* enum_labels = nullptr;  // Enum option labels, in order
    uint32_t enum_count = 0;
    const char* doc = nullptr;  // tooltip text
};

// One row of the component schema. `fields` points at a static array of
// `field_count` descriptors owned by scene_registry.cpp — never freed, valid
// for the life of the process, so this struct is safe to copy or hold by
// pointer. `struct_size`/`struct_align` let a caller stack-allocate a buffer
// that can hold a copy of the component (see the kMax* bounds below).
struct ComponentDescriptor {
    ComponentKind kind{};
    const char* name = nullptr;
    const FieldDescriptor* fields = nullptr;
    uint32_t field_count = 0;
    bool allow_multiple = false;
    uint32_t struct_size = 0;   // sizeof the ECS component struct
    uint32_t struct_align = 0;  // alignof the ECS component struct
};

// Upper bounds over every registered component's struct_size/struct_align, so
// a caller can declare one stack buffer that holds any component copy.
// scene_registry.cpp static_asserts every component against these.
inline constexpr uint32_t kMaxComponentStructSize = 512;
inline constexpr uint32_t kMaxComponentStructAlign = 16;

// Why a recipe was rejected. `message` is human-facing; `authored_id` names the
// recipe at fault and `field_path` the component or field where known (it is
// left empty for batch-level failures such as a parent cycle). Only meaningful
// when the call that filled it returned false.
struct RecipeError {
    std::string message;
    std::string authored_id;
    std::string field_path;
};

// Monotonic counter of scene bootstraps, held as a flecs singleton. Every
// entity `instantiate` creates is stamped with the value, so a stale GPU slot
// referring to an earlier generation can be recognized and dropped. Bumped once
// per successful instantiate; `instantiate` fails rather than wrapping past
// UINT32_MAX, which is the width of `SceneEntityId::generation`.
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
const FieldDescriptor* find_field(const ComponentDescriptor& component, const char* field);

// ---------------------------------------------------------------------------
// Generic offset-based field access (property-system spec S7).
//
// `component` points at ONE component instance — in the editor that is a copy
// fetched onto the stack, never the live ECS storage: a setter mutates the
// copy and the caller re-sets the whole component so the ECS applies the edit
// transactionally. Every accessor returns false when the descriptor's type
// does not match the call (so asking for a float on a Quaternion field fails
// rather than reinterpreting bytes), and every setter returns false for a
// FieldReadOnly field.
//
// Int/Enum setters clamp to [range_min, range_max] when has_range; float
// setters do not (the ImGui slider already bounds them, and the callers'
// historical behavior was unclamped).
// ---------------------------------------------------------------------------
bool field_get_float(const void* component, const FieldDescriptor& field, float& out);
bool field_set_float(void* component, const FieldDescriptor& field, float value);
// Accepts Int and Enum fields alike — the panel edits enums through the int
// path (WidgetKind::EnumDropdown reads/writes an int index).
bool field_get_int(const void* component, const FieldDescriptor& field, int32_t& out);
bool field_set_int(void* component, const FieldDescriptor& field, int32_t value);
bool field_get_uint(const void* component, const FieldDescriptor& field, uint32_t& out);
bool field_set_uint(void* component, const FieldDescriptor& field, uint32_t value);
bool field_get_bool(const void* component, const FieldDescriptor& field, bool& out);
bool field_set_bool(void* component, const FieldDescriptor& field, bool value);
bool field_get_float3(const void* component, const FieldDescriptor& field, Float3& out);
bool field_set_float3(void* component, const FieldDescriptor& field, const Float3& value);
bool field_get_quat(const void* component, const FieldDescriptor& field, Quaternion& out);
bool field_set_quat(void* component, const FieldDescriptor& field, const Quaternion& value);

// props::Desc view of an ECS field, for the props helpers (parse_and_set,
// format_value, the JSON codec) that take one. Returns false — leaving `out`
// untouched — for the two shapes props cannot address: Quaternion fields
// (ECS-only by design, spec S7) and Int/UInt/Enum fields whose storage is not
// 4 bytes wide. The const char* members are copied as pointers: `field` (and
// the strings it points at) must outlive `out`.
bool to_props_desc(const FieldDescriptor& field, matter::props::Desc& out);

bool validate(const RawEntityRecipe& raw, EntityRecipe& out, RecipeError& err,
             const PartResolver& resolve_part = nullptr);

// SceneEntityId::value is split into two namespaces by its top bit, and this
// constant is the ONE definition of that split. World-authored ids are
// `hash_authored_id` FNV-1a hashes with the bit CLEARED; ids minted at runtime
// by SceneService (scene/scene_service.cpp allocate_id) carry it SET. Keeping
// the allocators in disjoint halves is what makes them collision-free across a
// reload, which a liveness scan over currently-loaded entities cannot achieve,
// and it lets a reader classify an id's provenance without the world
// definition in hand.
inline constexpr uint64_t kRuntimeIdBit = 1ULL << 63;

// True when `value` was minted at runtime rather than hashed from an authored
// id. The zero sentinel ("no id") reports false, like any authored value.
inline constexpr bool is_runtime_id(uint64_t value) {
    return (value & kRuntimeIdBit) != 0;
}

// Stable authored identity: FNV-1a bytes with kRuntimeIdBit cleared.
uint64_t hash_authored_id(const std::string& id);

// Validate an edited copy against its entity before committing ECS storage.
bool validate_character_component(flecs::entity entity,
                                  const character::CharacterController& value,
                                  std::string& error);

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

// Creates entities for `count` already-validated recipes and wires their parent
// relationships, bumping `gen` once on success. NOT transactional: a mid-batch
// failure leaves the entities created so far in the world — prefer
// bootstrap_transactional() unless you are managing that yourself.
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

// Flecs module that registers reflection metadata for SceneEntityId,
// PartInstance, PartInstanceError(Code) and SceneGeneration. Import once per
// world with `world.import<matter::scene::SceneModule>()`. Types only — it
// installs no systems and creates no entities.
struct SceneModule {
    explicit SceneModule(flecs::world& world);
};

} // namespace matter::scene
