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
#include <deque>
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
    // Whole-struct assignment (dst = src). Baselines and diffs go field-by-field
    // through the schema, but a DRAFT must be a faithful copy of the instance
    // including members the schema does not describe — the streaming-LOD group
    // carries std::vector ring lists that hand-written widgets edit inside the
    // draft. Only the draft mechanism uses this.
    void      (*copy_assign)(void* dst, const void* src) = nullptr;

    // Optional context for a group whose value layout is decided at RUNTIME
    // (DynamicGroup below — a world script's `static props` block). Such a
    // group has no C++ type to instantiate a template on, so its construct /
    // destruct / copy have to be told WHICH schema they are walking. When
    // `ctx` is non-null these take precedence over the three slots above and
    // receive it as their first argument; everything else about the Group is
    // unchanged, which is what lets a dynamic group bind, baseline, draft and
    // serialize exactly like a static one.
    void* ctx = nullptr;
    void (*ctx_construct)(void* ctx, void* p) = nullptr;
    void (*ctx_destruct)(void* ctx, void* p) = nullptr;
    void (*ctx_copy_assign)(void* ctx, void* dst, const void* src) = nullptr;
};

// The four operations above, with the ctx / plain dispatch applied once.
// Everything inside props.cpp goes through these rather than touching the
// function-pointer slots directly.
bool group_can_instantiate(const Group& g);
void group_construct(const Group& g, void* p);
void group_destruct(const Group& g, void* p);
bool group_copy_assign(const Group& g, void* dst, const void* src);

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
template <class S>
void copy_assign_impl(void* dst, const void* src) {
    *static_cast<S*>(dst) = *static_cast<const S*>(src);
}

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
        g_.copy_assign = &detail::copy_assign_impl<S>;
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

    // ---- RequiresReload draft (see the free functions below) --------------
    void* draft() { return draft_; }
    const void* draft() const { return draft_; }
    // Allocates (once) a full copy of the instance and returns it. Null only
    // when the schema cannot construct/copy a struct.
    void* ensure_draft();
    void discard_draft();

private:
    BindingId id_ = kInvalidBinding;
    const Group* schema_ = nullptr;
    void* instance_ = nullptr;
    void* baseline_ = nullptr;
    void* draft_ = nullptr;
    Scope scope_ = Scope::Session;
    bool dirty_ = false;
    std::vector<uint8_t> env_forced_;

    void* alloc_instance() const;
    void free_instance(void* p) const;
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
// RequiresReload drafts (spec S6.1). A group whose fields are consumed once,
// at world connect, cannot be edited live: the edit would sit in the struct
// looking applied while nothing consumed it. Such a group is edited into a
// DRAFT — a full copy of the instance — and an explicit Apply pushes the draft
// back and triggers the reload.
//
// SEMANTICS (chosen; the alternative was persisting drafts):
//   * A draft is UI-transient. Persistence — save_scope / dump_modified /
//     baselines — reads the LIVE INSTANCE and never sees the draft.
//   * apply_draft copies draft -> instance (whole struct, via
//     Group::copy_assign) and marks the binding dirty, so the very next save
//     writes the applied values.
//   * discard_draft drops it; so does a scope reload of the group.
// Rationale: the file is a record of what the world was last CONFIGURED with.
// Writing an un-applied draft would produce a file that disagrees with the
// running world, and on the next launch those values would silently become
// "applied" without the user ever pressing Apply.
//
// Undescribed members (the streaming-LOD ring vectors) ride along in the draft
// via Group::copy_assign, so hand-written widgets can edit them in the same
// draft the schema fields live in. They are not persisted — the schema has no
// list type yet.
// ---------------------------------------------------------------------------

// True when any described field carries the RequiresReload flag.
bool group_requires_reload(const Group& g);

// Allocates the draft on first call (a copy of the instance) and returns it.
void* ensure_draft(Binding& b);
// The draft or nullptr; never allocates.
void* draft_of(Binding& b);
const void* draft_of(const Binding& b);
// A draft exists AND at least one described field differs from the instance.
bool has_pending_draft(const Binding& b);
// draft -> instance (whole struct), clear the draft, mark dirty. Returns true
// when a described field actually changed.
bool apply_draft(Binding& b);
void discard_draft(Binding& b);

// ---------------------------------------------------------------------------
// Text parsing (layer 5 and the FIFO `set` command share ONE parser).
// parse_and_set accepts, per Desc::type: a decimal/float literal for the
// numeric types; 1/0/true/false/yes/no/on/off for Bool; an enum label or an
// index for Enum; three numbers separated by any non-numeric run for
// Float3/Color3; the raw text for String. Clamps through the typed setters.
// Returns false (leaving the field untouched) when the text does not parse.
// ---------------------------------------------------------------------------
bool parse_and_set(void* instance, const Desc& d, const char* text);
bool parse_and_set(Binding& b, const Desc& d, const char* text);
// Human/FIFO-readable rendering of the current value ("0.75", "true", "high",
// "1, 2, 3").
std::string format_value(const void* instance, const Desc& d);

// Splits "<group.path>.<field>" on the LAST '.' and looks both halves up.
// Returns false (with both outputs untouched) when either half is unknown.
bool resolve_field(Registry& r, const char* full_path, Binding*& binding,
                   const Desc*& desc);

// ---------------------------------------------------------------------------
// Env layer (layer 5). Fields whose Desc carries `env` are overwritten from the
// process environment and flagged so the UI can disable them and so a later
// file load does not silently fight the override.
// ---------------------------------------------------------------------------
void apply_env(Binding& b);
void apply_env(Registry& r);
// Unbound form, for engine-side structs that have a schema but no registry
// (headless/test runs where no editor ever binds them). Idempotent.
void apply_env(void* instance, const Group& g);

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

// One binding's entry out of the same document shape. Used for groups whose
// values must reach the engine BEFORE the connect that would otherwise trigger
// the whole-scope load (the streaming-LOD overrides).
void load_group(Binding& b, const jsondoc::Value& doc);

// Every non-baseline field of EVERY scope, as { "<group.path>": {...} }.
// Unlike save_scope this is a plain diagnostic snapshot, so none of the
// persistence exclusions apply: NoSerialize live toggles, ReadOnly init-time
// values and env-forced overrides are all exactly what a bug report needs.
// Only fields equal to their baseline are omitted, and a group entirely at its
// baseline produces no key. Feeds the issue report's "props" section.
void dump_modified(const Registry& r, jsondoc::Value& out);

// File helpers. save_scope_file reads any existing file first (so unknown
// content survives), writes a temp sibling, then atomically replaces the
// target via part_asset::replace_file_atomic_detailed. When the scope has no
// non-baseline values and no file exists yet, nothing is written and the call
// still reports success.
bool save_scope_file(const Registry& r, Scope scope, const std::string& path);
bool load_scope_file(Registry& r, Scope scope, const std::string& path);
bool load_group_file(Binding& b, const std::string& path);

// ---------------------------------------------------------------------------
// Dynamic groups (spec S9 — script-defined properties)
//
// A DynamicGroup is a Group whose fields come from DATA (a world script's
// `static props` block) instead of from a C++ struct, and which owns the
// buffer those values live in. It is bindable into a Registry exactly like a
// static group: the registry, the editor's generic renderer, sparse
// persistence, drafts, the FIFO `set` path and dump_modified all go through
// Desc offsets and the typed accessors, none of which care where the schema
// came from.
//
// VALUE BUFFER LAYOUT (the "struct" the Group describes):
//   field i lives at offset i * kValueLane. One uniform lane per field keeps
//   the offset arithmetic to a single constant, and makes a String lane big
//   enough to hold a real std::string object — which is what get_string /
//   set_string / copy_field require, since they reinterpret the bytes at the
//   offset AS a std::string. Lane contents by Desc::type:
//     Float                -> float
//     Int, Enum            -> int32_t
//     UInt                 -> uint32_t
//     Bool                 -> bool
//     Float3, Color3       -> float[3]
//     String               -> std::string (placement-new'd, destroyed by
//                             ctx_destruct)
//   The unused tail of every lane is zero padding. kValueLane is
//   sizeof(std::string) rounded up to the lane alignment (32 bytes with
//   libstdc++ x86-64), so it costs a few dozen bytes per world — dynamic
//   groups hold a handful of tunables, not arrays.
//
// LIFETIME: the DynamicGroup owns the Desc array, every string the Desc
// pointers reference (names, labels, docs, units, enum label array) and the
// live value buffer. A Registry binding refers to all of that by bare pointer,
// so the binding MUST be removed before the DynamicGroup dies — use
// bind_into / unbind_from, which track it and make the destructor complain
// (stderr + abort in a debug build) rather than leave a dangling Binding.
// ---------------------------------------------------------------------------

// One declared field, in the builder's owning form.
struct DynamicField {
    std::string name;    // required, unique within the group
    std::string label;   // empty -> name
    std::string doc;
    std::string units;
    Type type = Type::Float;
    bool  has_range = false;
    float min = 0.0f, max = 0.0f, step = 0.0f;
    uint32_t flags = 0;
    // The declared default, read according to `type`.
    double      number_default = 0.0;   // Float / Int / UInt / Enum
    bool        bool_default = false;   // Bool
    std::string string_default;         // String
    float       float3_default[3] = {0.0f, 0.0f, 0.0f};  // Float3 / Color3
    std::vector<std::string> enum_labels;  // required (non-empty) for Enum
};

class DynamicGroupBuilder;

class DynamicGroup {
public:
    ~DynamicGroup();
    DynamicGroup(const DynamicGroup&) = delete;
    DynamicGroup& operator=(const DynamicGroup&) = delete;

    const Group& group() const { return group_; }
    operator const Group&() const { return group_; }
    const char* path() const { return path_.c_str(); }
    const char* label() const { return label_.c_str(); }

    uint32_t field_count() const { return static_cast<uint32_t>(descs_.size()); }
    const Desc& field(uint32_t index) const { return descs_[index]; }
    // -1 when no field carries that name.
    int32_t index_of(const char* name) const;

    // The live value buffer — what bind_into hands the Registry.
    void* instance() { return values_; }
    const void* instance() const { return values_; }

    // By-index typed access onto the live buffer, for the host (the script
    // getProp path, engine consumers). These are the free accessors above
    // applied to field(index); out-of-range indices are inert.
    float       get_float(uint32_t index) const;
    int32_t     get_int(uint32_t index) const;
    uint32_t    get_uint(uint32_t index) const;
    bool        get_bool(uint32_t index) const;
    int32_t     get_enum(uint32_t index) const;
    void        get_float3(uint32_t index, float out[3]) const;
    std::string get_string(uint32_t index) const;

    bool set_float(uint32_t index, float v);
    bool set_int(uint32_t index, int32_t v);
    bool set_uint(uint32_t index, uint32_t v);
    bool set_bool(uint32_t index, bool v);
    bool set_enum(uint32_t index, int32_t v);
    bool set_float3(uint32_t index, const float v[3]);
    bool set_string(uint32_t index, const std::string& v);

    // format_value / parse_and_set on field(index) — the same text contract
    // the FIFO `set` command uses.
    std::string format(uint32_t index) const;

    // Binding discipline: exactly one registry binding at a time, released
    // before this object dies.
    BindingId bind_into(Registry& r, Scope scope);
    void unbind_from(Registry& r);
    BindingId binding() const { return binding_; }

private:
    friend class DynamicGroupBuilder;
    DynamicGroup() = default;

    static void construct_thunk(void* ctx, void* p);
    static void destruct_thunk(void* ctx, void* p);
    static void copy_thunk(void* ctx, void* dst, const void* src);

    void construct_values(void* p) const;
    void destruct_values(void* p) const;
    void copy_values(void* dst, const void* src) const;

    std::string path_;
    std::string label_;
    // Stable storage for every const char* a Desc points at. A deque never
    // moves an element it has already handed out, unlike a vector.
    std::deque<std::string> strings_;
    // One contiguous const char* array per enum field, each pointing into
    // strings_. Built once and never resized afterwards.
    std::deque<std::vector<const char*>> enum_arrays_;
    std::vector<Desc> descs_;   // built once; Group::fields points at descs_.data()
    std::vector<DynamicField> specs_;  // declared defaults, for construct_values
    Group group_{};
    void* values_ = nullptr;
    Registry* bound_registry_ = nullptr;
    BindingId binding_ = kInvalidBinding;
};

class DynamicGroupBuilder {
public:
    DynamicGroupBuilder(std::string path, std::string label);

    // Rejects an empty or duplicate name, and an Enum without labels. `error`
    // (when non-null) receives the reason. A rejected field poisons the
    // builder: build() then returns null.
    bool add(DynamicField field, std::string* error = nullptr);

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }
    size_t field_count() const { return fields_.size(); }

    // Null when a field was rejected, when no field was added, or when the
    // path is empty. One-shot: the builder is empty afterwards.
    std::unique_ptr<DynamicGroup> build();

private:
    std::string path_;
    std::string label_;
    std::vector<DynamicField> fields_;
    std::string error_;
    bool ok_ = true;
};

}  // namespace props
}  // namespace matter
