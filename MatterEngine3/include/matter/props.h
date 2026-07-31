#pragma once

// matter::props — schema-over-existing-structs property system core.
// ImGui-free and engine-side; the editor builds its generic panel on top.
// See docs/superpowers/specs/2026-07-31-property-system-design.md.
//
// The registry is NOT on any read path: engine code keeps reading its plain
// struct members. A Group describes byte offsets into a struct; a Binding
// pairs a Group with one live instance.

#include "matter/json_doc.h"
#include "matter/math_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace matter {
namespace props {

// String fields are std::string members (not fixed char buffers): the described
// structs are ordinary engine settings structs and nothing memcpys them here.
// Every baseline capture, reset and diff in this file goes field-by-field
// through the typed accessors below, so a non-trivially-copyable member is
// safe. The cost is that a Group must be able to construct AND destroy a
// default instance — hence destruct_default alongside the spec's
// construct_default.
enum class Type : uint8_t { Float, Int, UInt, Bool, Enum, Float3, Color3, String };

enum Flags : uint32_t {
    None           = 0,
    Logarithmic    = 1u << 0,  // slider uses ImGuiSliderFlags_Logarithmic
    ReadOnly       = 1u << 1,  // displayed, never editable/persisted
    RequiresReload = 1u << 2,  // edit lands in a draft; applied on world reconnect
    NoSerialize    = 1u << 3,  // editable live, never written to disk
};

struct Desc {
    const char* name = nullptr;   // "phase_g" — JSON key and UI id
    const char* label = nullptr;  // "Phase g" — UI text (nullptr → name)
    Type        type = Type::Float;
    uint32_t    offset = 0;       // byte offset into the owning struct
    float       min = 0.0f, max = 0.0f;
    bool        has_range = false;
    float       step = 0.0f;      // 0 → widget default
    const char* doc = nullptr;    // tooltip
    const char* units = nullptr;  // "m", "EV"
    const char* const* enum_labels = nullptr;
    uint32_t    enum_count = 0;
    const char* env = nullptr;    // optional "MATTER_*" override var
    uint32_t    flags = 0;
};

struct Group {
    const char* path = nullptr;   // "render.volumetrics" — registry + JSON key
    const char* label = nullptr;  // "Volumetrics" — panel header
    const Desc* fields = nullptr;
    uint32_t    field_count = 0;
    uint32_t    struct_size = 0;
    uint32_t    struct_align = 0;
    void      (*construct_default)(void*) = nullptr;  // placement-new a default instance
    void      (*destruct_default)(void*) = nullptr;   // matching destructor call
};

// ---------------------------------------------------------------------------
// Schema builder. Keyed on pointer-to-member so type and offset are deduced
// and cannot disagree with the struct.
// ---------------------------------------------------------------------------
namespace detail {

template <class> inline constexpr bool always_false = false;

template <class M>
constexpr Type deduce_type() {
    if constexpr (std::is_enum_v<M>) {
        static_assert(sizeof(M) == 4, "enum property members must be 4 bytes");
        return Type::Enum;
    } else if constexpr (std::is_same_v<M, bool>) {
        return Type::Bool;
    } else if constexpr (std::is_same_v<M, float>) {
        return Type::Float;
    } else if constexpr (std::is_same_v<M, int32_t>) {
        return Type::Int;
    } else if constexpr (std::is_same_v<M, uint32_t>) {
        return Type::UInt;
    } else if constexpr (std::is_same_v<M, float[3]>) {
        return Type::Float3;
    } else if constexpr (std::is_same_v<M, Float3>) {
        return Type::Float3;
    } else if constexpr (std::is_same_v<M, std::string>) {
        return Type::String;
    } else {
        static_assert(always_false<M>, "unsupported property member type");
    }
}

// Offset of a pointer-to-member. Uses a real (never-constructed) storage
// object rather than the null-pointer trick so no null dereference is formed.
template <class S, class M>
uint32_t member_offset(M S::*m) {
    alignas(S) static unsigned char storage[sizeof(S)];
    const S* base = reinterpret_cast<const S*>(storage);
    return static_cast<uint32_t>(reinterpret_cast<const char*>(&(base->*m)) -
                                 reinterpret_cast<const char*>(base));
}

template <class S>
void construct_default_impl(void* p) { new (p) S(); }
template <class S>
void destruct_default_impl(void* p) { static_cast<S*>(p)->~S(); }

}  // namespace detail

class PropBuilder {
public:
    PropBuilder(const char* name, Type type, uint32_t offset) {
        d_.name = name;
        d_.type = type;
        d_.offset = offset;
    }

    PropBuilder& label(const char* v) { d_.label = v; return *this; }
    PropBuilder& range(float lo, float hi) { d_.min = lo; d_.max = hi; d_.has_range = true; return *this; }
    PropBuilder& step(float v) { d_.step = v; return *this; }
    PropBuilder& doc(const char* v) { d_.doc = v; return *this; }
    PropBuilder& units(const char* v) { d_.units = v; return *this; }
    PropBuilder& env(const char* v) { d_.env = v; return *this; }
    PropBuilder& log() { d_.flags |= Logarithmic; return *this; }
    PropBuilder& read_only() { d_.flags |= ReadOnly; return *this; }
    PropBuilder& requires_reload() { d_.flags |= RequiresReload; return *this; }
    PropBuilder& no_serialize() { d_.flags |= NoSerialize; return *this; }

    // Reinterprets an integral field as an enum index over `labels`.
    PropBuilder& enums(const char* const* labels, uint32_t count) {
        d_.enum_labels = labels;
        d_.enum_count = count;
        d_.type = Type::Enum;
        return *this;
    }

    // Color3 is a flag-like distinction on Float3 storage (spec S11.3).
    PropBuilder& color() { d_.type = Type::Color3; return *this; }

    const Desc& desc() const { return d_; }

private:
    Desc d_;
};

template <class S, class M>
PropBuilder prop(M S::*m, const char* name) {
    return PropBuilder(name, detail::deduce_type<M>(), detail::member_offset(m));
}

// Owns the Desc array a Group points into. Declare at namespace or function
// static scope:
//   static const auto s_vol = props::group<VulkanVolumetricsSettings>(
//       "render.volumetrics", "Volumetrics", prop(&V::phase_g, "phase_g")...);
//   registry.bind(s_vol, &instance, Scope::World);
// Non-copyable so `group.fields` can never be left dangling; C++17 guaranteed
// copy elision makes the static-init form above work regardless.
template <class S, size_t N>
class GroupDef {
public:
    GroupDef(const char* path, const char* label, const Desc (&in)[N]) {
        for (size_t i = 0; i < N; ++i) descs_[i] = in[i];
        g_.path = path;
        g_.label = label;
        g_.fields = descs_;
        g_.field_count = static_cast<uint32_t>(N);
        g_.struct_size = static_cast<uint32_t>(sizeof(S));
        g_.struct_align = static_cast<uint32_t>(alignof(S));
        g_.construct_default = &detail::construct_default_impl<S>;
        g_.destruct_default = &detail::destruct_default_impl<S>;
    }

    GroupDef(const GroupDef&) = delete;
    GroupDef& operator=(const GroupDef&) = delete;

    const Group& group() const { return g_; }
    operator const Group&() const { return g_; }

private:
    Desc descs_[N];
    Group g_;
};

template <class S, class... B>
GroupDef<S, sizeof...(B)> group(const char* path, const char* label, const B&... builders) {
    static_assert(sizeof...(B) > 0, "a property group needs at least one field");
    const Desc arr[] = { builders.desc()... };
    return GroupDef<S, sizeof...(B)>(path, label, arr);
}

// ---------------------------------------------------------------------------
// Registry and bindings
// ---------------------------------------------------------------------------
enum class Scope : uint8_t {
    User,     // per-machine editor prefs — gitignored, autosaved
    Project,  // projects/<name>/editor/properties.json
    World,    // projects/<name>/editor/worlds/<World>.props.json
    Session,  // live-only, never persisted
};

using BindingId = uint32_t;
inline constexpr BindingId kInvalidBinding = 0;

// A schema bound to one live instance. `baseline` is an owned, fully
// constructed instance of the same struct holding the layer-2 (post-authored)
// values; sparse save diffs against it and reset_field copies from it.
class Binding {
public:
    Binding(BindingId id, const Group& schema, void* instance, Scope scope);
    ~Binding();
    Binding(const Binding&) = delete;
    Binding& operator=(const Binding&) = delete;

    BindingId id() const { return id_; }
    const Group& schema() const { return *schema_; }
    void* instance() const { return instance_; }
    const void* baseline() const { return baseline_; }
    Scope scope() const { return scope_; }

    bool dirty() const { return dirty_; }
    void set_dirty(bool v) { dirty_ = v; }

    // Copies every described field from the instance into the baseline.
    void capture_baseline();

    bool env_forced(uint32_t field_index) const {
        return field_index < env_forced_.size() && env_forced_[field_index] != 0;
    }
    void set_env_forced(uint32_t field_index, bool v) {
        if (field_index < env_forced_.size()) env_forced_[field_index] = v ? 1 : 0;
    }

private:
    BindingId id_ = kInvalidBinding;
    const Group* schema_ = nullptr;
    void* instance_ = nullptr;
    void* baseline_ = nullptr;
    Scope scope_ = Scope::Session;
    bool dirty_ = false;
    std::vector<uint8_t> env_forced_;
};

class Registry {
public:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // instance must outlive the binding; unbind before destroying it.
    // The baseline starts as the compiled default (struct member initializers);
    // call capture_baseline once the world-JS layer has landed.
    BindingId bind(const Group& schema, void* instance, Scope scope);
    void unbind(BindingId id);
    void capture_baseline(BindingId id);

    Binding* get(BindingId id);
    const Binding* get(BindingId id) const;
    Binding* find(const char* group_path);
    const Binding* find(const char* group_path) const;

    // Enumeration for panels / persistence, in bind order.
    size_t size() const { return bindings_.size(); }
    Binding& at(size_t index) { return *bindings_[index]; }
    const Binding& at(size_t index) const { return *bindings_[index]; }

private:
    std::vector<std::unique_ptr<Binding>> bindings_;
    BindingId next_id_ = 1;
};

// ---------------------------------------------------------------------------
// Typed accessors. The raw (void* instance) forms are the primitives; the
// Binding forms additionally mark the binding dirty on a real change.
// Every set_* clamps to [min,max] when desc.has_range, and returns true only
// when the stored value actually changed.
// ---------------------------------------------------------------------------
float       get_float(const void* instance, const Desc& d);
int32_t     get_int(const void* instance, const Desc& d);
uint32_t    get_uint(const void* instance, const Desc& d);
bool        get_bool(const void* instance, const Desc& d);
int32_t     get_enum(const void* instance, const Desc& d);
void        get_float3(const void* instance, const Desc& d, float out[3]);
std::string get_string(const void* instance, const Desc& d);

bool set_float(void* instance, const Desc& d, float v);
bool set_int(void* instance, const Desc& d, int32_t v);
bool set_uint(void* instance, const Desc& d, uint32_t v);
bool set_bool(void* instance, const Desc& d, bool v);
bool set_enum(void* instance, const Desc& d, int32_t v);
bool set_float3(void* instance, const Desc& d, const float v[3]);
bool set_string(void* instance, const Desc& d, const std::string& v);

float       get_float(const Binding& b, const Desc& d);
int32_t     get_int(const Binding& b, const Desc& d);
uint32_t    get_uint(const Binding& b, const Desc& d);
bool        get_bool(const Binding& b, const Desc& d);
int32_t     get_enum(const Binding& b, const Desc& d);
void        get_float3(const Binding& b, const Desc& d, float out[3]);
std::string get_string(const Binding& b, const Desc& d);

bool set_float(Binding& b, const Desc& d, float v);
bool set_int(Binding& b, const Desc& d, int32_t v);
bool set_uint(Binding& b, const Desc& d, uint32_t v);
bool set_bool(Binding& b, const Desc& d, bool v);
bool set_enum(Binding& b, const Desc& d, int32_t v);
bool set_float3(Binding& b, const Desc& d, const float v[3]);
bool set_string(Binding& b, const Desc& d, const std::string& v);

// Copies one described field between two instances of the same struct.
void copy_field(void* dst, const void* src, const Desc& d);
bool fields_equal(const void* a, const void* b, const Desc& d);

bool is_field_default(const Binding& b, const Desc& d);
bool is_group_default(const Binding& b);
void reset_field(Binding& b, const Desc& d);
void reset_group(Binding& b);

// ---------------------------------------------------------------------------
// Env layer (layer 5). Fields whose Desc carries `env` are overwritten from the
// process environment and flagged so the UI can disable them and so a later
// file load does not silently fight the override.
// ---------------------------------------------------------------------------
void apply_env(Binding& b);
void apply_env(Registry& r);

// ---------------------------------------------------------------------------
// Persistence (spec S5): { "version": 1, "groups": { "<path>": {...} } }
//
// save_scope is sparse — only fields differing from the baseline are written,
// and a field edited back to its baseline is erased from the document. Unknown
// groups and unknown keys inside known groups are preserved untouched.
// ReadOnly / NoSerialize / env-forced fields are neither written nor erased.
//
// load_scope is tolerant — a missing group or field keeps the current value,
// and a wrong-typed value is skipped with a warning on stderr.
// ---------------------------------------------------------------------------
void save_scope(const Registry& r, Scope scope, jsondoc::Value& doc);
void load_scope(Registry& r, Scope scope, const jsondoc::Value& doc);

// File helpers. save_scope_file reads any existing file first (so unknown
// content survives), writes a temp sibling, then atomically replaces the
// target via part_asset::replace_file_atomic_detailed. When the scope has no
// non-baseline values and no file exists yet, nothing is written and the call
// still reports success.
bool save_scope_file(const Registry& r, Scope scope, const std::string& path);
bool load_scope_file(Registry& r, Scope scope, const std::string& path);

}  // namespace props
}  // namespace matter
