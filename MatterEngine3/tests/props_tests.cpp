// props_tests.cpp — headless unit tests for matter::props + matter::jsondoc.
// No raylib / GL / Vulkan: links only props.cpp + json_doc.cpp.

#include "check.h"

#include "matter/json_doc.h"
#include "matter/props.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace matter;
using props::Binding;
using props::BindingId;
using props::Desc;
using props::Registry;
using props::Scope;
using props::Type;

namespace {

const char* const kModeLabels[] = {"off", "low", "high"};

struct Tunables {
    float          density  = 0.25f;
    int32_t        steps    = 30;
    uint32_t       budget   = 1024u;
    bool           enabled  = true;
    int32_t        mode     = 1;
    float          color[3] = {0.9f, 0.92f, 0.95f};
    Float3         wind{1.0f, 2.0f, 3.0f};
    std::string    title    = "default";
    float          hidden   = 7.0f;  // NoSerialize
};

const auto& tunables_schema() {
    static const auto def = props::group<Tunables>(
        "test.tunables", "Tunables",
        props::prop(&Tunables::density, "density")
            .label("Density").range(0.0f, 1.0f).doc("test density").units("m")
            .env("MATTER_PROPS_TEST_DENSITY"),
        props::prop(&Tunables::steps, "steps").range(1.0f, 64.0f)
            .env("MATTER_PROPS_TEST_STEPS"),
        props::prop(&Tunables::budget, "budget"),
        props::prop(&Tunables::enabled, "enabled").env("MATTER_PROPS_TEST_ENABLED"),
        props::prop(&Tunables::mode, "mode").enums(kModeLabels, 3),
        props::prop(&Tunables::color, "color").color(),
        props::prop(&Tunables::wind, "wind").env("MATTER_PROPS_TEST_WIND"),
        props::prop(&Tunables::title, "title"),
        props::prop(&Tunables::hidden, "hidden").no_serialize());
    return def;
}

const Desc& field(const char* name) {
    const props::Group& g = tunables_schema().group();
    for (uint32_t i = 0; i < g.field_count; ++i)
        if (!strcmp(g.fields[i].name, name)) return g.fields[i];
    static Desc missing;  // unreachable in a green run
    return missing;
}

uint32_t offset_of(const Tunables& t, const void* member) {
    return static_cast<uint32_t>(reinterpret_cast<const char*>(member) -
                                 reinterpret_cast<const char*>(&t));
}

void set_env(const char* key, const char* value) {
    const std::string kv = std::string(key) + "=" + value;
#ifdef _WIN32
    _putenv(kv.c_str());
#else
    setenv(key, value, 1);
#endif
}

std::string doc_to_string(const jsondoc::Value& v) { return jsondoc::write_json(v); }

jsondoc::Value doc_from_string(const char* text) {
    jsondoc::Value v;
    jsondoc::parse_json(std::string(text), v);
    return v;
}

// ---------------------------------------------------------------------------

void test_builder_layout() {
    const props::Group& g = tunables_schema().group();
    CHECK(g.field_count == 9, "builder: field count");
    CHECK(!strcmp(g.path, "test.tunables"), "builder: group path");
    CHECK(!strcmp(g.label, "Tunables"), "builder: group label");
    CHECK(g.struct_size == sizeof(Tunables), "builder: struct size");

    CHECK(field("density").type == Type::Float, "builder: float type");
    CHECK(field("steps").type == Type::Int, "builder: int type");
    CHECK(field("budget").type == Type::UInt, "builder: uint type");
    CHECK(field("enabled").type == Type::Bool, "builder: bool type");
    CHECK(field("mode").type == Type::Enum, "builder: enum type");
    CHECK(field("color").type == Type::Color3, "builder: color3 type");
    CHECK(field("wind").type == Type::Float3, "builder: float3 type");
    CHECK(field("title").type == Type::String, "builder: string type");

    Tunables t;
    CHECK(field("density").offset == offset_of(t, &t.density), "builder: density offset");
    CHECK(field("steps").offset == offset_of(t, &t.steps), "builder: steps offset");
    CHECK(field("budget").offset == offset_of(t, &t.budget), "builder: budget offset");
    CHECK(field("enabled").offset == offset_of(t, &t.enabled), "builder: enabled offset");
    CHECK(field("mode").offset == offset_of(t, &t.mode), "builder: mode offset");
    CHECK(field("color").offset == offset_of(t, &t.color), "builder: color offset");
    CHECK(field("wind").offset == offset_of(t, &t.wind), "builder: wind offset");
    CHECK(field("title").offset == offset_of(t, &t.title), "builder: title offset");

    CHECK(field("density").has_range && field("density").min == 0.0f &&
          field("density").max == 1.0f, "builder: range");
    CHECK(!strcmp(field("density").label, "Density"), "builder: label chainer");
    CHECK(!strcmp(field("density").doc, "test density"), "builder: doc chainer");
    CHECK(!strcmp(field("density").units, "m"), "builder: units chainer");
    CHECK(!strcmp(field("density").env, "MATTER_PROPS_TEST_DENSITY"), "builder: env chainer");
    CHECK(field("mode").enum_count == 3 && field("mode").enum_labels == kModeLabels,
          "builder: enum labels");
    CHECK((field("hidden").flags & props::NoSerialize) != 0, "builder: no_serialize flag");
}

void test_defaults_and_reset() {
    Registry reg;
    Tunables inst;
    const BindingId id = reg.bind(tunables_schema(), &inst, Scope::World);
    CHECK(id != props::kInvalidBinding, "bind: returns an id");
    Binding* b = reg.get(id);
    CHECK(b != nullptr, "bind: get by id");
    CHECK(reg.find("test.tunables") == b, "bind: find by path");
    CHECK(reg.find("nope") == nullptr, "bind: find unknown path");

    // Baseline starts at the compiled defaults (member initializers).
    CHECK(props::is_group_default(*b), "defaults: fresh binding is all-default");
    CHECK(props::get_float(*b, field("density")) == 0.25f, "defaults: density");
    CHECK(props::get_int(*b, field("steps")) == 30, "defaults: steps");
    CHECK(props::get_uint(*b, field("budget")) == 1024u, "defaults: budget");
    CHECK(props::get_bool(*b, field("enabled")), "defaults: enabled");
    CHECK(props::get_string(*b, field("title")) == "default", "defaults: title");
    float xyz[3];
    props::get_float3(*b, field("wind"), xyz);
    CHECK(xyz[0] == 1.0f && xyz[1] == 2.0f && xyz[2] == 3.0f, "defaults: wind");

    CHECK(!b->dirty(), "dirty: clean after bind");
    CHECK(props::set_float(*b, field("density"), 0.5f), "set: reports change");
    CHECK(b->dirty(), "dirty: set marks dirty");
    CHECK(!props::set_float(*b, field("density"), 0.5f), "set: no-op reports no change");
    CHECK(!props::is_field_default(*b, field("density")), "is_field_default: edited field");
    CHECK(props::is_field_default(*b, field("steps")), "is_field_default: untouched field");
    CHECK(!props::is_group_default(*b), "is_group_default: after an edit");

    props::reset_field(*b, field("density"));
    CHECK(props::get_float(*b, field("density")) == 0.25f, "reset_field: back to baseline");
    CHECK(props::is_group_default(*b), "reset_field: group default again");

    // capture_baseline promotes the current values (the "world JS authored"
    // layer) to the reset target.
    inst.density = 0.4f;
    inst.title = "authored";
    reg.capture_baseline(id);
    CHECK(props::is_group_default(*b), "capture_baseline: current values become the baseline");
    props::set_float(*b, field("density"), 0.9f);
    props::set_string(*b, field("title"), "edited");
    props::reset_group(*b);
    CHECK(props::get_float(*b, field("density")) == 0.4f, "capture_baseline: reset to captured float");
    CHECK(props::get_string(*b, field("title")) == "authored",
          "capture_baseline: reset to captured string");

    reg.unbind(id);
    CHECK(reg.size() == 0, "unbind: removes the binding");
    CHECK(reg.find("test.tunables") == nullptr, "unbind: no longer findable");
}

void test_range_clamping() {
    Registry reg;
    Tunables inst;
    Binding* b = reg.get(reg.bind(tunables_schema(), &inst, Scope::World));

    props::set_float(*b, field("density"), 5.0f);
    CHECK(inst.density == 1.0f, "clamp: float above max");
    props::set_float(*b, field("density"), -2.0f);
    CHECK(inst.density == 0.0f, "clamp: float below min");

    props::set_int(*b, field("steps"), 999);
    CHECK(inst.steps == 64, "clamp: int above max");
    props::set_int(*b, field("steps"), -5);
    CHECK(inst.steps == 1, "clamp: int below min");

    props::set_enum(*b, field("mode"), 99);
    CHECK(inst.mode == 2, "clamp: enum above label count");
    props::set_enum(*b, field("mode"), -3);
    CHECK(inst.mode == 0, "clamp: enum below zero");

    // Unranged fields are untouched by clamping.
    props::set_uint(*b, field("budget"), 999999u);
    CHECK(inst.budget == 999999u, "clamp: unranged uint passes through");
}

void test_sparse_save() {
    Registry reg;
    Tunables inst;
    Binding* b = reg.get(reg.bind(tunables_schema(), &inst, Scope::World));

    jsondoc::Value doc;
    props::save_scope(reg, Scope::World, doc);
    CHECK(doc_to_string(doc) == "{\"version\":1,\"groups\":{}}",
          "sparse save: no edits produces no group");

    props::set_float(*b, field("density"), 0.5f);
    props::save_scope(reg, Scope::World, doc);
    CHECK(doc_to_string(doc) ==
              "{\"version\":1,\"groups\":{\"test.tunables\":{\"density\":0.5}}}",
          "sparse save: only the edited field is written");

    // A field edited back to its baseline drops out again.
    props::set_float(*b, field("density"), 0.25f);
    props::save_scope(reg, Scope::World, doc);
    CHECK(doc_to_string(doc) == "{\"version\":1,\"groups\":{}}",
          "sparse save: field edited back to baseline drops out");

    // NoSerialize is never written even when it differs.
    props::set_float(*b, field("hidden"), 99.0f);
    props::save_scope(reg, Scope::World, doc);
    CHECK(doc_to_string(doc) == "{\"version\":1,\"groups\":{}}",
          "sparse save: NoSerialize field is not written");

    // Other scopes are untouched by a World save.
    Tunables other;
    Registry reg2;
    Binding* b2 = reg2.get(reg2.bind(tunables_schema(), &other, Scope::User));
    props::set_int(*b2, field("steps"), 7);
    jsondoc::Value world_doc;
    props::save_scope(reg2, Scope::World, world_doc);
    CHECK(doc_to_string(world_doc) == "{\"version\":1,\"groups\":{}}",
          "sparse save: scope filter excludes other scopes");
}

void test_round_trip() {
    Registry src_reg;
    Tunables src;
    Binding* sb = src_reg.get(src_reg.bind(tunables_schema(), &src, Scope::World));

    props::set_float(*sb, field("density"), 0.5f);
    props::set_int(*sb, field("steps"), 12);
    props::set_uint(*sb, field("budget"), 77u);
    props::set_bool(*sb, field("enabled"), false);
    props::set_enum(*sb, field("mode"), 2);
    const float col[3] = {0.1f, 0.25f, 0.5f};
    props::set_float3(*sb, field("color"), col);
    const float wind[3] = {4.0f, 5.0f, 6.5f};
    props::set_float3(*sb, field("wind"), wind);
    props::set_string(*sb, field("title"), "hello \"world\"");

    jsondoc::Value doc;
    props::save_scope(src_reg, Scope::World, doc);

    // Reparse the serialized bytes so the test exercises the writer too.
    jsondoc::Value reparsed;
    CHECK(jsondoc::parse_json(doc_to_string(doc), reparsed), "round trip: reparse saved bytes");

    Registry dst_reg;
    Tunables dst;
    Binding* db = dst_reg.get(dst_reg.bind(tunables_schema(), &dst, Scope::World));
    props::load_scope(dst_reg, Scope::World, reparsed);

    CHECK(dst.density == 0.5f, "round trip: float");
    CHECK(dst.steps == 12, "round trip: int");
    CHECK(dst.budget == 77u, "round trip: uint");
    CHECK(dst.enabled == false, "round trip: bool");
    CHECK(dst.mode == 2, "round trip: enum");
    CHECK(dst.color[0] == 0.1f && dst.color[1] == 0.25f && dst.color[2] == 0.5f,
          "round trip: color3");
    CHECK(dst.wind.x == 4.0f && dst.wind.y == 5.0f && dst.wind.z == 6.5f,
          "round trip: float3");
    CHECK(dst.title == "hello \"world\"", "round trip: string with escapes");

    // Everything came back, so a re-save of the loaded instance against the
    // same (compiled-default) baseline reproduces the same document.
    jsondoc::Value redoc;
    props::save_scope(dst_reg, Scope::World, redoc);
    CHECK(doc_to_string(redoc) == doc_to_string(doc), "round trip: re-save is stable");
    (void)db;
}

void test_tolerant_load() {
    Registry reg;
    Tunables inst;
    Binding* b = reg.get(reg.bind(tunables_schema(), &inst, Scope::World));

    // steps missing entirely, budget wrong type, density fine.
    const jsondoc::Value doc = doc_from_string(
        "{\"version\":1,\"groups\":{\"test.tunables\":"
        "{\"density\":0.75,\"budget\":\"not-a-number\",\"enabled\":3}}}");
    props::load_scope(reg, Scope::World, doc);

    CHECK(inst.density == 0.75f, "tolerant load: valid field applied");
    CHECK(inst.steps == 30, "tolerant load: missing field keeps current value");
    CHECK(inst.budget == 1024u, "tolerant load: wrong-typed field skipped");
    CHECK(inst.enabled == true, "tolerant load: number-for-bool skipped");

    // Unknown group, non-object group, and a garbage document are all no-ops.
    props::load_scope(reg, Scope::World, doc_from_string("{\"version\":1,\"groups\":{}}"));
    props::load_scope(reg, Scope::World, doc_from_string("{\"groups\":[1,2,3]}"));
    props::load_scope(reg, Scope::World, doc_from_string("[]"));
    CHECK(inst.density == 0.75f, "tolerant load: malformed documents are no-ops");
    (void)b;
}

void test_unknown_content_preserved() {
    Registry reg;
    Tunables inst;
    Binding* b = reg.get(reg.bind(tunables_schema(), &inst, Scope::World));

    const char* kSrc =
        "{\"version\":1,\"groups\":{"
        "\"other.unknown\":{\"x\":1,\"y\":\"keep\"},"
        "\"test.tunables\":{\"density\":0.5,\"future_field\":7}}}";
    jsondoc::Value doc = doc_from_string(kSrc);
    props::load_scope(reg, Scope::World, doc);
    CHECK(inst.density == 0.5f, "preserve: known field loaded");

    props::save_scope(reg, Scope::World, doc);
    CHECK(doc_to_string(doc) == std::string(kSrc),
          "preserve: unknown group and unknown key survive a save byte-wise");
    (void)b;
}

void test_env_layer() {
    set_env("MATTER_PROPS_TEST_DENSITY", "0.75");
    set_env("MATTER_PROPS_TEST_STEPS", "48");
    set_env("MATTER_PROPS_TEST_ENABLED", "false");
    set_env("MATTER_PROPS_TEST_WIND", "9,8,7");

    Registry reg;
    Tunables inst;
    Binding* b = reg.get(reg.bind(tunables_schema(), &inst, Scope::World));
    props::apply_env(reg);

    CHECK(inst.density == 0.75f, "env: float parsed");
    CHECK(inst.steps == 48, "env: int parsed");
    CHECK(inst.enabled == false, "env: bool parsed");
    CHECK(inst.wind.x == 9.0f && inst.wind.y == 8.0f && inst.wind.z == 7.0f,
          "env: float3 parsed");
    CHECK(b->env_forced(0), "env: density flagged as env-forced");
    CHECK(!b->env_forced(2), "env: budget has no env var");

    // Env is layer 5: a file load must not fight it, and a save must not
    // persist the env value.
    props::load_scope(reg, Scope::World, doc_from_string(
        "{\"version\":1,\"groups\":{\"test.tunables\":{\"density\":0.1,\"budget\":42}}}"));
    CHECK(inst.density == 0.75f, "env: load does not overwrite an env-forced field");
    CHECK(inst.budget == 42u, "env: load still applies non-env fields");

    jsondoc::Value doc;
    props::save_scope(reg, Scope::World, doc);
    CHECK(doc_to_string(doc) ==
              "{\"version\":1,\"groups\":{\"test.tunables\":{\"budget\":42}}}",
          "env: env-forced field is not persisted");

    // Env values are clamped like any other write.
    set_env("MATTER_PROPS_TEST_DENSITY", "42");
    props::apply_env(*b);
    CHECK(inst.density == 1.0f, "env: value clamped to range");

    set_env("MATTER_PROPS_TEST_DENSITY", "");
    set_env("MATTER_PROPS_TEST_STEPS", "");
    set_env("MATTER_PROPS_TEST_ENABLED", "");
    set_env("MATTER_PROPS_TEST_WIND", "");
    props::apply_env(*b);
    CHECK(!b->env_forced(0), "env: unset var clears the env-forced flag");
}

void test_json_doc() {
    // Key order is insertion order and integral numbers print without ".0".
    const char* kSrc =
        "{\"b\":1,\"a\":2.5,\"z\":{\"n\":3},\"arr\":[1,2,\"x\"],\"t\":true,"
        "\"f\":false,\"nul\":null,\"neg\":-4}";
    jsondoc::Value v;
    CHECK(jsondoc::parse_json(std::string(kSrc), v), "json: parse");
    CHECK(v.kind == jsondoc::Value::Kind::Object, "json: root is object");
    CHECK(v.obj.size() == 8, "json: key count");
    CHECK(v.obj[0].first == "b" && v.obj[1].first == "a" && v.obj[2].first == "z",
          "json: insertion order preserved");
    CHECK(jsondoc::write_json(v) == std::string(kSrc), "json: write round-trips byte-wise");

    // Escapes.
    const char* kEsc = "{\"s\":\"a\\\"b\\\\c\\nd\\te\"}";
    jsondoc::Value e;
    CHECK(jsondoc::parse_json(std::string(kEsc), e), "json: parse escapes");
    CHECK(e.find("s") != nullptr && e.find("s")->str == "a\"b\\c\nd\te",
          "json: escapes decoded");
    CHECK(jsondoc::write_json(e) == std::string(kEsc), "json: escapes re-encoded");

    // find / set / erase.
    jsondoc::Value o;
    o.kind = jsondoc::Value::Kind::Object;
    jsondoc::Value n;
    n.kind = jsondoc::Value::Kind::Number;
    n.num = 1;
    o.set("first", n);
    n.num = 2;
    o.set("second", n);
    n.num = 9;
    o.set("first", n);  // overwrite in place
    CHECK(jsondoc::write_json(o) == "{\"first\":9,\"second\":2}",
          "json: set overwrites in place, preserving position");
    CHECK(o.erase("first") && !o.erase("first"), "json: erase");
    CHECK(jsondoc::write_json(o) == "{\"second\":2}", "json: after erase");

    // A large integral value still prints as an integer; a fractional one uses %.9g.
    jsondoc::Value big;
    big.kind = jsondoc::Value::Kind::Number;
    big.num = 1234567.0;
    CHECK(jsondoc::write_json(big) == "1234567", "json: large integral has no .0");
    big.num = 0.125;
    CHECK(jsondoc::write_json(big) == "0.125", "json: fractional");

    // Trailing garbage after a complete value is tolerated.
    jsondoc::Value tail;
    CHECK(jsondoc::parse_json(std::string("{\"a\":1} trailing"), tail),
          "json: trailing garbage tolerated");
}

// ---------------------------------------------------------------------------
// Stage 3
// ---------------------------------------------------------------------------

// A RequiresReload group over a struct with an UNDESCRIBED member, so the
// draft's whole-struct copy (Group::copy_assign) is actually exercised: the
// streaming-LOD group carries ring vectors exactly this way.
struct Reloadable {
    bool             enabled = true;
    int32_t          rungs = 3;
    std::vector<int> undescribed{1, 2};
};

const auto& reloadable_schema() {
    static const auto def = props::group<Reloadable>(
        "test.reload", "Reloadable",
        props::prop(&Reloadable::enabled, "enabled").requires_reload(),
        props::prop(&Reloadable::rungs, "rungs").range(0.0f, 8.0f).requires_reload());
    return def;
}

const Desc& reload_field(const char* name) {
    const props::Group& g = reloadable_schema().group();
    for (uint32_t i = 0; i < g.field_count; ++i)
        if (!strcmp(g.fields[i].name, name)) return g.fields[i];
    static Desc missing;
    return missing;
}

void test_draft_lifecycle() {
    CHECK(props::group_requires_reload(reloadable_schema().group()),
          "draft: group_requires_reload");
    CHECK(!props::group_requires_reload(tunables_schema().group()),
          "draft: a live group does not require reload");

    Registry reg;
    Reloadable inst;
    Binding* b = reg.get(reg.bind(reloadable_schema(), &inst, Scope::World));
    CHECK(!props::has_pending_draft(*b), "draft: none before ensure_draft");
    CHECK(props::draft_of(*b) == nullptr, "draft: draft_of null before ensure");

    Reloadable* draft = static_cast<Reloadable*>(props::ensure_draft(*b));
    CHECK(draft != nullptr && draft != &inst, "draft: allocated separately");
    CHECK(props::ensure_draft(*b) == draft, "draft: ensure_draft is idempotent");
    CHECK(draft->undescribed == inst.undescribed,
          "draft: undescribed members ride along via copy_assign");
    CHECK(!props::has_pending_draft(*b),
          "draft: a fresh copy is not pending");

    // Edits land in the draft only.
    props::set_int(draft, reload_field("rungs"), 6);
    props::set_bool(draft, reload_field("enabled"), false);
    draft->undescribed.push_back(3);
    CHECK(props::has_pending_draft(*b), "draft: pending after an edit");
    CHECK(inst.rungs == 3 && inst.enabled, "draft: instance untouched while pending");
    CHECK(!b->dirty(), "draft: a draft edit does not dirty the binding");

    // Persistence reads the LIVE instance, never the draft.
    jsondoc::Value doc;
    props::save_scope(reg, Scope::World, doc);
    CHECK(doc_to_string(doc) == "{\"version\":1,\"groups\":{}}",
          "draft: a pending draft is not persisted");

    CHECK(props::apply_draft(*b), "draft: apply reports a change");
    CHECK(inst.rungs == 6 && !inst.enabled, "draft: apply copies into the instance");
    CHECK(inst.undescribed.size() == 3,
          "draft: apply copies undescribed members too");
    CHECK(b->dirty(), "draft: apply marks the binding dirty");
    CHECK(props::draft_of(*b) == nullptr, "draft: apply clears the draft");

    jsondoc::Value after;
    props::save_scope(reg, Scope::World, after);
    CHECK(doc_to_string(after) ==
              "{\"version\":1,\"groups\":{\"test.reload\":{\"enabled\":false,\"rungs\":6}}}",
          "draft: applied values persist");

    // Discard drops an edit without touching the instance.
    Reloadable* second = static_cast<Reloadable*>(props::ensure_draft(*b));
    props::set_int(second, reload_field("rungs"), 1);
    CHECK(props::has_pending_draft(*b), "draft: second draft pending");
    props::discard_draft(*b);
    CHECK(props::draft_of(*b) == nullptr, "draft: discard clears it");
    CHECK(inst.rungs == 6, "draft: discard leaves the instance alone");

    // Applying an untouched draft is a no-op, not a spurious dirty.
    b->set_dirty(false);
    props::ensure_draft(*b);
    CHECK(!props::apply_draft(*b), "draft: applying an unchanged draft reports false");
    CHECK(!b->dirty(), "draft: an unchanged apply does not dirty");
}

void test_parse_and_set() {
    Tunables t;
    CHECK(props::parse_and_set(&t, field("density"), "0.5") && t.density == 0.5f,
          "parse: float");
    CHECK(props::parse_and_set(&t, field("density"), "9") && t.density == 1.0f,
          "parse: float clamped to range");
    CHECK(props::parse_and_set(&t, field("steps"), "12") && t.steps == 12,
          "parse: int");
    CHECK(props::parse_and_set(&t, field("budget"), "77") && t.budget == 77u,
          "parse: uint");
    CHECK(props::parse_and_set(&t, field("budget"), "-3") && t.budget == 0u,
          "parse: negative uint floors at 0");
    CHECK(props::parse_and_set(&t, field("enabled"), "off") && !t.enabled,
          "parse: bool word");
    CHECK(props::parse_and_set(&t, field("enabled"), "1") && t.enabled,
          "parse: bool digit");
    CHECK(props::parse_and_set(&t, field("mode"), "high") && t.mode == 2,
          "parse: enum by label");
    CHECK(props::parse_and_set(&t, field("mode"), "0") && t.mode == 0,
          "parse: enum by index");
    CHECK(props::parse_and_set(&t, field("wind"), "4 5 6") && t.wind.x == 4.0f &&
              t.wind.y == 5.0f && t.wind.z == 6.0f,
          "parse: float3 space separated");
    CHECK(props::parse_and_set(&t, field("color"), "0.1,0.2,0.3"),
          "parse: color3 comma separated");
    CHECK(props::parse_and_set(&t, field("title"), "hello world") &&
              t.title == "hello world",
          "parse: string takes the raw text");

    // Bad input leaves the field alone.
    const float density = t.density;
    CHECK(!props::parse_and_set(&t, field("density"), "abc") && t.density == density,
          "parse: unparsable float rejected");
    CHECK(!props::parse_and_set(&t, field("steps"), "") && t.steps == 12,
          "parse: empty int rejected");
    CHECK(!props::parse_and_set(&t, field("enabled"), "maybe") && t.enabled,
          "parse: unparsable bool rejected");
    CHECK(!props::parse_and_set(&t, field("wind"), "1,2"),
          "parse: short float3 rejected");
    CHECK(!props::parse_and_set(&t, field("density"), nullptr),
          "parse: null text rejected");

    // The Binding form marks dirty only on a successful parse.
    Registry reg;
    Binding* b = reg.get(reg.bind(tunables_schema(), &t, Scope::User));
    b->set_dirty(false);
    CHECK(!props::parse_and_set(*b, field("steps"), "nope") && !b->dirty(),
          "parse: a failed set does not dirty the binding");
    CHECK(props::parse_and_set(*b, field("steps"), "5") && b->dirty(),
          "parse: a successful set dirties the binding");

    CHECK(props::format_value(&t, field("steps")) == "5", "format: int");
    CHECK(props::format_value(&t, field("enabled")) == "true", "format: bool");
    CHECK(props::format_value(&t, field("mode")) == "off", "format: enum label");
    CHECK(props::format_value(&t, field("title")) == "hello world", "format: string");
    CHECK(props::format_value(&t, field("wind")) == "4, 5, 6", "format: float3");
}

void test_resolve_field() {
    Registry reg;
    Tunables t;
    reg.bind(tunables_schema(), &t, Scope::User);

    Binding* b = nullptr;
    const Desc* d = nullptr;
    CHECK(props::resolve_field(reg, "test.tunables.density", b, d) && b && d &&
              !strcmp(d->name, "density"),
          "resolve: splits on the LAST dot");
    CHECK(!props::resolve_field(reg, "test.tunables.nope", b, d),
          "resolve: unknown field");
    CHECK(!props::resolve_field(reg, "no.such.group.density", b, d),
          "resolve: unknown group");
    CHECK(!props::resolve_field(reg, "density", b, d), "resolve: no dot at all");
    CHECK(!props::resolve_field(reg, "test.tunables.", b, d),
          "resolve: trailing dot");
    CHECK(!props::resolve_field(reg, nullptr, b, d), "resolve: null path");
}

void test_dump_modified() {
    Registry reg;
    Tunables t;
    Reloadable r;
    Binding* tb = reg.get(reg.bind(tunables_schema(), &t, Scope::World));
    reg.bind(reloadable_schema(), &r, Scope::User);

    jsondoc::Value empty;
    props::dump_modified(reg, empty);
    CHECK(doc_to_string(empty) == "{}", "dump: nothing modified -> empty object");

    props::set_float(*tb, field("density"), 0.5f);
    // A diagnostic snapshot keeps what persistence drops: NoSerialize fields.
    props::set_float(&t, field("hidden"), 1.0f);
    props::set_int(&r, reload_field("rungs"), 7);

    jsondoc::Value doc;
    props::dump_modified(reg, doc);
    // Flat "<group.path>": {...}, every scope, no "version"/"groups" wrapper.
    CHECK(doc_to_string(doc) ==
              "{\"test.tunables\":{\"density\":0.5,\"hidden\":1},"
              "\"test.reload\":{\"rungs\":7}}",
          "dump: shape is group path -> changed fields, across scopes");
}

void test_load_group() {
    Registry reg;
    Tunables t;
    Reloadable r;
    reg.bind(tunables_schema(), &t, Scope::World);
    Binding* rb = reg.get(reg.bind(reloadable_schema(), &r, Scope::World));

    const jsondoc::Value doc = doc_from_string(
        "{\"version\":1,\"groups\":{\"test.tunables\":{\"steps\":9},"
        "\"test.reload\":{\"rungs\":5}}}");
    props::load_group(*rb, doc);
    CHECK(r.rungs == 5, "load_group: the named group is applied");
    CHECK(t.steps == 30, "load_group: other bindings in the scope are untouched");
}

// ---------------------------------------------------------------------------
// Stage 5 — dynamic groups (script-defined properties, spec S9)
// ---------------------------------------------------------------------------

props::DynamicField dyn_float(const char* name, double def, float lo, float hi) {
    props::DynamicField f;
    f.name = name;
    f.type = Type::Float;
    f.number_default = def;
    f.min = lo;
    f.max = hi;
    f.has_range = true;
    return f;
}

// float / bool / enum / string, the four kinds a `static props` block can
// declare, plus a doc/label to prove the owned-string plumbing.
std::unique_ptr<props::DynamicGroup> build_demo_group() {
    props::DynamicGroupBuilder builder("world.props", "World Props");

    props::DynamicField spin = dyn_float("spinSpeed", 1.25, 0.0f, 10.0f);
    spin.label = "Spin speed";
    spin.doc = "Rotations per second";
    spin.units = "rps";
    CHECK(builder.add(spin), "dynamic: float field accepted");

    props::DynamicField creak;
    creak.name = "creaky";
    creak.type = Type::Bool;
    creak.bool_default = true;
    CHECK(builder.add(creak), "dynamic: bool field accepted");

    props::DynamicField season;
    season.name = "season";
    season.type = Type::Enum;
    season.enum_labels = {"spring", "summer", "winter"};
    season.number_default = 2;
    CHECK(builder.add(season), "dynamic: enum field accepted");

    props::DynamicField banner;
    banner.name = "banner";
    banner.type = Type::String;
    banner.string_default = "hello";
    CHECK(builder.add(banner), "dynamic: string field accepted");

    return builder.build();
}

void test_dynamic_group_build() {
    auto dg = build_demo_group();
    CHECK(dg != nullptr, "dynamic: build succeeds");
    const props::Group& g = dg->group();
    CHECK(!strcmp(g.path, "world.props"), "dynamic: group path");
    CHECK(!strcmp(g.label, "World Props"), "dynamic: group label");
    CHECK(g.field_count == 4, "dynamic: field count");
    CHECK(g.ctx == dg.get() && g.ctx_construct && g.ctx_destruct && g.ctx_copy_assign,
          "dynamic: the ctx hooks carry the schema");
    CHECK(props::group_can_instantiate(g),
          "dynamic: instantiable through the ctx hooks alone");

    // Uniform lane layout: strictly increasing, one lane apart, big enough for
    // a std::string, and the buffer covers the last lane.
    CHECK(g.fields[0].offset == 0, "dynamic: first field at offset 0");
    const uint32_t lane = g.fields[1].offset - g.fields[0].offset;
    CHECK(lane >= sizeof(std::string), "dynamic: a lane holds a std::string");
    for (uint32_t i = 1; i < g.field_count; ++i)
        CHECK(g.fields[i].offset == i * lane, "dynamic: uniform lane stride");
    CHECK(g.struct_size == g.field_count * lane, "dynamic: buffer covers every lane");
    CHECK(g.struct_align >= alignof(std::string), "dynamic: buffer alignment");

    // Owned strings, not the caller's temporaries.
    CHECK(!strcmp(g.fields[0].name, "spinSpeed"), "dynamic: name copied");
    CHECK(!strcmp(g.fields[0].label, "Spin speed"), "dynamic: label copied");
    CHECK(!strcmp(g.fields[0].doc, "Rotations per second"), "dynamic: doc copied");
    CHECK(!strcmp(g.fields[0].units, "rps"), "dynamic: units copied");
    CHECK(g.fields[1].label == nullptr,
          "dynamic: an unlabelled field leaves label null (Desc falls back to name)");
    CHECK(g.fields[2].enum_count == 3 &&
              !strcmp(g.fields[2].enum_labels[0], "spring") &&
              !strcmp(g.fields[2].enum_labels[2], "winter"),
          "dynamic: enum labels are an owned const char* const* array");
    CHECK(g.fields[0].env == nullptr, "dynamic: script fields carry no env var");

    // Declared defaults landed in the buffer.
    CHECK(dg->index_of("season") == 2, "dynamic: index_of");
    CHECK(dg->index_of("nope") == -1, "dynamic: index_of misses");
    CHECK(dg->get_float(0) == 1.25f, "dynamic: float default");
    CHECK(dg->get_bool(1), "dynamic: bool default");
    CHECK(dg->get_enum(2) == 2, "dynamic: enum default");
    CHECK(dg->get_string(3) == "hello", "dynamic: string default");
    CHECK(dg->format(2) == "winter", "dynamic: format renders the enum label");
    // Out-of-range indices are inert rather than UB.
    CHECK(dg->get_float(99) == 0.0f && !dg->set_float(99, 1.0f),
          "dynamic: out-of-range index is inert");
}

void test_dynamic_group_rejections() {
    props::DynamicGroupBuilder empty_path("", "X");
    props::DynamicField f = dyn_float("a", 1.0, 0.0f, 2.0f);
    empty_path.add(f);
    CHECK(empty_path.build() == nullptr, "dynamic: an empty path builds nothing");

    props::DynamicGroupBuilder no_fields("world.props", "X");
    CHECK(no_fields.build() == nullptr, "dynamic: a group with no fields builds nothing");

    props::DynamicGroupBuilder dupes("world.props", "X");
    CHECK(dupes.add(f), "dynamic: first field accepted");
    std::string why;
    CHECK(!dupes.add(f, &why), "dynamic: duplicate name rejected");
    CHECK(why.find("duplicate") != std::string::npos, "dynamic: duplicate reason");
    CHECK(dupes.build() == nullptr, "dynamic: a rejected field poisons the builder");

    props::DynamicGroupBuilder unnamed("world.props", "X");
    props::DynamicField blank = dyn_float("", 0.0, 0.0f, 1.0f);
    CHECK(!unnamed.add(blank, &why), "dynamic: empty name rejected");

    props::DynamicGroupBuilder bad_enum("world.props", "X");
    props::DynamicField e;
    e.name = "mode";
    e.type = Type::Enum;
    CHECK(!bad_enum.add(e, &why), "dynamic: enum without labels rejected");
    CHECK(why.find("labels") != std::string::npos, "dynamic: enum reason");

    // An out-of-range enum default is clamped into the label array rather than
    // left to index past it in the combo.
    props::DynamicGroupBuilder clamped("world.props", "X");
    props::DynamicField over;
    over.name = "mode";
    over.type = Type::Enum;
    over.enum_labels = {"a", "b"};
    over.number_default = 9;
    CHECK(clamped.add(over), "dynamic: enum with labels accepted");
    auto cg = clamped.build();
    CHECK(cg && cg->get_enum(0) == 1, "dynamic: enum default clamped to the last label");
}

void test_dynamic_group_registry() {
    auto dg = build_demo_group();
    Registry reg;
    const BindingId id = dg->bind_into(reg, Scope::World);
    CHECK(id != props::kInvalidBinding, "dynamic: bind_into returns a binding");
    CHECK(dg->bind_into(reg, Scope::World) == id, "dynamic: bind_into is idempotent");

    Binding* b = reg.get(id);
    CHECK(b && b->instance() == dg->instance(),
          "dynamic: the binding points at the group's own value buffer");
    CHECK(reg.find("world.props") == b, "dynamic: findable by path");
    // bind() allocated + constructed a baseline through the ctx hooks, so it
    // already holds the declared defaults.
    CHECK(b->baseline() != nullptr, "dynamic: baseline allocated");
    CHECK(props::is_group_default(*b), "dynamic: fresh group is at its baseline");

    b->capture_baseline();  // the on_world_connected step
    CHECK(props::is_group_default(*b), "dynamic: baseline capture is a no-op here");

    // Edits through the generic accessors, exactly as the editor panel does.
    const Desc& spin = b->schema().fields[0];
    const Desc& banner = b->schema().fields[3];
    CHECK(props::set_float(*b, spin, 4.5f), "dynamic: float edit");
    CHECK(props::set_string(*b, banner, "edited"), "dynamic: string edit");
    CHECK(b->dirty(), "dynamic: an edit marks the binding dirty");
    CHECK(!props::is_field_default(*b, spin), "dynamic: edited field is off baseline");
    CHECK(!props::is_field_default(*b, banner), "dynamic: edited string is off baseline");
    CHECK(props::is_field_default(*b, b->schema().fields[1]),
          "dynamic: untouched field stays at baseline");
    CHECK(dg->get_float(0) == 4.5f && dg->get_string(3) == "edited",
          "dynamic: by-index reads see the registry edit");

    // Range clamping rides on the same Desc as a static group's.
    props::set_float(*b, spin, 99.0f);
    CHECK(dg->get_float(0) == 10.0f, "dynamic: declared max clamps");

    // Sparse save: only the off-baseline fields, and the String encodes as one.
    jsondoc::Value doc;
    props::save_scope(reg, Scope::World, doc);
    CHECK(doc_to_string(doc) ==
              "{\"version\":1,\"groups\":{\"world.props\":"
              "{\"spinSpeed\":10,\"banner\":\"edited\"}}}",
          "dynamic: sparse save writes only the edited fields");

    // Round-trip into a second, independently built group.
    auto other = build_demo_group();
    Registry other_reg;
    other->bind_into(other_reg, Scope::World);
    props::load_scope(other_reg, Scope::World, doc);
    CHECK(other->get_float(0) == 10.0f && other->get_string(3) == "edited",
          "dynamic: load restores float + string");
    CHECK(other->get_bool(1) && other->get_enum(2) == 2,
          "dynamic: absent keys keep the declared defaults");

    props::reset_group(*b);
    CHECK(props::is_group_default(*b) && dg->get_string(3) == "hello",
          "dynamic: reset_group restores the declared defaults, strings included");

    dg->unbind_from(reg);
    CHECK(reg.size() == 0 && dg->binding() == props::kInvalidBinding,
          "dynamic: unbind_from drops the binding");
    other->unbind_from(other_reg);
}

void test_dynamic_group_draft() {
    // RequiresReload on a dynamic group: the draft is a full second value
    // buffer built through the ctx hooks, including its String lanes.
    props::DynamicGroupBuilder builder("world.props", "World Props");
    props::DynamicField f = dyn_float("spinSpeed", 1.0, 0.0f, 10.0f);
    f.flags = props::RequiresReload;
    builder.add(f);
    props::DynamicField name;
    name.name = "banner";
    name.type = Type::String;
    name.string_default = "before";
    name.flags = props::RequiresReload;
    builder.add(name);
    auto dg = builder.build();
    CHECK(dg != nullptr, "dynamic draft: build");

    Registry reg;
    Binding* b = reg.get(dg->bind_into(reg, Scope::World));
    CHECK(props::group_requires_reload(b->schema()),
          "dynamic draft: flags reach group_requires_reload");

    void* draft = props::ensure_draft(*b);
    CHECK(draft != nullptr && draft != dg->instance(),
          "dynamic draft: a separate buffer");
    CHECK(!props::has_pending_draft(*b), "dynamic draft: a fresh copy is not pending");

    props::set_float(draft, b->schema().fields[0], 7.0f);
    props::set_string(draft, b->schema().fields[1], "after");
    CHECK(props::has_pending_draft(*b), "dynamic draft: pending after an edit");
    CHECK(dg->get_float(0) == 1.0f && dg->get_string(1) == "before",
          "dynamic draft: the live buffer is untouched while pending");

    CHECK(props::apply_draft(*b), "dynamic draft: apply reports a change");
    CHECK(dg->get_float(0) == 7.0f && dg->get_string(1) == "after",
          "dynamic draft: apply copies every lane, strings included");
    CHECK(props::draft_of(*b) == nullptr, "dynamic draft: apply clears the draft");
    CHECK(b->dirty(), "dynamic draft: apply marks dirty");

    // Discard leaves the live buffer alone.
    props::set_float(props::ensure_draft(*b), b->schema().fields[0], 2.0f);
    props::discard_draft(*b);
    CHECK(dg->get_float(0) == 7.0f, "dynamic draft: discard drops the edit");

    dg->unbind_from(reg);
}

void test_dynamic_group_teardown() {
    // Destruction order: the group outlives nothing, and every String lane in
    // the live buffer, the baseline and the draft has to be destroyed exactly
    // once. There is no valgrind here — this exercises all three paths so an
    // ASAN/UBSAN run of the suite has something to catch.
    {
        Registry reg;
        auto dg = build_demo_group();
        Binding* b = reg.get(dg->bind_into(reg, Scope::World));
        props::set_string(*b, b->schema().fields[3], std::string(200, 'x'));
        props::ensure_draft(*b);  // a third buffer with its own String lanes
        dg->unbind_from(reg);     // binding (baseline + draft) dies first
        // dg dies here, releasing the live buffer.
    }
    {
        // Reverse order: the DynamicGroup goes first. It must not leave a
        // dangling Binding behind — it drops it on the way out (and complains).
        Registry reg;
        {
            auto dg = build_demo_group();
            dg->bind_into(reg, Scope::World);
            CHECK(reg.size() == 1, "dynamic teardown: bound");
        }
        CHECK(reg.size() == 0,
              "dynamic teardown: a group destroyed while bound drops its binding");
    }
}

}  // namespace

void test_non_finite() {
    Registry reg;
    Tunables inst;
    Binding* b = reg.get(reg.bind(tunables_schema(), &inst, Scope::World));

    // Setters refuse NaN/inf outright (json_doc cannot re-parse them).
    const float nan_v = std::nanf("");
    const float inf_v = std::numeric_limits<float>::infinity();
    CHECK(!props::set_float(*b, field("density"), nan_v), "nonfinite: NaN refused");
    CHECK(inst.density == 0.25f, "nonfinite: NaN left value untouched");
    const float bad3[3] = {1.0f, inf_v, 1.0f};
    CHECK(!props::set_float3(*b, field("wind"), bad3), "nonfinite: inf float3 refused");
    CHECK(inst.wind.x == 1.0f && inst.wind.y == 2.0f && inst.wind.z == 3.0f,
          "nonfinite: float3 all-or-nothing");

    // Direct struct writes bypass setters; the save path must skip such
    // fields rather than emit a token the parser rejects on next launch.
    inst.density = inf_v;
    inst.title = "edited";
    jsondoc::Value doc;
    props::save_scope(reg, Scope::World, doc);
    const std::string out = doc_to_string(doc);
    CHECK(out.find("density") == std::string::npos, "nonfinite: field not persisted");
    CHECK(out.find("edited") != std::string::npos, "nonfinite: finite siblings persist");
    jsondoc::Value reparsed;
    CHECK(jsondoc::parse_json(out, reparsed), "nonfinite: saved doc reparses");
}

int main() {
    test_builder_layout();
    test_defaults_and_reset();
    test_range_clamping();
    test_non_finite();
    test_sparse_save();
    test_round_trip();
    test_tolerant_load();
    test_unknown_content_preserved();
    test_env_layer();
    test_json_doc();
    test_draft_lifecycle();
    test_parse_and_set();
    test_resolve_field();
    test_dump_modified();
    test_load_group();
    test_dynamic_group_build();
    test_dynamic_group_rejections();
    test_dynamic_group_registry();
    test_dynamic_group_draft();
    test_dynamic_group_teardown();
    return check_summary();
}
