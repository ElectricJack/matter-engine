#include "scene_registry.h"
#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/streaming.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <type_traits>
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
//
// Every offset comes from offsetof on the real struct, so a renamed or
// reordered member is a compile error rather than silent drift; the field
// *type* is the part a table can still get wrong, which is what
// scene_registry_tests' offset/type round-trip checks cover.
// ---------------------------------------------------------------------------

namespace {

// offsetof of a member, and of a member of the shared ColliderProperties
// sub-struct every collider embeds as `properties`.
#define ME_FIELD_OFF(S, m) static_cast<uint32_t>(offsetof(S, m))
#define ME_COLLIDER_PROP_OFF(S, m)                                  \
    (static_cast<uint32_t>(offsetof(S, properties)) +               \
     static_cast<uint32_t>(offsetof(physics::ColliderProperties, m)))

constexpr FieldDescriptor fd(const char* name, FieldType type, uint32_t offset) {
    FieldDescriptor d{};
    d.name = name;
    d.type = type;
    d.offset = offset;
    return d;
}

constexpr FieldDescriptor fd_float(const char* name, uint32_t offset, float lo, float hi) {
    FieldDescriptor d = fd(name, FieldType::Float, offset);
    d.range_min = lo;
    d.range_max = hi;
    d.has_range = true;
    return d;
}

// Enum labels are the schema's now (they used to live in the editor's
// enum_options_for table). The implicit range is the label index span, which
// is what clamps a write.
constexpr FieldDescriptor fd_enum(const char* name, uint32_t offset, uint8_t storage,
                                  const char* const* labels, uint32_t label_count) {
    FieldDescriptor d = fd(name, FieldType::Enum, offset);
    d.storage_size = storage;
    d.enum_labels = labels;
    d.enum_count = label_count;
    d.range_min = 0.0f;
    d.range_max = label_count > 0 ? static_cast<float>(label_count - 1) : 0.0f;
    d.has_range = true;
    return d;
}

constexpr FieldDescriptor fd_uint(const char* name, uint32_t offset, uint8_t storage,
                                  uint32_t flags) {
    FieldDescriptor d = fd(name, FieldType::UInt, offset);
    d.storage_size = storage;
    d.flags = flags;
    return d;
}

const char* const s_rigid_body_type_labels[] = {"Static", "Kinematic", "Dynamic"};

}  // namespace

static const FieldDescriptor s_transform_fields[] = {
    fd("translation", FieldType::Float3, ME_FIELD_OFF(ecs::LocalTransform, translation)),
    fd("rotation", FieldType::Quaternion, ME_FIELD_OFF(ecs::LocalTransform, rotation)),
    fd("scale", FieldType::Float3, ME_FIELD_OFF(ecs::LocalTransform, scale)),
};

static const FieldDescriptor s_rigid_body_fields[] = {
    fd_enum("type", ME_FIELD_OFF(physics::RigidBody, type),
            sizeof(physics::RigidBodyType), s_rigid_body_type_labels, 3),
    fd_float("linear_damping", ME_FIELD_OFF(physics::RigidBody, linear_damping), 0.0f, 100.0f),
    fd_float("angular_damping", ME_FIELD_OFF(physics::RigidBody, angular_damping), 0.0f, 100.0f),
    fd_float("gravity_scale", ME_FIELD_OFF(physics::RigidBody, gravity_scale), -10.0f, 10.0f),
    fd_float("sleep_threshold", ME_FIELD_OFF(physics::RigidBody, sleep_threshold), 0.0f, 10.0f),
    fd("enable_sleep", FieldType::Bool, ME_FIELD_OFF(physics::RigidBody, enable_sleep)),
    fd("continuous", FieldType::Bool, ME_FIELD_OFF(physics::RigidBody, continuous)),
};

static const FieldDescriptor s_velocity_fields[] = {
    fd("linear", FieldType::Float3, ME_FIELD_OFF(physics::PhysicsVelocity, linear)),
    fd("angular", FieldType::Float3, ME_FIELD_OFF(physics::PhysicsVelocity, angular)),
};

static const FieldDescriptor s_sphere_collider_fields[] = {
    fd("center", FieldType::Float3, ME_FIELD_OFF(physics::SphereCollider, center)),
    fd_float("radius", ME_FIELD_OFF(physics::SphereCollider, radius), 0.001f, 1000.0f),
    fd_float("density", ME_COLLIDER_PROP_OFF(physics::SphereCollider, density), 0.0f, 100.0f),
    fd_float("friction", ME_COLLIDER_PROP_OFF(physics::SphereCollider, friction), 0.0f, 1.0f),
    fd_float("restitution", ME_COLLIDER_PROP_OFF(physics::SphereCollider, restitution), 0.0f, 1.0f),
    fd("sensor", FieldType::Bool, ME_COLLIDER_PROP_OFF(physics::SphereCollider, sensor)),
};

static const FieldDescriptor s_capsule_collider_fields[] = {
    fd("point_a", FieldType::Float3, ME_FIELD_OFF(physics::CapsuleCollider, point_a)),
    fd("point_b", FieldType::Float3, ME_FIELD_OFF(physics::CapsuleCollider, point_b)),
    fd_float("radius", ME_FIELD_OFF(physics::CapsuleCollider, radius), 0.001f, 1000.0f),
    fd_float("density", ME_COLLIDER_PROP_OFF(physics::CapsuleCollider, density), 0.0f, 100.0f),
    fd_float("friction", ME_COLLIDER_PROP_OFF(physics::CapsuleCollider, friction), 0.0f, 1.0f),
    fd_float("restitution", ME_COLLIDER_PROP_OFF(physics::CapsuleCollider, restitution), 0.0f, 1.0f),
    fd("sensor", FieldType::Bool, ME_COLLIDER_PROP_OFF(physics::CapsuleCollider, sensor)),
};

static const FieldDescriptor s_box_collider_fields[] = {
    fd("center", FieldType::Float3, ME_FIELD_OFF(physics::BoxCollider, center)),
    fd("rotation", FieldType::Quaternion, ME_FIELD_OFF(physics::BoxCollider, rotation)),
    fd("half_extents", FieldType::Float3, ME_FIELD_OFF(physics::BoxCollider, half_extents)),
    fd_float("density", ME_COLLIDER_PROP_OFF(physics::BoxCollider, density), 0.0f, 100.0f),
    fd_float("friction", ME_COLLIDER_PROP_OFF(physics::BoxCollider, friction), 0.0f, 1.0f),
    fd_float("restitution", ME_COLLIDER_PROP_OFF(physics::BoxCollider, restitution), 0.0f, 1.0f),
    fd("sensor", FieldType::Bool, ME_COLLIDER_PROP_OFF(physics::BoxCollider, sensor)),
};

static const FieldDescriptor s_convex_hull_fields[] = {
    // Read-only: point_count only means anything alongside the points[] array
    // the schema has no type for, so writing it alone would leave the hull
    // describing garbage. (The editor already rejected this write.)
    fd_uint("point_count", ME_FIELD_OFF(physics::ConvexHullCollider, point_count),
            sizeof(uint32_t), FieldReadOnly),
    fd_float("density", ME_COLLIDER_PROP_OFF(physics::ConvexHullCollider, density), 0.0f, 100.0f),
    fd_float("friction", ME_COLLIDER_PROP_OFF(physics::ConvexHullCollider, friction), 0.0f, 1.0f),
    fd_float("restitution", ME_COLLIDER_PROP_OFF(physics::ConvexHullCollider, restitution), 0.0f, 1.0f),
    fd("sensor", FieldType::Bool, ME_COLLIDER_PROP_OFF(physics::ConvexHullCollider, sensor)),
};

static const FieldDescriptor s_part_instance_fields[] = {
    // Read-only: part_hash is 64-bit, and the UInt accessors are 32-bit, so a
    // generic write would silently truncate. Part assignment goes through the
    // editor's part picker, which carries the full hash.
    fd_uint("part_hash", ME_FIELD_OFF(PartInstance, part_hash),
            sizeof(uint64_t), FieldReadOnly),
    fd("visible", FieldType::Bool, ME_FIELD_OFF(PartInstance, visible)),
    fd("casts_shadow", FieldType::Bool, ME_FIELD_OFF(PartInstance, casts_shadow)),
};

static const FieldDescriptor s_sector_streaming_fields[] = {};

// ---------------------------------------------------------------------------
// Component descriptor table.
// ---------------------------------------------------------------------------

static const ComponentDescriptor s_descriptors[] = {
    {ComponentKind::Transform, "LocalTransform", s_transform_fields, 3, false,
     sizeof(ecs::LocalTransform), alignof(ecs::LocalTransform)},
    {ComponentKind::RigidBody, "RigidBody", s_rigid_body_fields, 7, false,
     sizeof(physics::RigidBody), alignof(physics::RigidBody)},
    {ComponentKind::Velocity, "PhysicsVelocity", s_velocity_fields, 2, false,
     sizeof(physics::PhysicsVelocity), alignof(physics::PhysicsVelocity)},
    {ComponentKind::SphereCollider, "SphereCollider", s_sphere_collider_fields, 6, false,
     sizeof(physics::SphereCollider), alignof(physics::SphereCollider)},
    {ComponentKind::CapsuleCollider, "CapsuleCollider", s_capsule_collider_fields, 7, false,
     sizeof(physics::CapsuleCollider), alignof(physics::CapsuleCollider)},
    {ComponentKind::BoxCollider, "BoxCollider", s_box_collider_fields, 7, false,
     sizeof(physics::BoxCollider), alignof(physics::BoxCollider)},
    {ComponentKind::ConvexHullCollider, "ConvexHullCollider", s_convex_hull_fields, 5, false,
     sizeof(physics::ConvexHullCollider), alignof(physics::ConvexHullCollider)},
    {ComponentKind::PartInstance, "PartInstance", s_part_instance_fields, 3, false,
     sizeof(PartInstance), alignof(PartInstance)},
    {ComponentKind::SectorStreaming, "SectorStreaming", s_sector_streaming_fields, 0, false,
     sizeof(streaming::SectorStreaming), alignof(streaming::SectorStreaming)},
};

static constexpr uint32_t s_descriptor_count = sizeof(s_descriptors) / sizeof(s_descriptors[0]);

// The generic accessors copy a component into a caller-provided stack buffer
// sized by these bounds; keep them honest.
static_assert(sizeof(physics::ConvexHullCollider) <= kMaxComponentStructSize,
              "kMaxComponentStructSize too small for ConvexHullCollider");
static_assert(sizeof(physics::BoxCollider) <= kMaxComponentStructSize,
              "kMaxComponentStructSize too small for BoxCollider");
static_assert(alignof(physics::ConvexHullCollider) <= kMaxComponentStructAlign,
              "kMaxComponentStructAlign too small");
static_assert(alignof(ecs::LocalTransform) <= kMaxComponentStructAlign,
              "kMaxComponentStructAlign too small");

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

const FieldDescriptor* find_field(const ComponentDescriptor& component, const char* field) {
    if (!field) return nullptr;
    for (uint32_t i = 0; i < component.field_count; ++i) {
        if (component.fields[i].name && std::strcmp(component.fields[i].name, field) == 0)
            return &component.fields[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Generic offset-based field access.
// ---------------------------------------------------------------------------

namespace {

template <class T>
const T& field_as(const void* component, const FieldDescriptor& f) {
    return *reinterpret_cast<const T*>(static_cast<const char*>(component) + f.offset);
}
template <class T>
T& field_as(void* component, const FieldDescriptor& f) {
    return *reinterpret_cast<T*>(static_cast<char*>(component) + f.offset);
}

bool writable(const FieldDescriptor& f) { return (f.flags & FieldReadOnly) == 0; }

// Reads an integral field of storage_size bytes as an unsigned 64-bit value.
bool read_integral(const void* component, const FieldDescriptor& f, uint64_t& out) {
    switch (f.storage_size) {
        case 1: out = field_as<uint8_t>(component, f); return true;
        case 2: out = field_as<uint16_t>(component, f); return true;
        case 4: out = field_as<uint32_t>(component, f); return true;
        case 8: out = field_as<uint64_t>(component, f); return true;
        default: return false;
    }
}

bool write_integral(void* component, const FieldDescriptor& f, uint64_t value) {
    switch (f.storage_size) {
        case 1: field_as<uint8_t>(component, f) = static_cast<uint8_t>(value); return true;
        case 2: field_as<uint16_t>(component, f) = static_cast<uint16_t>(value); return true;
        case 4: field_as<uint32_t>(component, f) = static_cast<uint32_t>(value); return true;
        case 8: field_as<uint64_t>(component, f) = value; return true;
        default: return false;
    }
}

int32_t clamp_to_range(const FieldDescriptor& f, int32_t v) {
    if (!f.has_range) return v;
    const int32_t lo = static_cast<int32_t>(f.range_min);
    const int32_t hi = static_cast<int32_t>(f.range_max);
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

bool field_get_float(const void* component, const FieldDescriptor& f, float& out) {
    if (!component || f.type != FieldType::Float) return false;
    out = field_as<float>(component, f);
    return true;
}

bool field_set_float(void* component, const FieldDescriptor& f, float value) {
    if (!component || f.type != FieldType::Float || !writable(f)) return false;
    field_as<float>(component, f) = value;
    return true;
}

bool field_get_int(const void* component, const FieldDescriptor& f, int32_t& out) {
    if (!component) return false;
    if (f.type == FieldType::Int) {
        out = field_as<int32_t>(component, f);
        return true;
    }
    if (f.type != FieldType::Enum) return false;
    uint64_t raw = 0;
    if (!read_integral(component, f, raw)) return false;
    out = static_cast<int32_t>(raw);
    return true;
}

bool field_set_int(void* component, const FieldDescriptor& f, int32_t value) {
    if (!component || !writable(f)) return false;
    if (f.type == FieldType::Int) {
        field_as<int32_t>(component, f) = clamp_to_range(f, value);
        return true;
    }
    if (f.type != FieldType::Enum) return false;
    const int32_t clamped = clamp_to_range(f, value);
    return write_integral(component, f, static_cast<uint64_t>(clamped < 0 ? 0 : clamped));
}

bool field_get_uint(const void* component, const FieldDescriptor& f, uint32_t& out) {
    if (!component || f.type != FieldType::UInt) return false;
    uint64_t raw = 0;
    if (!read_integral(component, f, raw)) return false;
    // Wider storage (PartInstance::part_hash) truncates to the low 32 bits,
    // which is what the editor's hand-written getter did.
    out = static_cast<uint32_t>(raw);
    return true;
}

bool field_set_uint(void* component, const FieldDescriptor& f, uint32_t value) {
    if (!component || f.type != FieldType::UInt || !writable(f)) return false;
    uint32_t v = value;
    if (f.has_range) {
        const uint32_t lo = f.range_min <= 0.0f ? 0u : static_cast<uint32_t>(f.range_min);
        const uint32_t hi = f.range_max <= 0.0f ? 0u : static_cast<uint32_t>(f.range_max);
        v = v < lo ? lo : (v > hi ? hi : v);
    }
    return write_integral(component, f, v);
}

bool field_get_bool(const void* component, const FieldDescriptor& f, bool& out) {
    if (!component || f.type != FieldType::Bool) return false;
    out = field_as<bool>(component, f);
    return true;
}

bool field_set_bool(void* component, const FieldDescriptor& f, bool value) {
    if (!component || f.type != FieldType::Bool || !writable(f)) return false;
    field_as<bool>(component, f) = value;
    return true;
}

bool field_get_float3(const void* component, const FieldDescriptor& f, Float3& out) {
    if (!component || f.type != FieldType::Float3) return false;
    out = field_as<Float3>(component, f);
    return true;
}

bool field_set_float3(void* component, const FieldDescriptor& f, const Float3& value) {
    if (!component || f.type != FieldType::Float3 || !writable(f)) return false;
    field_as<Float3>(component, f) = value;
    return true;
}

bool field_get_quat(const void* component, const FieldDescriptor& f, Quaternion& out) {
    if (!component || f.type != FieldType::Quaternion) return false;
    out = field_as<Quaternion>(component, f);
    return true;
}

bool field_set_quat(void* component, const FieldDescriptor& f, const Quaternion& value) {
    if (!component || f.type != FieldType::Quaternion || !writable(f)) return false;
    field_as<Quaternion>(component, f) = value;
    return true;
}

bool to_props_desc(const FieldDescriptor& f, matter::props::Desc& out) {
    matter::props::Desc d;
    switch (f.type) {
        case FieldType::Float:  d.type = matter::props::Type::Float; break;
        case FieldType::Int:    d.type = matter::props::Type::Int; break;
        case FieldType::UInt:   d.type = matter::props::Type::UInt; break;
        case FieldType::Bool:   d.type = matter::props::Type::Bool; break;
        case FieldType::Enum:   d.type = matter::props::Type::Enum; break;
        case FieldType::Float3: d.type = matter::props::Type::Float3; break;
        // Quaternion stays ECS-only (spec S7): props has no such type, and the
        // Euler-edit widget that consumes it is ECS-specific.
        case FieldType::Quaternion: return false;
    }
    const bool integral = f.type == FieldType::Int || f.type == FieldType::UInt ||
                          f.type == FieldType::Enum;
    // props' typed accessors address Int/UInt/Enum as int32_t/uint32_t.
    if (integral && f.storage_size != 4) return false;

    d.name = f.name;
    d.offset = f.offset;
    d.min = f.range_min;
    d.max = f.range_max;
    d.has_range = f.has_range;
    d.doc = f.doc;
    d.enum_labels = f.enum_labels;
    d.enum_count = f.enum_count;
    if (f.flags & FieldReadOnly) d.flags |= matter::props::ReadOnly;
    out = d;
    return true;
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

static bool extract_float_array(const std::string& json, const std::string& field,
                                float* out, size_t count) {
    size_t pos = json.find("\"" + field + "\"");
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (pos >= json.size() || json[pos] != '[') return false;
    ++pos;
    for (size_t i = 0; i < count; ++i) {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',')) ++pos;
        if (pos >= json.size()) return false;
        char* end = nullptr;
        out[i] = std::strtof(json.c_str() + pos, &end);
        if (end == json.c_str() + pos) return false;
        pos = end - json.c_str();
    }
    return true;
}

static bool extract_float_field(const std::string& json, const std::string& field,
                                float& out) {
    size_t pos = json.find("\"" + field + "\"");
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (pos >= json.size()) return false;
    char* end = nullptr;
    float val = std::strtof(json.c_str() + pos, &end);
    if (end == json.c_str() + pos) return false;
    out = val;
    return true;
}

static bool extract_bool_field(const std::string& json,
                               const std::string& field, bool& out) {
    size_t pos = json.find("\"" + field + "\"");
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (json.compare(pos, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
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
        // A SceneEntityId value is stable across reloads, while generation
        // identifies this specific incarnation to deferred GPU retirement.
        // instantiate() commits exactly one new scene generation at its end.
        const uint64_t next_generation = gen.value + 1u;
        if (next_generation == 0 || next_generation > UINT32_MAX) {
            err.message = "scene entity generation exhausted";
            err.authored_id = recipe.authored_id;
            return false;
        }
        e.set<SceneEntityId>({hash, static_cast<uint32_t>(next_generation)});

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
            case ComponentKind::Transform: {
                std::string tj = extract_component_value_json(recipe.components_json, key);
                ecs::LocalTransform lt{};
                extract_float_array(tj, "translation", &lt.translation.x, 3);
                float rot[4];
                if (extract_float_array(tj, "rotation", rot, 4))
                    lt.rotation = {rot[0], rot[1], rot[2], rot[3]};
                float sc[3] = {1.0f, 1.0f, 1.0f};
                if (extract_float_array(tj, "scale", sc, 3))
                    lt.scale = {sc[0], sc[1], sc[2]};
                e.set<ecs::LocalTransform>(lt);
                break;
            }
            case ComponentKind::RigidBody: {
                std::string rj = extract_component_value_json(recipe.components_json, key);
                physics::RigidBody rb{};
                std::string type_str;
                if (extract_string_field(rj, "type", type_str)) {
                    if (type_str == "dynamic") rb.type = physics::RigidBodyType::Dynamic;
                    else if (type_str == "kinematic") rb.type = physics::RigidBodyType::Kinematic;
                    else rb.type = physics::RigidBodyType::Static;
                }
                float f;
                if (extract_float_field(rj, "linearDamping", f)) rb.linear_damping = f;
                if (extract_float_field(rj, "angularDamping", f)) rb.angular_damping = f;
                if (extract_float_field(rj, "gravityScale", f)) rb.gravity_scale = f;
                e.set<physics::RigidBody>(rb);
                break;
            }
            case ComponentKind::Velocity: {
                std::string vj = extract_component_value_json(recipe.components_json, key);
                physics::PhysicsVelocity vel{};
                extract_float_array(vj, "linear", &vel.linear.x, 3);
                extract_float_array(vj, "angular", &vel.angular.x, 3);
                e.set<physics::PhysicsVelocity>(vel);
                break;
            }
            case ComponentKind::SphereCollider: {
                std::string sj = extract_component_value_json(recipe.components_json, key);
                physics::SphereCollider sc{};
                extract_float_array(sj, "center", &sc.center.x, 3);
                float f;
                if (extract_float_field(sj, "radius", f)) sc.radius = f;
                if (extract_float_field(sj, "density", f)) sc.properties.density = f;
                if (extract_float_field(sj, "friction", f)) sc.properties.friction = f;
                if (extract_float_field(sj, "restitution", f)) sc.properties.restitution = f;
                e.set<physics::SphereCollider>(sc);
                break;
            }
            case ComponentKind::CapsuleCollider: {
                std::string cj = extract_component_value_json(recipe.components_json, key);
                physics::CapsuleCollider cc{};
                extract_float_array(cj, "pointA", &cc.point_a.x, 3);
                extract_float_array(cj, "pointB", &cc.point_b.x, 3);
                float f;
                if (extract_float_field(cj, "radius", f)) cc.radius = f;
                if (extract_float_field(cj, "density", f)) cc.properties.density = f;
                if (extract_float_field(cj, "friction", f)) cc.properties.friction = f;
                if (extract_float_field(cj, "restitution", f)) cc.properties.restitution = f;
                e.set<physics::CapsuleCollider>(cc);
                break;
            }
            case ComponentKind::BoxCollider: {
                std::string bj = extract_component_value_json(recipe.components_json, key);
                physics::BoxCollider bc{};
                extract_float_array(bj, "center", &bc.center.x, 3);
                extract_float_array(bj, "halfExtents", &bc.half_extents.x, 3);
                float f;
                if (extract_float_field(bj, "density", f)) bc.properties.density = f;
                if (extract_float_field(bj, "friction", f)) bc.properties.friction = f;
                if (extract_float_field(bj, "restitution", f)) bc.properties.restitution = f;
                e.set<physics::BoxCollider>(bc);
                break;
            }
            case ComponentKind::ConvexHullCollider:
                e.set<physics::ConvexHullCollider>({});
                break;
            case ComponentKind::PartInstance: {
                PartInstance pi{};
                pi.part_hash = recipe.part_hash;
                const std::string part_json =
                    extract_component_value_json(
                        recipe.components_json, "PartInstance");
                (void)extract_bool_field(
                    part_json, "visible", pi.visible);
                (void)extract_bool_field(
                    part_json, "casts_shadow", pi.casts_shadow);
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
        .member("value", &SceneEntityId::value)
        .member("generation", &SceneEntityId::generation);

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
