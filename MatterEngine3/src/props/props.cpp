#include "matter/props.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace matter {
namespace props {
namespace {

using jsondoc::Value;

inline void* field_ptr(void* base, const Desc& d) {
    return static_cast<char*>(base) + d.offset;
}
inline const void* field_ptr(const void* base, const Desc& d) {
    return static_cast<const char*>(base) + d.offset;
}

template <class T>
inline T& as(void* base, const Desc& d) { return *static_cast<T*>(field_ptr(base, d)); }
template <class T>
inline const T& as(const void* base, const Desc& d) { return *static_cast<const T*>(field_ptr(base, d)); }

inline bool is_float3(Type t) { return t == Type::Float3 || t == Type::Color3; }

inline float clampf(float v, const Desc& d) {
    if (!d.has_range) return v;
    return v < d.min ? d.min : (v > d.max ? d.max : v);
}

inline bool persistable(const Desc& d) {
    return (d.flags & (ReadOnly | NoSerialize)) == 0;
}

const char* type_name(Type t) {
    switch (t) {
        case Type::Float:  return "float";
        case Type::Int:    return "int";
        case Type::UInt:   return "uint";
        case Type::Bool:   return "bool";
        case Type::Enum:   return "enum";
        case Type::Float3: return "float3";
        case Type::Color3: return "color3";
        case Type::String: return "string";
    }
    return "?";
}

const char* kind_name(Value::Kind k) {
    switch (k) {
        case Value::Kind::Null:   return "null";
        case Value::Kind::Bool:   return "bool";
        case Value::Kind::Number: return "number";
        case Value::Kind::String: return "string";
        case Value::Kind::Array:  return "array";
        case Value::Kind::Object: return "object";
    }
    return "?";
}

void warn_type(const char* group_path, const Desc& d, const Value& v) {
    fprintf(stderr, "[props] %s.%s: expected %s, got %s — skipped\n",
            group_path ? group_path : "?", d.name ? d.name : "?",
            type_name(d.type), kind_name(v.kind));
}

Value number(double n) {
    Value v;
    v.kind = Value::Kind::Number;
    v.num = n;
    return v;
}

// Setters refuse NaN/inf, but engine code writes described structs directly,
// so a non-finite value can still reach a save. json_doc would emit it as a
// bare `nan`/`inf` token its own parser rejects, corrupting the whole scope
// file on the next load — such fields must never be encoded.
bool field_finite(const void* instance, const Desc& d) {
    switch (d.type) {
        case Type::Float:
            return std::isfinite(get_float(instance, d));
        case Type::Float3:
        case Type::Color3: {
            float xyz[3];
            get_float3(instance, d, xyz);
            return std::isfinite(xyz[0]) && std::isfinite(xyz[1]) &&
                   std::isfinite(xyz[2]);
        }
        default:
            return true;
    }
}

Value encode_field(const void* instance, const Desc& d) {
    Value v;
    switch (d.type) {
        case Type::Float:  return number(get_float(instance, d));
        case Type::Int:    return number(get_int(instance, d));
        case Type::UInt:   return number(get_uint(instance, d));
        case Type::Enum:   return number(get_enum(instance, d));
        case Type::Bool:
            v.kind = Value::Kind::Bool;
            v.b = get_bool(instance, d);
            return v;
        case Type::Float3:
        case Type::Color3: {
            float xyz[3];
            get_float3(instance, d, xyz);
            v.kind = Value::Kind::Array;
            v.arr.push_back(number(xyz[0]));
            v.arr.push_back(number(xyz[1]));
            v.arr.push_back(number(xyz[2]));
            return v;
        }
        case Type::String:
            v.kind = Value::Kind::String;
            v.str = get_string(instance, d);
            return v;
    }
    return v;
}

// Returns false when the JSON value's kind does not match the field type; the
// caller warns and keeps the current value.
bool decode_field(void* instance, const Desc& d, const Value& v) {
    switch (d.type) {
        case Type::Float:
            if (v.kind != Value::Kind::Number) return false;
            set_float(instance, d, static_cast<float>(v.num));
            return true;
        case Type::Int:
            if (v.kind != Value::Kind::Number) return false;
            set_int(instance, d, static_cast<int32_t>(v.num));
            return true;
        case Type::UInt:
            if (v.kind != Value::Kind::Number) return false;
            set_uint(instance, d, v.num < 0.0 ? 0u : static_cast<uint32_t>(v.num));
            return true;
        case Type::Enum:
            if (v.kind != Value::Kind::Number) return false;
            set_enum(instance, d, static_cast<int32_t>(v.num));
            return true;
        case Type::Bool:
            if (v.kind != Value::Kind::Bool) return false;
            set_bool(instance, d, v.b);
            return true;
        case Type::Float3:
        case Type::Color3: {
            if (v.kind != Value::Kind::Array || v.arr.size() != 3) return false;
            float xyz[3];
            for (int i = 0; i < 3; ++i) {
                if (v.arr[static_cast<size_t>(i)].kind != Value::Kind::Number) return false;
                xyz[i] = static_cast<float>(v.arr[static_cast<size_t>(i)].num);
            }
            set_float3(instance, d, xyz);
            return true;
        }
        case Type::String:
            if (v.kind != Value::Kind::String) return false;
            set_string(instance, d, v.str);
            return true;
    }
    return false;
}

// One shared env parser, replacing the three copy-pasted env_u32/env_f32/
// env_flag helpers (vt_residency.cpp, vt_enrich.cpp, vk_scene_renderer.cpp).
bool parse_env_bool(const char* s, bool& out) {
    if (!s || !*s) return false;
    if (!strcmp(s, "1") || !strcmp(s, "true") || !strcmp(s, "TRUE") ||
        !strcmp(s, "yes") || !strcmp(s, "on")) { out = true; return true; }
    if (!strcmp(s, "0") || !strcmp(s, "false") || !strcmp(s, "FALSE") ||
        !strcmp(s, "no") || !strcmp(s, "off")) { out = false; return true; }
    return false;
}

bool parse_env_number(const char* s, double& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const double v = strtod(s, &end);
    if (end == s) return false;
    out = v;
    return true;
}

// "1,2,3" / "1 2 3" — any non-numeric run separates components.
bool parse_env_float3(const char* s, float out[3]) {
    if (!s) return false;
    const char* p = s;
    for (int i = 0; i < 3; ++i) {
        while (*p && *p != '-' && *p != '+' && *p != '.' && !(*p >= '0' && *p <= '9')) ++p;
        char* end = nullptr;
        const double v = strtod(p, &end);
        if (end == p) return false;
        out[i] = static_cast<float>(v);
        p = end;
    }
    return true;
}

}  // namespace

bool equals_ignore_case(const char* a, const char* b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b) {
        if (tolower(static_cast<unsigned char>(*a)) !=
            tolower(static_cast<unsigned char>(*b)))
            return false;
    }
    return *a == *b;
}

// One shared text->field parser: the env layer (layer 5), the FIFO `set`
// command and any future script/CLI setter all go through this.
bool parse_and_set(void* instance, const Desc& d, const char* raw) {
    if (!instance || !raw) return false;
    switch (d.type) {
        case Type::Float: {
            double n;
            if (!parse_env_number(raw, n)) return false;
            set_float(instance, d, static_cast<float>(n));
            return true;
        }
        case Type::Int: {
            double n;
            if (!parse_env_number(raw, n)) return false;
            set_int(instance, d, static_cast<int32_t>(n));
            return true;
        }
        case Type::UInt: {
            double n;
            if (!parse_env_number(raw, n)) return false;
            set_uint(instance, d, n < 0.0 ? 0u : static_cast<uint32_t>(n));
            return true;
        }
        case Type::Enum: {
            // Accept an index or one of the declared labels. The label match is
            // case-INSENSITIVE: the labels are UI text ("Quality") while every
            // command line and env var in this codebase spells the same value
            // lowercase (MATTER_DLSS_MODE=quality, `dlss quality`), and making
            // the user match the combo's capitalisation would fail silently
            // into the numeric branch below. No group has two labels differing
            // only in case, so nothing becomes ambiguous.
            for (uint32_t i = 0; i < d.enum_count; ++i) {
                if (d.enum_labels && d.enum_labels[i] &&
                    equals_ignore_case(d.enum_labels[i], raw)) {
                    set_enum(instance, d, static_cast<int32_t>(i));
                    return true;
                }
            }
            double n;
            if (!parse_env_number(raw, n)) return false;
            set_enum(instance, d, static_cast<int32_t>(n));
            return true;
        }
        case Type::Bool: {
            bool b;
            if (!parse_env_bool(raw, b)) return false;
            set_bool(instance, d, b);
            return true;
        }
        case Type::Float3:
        case Type::Color3: {
            float xyz[3];
            if (!parse_env_float3(raw, xyz)) return false;
            set_float3(instance, d, xyz);
            return true;
        }
        case Type::String:
            set_string(instance, d, raw);
            return true;
    }
    return false;
}

bool parse_and_set(Binding& b, const Desc& d, const char* raw) {
    if (!parse_and_set(b.instance(), d, raw)) return false;
    b.set_dirty(true);
    return true;
}

std::string format_value(const void* instance, const Desc& d) {
    if (!instance) return {};
    char buf[128];
    switch (d.type) {
        case Type::Float:
            snprintf(buf, sizeof(buf), "%.6g",
                     static_cast<double>(get_float(instance, d)));
            return buf;
        case Type::Int:
            snprintf(buf, sizeof(buf), "%d", get_int(instance, d));
            return buf;
        case Type::UInt:
            snprintf(buf, sizeof(buf), "%u", get_uint(instance, d));
            return buf;
        case Type::Bool:
            return get_bool(instance, d) ? "true" : "false";
        case Type::Enum: {
            const int32_t v = get_enum(instance, d);
            if (d.enum_labels && v >= 0 && static_cast<uint32_t>(v) < d.enum_count &&
                d.enum_labels[v])
                return d.enum_labels[v];
            snprintf(buf, sizeof(buf), "%d", v);
            return buf;
        }
        case Type::Float3:
        case Type::Color3: {
            float xyz[3];
            get_float3(instance, d, xyz);
            snprintf(buf, sizeof(buf), "%.6g, %.6g, %.6g",
                     static_cast<double>(xyz[0]), static_cast<double>(xyz[1]),
                     static_cast<double>(xyz[2]));
            return buf;
        }
        case Type::String:
            return get_string(instance, d);
    }
    return {};
}

bool resolve_field(Registry& r, const char* full_path, Binding*& binding,
                   const Desc*& desc) {
    if (!full_path) return false;
    const char* dot = strrchr(full_path, '.');
    if (!dot || dot == full_path || !dot[1]) return false;
    const std::string group_path(full_path, static_cast<size_t>(dot - full_path));
    Binding* b = r.find(group_path.c_str());
    if (!b) return false;
    const Group& g = b->schema();
    for (uint32_t i = 0; i < g.field_count; ++i) {
        if (g.fields[i].name && !strcmp(g.fields[i].name, dot + 1)) {
            binding = b;
            desc = &g.fields[i];
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

// The construct/destruct/copy dispatch, ctx form first. A DynamicGroup has no
// C++ type to template on, so its hooks are told which schema to walk through
// Group::ctx; every other group keeps the plain void(*)(void*) slots.
bool group_can_instantiate(const Group& g) {
    return g.struct_size != 0 && (g.ctx_construct || g.construct_default);
}

void group_construct(const Group& g, void* p) {
    if (g.ctx_construct) g.ctx_construct(g.ctx, p);
    else if (g.construct_default) g.construct_default(p);
}

void group_destruct(const Group& g, void* p) {
    if (g.ctx_destruct) g.ctx_destruct(g.ctx, p);
    else if (g.destruct_default) g.destruct_default(p);
}

bool group_copy_assign(const Group& g, void* dst, const void* src) {
    if (g.ctx_copy_assign) { g.ctx_copy_assign(g.ctx, dst, src); return true; }
    if (g.copy_assign) { g.copy_assign(dst, src); return true; }
    return false;
}

namespace {
std::align_val_t group_align(const Group& g) {
    return std::align_val_t{g.struct_align ? static_cast<size_t>(g.struct_align)
                                           : alignof(std::max_align_t)};
}
}  // namespace

void* Binding::alloc_instance() const {
    if (!schema_ || !group_can_instantiate(*schema_)) return nullptr;
    void* p = ::operator new(schema_->struct_size, group_align(*schema_));
    group_construct(*schema_, p);
    return p;
}

void Binding::free_instance(void* p) const {
    if (!p) return;
    group_destruct(*schema_, p);
    ::operator delete(p, group_align(*schema_));
}

Binding::Binding(BindingId id, const Group& schema, void* instance, Scope scope)
    : id_(id), schema_(&schema), instance_(instance), scope_(scope) {
    env_forced_.assign(schema.field_count, 0);
    baseline_ = alloc_instance();
}

Binding::~Binding() {
    free_instance(draft_);
    free_instance(baseline_);
}

void* Binding::ensure_draft() {
    if (draft_) return draft_;
    if (!instance_ || !schema_) return nullptr;
    if (!schema_->ctx_copy_assign && !schema_->copy_assign) return nullptr;
    draft_ = alloc_instance();
    if (draft_) group_copy_assign(*schema_, draft_, instance_);
    return draft_;
}

void Binding::discard_draft() {
    free_instance(draft_);
    draft_ = nullptr;
}

void Binding::capture_baseline() {
    if (!baseline_ || !instance_) return;
    for (uint32_t i = 0; i < schema_->field_count; ++i)
        copy_field(baseline_, instance_, schema_->fields[i]);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

BindingId Registry::bind(const Group& schema, void* instance, Scope scope) {
    if (!instance || !schema.path) return kInvalidBinding;
    const BindingId id = next_id_++;
    bindings_.push_back(std::make_unique<Binding>(id, schema, instance, scope));
    return id;
}

void Registry::unbind(BindingId id) {
    for (size_t i = 0; i < bindings_.size(); ++i) {
        if (bindings_[i]->id() == id) {
            bindings_.erase(bindings_.begin() + static_cast<long>(i));
            return;
        }
    }
}

void Registry::capture_baseline(BindingId id) {
    if (Binding* b = get(id)) b->capture_baseline();
}

Binding* Registry::get(BindingId id) {
    for (auto& b : bindings_)
        if (b->id() == id) return b.get();
    return nullptr;
}

const Binding* Registry::get(BindingId id) const {
    return const_cast<Registry*>(this)->get(id);
}

Binding* Registry::find(const char* group_path) {
    if (!group_path) return nullptr;
    for (auto& b : bindings_)
        if (b->schema().path && !strcmp(b->schema().path, group_path)) return b.get();
    return nullptr;
}

const Binding* Registry::find(const char* group_path) const {
    return const_cast<Registry*>(this)->find(group_path);
}

// ---------------------------------------------------------------------------
// Typed accessors
// ---------------------------------------------------------------------------

float get_float(const void* instance, const Desc& d) { return as<float>(instance, d); }
int32_t get_int(const void* instance, const Desc& d) { return as<int32_t>(instance, d); }
uint32_t get_uint(const void* instance, const Desc& d) { return as<uint32_t>(instance, d); }
bool get_bool(const void* instance, const Desc& d) { return as<bool>(instance, d); }
int32_t get_enum(const void* instance, const Desc& d) { return as<int32_t>(instance, d); }

void get_float3(const void* instance, const Desc& d, float out[3]) {
    const float* p = static_cast<const float*>(field_ptr(instance, d));
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
}

std::string get_string(const void* instance, const Desc& d) {
    return as<std::string>(instance, d);
}

bool set_float(void* instance, const Desc& d, float v) {
    // NaN/inf are refused outright: json_doc can emit but not re-parse them,
    // so one poisoned value would corrupt the whole scope file on save.
    if (!std::isfinite(v)) return false;
    float& dst = as<float>(instance, d);
    const float nv = clampf(v, d);
    if (dst == nv) return false;
    dst = nv;
    return true;
}

bool set_int(void* instance, const Desc& d, int32_t v) {
    int32_t& dst = as<int32_t>(instance, d);
    int32_t nv = v;
    if (d.has_range) {
        const int32_t lo = static_cast<int32_t>(d.min);
        const int32_t hi = static_cast<int32_t>(d.max);
        nv = nv < lo ? lo : (nv > hi ? hi : nv);
    }
    if (dst == nv) return false;
    dst = nv;
    return true;
}

bool set_uint(void* instance, const Desc& d, uint32_t v) {
    uint32_t& dst = as<uint32_t>(instance, d);
    uint32_t nv = v;
    if (d.has_range) {
        const uint32_t lo = d.min <= 0.0f ? 0u : static_cast<uint32_t>(d.min);
        const uint32_t hi = d.max <= 0.0f ? 0u : static_cast<uint32_t>(d.max);
        nv = nv < lo ? lo : (nv > hi ? hi : nv);
    }
    if (dst == nv) return false;
    dst = nv;
    return true;
}

bool set_bool(void* instance, const Desc& d, bool v) {
    bool& dst = as<bool>(instance, d);
    if (dst == v) return false;
    dst = v;
    return true;
}

bool set_enum(void* instance, const Desc& d, int32_t v) {
    int32_t& dst = as<int32_t>(instance, d);
    int32_t nv = v;
    if (d.enum_count > 0) {
        const int32_t hi = static_cast<int32_t>(d.enum_count) - 1;
        nv = nv < 0 ? 0 : (nv > hi ? hi : nv);
    }
    if (dst == nv) return false;
    dst = nv;
    return true;
}

bool set_float3(void* instance, const Desc& d, const float v[3]) {
    // All-or-nothing, same non-finite rule as set_float.
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
        return false;
    float* dst = static_cast<float*>(field_ptr(instance, d));
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        const float nv = clampf(v[i], d);
        if (dst[i] != nv) { dst[i] = nv; changed = true; }
    }
    return changed;
}

bool set_string(void* instance, const Desc& d, const std::string& v) {
    std::string& dst = as<std::string>(instance, d);
    if (dst == v) return false;
    dst = v;
    return true;
}

float get_float(const Binding& b, const Desc& d) { return get_float(b.instance(), d); }
int32_t get_int(const Binding& b, const Desc& d) { return get_int(b.instance(), d); }
uint32_t get_uint(const Binding& b, const Desc& d) { return get_uint(b.instance(), d); }
bool get_bool(const Binding& b, const Desc& d) { return get_bool(b.instance(), d); }
int32_t get_enum(const Binding& b, const Desc& d) { return get_enum(b.instance(), d); }
void get_float3(const Binding& b, const Desc& d, float out[3]) { get_float3(b.instance(), d, out); }
std::string get_string(const Binding& b, const Desc& d) { return get_string(b.instance(), d); }

#define MATTER_PROPS_BINDING_SET(fn, argtype)                       \
    bool fn(Binding& b, const Desc& d, argtype v) {                 \
        if (!fn(b.instance(), d, v)) return false;                  \
        b.set_dirty(true);                                          \
        return true;                                                \
    }

MATTER_PROPS_BINDING_SET(set_float, float)
MATTER_PROPS_BINDING_SET(set_int, int32_t)
MATTER_PROPS_BINDING_SET(set_uint, uint32_t)
MATTER_PROPS_BINDING_SET(set_bool, bool)
MATTER_PROPS_BINDING_SET(set_enum, int32_t)
MATTER_PROPS_BINDING_SET(set_float3, const float*)
MATTER_PROPS_BINDING_SET(set_string, const std::string&)

#undef MATTER_PROPS_BINDING_SET

// ---------------------------------------------------------------------------
// Field copy / compare / reset
// ---------------------------------------------------------------------------

void copy_field(void* dst, const void* src, const Desc& d) {
    switch (d.type) {
        case Type::Float:  as<float>(dst, d) = as<float>(src, d); break;
        case Type::Int:
        case Type::Enum:   as<int32_t>(dst, d) = as<int32_t>(src, d); break;
        case Type::UInt:   as<uint32_t>(dst, d) = as<uint32_t>(src, d); break;
        case Type::Bool:   as<bool>(dst, d) = as<bool>(src, d); break;
        case Type::Float3:
        case Type::Color3: {
            float* p = static_cast<float*>(field_ptr(dst, d));
            const float* q = static_cast<const float*>(field_ptr(src, d));
            p[0] = q[0]; p[1] = q[1]; p[2] = q[2];
            break;
        }
        case Type::String: as<std::string>(dst, d) = as<std::string>(src, d); break;
    }
}

bool fields_equal(const void* a, const void* b, const Desc& d) {
    switch (d.type) {
        case Type::Float:  return as<float>(a, d) == as<float>(b, d);
        case Type::Int:
        case Type::Enum:   return as<int32_t>(a, d) == as<int32_t>(b, d);
        case Type::UInt:   return as<uint32_t>(a, d) == as<uint32_t>(b, d);
        case Type::Bool:   return as<bool>(a, d) == as<bool>(b, d);
        case Type::Float3:
        case Type::Color3: {
            const float* p = static_cast<const float*>(field_ptr(a, d));
            const float* q = static_cast<const float*>(field_ptr(b, d));
            return p[0] == q[0] && p[1] == q[1] && p[2] == q[2];
        }
        case Type::String: return as<std::string>(a, d) == as<std::string>(b, d);
    }
    return true;
}

bool is_field_default(const Binding& b, const Desc& d) {
    if (!b.baseline()) return true;
    return fields_equal(b.instance(), b.baseline(), d);
}

bool is_group_default(const Binding& b) {
    const Group& g = b.schema();
    for (uint32_t i = 0; i < g.field_count; ++i)
        if (!is_field_default(b, g.fields[i])) return false;
    return true;
}

void reset_field(Binding& b, const Desc& d) {
    if (!b.baseline()) return;
    if (fields_equal(b.instance(), b.baseline(), d)) return;
    copy_field(b.instance(), b.baseline(), d);
    b.set_dirty(true);
}

void reset_group(Binding& b) {
    const Group& g = b.schema();
    for (uint32_t i = 0; i < g.field_count; ++i) reset_field(b, g.fields[i]);
}

// ---------------------------------------------------------------------------
// RequiresReload drafts
// ---------------------------------------------------------------------------

bool group_requires_reload(const Group& g) {
    for (uint32_t i = 0; i < g.field_count; ++i)
        if (g.fields[i].flags & RequiresReload) return true;
    return false;
}

void* ensure_draft(Binding& b) { return b.ensure_draft(); }
void* draft_of(Binding& b) { return b.draft(); }
const void* draft_of(const Binding& b) { return b.draft(); }

bool has_pending_draft(const Binding& b) {
    const void* d = b.draft();
    if (!d || !b.instance()) return false;
    const Group& g = b.schema();
    for (uint32_t i = 0; i < g.field_count; ++i)
        if (!fields_equal(d, b.instance(), g.fields[i])) return true;
    return false;
}

bool apply_draft(Binding& b) {
    void* d = b.draft();
    if (!d || !b.instance()) return false;
    const bool changed = has_pending_draft(b);
    // Whole-struct assignment: undescribed members (ring lists) are part of the
    // user's intent even though only the described fields are diffed/persisted.
    group_copy_assign(b.schema(), b.instance(), d);
    b.discard_draft();
    if (changed) b.set_dirty(true);
    return changed;
}

void discard_draft(Binding& b) { b.discard_draft(); }

// ---------------------------------------------------------------------------
// Env layer
// ---------------------------------------------------------------------------

namespace {
// Desc::env_negate is applied HERE rather than inside parse_and_set, because
// parse_and_set is also the FIFO `set` path and there the written text must
// mean itself. Flipping after the shared parser has run keeps one parser and
// one polarity rule, both in one place.
void apply_env_negation(void* instance, const Desc& d) {
    if (!d.env_negate || d.type != Type::Bool) return;
    set_bool(instance, d, !get_bool(instance, d));
}
}  // namespace

void apply_env(void* instance, const Group& g) {
    if (!instance) return;
    for (uint32_t i = 0; i < g.field_count; ++i) {
        const Desc& d = g.fields[i];
        if (!d.env) continue;
        const char* raw = getenv(d.env);
        if (!raw || !*raw) continue;
        if (!parse_and_set(instance, d, raw))
            fprintf(stderr, "[props] %s: unparsable %s value \"%s\" for %s.%s — ignored\n",
                    d.env, type_name(d.type), raw, g.path ? g.path : "?",
                    d.name ? d.name : "?");
        else
            apply_env_negation(instance, d);
    }
}

void apply_env(Binding& b) {
    const Group& g = b.schema();
    for (uint32_t i = 0; i < g.field_count; ++i) {
        const Desc& d = g.fields[i];
        if (!d.env) continue;
        const char* raw = getenv(d.env);
        if (!raw || !*raw) { b.set_env_forced(i, false); continue; }
        if (!parse_and_set(b.instance(), d, raw)) {
            fprintf(stderr, "[props] %s: unparsable %s value \"%s\" for %s.%s — ignored\n",
                    d.env, type_name(d.type), raw, g.path ? g.path : "?", d.name ? d.name : "?");
            b.set_env_forced(i, false);
            continue;
        }
        apply_env_negation(b.instance(), d);
        b.set_env_forced(i, true);
    }
}

void apply_env(Registry& r) {
    for (size_t i = 0; i < r.size(); ++i) apply_env(r.at(i));
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void save_scope(const Registry& r, Scope scope, Value& doc) {
    if (doc.kind != Value::Kind::Object) {
        doc = Value();
        doc.kind = Value::Kind::Object;
    }
    const Value* existing_version = doc.find("version");
    if (!existing_version || existing_version->kind != Value::Kind::Number)
        doc.set("version", number(1));

    Value* groups = doc.find("groups");
    if (!groups || groups->kind != Value::Kind::Object) {
        Value empty;
        empty.kind = Value::Kind::Object;
        groups = &doc.set("groups", empty);
    }

    for (size_t bi = 0; bi < r.size(); ++bi) {
        const Binding& b = r.at(bi);
        if (b.scope() != scope) continue;
        const Group& g = b.schema();

        // Only materialize the group object once a field actually differs, so
        // an untouched group produces no key at all.
        Value* obj = groups->find(g.path);
        if (obj && obj->kind != Value::Kind::Object) obj = nullptr;

        for (uint32_t i = 0; i < g.field_count; ++i) {
            const Desc& d = g.fields[i];
            if (!persistable(d)) continue;
            if (b.env_forced(i)) continue;  // env is layer 5, not a user edit
            if (!fields_equal(b.instance(), b.baseline(), d)) {
                if (!field_finite(b.instance(), d)) {
                    // Keep whatever the file already holds for this field.
                    std::fprintf(stderr,
                                 "[props] %s.%s is not finite - not persisting it\n",
                                 g.path, d.name);
                    continue;
                }
                if (!obj) {
                    Value empty;
                    empty.kind = Value::Kind::Object;
                    obj = &groups->set(g.path, empty);
                }
                obj->set(d.name, encode_field(b.instance(), d));
            } else if (obj) {
                obj->erase(d.name);
            }
        }

        if (obj && obj->obj.empty()) groups->erase(g.path);
    }
}

void load_group(Binding& b, const Value& doc) {
    if (doc.kind != Value::Kind::Object) return;
    const Value* groups = doc.find("groups");
    if (!groups || groups->kind != Value::Kind::Object) return;
    const Group& g = b.schema();
    const Value* obj = groups->find(g.path);
    if (!obj || obj->kind != Value::Kind::Object) return;

    for (uint32_t i = 0; i < g.field_count; ++i) {
        const Desc& d = g.fields[i];
        if (!persistable(d)) continue;
        if (b.env_forced(i)) continue;
        const Value* v = obj->find(d.name);
        if (!v) continue;  // missing → keep current value
        if (!decode_field(b.instance(), d, *v)) warn_type(g.path, d, *v);
    }
}

void load_scope(Registry& r, Scope scope, const Value& doc) {
    for (size_t bi = 0; bi < r.size(); ++bi) {
        Binding& b = r.at(bi);
        if (b.scope() != scope) continue;
        load_group(b, doc);
    }
}

void dump_modified(const Registry& r, Value& out) {
    out = Value();
    out.kind = Value::Kind::Object;
    for (size_t bi = 0; bi < r.size(); ++bi) {
        const Binding& b = r.at(bi);
        const Group& g = b.schema();
        if (!b.instance() || !b.baseline()) continue;
        Value* obj = nullptr;
        for (uint32_t i = 0; i < g.field_count; ++i) {
            const Desc& d = g.fields[i];
            if (fields_equal(b.instance(), b.baseline(), d)) continue;
            if (!field_finite(b.instance(), d)) continue;
            if (!obj) {
                Value empty;
                empty.kind = Value::Kind::Object;
                obj = &out.set(g.path, empty);
            }
            obj->set(d.name, encode_field(b.instance(), d));
        }
    }
}

// ---------------------------------------------------------------------------
// Dynamic groups (spec S9)
//
// Value buffer layout: one uniform lane per declared field, field i at
// offset i * kValueLane. See the header for the per-type lane contents and the
// reasoning (a String lane must be able to hold a real std::string, because
// get_string/set_string/copy_field reinterpret the bytes at the offset AS one).
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kLaneAlign =
    alignof(std::string) > alignof(double) ? alignof(std::string) : alignof(double);
constexpr size_t kValueLane =
    ((sizeof(std::string) + kLaneAlign - 1) / kLaneAlign) * kLaneAlign;

// Copies `text` into `store` and returns a pointer that stays valid for the
// life of the store — a deque never relocates an element it already handed out.
// An empty string yields nullptr, which is what every optional Desc slot uses
// to mean "absent".
const char* intern(std::deque<std::string>& store, const std::string& text) {
    if (text.empty()) return nullptr;
    store.push_back(text);
    return store.back().c_str();
}

}  // namespace

void DynamicGroup::construct_thunk(void* ctx, void* p) {
    static_cast<const DynamicGroup*>(ctx)->construct_values(p);
}
void DynamicGroup::destruct_thunk(void* ctx, void* p) {
    static_cast<const DynamicGroup*>(ctx)->destruct_values(p);
}
void DynamicGroup::copy_thunk(void* ctx, void* dst, const void* src) {
    static_cast<const DynamicGroup*>(ctx)->copy_values(dst, src);
}

void DynamicGroup::construct_values(void* p) const {
    // Zero first so every lane's padding tail is deterministic; a memcmp of two
    // buffers is then meaningful even though nothing here relies on it.
    memset(p, 0, group_.struct_size);
    for (size_t i = 0; i < descs_.size(); ++i) {
        void* lane = static_cast<char*>(p) + descs_[i].offset;
        const DynamicField& f = specs_[i];
        switch (descs_[i].type) {
            case Type::Float:
                *static_cast<float*>(lane) = static_cast<float>(f.number_default);
                break;
            case Type::Int:
            case Type::Enum:
                *static_cast<int32_t*>(lane) = static_cast<int32_t>(f.number_default);
                break;
            case Type::UInt:
                *static_cast<uint32_t*>(lane) =
                    f.number_default < 0.0 ? 0u
                                           : static_cast<uint32_t>(f.number_default);
                break;
            case Type::Bool:
                *static_cast<bool*>(lane) = f.bool_default;
                break;
            case Type::Float3:
            case Type::Color3: {
                float* v = static_cast<float*>(lane);
                v[0] = f.float3_default[0];
                v[1] = f.float3_default[1];
                v[2] = f.float3_default[2];
                break;
            }
            case Type::String:
                new (lane) std::string(f.string_default);
                break;
        }
    }
}

void DynamicGroup::destruct_values(void* p) const {
    for (size_t i = 0; i < descs_.size(); ++i) {
        if (descs_[i].type != Type::String) continue;
        void* lane = static_cast<char*>(p) + descs_[i].offset;
        using StringLane = std::string;
        static_cast<std::string*>(lane)->~StringLane();
    }
}

void DynamicGroup::copy_values(void* dst, const void* src) const {
    // A dynamic group has no undescribed members, so field-by-field IS the
    // whole struct — no memcpy, which would shred the String lanes.
    for (const Desc& d : descs_) copy_field(dst, src, d);
}

DynamicGroup::~DynamicGroup() {
    if (binding_ != kInvalidBinding) {
        // The Binding holds bare pointers into this object (Desc array, every
        // Desc string, the value buffer). Salvage rather than abort: dropping
        // the binding here is strictly better than leaving it dangling, but the
        // owner got the lifetime wrong and should hear about it.
        fprintf(stderr,
                "[props] DynamicGroup '%s' destroyed while still bound "
                "(binding %u) — unbind_from() before releasing it\n",
                path_.c_str(), binding_);
        if (bound_registry_) bound_registry_->unbind(binding_);
        binding_ = kInvalidBinding;
        bound_registry_ = nullptr;
    }
    if (values_) {
        destruct_values(values_);
        ::operator delete(values_, group_align(group_));
        values_ = nullptr;
    }
}

int32_t DynamicGroup::index_of(const char* name) const {
    if (!name) return -1;
    for (size_t i = 0; i < descs_.size(); ++i)
        if (descs_[i].name && !strcmp(descs_[i].name, name))
            return static_cast<int32_t>(i);
    return -1;
}

#define MATTER_PROPS_DYN_GET(fn, type, fallback)                       \
    type DynamicGroup::fn(uint32_t index) const {                      \
        if (index >= descs_.size() || !values_) return fallback;       \
        return props::fn(values_, descs_[index]);                      \
    }

MATTER_PROPS_DYN_GET(get_float, float, 0.0f)
MATTER_PROPS_DYN_GET(get_int, int32_t, 0)
MATTER_PROPS_DYN_GET(get_uint, uint32_t, 0u)
MATTER_PROPS_DYN_GET(get_bool, bool, false)
MATTER_PROPS_DYN_GET(get_enum, int32_t, 0)
MATTER_PROPS_DYN_GET(get_string, std::string, std::string())

#undef MATTER_PROPS_DYN_GET

void DynamicGroup::get_float3(uint32_t index, float out[3]) const {
    if (index >= descs_.size() || !values_) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    props::get_float3(values_, descs_[index], out);
}

#define MATTER_PROPS_DYN_SET(fn, argtype)                              \
    bool DynamicGroup::fn(uint32_t index, argtype v) {                 \
        if (index >= descs_.size() || !values_) return false;          \
        return props::fn(values_, descs_[index], v);                   \
    }

MATTER_PROPS_DYN_SET(set_float, float)
MATTER_PROPS_DYN_SET(set_int, int32_t)
MATTER_PROPS_DYN_SET(set_uint, uint32_t)
MATTER_PROPS_DYN_SET(set_bool, bool)
MATTER_PROPS_DYN_SET(set_enum, int32_t)
MATTER_PROPS_DYN_SET(set_float3, const float*)
MATTER_PROPS_DYN_SET(set_string, const std::string&)

#undef MATTER_PROPS_DYN_SET

std::string DynamicGroup::format(uint32_t index) const {
    if (index >= descs_.size() || !values_) return {};
    return format_value(values_, descs_[index]);
}

BindingId DynamicGroup::bind_into(Registry& r, Scope scope) {
    if (binding_ != kInvalidBinding) return binding_;
    const BindingId id = r.bind(group_, values_, scope);
    if (id != kInvalidBinding) {
        binding_ = id;
        bound_registry_ = &r;
    }
    return id;
}

void DynamicGroup::unbind_from(Registry& r) {
    if (binding_ == kInvalidBinding) return;
    r.unbind(binding_);
    binding_ = kInvalidBinding;
    bound_registry_ = nullptr;
}

DynamicGroupBuilder::DynamicGroupBuilder(std::string path, std::string label)
    : path_(std::move(path)), label_(std::move(label)) {}

bool DynamicGroupBuilder::add(DynamicField field, std::string* error) {
    auto reject = [&](const std::string& why) {
        ok_ = false;
        error_ = why;
        if (error) *error = why;
        return false;
    };
    if (field.name.empty()) return reject("property name must not be empty");
    for (const DynamicField& existing : fields_)
        if (existing.name == field.name)
            return reject("duplicate property name '" + field.name + "'");
    if (field.type == Type::Enum && field.enum_labels.empty())
        return reject("enum property '" + field.name + "' declares no labels");
    fields_.push_back(std::move(field));
    return true;
}

std::unique_ptr<DynamicGroup> DynamicGroupBuilder::build() {
    if (!ok_ || path_.empty() || fields_.empty()) return nullptr;

    std::unique_ptr<DynamicGroup> g(new DynamicGroup());
    g->path_ = path_;
    g->label_ = label_.empty() ? path_ : label_;
    g->specs_ = std::move(fields_);
    fields_.clear();

    const size_t count = g->specs_.size();
    g->descs_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        DynamicField& f = g->specs_[i];
        Desc d;
        g->strings_.push_back(f.name);
        d.name = g->strings_.back().c_str();
        d.label = intern(g->strings_, f.label);
        d.doc = intern(g->strings_, f.doc);
        d.units = intern(g->strings_, f.units);
        d.type = f.type;
        d.offset = static_cast<uint32_t>(i * kValueLane);
        d.min = f.min;
        d.max = f.max;
        d.has_range = f.has_range;
        d.step = f.step;
        d.flags = f.flags;
        // Script-declared fields have no env var: a world's tunables are named
        // by the world, and MATTER_* is a fixed engine-side namespace.
        d.env = nullptr;
        if (f.type == Type::Enum) {
            g->enum_arrays_.emplace_back();
            std::vector<const char*>& labels = g->enum_arrays_.back();
            labels.reserve(f.enum_labels.size());
            for (const std::string& text : f.enum_labels) {
                g->strings_.push_back(text);
                labels.push_back(g->strings_.back().c_str());
            }
            d.enum_labels = labels.data();
            d.enum_count = static_cast<uint32_t>(labels.size());
            // Keep the declared default addressable: an out-of-range index
            // would make the combo read past the label array.
            const double hi = static_cast<double>(labels.size()) - 1.0;
            if (f.number_default < 0.0) f.number_default = 0.0;
            if (f.number_default > hi) f.number_default = hi;
        }
        g->descs_.push_back(d);
    }

    g->group_.path = g->path_.c_str();
    g->group_.label = g->label_.c_str();
    g->group_.fields = g->descs_.data();
    g->group_.field_count = static_cast<uint32_t>(count);
    g->group_.struct_size = static_cast<uint32_t>(count * kValueLane);
    g->group_.struct_align = static_cast<uint32_t>(kLaneAlign);
    g->group_.ctx = g.get();
    g->group_.ctx_construct = &DynamicGroup::construct_thunk;
    g->group_.ctx_destruct = &DynamicGroup::destruct_thunk;
    g->group_.ctx_copy_assign = &DynamicGroup::copy_thunk;

    g->values_ = ::operator new(g->group_.struct_size, group_align(g->group_));
    g->construct_values(g->values_);
    return g;
}

}  // namespace props
}  // namespace matter
