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

}  // namespace

int main() {
    test_widget_selection();
    test_format();
    test_paths_and_labels();
    test_filter();
    test_find_field();
    test_lod_ring_round_trip();
    return check_summary();
}
