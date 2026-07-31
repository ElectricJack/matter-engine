// property_editor_tests.cpp — the ImGui-free half of the editor's generic
// property renderer (MatterEditor/src/property_editor.h): widget selection,
// format construction, path formatting and the Tunables filter. No ImGui
// context is created; only the header's inline decisions are exercised.

#include "check.h"

#include "../../MatterEditor/src/property_editor.h"

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

}  // namespace

int main() {
    test_widget_selection();
    test_format();
    test_paths_and_labels();
    test_filter();
    return check_summary();
}
