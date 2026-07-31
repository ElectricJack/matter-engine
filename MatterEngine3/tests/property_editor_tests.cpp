// property_editor_tests.cpp — the ImGui-free half of the editor's generic
// property renderer (MatterEditor/src/property_editor.h): widget selection,
// format construction, path formatting and the Tunables filter. No ImGui
// context is created; only the header's inline decisions are exercised.

#include "check.h"

#include "../../MatterEditor/src/property_editor.h"
#include "../../MatterEditor/src/streaming_lod_prefs.h"
#include "matter/json_doc.h"
#include "matter/world_definition.h"   // FogSettings — the render.fog group

#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

// Tunables de-duplication (issue dd98763c-2ea2-9a1c-fc93-e755f94ee938): the
// checkbox at the top of Tunables hides a group another panel already draws.
// draw_tunables_contents itself needs a live ImGui context and an EditorProps
// registry to exercise, so — same discipline as prop_starts_new_category
// above — the two decisions it is built from are pure functions tested here,
// and this test drives them through the SAME kind of "walk a sorted listing"
// loop draw_tunables_contents runs, so the category-header interaction is
// covered without needing ImGui at all.
void test_tunables_hide_duplicates() {
    // ---- prop_effective_hide_duplicates: search always wins ---------------
    CHECK(viewer::prop_effective_hide_duplicates(true, false),
          "checkbox on, no filter: de-dup is active");
    CHECK(!viewer::prop_effective_hide_duplicates(true, true),
          "typing a filter disables the checkbox regardless of its stored value");
    CHECK(!viewer::prop_effective_hide_duplicates(false, false),
          "checkbox off, no filter: de-dup is inactive");
    CHECK(!viewer::prop_effective_hide_duplicates(false, true),
          "checkbox off AND filtering: still inactive (filtering doesn't need to add anything)");

    // ---- prop_group_is_duplicate_hidden: the per-row rule ------------------
    CHECK(!viewer::prop_group_is_duplicate_hidden(false, true),
          "checkbox effectively off: a claimed group still shows");
    CHECK(!viewer::prop_group_is_duplicate_hidden(true, false),
          "checkbox on: an UNclaimed group still shows (nothing to de-dup)");
    CHECK(viewer::prop_group_is_duplicate_hidden(true, true),
          "checkbox on: a claimed group is hidden");

    // ---- the interaction with category headers -----------------------------
    // A realistic sorted listing, as draw_tunables_contents's registry walk
    // produces it: some groups claimed by a dedicated panel this frame
    // (EditorProps::panel_home would return non-null), some not.
    //   camera.prefs, render.pom, stream.lod, viewer.budget, vt.residency
    //     -> Performance (claimed)
    //   render.lighting -> Lighting (claimed)
    //   render.volumetrics -> deliberately left UNclaimed here, so the
    //     "render" category has both a hidden AND a surviving row — the case
    //     that actually exercises the no-dangling-header rule, rather than
    //     every row in a category sharing one fate.
    //   world.props -> nobody else draws a world's own script-declared
    //     group, so it is never claimed.
    struct Row { const char* path; bool claimed; };
    const Row rows[] = {
        {"camera.prefs",       true},
        {"render.lighting",    true},
        {"render.pom",         true},
        {"render.volumetrics", false},
        {"stream.lod",         true},
        {"viewer.budget",      true},
        {"vt.residency",       true},
        {"world.props",        false},
    };
    for (size_t i = 1; i < sizeof(rows) / sizeof(rows[0]); ++i)
        CHECK(std::strcmp(rows[i - 1].path, rows[i].path) < 0,
              "hide-duplicates fixture is in the panel's sort order");

    struct Walk { int headers = 0; int drawn = 0; bool any_hidden = false; };
    auto walk = [&](bool hide_duplicates) {
        Walk w;
        const char* prev = nullptr;
        for (const Row& row : rows) {
            // Mirrors draw_tunables_contents's loop exactly: the duplicate
            // check runs (and, on a hit, `continue`s) BEFORE the category
            // header is considered, so a hidden row can never open one.
            if (viewer::prop_group_is_duplicate_hidden(hide_duplicates, row.claimed)) {
                w.any_hidden = true;
                continue;
            }
            if (viewer::prop_starts_new_category(prev, row.path)) ++w.headers;
            prev = row.path;
            ++w.drawn;
        }
        return w;
    };

    const Walk hidden = walk(/*hide_duplicates=*/true);
    CHECK(hidden.drawn == 2,
          "hide-duplicates on: only the two unclaimed groups remain "
          "(render.volumetrics, world.props)");
    CHECK(hidden.any_hidden, "hide-duplicates on: something was actually hidden");
    // camera/stream/viewer/vt are single-row categories that are ENTIRELY
    // claimed — each must contribute ZERO headers, not a header over nothing.
    // render keeps its header because render.volumetrics survives; world
    // keeps its header because world.props was never claimed. 2 headers for
    // 2 surviving rows is exactly the "one header per surviving category,
    // none dangling" invariant test_tunables_categories already pins for the
    // plain filter — this is the same rule under the OTHER hiding path.
    CHECK(hidden.headers == 2,
          "hide-duplicates on: no dangling header over an all-hidden category");

    const Walk shown = walk(/*hide_duplicates=*/false);  // checkbox off, or filtering
    CHECK(shown.drawn == 8, "hide-duplicates off: every group reappears, claimed or not");
    CHECK(!shown.any_hidden, "hide-duplicates off: nothing is hidden");
    CHECK(shown.headers == 6,
          "hide-duplicates off: back to one header per category (camera, "
          "render, stream, viewer, vt, world) — the claimed groups that "
          "disappeared above reappear without duplicating a header");
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

// The streamer pacing knobs added to stream.lod in WS2. They ride the SAME
// RequiresReload draft as the ring strings — the SectorStreamer is constructed
// from the resolved profile at connect and never reconfigured — and their
// compiled defaults are matter_stream::Config's own, which is what keeps a
// default-constructed prefs a no-op override (asserted above).
void test_streaming_pacing_fields() {
    const props::Group& g = viewer::streaming_lod_group();
    struct Expect { const char* name; Type type; const char* env; };
    const Expect expected[] = {
        {"hysteresis", Type::Float, "MATTER_STREAM_HYSTERESIS"},
        {"max_inflight", Type::Int, "MATTER_STREAM_INFLIGHT"},
        {"fail_cooldown_updates", Type::Int, "MATTER_STREAM_FAIL_COOLDOWN"},
    };
    for (const Expect& e : expected) {
        const Desc* d = viewer::prop_find_field(g, e.name);
        CHECK(d != nullptr, "stream.lod: pacing field is described");
        if (!d) continue;
        CHECK(d->type == e.type, "stream.lod: pacing field type");
        CHECK(d->flags & props::RequiresReload,
              "stream.lod: pacing edits are drafted, not applied live");
        CHECK(d->env && !std::strcmp(d->env, e.env),
              "stream.lod: the pre-migration env var still names the field");
        CHECK(d->has_range, "stream.lod: pacing fields are ranged sliders");
    }

    // Defaults mirror the engine's, so binding changes nothing until edited.
    viewer::StreamingLodPrefs prefs;
    CHECK(prefs.hysteresis == 16.0f, "stream.lod: hysteresis default");
    CHECK(prefs.max_inflight == 64, "stream.lod: max_inflight default");
    CHECK(prefs.fail_cooldown_updates == 64,
          "stream.lod: fail cooldown default");

    // An edit lands in the DRAFT, which the panel and the schema fields share;
    // the live instance (what streaming_config_from reads at connect) only
    // moves on apply_draft.
    props::Registry reg;
    props::Binding* b =
        reg.get(reg.bind(g, &prefs, props::Scope::World));
    const Desc* inflight = viewer::prop_find_field(g, "max_inflight");
    void* draft = props::ensure_draft(*b);
    CHECK(draft != nullptr && draft != b->instance(),
          "stream.lod: a RequiresReload group edits into a draft");
    CHECK(props::set_int(draft, *inflight, 16), "stream.lod: draft write");
    CHECK(prefs.max_inflight == 64,
          "stream.lod: the live override is untouched before Apply");
    CHECK(props::has_pending_draft(*b), "stream.lod: the Apply bar shows");
    CHECK(props::apply_draft(*b), "stream.lod: Apply pushes the draft");
    CHECK(prefs.max_inflight == 16, "stream.lod: Apply reaches the instance");
}

// render.fog is the WS2 flagship LIVE World group: fog is re-handed to the
// renderer on every WorldSession::render call, so an override needs no reload.
// The group is defined in editor_props.cpp (which pulls in ImGui-adjacent
// headers), so what is asserted here is the SHAPE the editor relies on —
// FogSettings is describable, colour is a Color3 while wind stays a Float3, and
// the connect-time baseline flow behaves the way "Reset to World" needs.
void test_fog_group_baseline_flow() {
    using matter::FogSettings;
    static const auto fog_def = props::group<FogSettings>(
        "render.fog", "Fog",
        props::prop(&FogSettings::density, "density").range(0.0f, 1.0f).log(),
        props::prop(&FogSettings::floor, "floor").range(-500.0f, 2000.0f),
        props::prop(&FogSettings::falloff, "falloff").range(1.0f, 2000.0f),
        props::prop(&FogSettings::color, "color").color(),
        props::prop(&FogSettings::wind, "wind"),
        props::prop(&FogSettings::height_layer, "height_layer"),
        props::prop(&FogSettings::min_height, "min_height").range(-500.0f, 4000.0f),
        props::prop(&FogSettings::max_height, "max_height").range(-500.0f, 4000.0f),
        props::prop(&FogSettings::noise_scale, "noise_scale").range(0.0001f, 0.05f));
    const props::Group& g = fog_def;

    // The authored colour is a [0,1] RGB triple, so ColorEdit3's clamp is the
    // right domain; wind is a signed velocity and must NOT be clamped there.
    CHECK(viewer::prop_widget_for(*viewer::prop_find_field(g, "color")) ==
              PropWidget::ColorEdit3,
          "render.fog: the authored colour is a colour editor");
    CHECK(viewer::prop_widget_for(*viewer::prop_find_field(g, "wind")) ==
              PropWidget::Float3Drag,
          "render.fog: wind is a plain Float3, not a colour");
    CHECK(viewer::prop_widget_for(*viewer::prop_find_field(g, "height_layer")) ==
              PropWidget::Checkbox,
          "render.fog: the cloud-layer toggle is a checkbox");
    // Live, not reload-gated: every field is consumed per frame.
    CHECK(!props::group_requires_reload(g),
          "render.fog: fog reaches the renderer every frame, so edits are live");

    // ---- the connect seam ------------------------------------------------
    // Layer 1: the compiled default is what bind() captures as baseline.
    FogSettings live;
    props::Registry reg;
    props::Binding* b = reg.get(reg.bind(g, &live, props::Scope::World));
    CHECK(props::is_group_default(*b), "render.fog: bind captures layer 1");

    // Layer 2: the world's authored fog lands in the struct (main.cpp seeds it
    // from WorldSession::world_fog), and only THEN is the baseline captured.
    live.density = 0.02f;
    live.falloff = 120.0f;
    live.color[0] = 0.4f;
    b->capture_baseline();
    b->set_dirty(false);
    CHECK(props::is_group_default(*b),
          "render.fog: after capture, the authored values ARE the baseline");

    // Layer 6: a live edit is a diff against the authored values...
    const Desc* density = viewer::prop_find_field(g, "density");
    CHECK(props::set_float(*b, *density, 0.5f), "render.fog: live edit");
    CHECK(!props::is_field_default(*b, *density), "render.fog: edit shows modified");
    CHECK(b->dirty(), "render.fog: a live edit marks the group dirty");

    // ...so the sparse save writes that one field and nothing else.
    matter::jsondoc::Value doc;
    props::save_scope(reg, props::Scope::World, doc);
    const std::string out = matter::jsondoc::write_json(doc);
    CHECK(out.find("\"density\"") != std::string::npos,
          "render.fog: the edited field persists");
    CHECK(out.find("falloff") == std::string::npos,
          "render.fog: an authored-but-unedited field is not persisted");

    // "Reset to World" restores the AUTHORED values, not the compiled ones.
    props::reset_group(*b);
    CHECK(live.density == 0.02f, "render.fog: reset restores what the world authored");
    CHECK(live.falloff == 120.0f, "render.fog: reset leaves siblings authored");
    CHECK(props::is_group_default(*b), "render.fog: reset clears every diff");

    // And the FIFO `set render.fog.<field> <text>` path reaches it.
    props::Binding* found = nullptr;
    const Desc* desc = nullptr;
    CHECK(props::resolve_field(reg, "render.fog.color", found, desc),
          "render.fog: resolve_field splits group and field");
    CHECK(found == b && desc == viewer::prop_find_field(g, "color"),
          "render.fog: resolve_field lands on the colour");
    CHECK(props::parse_and_set(*b, *desc, "0.1, 0.2, 0.3"),
          "render.fog: a Color3 parses from text");
    CHECK(live.color[0] == 0.1f && live.color[2] == 0.3f,
          "render.fog: the parsed colour reached the struct");
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

// ---------------------------------------------------------------------------
// draw.overrides — the per-module view-time filter (WS3).
//
// The section renderer folds three fields per module into one row, so the
// grouping is the piece worth pinning here. The baseline discipline is pinned
// too: it is the same "never re-capture" rule world.props has, and getting it
// wrong silently erases every override from the world file on the next save.
// ---------------------------------------------------------------------------
void test_draw_override_rows() {
    std::unique_ptr<props::DynamicGroup> dg =
        matter::build_draw_override_group({"AlpineGrass", "Tree"});
    CHECK(dg != nullptr, "draw.overrides: built");
    const props::Group& g = dg->group();

    const std::vector<viewer::DrawOverrideRow> rows =
        viewer::draw_override_rows(g);
    CHECK(rows.size() == 2, "draw.overrides: one row per module, not per field");
    CHECK(rows[0].module == "AlpineGrass" && rows[1].module == "Tree",
          "draw.overrides: rows keep field (module) order");
    for (const viewer::DrawOverrideRow& r : rows)
        CHECK(r.hide >= 0 && r.max_dist >= 0 && r.lod_bias >= 0,
              "draw.overrides: every row resolves all three fields");
    CHECK(std::string(g.fields[rows[1].hide].name) == "Tree/hide",
          "draw.overrides: the row indexes point at the right fields");

    // Widget choices the section relies on.
    CHECK(viewer::prop_widget_for(g.fields[rows[0].hide]) == PropWidget::Checkbox,
          "draw.overrides: hide is a checkbox");
    CHECK(viewer::prop_widget_for(g.fields[rows[0].max_dist]) ==
              PropWidget::FloatDrag,
          "draw.overrides: max_dist is an unranged drag so 0 = unlimited");
    CHECK(viewer::prop_widget_for(g.fields[rows[0].lod_bias]) ==
              PropWidget::FloatSlider,
          "draw.overrides: lod_bias is a ranged slider");
    CHECK(!props::group_requires_reload(g),
          "draw.overrides: every field takes effect live, never on reload");

    // A group whose fields are not module/field named must not produce rows.
    props::DynamicGroupBuilder plain("world.props", "World Props");
    props::DynamicField f;
    f.name = "spinSpeed";
    f.type = props::Type::Float;
    plain.add(std::move(f));
    std::unique_ptr<props::DynamicGroup> other = plain.build();
    CHECK(viewer::draw_override_rows(other->group()).empty(),
          "draw.overrides: an unrelated dynamic group folds to no rows");

    dg.reset();
}

void test_draw_override_baseline_rule() {
    // The session PRESERVES this group across a reload whose module set did not
    // change, so the buffer arriving at the next connect still holds the user's
    // overrides. EditorProps therefore skips the baseline re-capture for it —
    // this test is what that rule protects against.
    std::unique_ptr<props::DynamicGroup> dg =
        matter::build_draw_override_group({"Grass", "Tree"});
    props::Registry reg;
    props::Binding* b = reg.get(dg->bind_into(reg, props::Scope::World));
    const int32_t hide = dg->index_of("Grass/hide");
    CHECK(hide >= 0, "draw.overrides: field lookup");

    dg->set_bool(static_cast<uint32_t>(hide), true);
    b->set_dirty(true);
    matter::jsondoc::Value doc;
    props::save_scope(reg, props::Scope::World, doc);
    CHECK(matter::jsondoc::write_json(doc).find("Grass/hide") != std::string::npos,
          "draw.overrides: the override persists while the baseline is neutral");

    // What on_world_connected must NOT do.
    b->capture_baseline();
    matter::jsondoc::Value after;
    props::save_scope(reg, props::Scope::World, after);
    CHECK(matter::jsondoc::write_json(after).find("Grass/hide") == std::string::npos,
          "draw.overrides: a baseline re-capture WOULD erase the override "
          "(hence the skip in on_world_connected)");

    dg->unbind_from(reg);

    // Rebuild rule: the same module set yields the same schema, so the session
    // has no reason to swap the group — and a CHANGED set yields a different
    // one, which is exactly when it must.
    std::unique_ptr<props::DynamicGroup> again =
        matter::build_draw_override_group({"Grass", "Tree"});
    CHECK(again->field_count() == 6 &&
              std::string(again->group().fields[0].name) == "Grass/hide",
          "draw.overrides: an unchanged module set reproduces the schema");
    std::unique_ptr<props::DynamicGroup> grown =
        matter::build_draw_override_group({"Grass", "Rock", "Tree"});
    CHECK(grown->field_count() == 9,
          "draw.overrides: a grown module set is a different schema");
}

}  // namespace

int main() {
    test_widget_selection();
    test_format();
    test_paths_and_labels();
    test_filter();
    test_tunables_categories();
    test_tunables_hide_duplicates();
    test_find_field();
    test_lod_ring_round_trip();
    test_streaming_pacing_fields();
    test_fog_group_baseline_flow();
    test_dynamic_group_renders_generically();
    test_draw_override_rows();
    test_draw_override_baseline_rule();
    return check_summary();
}
