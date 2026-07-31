// props_tests.cpp — headless unit tests for matter::props + matter::jsondoc.
// No raylib / GL / Vulkan: links only props.cpp + json_doc.cpp.

#include "check.h"

#include "matter/json_doc.h"
#include "matter/props.h"

#include <cstdlib>
#include <cstring>
#include <string>

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

}  // namespace

int main() {
    test_builder_layout();
    test_defaults_and_reset();
    test_range_clamping();
    test_sparse_save();
    test_round_trip();
    test_tolerant_load();
    test_unknown_content_preserved();
    test_env_layer();
    test_json_doc();
    return check_summary();
}
