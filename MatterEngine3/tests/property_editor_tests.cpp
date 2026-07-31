// property_editor_tests.cpp — the ImGui-free half of the editor's generic
// property renderer (MatterEditor/src/property_editor.h): widget selection,
// format construction, path formatting and the Tunables filter. No ImGui
// context is created; only the header's inline decisions are exercised.

#include "check.h"

#include "../../MatterEditor/src/property_editor.h"
#include "../../MatterEditor/src/streaming_lod_prefs.h"

#include <cstring>
#include <string>

using namespace matter;
using props::Desc;
using props::Type;
using viewer::PropWidget;

namespace {

struct Sample {
    float    ranged   = 1.0f;
    float    unranged = 2.0f;
    int32_t  steps    = 30;
    uint32_t budget   = 8u;
    bool     enabled  = true;
    int32_t  mode     = 0;
    float    tint[3]  = {1.0f, 1.0f, 1.0f};
    float    locked   = 5.0f;
};

const char* const kModes[] = {"off", "on"};

const auto& schema() {
    static const auto def = props::group<Sample>(
        "render.sample", "Sample",
        props::prop(&Sample::ranged, "ranged").label("Ranged").range(0.0f, 4.0f),
        props::prop(&Sample::unranged, "unranged"),
        props::prop(&Sample::steps, "steps").range(4.0f, 64.0f),
        props::prop(&Sample::budget, "budget"),
        props::prop(&Sample::enabled, "enabled"),
        props::prop(&Sample::mode, "mode").enums(kModes, 2),
        props::prop(&Sample::tint, "tint").color(),
        props::prop(&Sample::locked, "locked").read_only());
    return def;
}

const Desc& field(const char* name) {
    const props::Group& g = schema();
    for (uint32_t i = 0; i < g.field_count; ++i)
        if (!std::strcmp(g.fields[i].name, name)) return g.fields[i];
    static Desc none;
    return none;
}

void test_widget_selection() {
    CHECK(viewer::prop_widget_for(field("ranged")) == PropWidget::FloatSlider,
          "a ranged float is a slider");
    CHECK(viewer::prop_widget_for(field("unranged")) == PropWidget::FloatDrag,
          "an unranged float is a drag");
    CHECK(viewer::prop_widget_for(field("steps")) == PropWidget::IntSlider,
          "a ranged int is a slider");
    CHECK(viewer::prop_widget_for(field("budget")) == PropWidget::UIntDrag,
          "an unranged uint is a drag");
    CHECK(viewer::prop_widget_for(field("enabled")) == PropWidget::Checkbox,
          "a bool is a checkbox");
    CHECK(viewer::prop_widget_for(field("mode")) == PropWidget::EnumCombo,
          "an enum is a combo");
    CHECK(viewer::prop_widget_for(field("tint")) == PropWidget::ColorEdit3,
          "a Color3 is a color editor");
    // ReadOnly short-circuits the type switch entirely.
    CHECK(viewer::prop_widget_for(field("locked")) == PropWidget::ReadOnlyText,
          "a ReadOnly field is printed, not edited");
}

void test_format() {
    Desc d;
    d.type = Type::Float;
    CHECK(viewer::prop_format_for(d) == "%.3f", "unranged floats get 3 decimals");
    d.has_range = true;
    d.min = 0.0f;
    d.max = 0.5f;
    CHECK(viewer::prop_format_for(d) == "%.3f", "a narrow range keeps 3 decimals");
    d.max = 6.0f;
    d.min = -6.0f;
    CHECK(viewer::prop_format_for(d) == "%.2f", "a mid range drops to 2 decimals");
    d.units = "EV";
    CHECK(viewer::prop_format_for(d) == "%.2f EV", "units are folded into the format");
    d.units = nullptr;
    d.min = 5.0f;
    d.max = 10241.0f;
    CHECK(viewer::prop_format_for(d) == "%.0f", "a metre-scale range drops decimals");
    d.type = Type::Int;
    CHECK(viewer::prop_format_for(d) == "%d", "ints format as integers");
    d.type = Type::UInt;
    CHECK(viewer::prop_format_for(d) == "%u", "uints format as unsigned");
}

void test_paths_and_labels() {
    CHECK(viewer::prop_field_path(schema(), field("steps")) == "render.sample.steps",
          "field path is group.path + '.' + name");
    CHECK(!std::strcmp(viewer::prop_display_label(field("ranged")), "Ranged"),
          "an explicit label wins");
    CHECK(!std::strcmp(viewer::prop_display_label(field("steps")), "steps"),
          "a label-less field falls back to its name");
    CHECK(!std::strcmp(viewer::prop_scope_name(props::Scope::World), "World"),
          "scope tag text");
}

void test_filter() {
    CHECK(viewer::prop_filter_matches(nullptr, "anything"), "a null filter matches");
    CHECK(viewer::prop_filter_matches("", "anything"), "an empty filter matches");
    CHECK(viewer::prop_filter_matches("SAMP", "render.sample"),
          "the filter is case-insensitive");
    CHECK(!viewer::prop_filter_matches("nope", "render.sample"),
          "a non-substring does not match");
    CHECK(!viewer::prop_filter_matches("x", nullptr),
          "a null haystack never matches a real needle");
    CHECK(viewer::prop_filter_matches_group("render", schema()),
          "a group matches by path");
    CHECK(viewer::prop_filter_matches_group("budget", schema()),
          "a group matches through one of its fields");
    CHECK(!viewer::prop_filter_matches_group("volumetrics", schema()),
          "an unrelated filter rejects the group");
    CHECK(viewer::prop_filter_matches_field("STEP", field("steps")),
          "field filtering is case-insensitive");
    CHECK(!viewer::prop_filter_matches_field("steps", field("budget")),
          "field filtering rejects other fields");
}

// The Tunables panel groups the sorted bindings under a per-category header.
// Both halves of that — the category a path falls in, and the order that makes
// categories contiguous — are pure functions here so the panel needs no
// bucketing pass and this needs no ImGui context.
void test_tunables_categories() {
    CHECK(viewer::prop_path_category("render.lighting") == "render",
          "category is the first path segment");
    CHECK(viewer::prop_path_category("stream.lod") == "stream",
          "category: another segment");
    CHECK(viewer::prop_path_category("render.pom.extra") == "render",
          "category splits on the FIRST dot, not the last");
    CHECK(viewer::prop_path_category("standalone") == "standalone",
          "a dotless path is its own category");
    CHECK(viewer::prop_path_category(nullptr).empty(),
          "a null path has no category");
    CHECK(viewer::prop_path_category("").empty(),
          "an empty path has no category");

    CHECK(viewer::prop_category_label("render") == "Render",
          "category label title-cases the segment");
    CHECK(viewer::prop_category_label("vt") == "VT",
          "a two-letter segment is an acronym");
    CHECK(viewer::prop_category_label("world") == "World", "category label: world");
    CHECK(viewer::prop_category_label("") == "Other",
          "an empty category still gets a header");

    // Ordering: strcmp on path, which is what makes "did the category change"
    // a single comparison against the previous row.
    CHECK(viewer::prop_group_order_before(schema(), viewer::streaming_lod_group()),
          "groups order by path (render < stream)");
    CHECK(!viewer::prop_group_order_before(viewer::streaming_lod_group(), schema()),
          "group ordering is antisymmetric");

    CHECK(viewer::prop_starts_new_category(nullptr, "render.lighting"),
          "the first row always opens a category");
    CHECK(!viewer::prop_starts_new_category("render.lighting", "render.volumetrics"),
          "a same-category row does not repeat the header");
    CHECK(viewer::prop_starts_new_category("render.volumetrics", "stream.lod"),
          "a category change opens a new header");

    // Walk a realistic sorted listing and count the headers it produces.
    const char* paths[] = {"camera.prefs",     "render.lighting",
                           "render.pom",       "render.volumetrics",
                           "stream.lod",       "viewer.budget",
                           "vt.residency",     "world.props"};
    for (size_t i = 1; i < sizeof(paths) / sizeof(paths[0]); ++i)
        CHECK(std::strcmp(paths[i - 1], paths[i]) < 0,
              "listing fixture is in the panel's sort order");
    int headers = 0;
    const char* prev = nullptr;
    for (const char* p : paths) {
        if (viewer::prop_starts_new_category(prev, p)) ++headers;
        prev = p;
    }
    // camera, render, stream, viewer, vt, world — eight groups, six headers,
    // because render.lighting/pom/volumetrics share one.
    CHECK(headers == 6,
          "one header per distinct category, none repeated");
}

void test_find_field() {
    CHECK(viewer::prop_find_field(schema(), "steps") == &field("steps"),
          "prop_find_field returns the described field");
    CHECK(viewer::prop_find_field(schema(), "nope") == nullptr,
          "prop_find_field: unknown name");
    CHECK(viewer::prop_find_field(schema(), nullptr) == nullptr,
          "prop_find_field: null name");
}

// The streaming-LOD group persists each ring list as ONE String field, so this
// round-trip is what stands between a saved override and a silently dropped
// one (streaming_lod_prefs.h).
void test_lod_ring_round_trip() {
    const std::vector<viewer::LodRing> rings = viewer::parse_lod_rings("128:2,384:1,900:0");
    CHECK(rings.size() == 3, "rings: three pairs parsed");
    CHECK(rings[0].radius == 128.0f && rings[0].value == 2, "rings: first pair");
    CHECK(rings[2].radius == 900.0f && rings[2].value == 0, "rings: last pair");
    CHECK(viewer::format_lod_rings(rings) == "128:2,384:1,900:0",
          "rings: format is the parse's inverse");

    CHECK(viewer::parse_lod_rings("").empty(),
          "rings: empty text means 'keep the world's own rings'");
    CHECK(viewer::format_lod_rings({}).empty(), "rings: empty list formats empty");

    // Lenient separators: whitespace, semicolons, '=' — a hand-edited file or
    // a FIFO line should not need to be byte-exact.
    const std::vector<viewer::LodRing> loose = viewer::parse_lod_rings(" 64=5; 128 : 4 ");
    CHECK(loose.size() == 2 && loose[0].value == 5 && loose[1].radius == 128.0f,
          "rings: lenient separators");

    // Tolerant reader (S5): a malformed tail yields what was read, not garbage.
    const std::vector<viewer::LodRing> partial = viewer::parse_lod_rings("100:1,broken");
    CHECK(partial.size() == 1 && partial[0].radius == 100.0f,
          "rings: a malformed pair ends the parse");
    CHECK(viewer::parse_lod_rings("100").empty(),
          "rings: a radius with no value is not a ring");

    // Radii print without decimals: the panel drags whole metres.
    CHECK(viewer::format_lod_rings({{127.6f, 3}}) == "128:3",
          "rings: radii round to whole metres");

    // The group itself is a RequiresReload group over the prefs struct.
    CHECK(props::group_requires_reload(viewer::streaming_lod_group()),
          "stream.lod is a RequiresReload group");
    CHECK(viewer::prop_find_field(viewer::streaming_lod_group(), "scatter_rings") &&
              viewer::prop_find_field(viewer::streaming_lod_group(),
                                      "scatter_rings")->type == Type::String,
          "stream.lod: rings are a String field");

    // A default-constructed prefs is a no-op override: nothing to persist.
    viewer::StreamingLodPrefs prefs;
    props::Registry reg;
    props::Binding* b =
        reg.get(reg.bind(viewer::streaming_lod_group(), &prefs, props::Scope::World));
    CHECK(props::is_group_default(*b), "stream.lod: default prefs is a no-op override");
    matter::jsondoc::Value doc;
    props::save_scope(reg, props::Scope::World, doc);
    CHECK(matter::jsondoc::write_json(doc) == "{\"version\":1,\"groups\":{}}",
          "stream.lod: an untouched override writes no group");
}

// A script-declared group has to reach the SAME renderer decisions as a
// hand-written one — that is the whole reason the panel needs no dynamic-group
// special case (property-system S9).
void test_dynamic_group_renders_generically() {
    props::DynamicGroupBuilder builder("world.props", "World Props");
    props::DynamicField spin;
    spin.name = "spinSpeed";
    spin.label = "Spin speed";
    spin.units = "rps";
    spin.type = Type::Float;
    spin.number_default = 1.25;
    spin.min = 0.0f;
    spin.max = 10.0f;
    spin.has_range = true;
    builder.add(spin);
    props::DynamicField creak;
    creak.name = "creaky";
    creak.type = Type::Bool;
    builder.add(creak);
    props::DynamicField banner;
    banner.name = "banner";
    banner.type = Type::String;
    builder.add(banner);
    props::DynamicField season;
    season.name = "season";
    season.type = Type::Enum;
    season.enum_labels = {"spring", "summer", "winter"};
    builder.add(season);
    auto dg = builder.build();
    CHECK(dg != nullptr, "dynamic panel: group builds");

    const props::Group& g = dg->group();
    CHECK(viewer::prop_widget_for(g.fields[0]) == PropWidget::FloatSlider,
          "dynamic panel: a ranged float is a slider");
    CHECK(viewer::prop_widget_for(g.fields[1]) == PropWidget::Checkbox,
          "dynamic panel: bool is a checkbox");
    CHECK(viewer::prop_widget_for(g.fields[2]) == PropWidget::TextInput,
          "dynamic panel: string is a text input");
    CHECK(viewer::prop_widget_for(g.fields[3]) == PropWidget::EnumCombo,
          "dynamic panel: enum is a combo");
    CHECK(g.fields[3].enum_labels && g.fields[3].enum_count == 3,
          "dynamic panel: the combo has an owned label array to draw from");

    CHECK(viewer::prop_format_for(g.fields[0]) == "%.2f rps",
          "dynamic panel: units fold into the format");
    CHECK(!std::strcmp(viewer::prop_display_label(g.fields[0]), "Spin speed"),
          "dynamic panel: authored label wins");
    CHECK(!std::strcmp(viewer::prop_display_label(g.fields[1]), "creaky"),
          "dynamic panel: an unlabelled field falls back to its name");
    CHECK(viewer::prop_field_path(g, g.fields[0]) == "world.props.spinSpeed",
          "dynamic panel: copy-path / FIFO path");
    CHECK(viewer::prop_filter_matches_group("spin", g),
          "dynamic panel: the Tunables filter reaches dynamic field names");
    CHECK(!viewer::prop_filter_matches_group("nomatch", g),
          "dynamic panel: a non-matching filter hides the group");

    // The panel enumerates the registry, so binding is all it takes to appear.
    props::Registry reg;
    dg->bind_into(reg, props::Scope::World);
    CHECK(reg.size() == 1 && reg.find("world.props") != nullptr,
          "dynamic panel: a bound dynamic group is enumerable like any other");
    dg->unbind_from(reg);
}

}  // namespace

int main() {
    test_widget_selection();
    test_format();
    test_paths_and_labels();
    test_filter();
    test_tunables_categories();
    test_find_field();
    test_lod_ring_round_trip();
    test_dynamic_group_renders_generically();
    return check_summary();
}
