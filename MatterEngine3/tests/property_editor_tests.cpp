// property_editor_tests.cpp — the ImGui-free half of the editor's generic
// property renderer (MatterEditor/src/property_editor.h): widget selection,
// format construction, path formatting and the Tunables filter. No ImGui
// context is created; only the header's inline decisions are exercised.

#include "check.h"

#include "../../MatterEditor/src/property_editor.h"
#include "../../MatterEditor/src/streaming_lod_prefs.h"
#define VIEWER_UI_STATUS_ONLY
#include "../../MatterEditor/src/ui.h"
#undef VIEWER_UI_STATUS_ONLY
#define VIEWER_FIFO_PROPERTY_HELPERS_ONLY
#include "../../MatterEditor/src/viewer_commands.h"
#undef VIEWER_FIFO_PROPERTY_HELPERS_ONLY
#include "matter/json_doc.h"
#include "matter/atmosphere_lighting.h"
#include "matter/world_definition.h"   // FogSettings — the render.fog group

#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
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

// Runtime availability (issue render-rt-and-dlss-runtime-toggles). draw_field
// reads both of these once and uses them for the badge and the BeginDisabled
// bracket, so asserting them here is asserting what the panel does.
void test_field_availability() {
    // Precedence. A forced value is what is actually in effect, so "env" beats
    // a capability the forced value may not even need.
    CHECK(std::strcmp(viewer::prop_field_badge(true, "no RT here", false),
                      "env") == 0,
          "badge: env outranks unavailable");
    CHECK(std::strcmp(viewer::prop_field_badge(false, "no RT here", false),
                      "n/a") == 0,
          "badge: unavailable when not env-forced");
    CHECK(std::strcmp(viewer::prop_field_badge(false, nullptr, true),
                      "reload") == 0,
          "badge: draft edit when otherwise editable");
    CHECK(std::strcmp(viewer::prop_field_badge(false, "no RT here", true),
                      "n/a") == 0,
          "badge: unavailable outranks reload — a draft you cannot apply is "
          "not the useful thing to say");
    CHECK(viewer::prop_field_badge(false, nullptr, false) == nullptr,
          "badge: none for a plain editable field");
    // An EMPTY reason is not a reason. A veto that returns "" for an available
    // field must not grey it.
    CHECK(viewer::prop_field_badge(false, "", false) == nullptr,
          "badge: empty reason means available");

    CHECK(viewer::prop_field_locked(true, nullptr), "locked: env-forced");
    CHECK(viewer::prop_field_locked(false, "no RT here"), "locked: unavailable");
    CHECK(!viewer::prop_field_locked(false, ""), "locked: empty reason is not");
    CHECK(!viewer::prop_field_locked(false, nullptr), "locked: plain field is not");
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

// The physical-atmosphere controls use ordinary property descriptors: this is
// the contract the editor registry must bind verbatim.  Keeping the test in the
// ImGui-free suite exercises parsing, sparse persistence and baseline rules
// without requiring a window or a renderer.
void test_physical_atmosphere_quality_property_contract() {
    static const char* const xy_labels[] = {"0.5x", "0.75x", "1x", "1.5x", "2x"};
    static const char* const depth_labels[] = {"64", "96", "128", "192", "256"};
    static const char* const shadow_resolution_labels[] = {"128", "256", "512"};
    static const auto atmosphere = props::group<AtmosphereSettings>(
        "render.atmosphere", "Atmosphere",
        props::prop(&AtmosphereSettings::sea_level_y, "sea_level_y").label("Sea level").range(-1000.0f, 10000.0f).units("m"),
        props::prop(&AtmosphereSettings::rayleigh_scale, "rayleigh_scale").label("Rayleigh scale").range(0.0f, 4.0f),
        props::prop(&AtmosphereSettings::mie_scale, "mie_scale").label("Mie scale").range(0.0f, 4.0f),
        props::prop(&AtmosphereSettings::mie_anisotropy, "mie_anisotropy").label("Mie anisotropy").range(-0.2f, 0.99f),
        props::prop(&AtmosphereSettings::ozone_scale, "ozone_scale").label("Ozone scale").range(0.0f, 4.0f),
        props::prop(&AtmosphereSettings::ground_albedo, "ground_albedo").label("Ground albedo").range(0.0f, 1.0f));
    static const auto volumetrics = props::group<VulkanVolumetricsSettings>(
        "render.volumetrics", "Volumetrics",
        props::prop(&VulkanVolumetricsSettings::froxel_xy_scale, "froxel_xy_scale").label("Froxel XY scale").enums(xy_labels, 5),
        props::prop(&VulkanVolumetricsSettings::froxel_depth_slices, "froxel_depth_slices").label("Froxel depth slices").enums(depth_labels, 5),
        props::prop(&VulkanVolumetricsSettings::local_sun_march_steps, "local_sun_march_steps").label("Local sun march steps").range(0.0f, 32.0f),
        props::prop(&VulkanVolumetricsSettings::local_sun_march_distance_m, "local_sun_march_distance_m").label("Local sun march distance").range(0.0f, 1000.0f).units("m"),
        props::prop(&VulkanVolumetricsSettings::multiple_scattering_orders, "multiple_scattering_orders").label("Multiple scattering orders").range(1.0f, 4.0f),
        props::prop(&VulkanVolumetricsSettings::multiple_scattering_strength, "multiple_scattering_strength").label("Multiple scattering strength").range(0.0f, 1.0f),
        props::prop(&VulkanVolumetricsSettings::powder_strength, "powder_strength").label("Powder strength").range(0.0f, 1.0f));
    static const auto shadows = props::group<CloudShadowSettings>(
        "render.cloud_shadows", "Cloud Shadows",
        props::prop(&CloudShadowSettings::near_resolution, "near_resolution").label("Near resolution").enums(shadow_resolution_labels, 3),
        props::prop(&CloudShadowSettings::near_coverage_m, "near_coverage_m").label("Near coverage").range(250.0f, 10000.0f).units("m"),
        props::prop(&CloudShadowSettings::filter_scale, "filter_scale").label("Filter scale").range(0.0f, 4.0f),
        props::prop(&CloudShadowSettings::update_fraction, "update_fraction").label("Update fraction").range(0.0625f, 1.0f));
    static const auto clouds = props::group<FogSettings>(
        "render.clouds", "Clouds",
        props::PropBuilder("layer1_detail_erosion", Type::Float,
            static_cast<uint32_t>(offsetof(FogSettings, clouds) + sizeof(CloudLayer) + offsetof(CloudLayer, detail_erosion)))
            .label("Detail erosion").range(0.0f, 1.0f));

    AtmosphereSettings live_atmosphere;
    VulkanVolumetricsSettings live_volumetrics;
    CloudShadowSettings live_shadows;
    FogSettings live_fog;
    props::Registry registry;
    props::Binding* atmosphere_binding = registry.get(registry.bind(atmosphere, &live_atmosphere, props::Scope::World));
    props::Binding* volumetrics_binding = registry.get(registry.bind(volumetrics, &live_volumetrics, props::Scope::World));
    props::Binding* shadow_binding = registry.get(registry.bind(shadows, &live_shadows, props::Scope::World));
    props::Binding* cloud_binding = registry.get(registry.bind(clouds, &live_fog, props::Scope::World));
    CHECK(atmosphere_binding && volumetrics_binding && shadow_binding && cloud_binding,
          "physical atmosphere: every group binds");

    struct Edit { const char* path; const char* text; };
    const Edit edits[] = {
        {"render.atmosphere.mie_anisotropy", "0.72"},
        {"render.volumetrics.froxel_xy_scale", "1.5x"},
        {"render.volumetrics.froxel_depth_slices", "192"},
        {"render.volumetrics.multiple_scattering_orders", "3"},
        {"render.cloud_shadows.near_resolution", "512"},
        {"render.cloud_shadows.update_fraction", "0.5"},
        {"render.clouds.layer1_detail_erosion", "0.4"},
    };
    for (const Edit& edit : edits) {
        props::Binding* binding = nullptr;
        const Desc* desc = nullptr;
        CHECK(props::resolve_field(registry, edit.path, binding, desc),
              "physical atmosphere: generic path resolves");
        CHECK(props::parse_and_set(*binding, *desc, edit.text),
              "physical atmosphere: generic set accepts valid text");
    }
    CHECK(live_atmosphere.mie_anisotropy == 0.72f &&
              live_volumetrics.froxel_xy_scale == FroxelXyScale::X1_5 &&
              live_volumetrics.froxel_depth_slices == FroxelDepthSlices::D192 &&
              live_volumetrics.multiple_scattering_orders == 3 &&
              live_shadows.near_resolution == 2 && live_shadows.update_fraction == 0.5f &&
              live_fog.clouds[1].detail_erosion == 0.4f,
          "physical atmosphere: generic values reach the live structs");

    props::Binding* binding = nullptr;
    const Desc* desc = nullptr;
    CHECK(props::resolve_field(registry, "render.volumetrics.froxel_xy_scale", binding, desc),
          "physical atmosphere: enum resolves");
    CHECK(props::parse_and_set(*binding, *desc, "2X"),
          "physical atmosphere: enum labels are case-insensitive");
    const FroxelXyScale before_invalid = live_volumetrics.froxel_xy_scale;
    CHECK(!props::parse_and_set(*binding, *desc, "not-a-scale"),
          "physical atmosphere: invalid enum text is refused");
    CHECK(live_volumetrics.froxel_xy_scale == before_invalid,
          "physical atmosphere: invalid enum text does not mutate storage");

    for (props::Binding* candidate : {atmosphere_binding, volumetrics_binding, shadow_binding, cloud_binding}) {
        CHECK(candidate->scope() == props::Scope::World,
              "physical atmosphere: every binding is World scoped");
        CHECK(!props::group_requires_reload(candidate->schema()),
              "physical atmosphere: every field stays live");
    }
    props::Registry neutral_registry;
    AtmosphereSettings neutral_atmosphere;
    VulkanVolumetricsSettings neutral_volumetrics;
    CloudShadowSettings neutral_shadows;
    FogSettings neutral_fog;
    neutral_registry.bind(atmosphere, &neutral_atmosphere, props::Scope::World);
    neutral_registry.bind(volumetrics, &neutral_volumetrics, props::Scope::World);
    neutral_registry.bind(shadows, &neutral_shadows, props::Scope::World);
    neutral_registry.bind(clouds, &neutral_fog, props::Scope::World);
    matter::jsondoc::Value neutral_doc;
    props::save_scope(neutral_registry, props::Scope::World, neutral_doc);
    CHECK(matter::jsondoc::write_json(neutral_doc).find("render.atmosphere") == std::string::npos &&
              matter::jsondoc::write_json(neutral_doc).find("render.clouds") == std::string::npos,
          "physical atmosphere: neutral defaults remain sparse");

    apply_volumetric_quality_preset(VolumetricQualityPreset::Improved,
                                    live_volumetrics, live_shadows);
    CHECK(identify_volumetric_quality_preset(live_volumetrics, live_shadows) ==
              VolumetricQualityPreset::Improved,
          "physical atmosphere: Improved identifies after applying through values");
    ++live_volumetrics.multiple_scattering_orders;
    CHECK(identify_volumetric_quality_preset(live_volumetrics, live_shadows) ==
              VolumetricQualityPreset::Custom,
          "physical atmosphere: a one-field edit derives Custom");
}

// The test target deliberately does not link the editor's ImGui/Vulkan closure.
// Pin the registry/UI seam at its source boundary instead, so a later refactor
// cannot make the headless schema test green while dropping the live bindings.
void test_physical_atmosphere_editor_source_contract() {
    const auto read = [](const char* path) {
        std::ifstream in(path);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    };
    const std::string props_source = read("../../MatterEditor/src/editor_props.cpp");
    const std::string editor_source = read("../../MatterEditor/src/property_editor.cpp");
    const std::string reset_source = read("../../MatterEditor/src/ui_lighting_controls.cpp");
    const std::string main_source = read("../../MatterEditor/src/main.cpp");
    const std::string ui_source = read("../../MatterEditor/src/ui.cpp");
    const std::string issue_source = read("../../MatterEditor/src/issue_reporter.cpp");
    const std::string session_source = read("../include/matter/world_session.h");
    const std::string renderer_source = read("../src/render/vk_scene_renderer.h");
    const std::string volumetrics_source = read("../src/render/vk_volumetrics.h");
    CHECK(!props_source.empty() && !editor_source.empty() && !reset_source.empty() &&
              !main_source.empty() && !ui_source.empty() && !issue_source.empty() &&
              !session_source.empty() && !renderer_source.empty() &&
              !volumetrics_source.empty(),
          "physical atmosphere: editor sources are available from the test cwd");
    for (const char* token : {"\"render.atmosphere\", \"Atmosphere\"",
                              "\"render.volumetrics\", \"Volumetrics\"",
                              "\"render.cloud_shadows\", \"Cloud Shadows\"",
                              "\"render.clouds\", \"Clouds\"",
                              "MATTER_ATMOSPHERE_MIE_ANISOTROPY",
                              "MATTER_FROXEL_XY_SCALE", "MATTER_FROXEL_DEPTH_SLICES",
                              "MATTER_CLOUD_SCATTER_ORDERS", "layer\" #i \"_detail_erosion"})
        CHECK(props_source.find(token) != std::string::npos,
              "physical atmosphere: production registry exposes required schema/env token");
    for (const char* token : {"atmosphere_ = registry_.bind", "cloud_shadows_ = registry_.bind",
                              "EditorProps::atmosphere", "EditorProps::cloud_shadows"})
        CHECK(props_source.find(token) != std::string::npos,
              "physical atmosphere: production registry binds/accesses World groups");
    const size_t lighting_begin = editor_source.find("void draw_lighting_contents");
    CHECK(lighting_begin != std::string::npos,
          "physical atmosphere: Lighting draw function is present");
    const std::string lighting_body = editor_source.substr(lighting_begin);
    size_t previous = 0;
    for (const char* token : {"props.lighting()", "props.atmosphere()", "props.volumetrics()",
                              "props.fog()", "props.cloud_shadows()", "props.clouds()"}) {
        const size_t found = lighting_body.find(token);
        CHECK(found != std::string::npos && found > previous,
              "physical atmosphere: Lighting draws all six groups in required order");
        previous = found;
    }
    for (const char* token : {"apply_volumetric_quality_preset", "apply_registered_fields",
                              "set_bool(binding", "set_enum(binding", "set_int(binding", "set_float(binding",
                              "Froxels requested/effective", "Froxel memory", "Last allocation error"})
        CHECK(editor_source.find(token) != std::string::npos,
              "physical atmosphere: Lighting presets/readouts stay on the property path");
    CHECK(reset_source.find("void reset_lighting_controls") != std::string::npos &&
              reset_source.find("stats.atmosphere = matter::AtmosphereSettings{};") != std::string::npos &&
              reset_source.find("stats.cloud_shadows = matter::CloudShadowSettings{};") != std::string::npos,
          "physical atmosphere: all physical lighting controls reset to the compiled baseline");
    CHECK(main_source.find("MATTER_CAPTURE_LIGHTING_UI") != std::string::npos &&
               main_source.find("ImGui::SetWindowFocus(\"Lighting\")") != std::string::npos,
          "physical atmosphere: screenshot automation can focus Lighting without a FIFO command");

    // Task 14 is intentionally append-only: existing STATS parsers consume the
    // legacy prefix by position, while the new lanes are available to newer
    // captures after the froxel generation field.
    for (const char* token : {"gpu_atmosphere_ms", "gpu_cloud_shadows_ms",
                              "gpu_vol_density_ms", "gpu_vol_scatter_ms",
                              "gpu_vol_integrate_ms", "cloud_shadow_memory_bytes"}) {
        CHECK(session_source.find(token) != std::string::npos,
              "physical atmosphere: FrameStats exposes timing and memory lanes");
        CHECK(main_source.find(token) != std::string::npos,
              "physical atmosphere: STATS propagates timing and memory lanes");
    }
    const size_t stats_begin = main_source.find("std::printf(\"STATS,");
    const std::string stats_body = main_source.substr(stats_begin);
    CHECK(stats_begin != std::string::npos &&
              stats_body.find("frame_stats.vol_resource_generation") <
                  stats_body.find("frame_stats.gpu_atmosphere_ms"),
          "physical atmosphere: STATS retains its legacy froxel suffix before new lanes");
    for (const char* token : {"kGpuZoneAtmosphere", "kGpuZoneCloudShadows",
                              "kGpuZoneVolDensity", "kGpuZoneVolScatter",
                              "kGpuZoneVolIntegrate", "kGpuZoneCount"})
        CHECK(renderer_source.find(token) != std::string::npos,
              "physical atmosphere: GPU timer zones append without renumbering legacy lanes");
    CHECK(volumetrics_source.find("VolumetricPassBoundary") != std::string::npos &&
              volumetrics_source.find("VolumetricPass") != std::string::npos,
          "physical atmosphere: volumetrics owns typed pass boundaries");
    for (const char* token : {"GPU atmosphere", "Cloud shadows", "Vol density",
                              "gpu_atmosphere_ms", "gpu_cloud_shadows_ms"})
        CHECK(editor_source.find(token) != std::string::npos ||
                  ui_source.find(token) != std::string::npos ||
                  issue_source.find(token) != std::string::npos,
              "physical atmosphere: Lighting, Performance, and issue reports retain timing lanes");
    CHECK(issue_source.find("cloud_shadow_memory_bytes") != std::string::npos,
          "physical atmosphere: issue reports retain actual cloud-shadow memory");
}

const auto& atmosphere_lighting_controls_schema() {
    using V = matter::VulkanLightingOverrides;
    static const auto group = props::group<V>(
        "render.lighting", "Lighting",
        props::prop(&V::sky_multiplier, "sky_multiplier")
            .label("Sky").range(0.0f, 4.0f),
        props::prop(&V::sky_tint, "sky_tint")
            .label("Sky tint").color(),
        props::prop(&V::day_ambient_multiplier, "day_ambient_multiplier")
            .label("Day ambient").range(0.0f, 4.0f)
            .env("MATTER_DAY_AMBIENT_MULTIPLIER")
            .doc("Physical irradiance at e >= +5 deg. Visible sky does not change ambient."),
        props::prop(&V::twilight_ambient_multiplier, "twilight_ambient_multiplier")
            .label("Twilight ambient").range(0.0f, 4.0f)
            .env("MATTER_TWILIGHT_AMBIENT_MULTIPLIER")
            .doc("Physical irradiance at e <= -6 deg. Ambient does not recolour the visible sky."),
        props::prop(&V::sky_irradiance_multiplier, "sky_irradiance_multiplier")
            .label("Sky irradiance").range(0.0f, 4.0f)
            .env("MATTER_SKY_IRRADIANCE_MULTIPLIER")
            .doc("Post-9SH physical ambient multiplier; independent of visible sky."),
        props::prop(&V::sunset_direct_ratio, "sunset_direct_ratio")
            .label("Sunset direct").range(0.0f, 1.0f)
            .env("MATTER_SUNSET_DIRECT_RATIO")
            .doc("Direct-world ratio at +5 deg. Sunset direct does not affect disc presentation."));
    return group;
}

const Desc& atmosphere_lighting_field(const char* name) {
    const props::Group& group = atmosphere_lighting_controls_schema();
    for (uint32_t i = 0; i < group.field_count; ++i)
        if (std::strcmp(group.fields[i].name, name) == 0) return group.fields[i];
    static Desc missing;
    return missing;
}

void test_atmosphere_lighting_property_defaults_metadata_and_round_trip() {
    VulkanLightingOverrides values{};
    props::Registry registry;
    props::Binding* binding = registry.get(registry.bind(
        atmosphere_lighting_controls_schema(), &values, props::Scope::World));
    CHECK(binding != nullptr, "new atmosphere lighting properties bind generically");

    jsondoc::Value old_document;
    CHECK(jsondoc::parse_json(
              "{\"version\":1,\"groups\":{\"render.lighting\":{\"sky_multiplier\":1.25}}}",
              old_document),
          "old lighting JSON parses without any new keys");
    props::load_scope(registry, props::Scope::World, old_document);
    CHECK(values.day_ambient_multiplier == 0.25f &&
              values.twilight_ambient_multiplier == 1.0f &&
              values.sky_irradiance_multiplier == 1.0f &&
              values.sunset_direct_ratio == 0.25f,
          "old lighting JSON preserves all four backwards defaults");

    struct Expected {
        const char* name;
        const char* label;
        float minimum;
        float maximum;
        const char* environment;
    };
    constexpr Expected expected[] = {
        {"day_ambient_multiplier", "Day ambient", 0.0f, 4.0f,
         "MATTER_DAY_AMBIENT_MULTIPLIER"},
        {"twilight_ambient_multiplier", "Twilight ambient", 0.0f, 4.0f,
         "MATTER_TWILIGHT_AMBIENT_MULTIPLIER"},
        {"sky_irradiance_multiplier", "Sky irradiance", 0.0f, 4.0f,
         "MATTER_SKY_IRRADIANCE_MULTIPLIER"},
        {"sunset_direct_ratio", "Sunset direct", 0.0f, 1.0f,
         "MATTER_SUNSET_DIRECT_RATIO"},
    };
    for (const Expected& item : expected) {
        const Desc& desc = atmosphere_lighting_field(item.name);
        CHECK(viewer::prop_field_path(atmosphere_lighting_controls_schema(), desc) ==
                  std::string("render.lighting.") + item.name,
              "new atmosphere lighting property has its exact generic path");
        CHECK(desc.label && std::strcmp(desc.label, item.label) == 0 &&
                  desc.has_range && desc.min == item.minimum &&
                  desc.max == item.maximum && desc.env &&
                  std::strcmp(desc.env, item.environment) == 0,
              "new atmosphere lighting property has exact label, range, and environment name");
        CHECK((desc.flags & props::ReadOnly) == 0,
              "new atmosphere lighting property remains live and editable");
    }

    CHECK(props::set_float(*binding,
                           atmosphere_lighting_field("day_ambient_multiplier"),
                           0.6f) &&
              props::set_float(*binding,
                               atmosphere_lighting_field("twilight_ambient_multiplier"),
                               1.7f) &&
              props::set_float(*binding,
                               atmosphere_lighting_field("sky_irradiance_multiplier"),
                               2.3f) &&
              props::set_float(*binding,
                               atmosphere_lighting_field("sunset_direct_ratio"),
                               0.4f),
          "all four atmosphere lighting properties edit through generic setters");
    jsondoc::Value saved;
    props::save_scope(registry, props::Scope::World, saved);
    VulkanLightingOverrides restored{};
    props::Registry restored_registry;
    restored_registry.bind(atmosphere_lighting_controls_schema(), &restored,
                           props::Scope::World);
    props::load_scope(restored_registry, props::Scope::World, saved);
    CHECK(restored.day_ambient_multiplier == 0.6f &&
              restored.twilight_ambient_multiplier == 1.7f &&
              restored.sky_irradiance_multiplier == 2.3f &&
              restored.sunset_direct_ratio == 0.4f,
          "all four atmosphere lighting properties round-trip generically");

    const std::string props_source = [] {
        std::ifstream in("../../MatterEditor/src/editor_props.cpp");
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }();
    for (const char* statement : {
             "Visible sky does not change ambient.",
             "Ambient does not recolour the visible sky.",
             "Sunset direct does not affect disc presentation."})
        CHECK(props_source.find(statement) != std::string::npos,
              "production lighting documentation states control independence explicitly");
}

void test_viewer_status_groups_are_exact_read_only_session_state() {
    viewer::ViewerSessionStatus session{};
    viewer::ViewerAtmosphereStatus atmosphere{};
    session.render_path = viewer::ViewerRenderPathStatus::NativeRt;
    session.presented_frame_serial = 4294967297ull;
    session.native_rt_available = true;
    atmosphere.generation_serial = 8589934593ull;
    atmosphere.resolved_elevation_deg = 5.0f;
    atmosphere.atmospheric_direct_base_rgb = {1.0f, 2.0f, 3.0f};

    props::Registry registry;
    props::Binding* session_binding = registry.get(registry.bind(
        viewer::viewer_session_status_group(), &session,
        props::Scope::Session));
    props::Binding* atmosphere_binding = registry.get(registry.bind(
        viewer::viewer_atmosphere_status_group(), &atmosphere,
        props::Scope::Session));
    CHECK(session_binding && atmosphere_binding,
          "viewer status groups bind at Session scope");

    struct Expected {
        const char* path;
        props::Type type;
    };
    const Expected expected[] = {
        {"viewer.session.render_path", Type::Enum},
        {"viewer.session.presented_frame_serial", Type::UInt64},
        {"viewer.session.native_rt_available", Type::Bool},
        {"viewer.atmosphere_status.generation_serial", Type::UInt64},
        {"viewer.atmosphere_status.resolved_elevation_deg", Type::Float},
        {"viewer.atmosphere_status.atmospheric_direct_base_rgb", Type::Float3},
        {"viewer.atmosphere_status.atmospheric_noon_direct_base_rgb", Type::Float3},
        {"viewer.atmosphere_status.direct_world_ratio", Type::Float},
        {"viewer.atmosphere_status.direct_base_rgb", Type::Float3},
        {"viewer.atmosphere_status.direct_world_sun_rgb", Type::Float3},
        {"viewer.atmosphere_status.sky_ambient_ratio", Type::Float},
        {"viewer.atmosphere_status.sky_display_modifier_rgb", Type::Float3},
        {"viewer.atmosphere_status.sky_irradiance_modifier_rgb", Type::Float3},
    };
    for (const Expected& item : expected) {
        props::Binding* binding = nullptr;
        const props::Desc* desc = nullptr;
        CHECK(props::resolve_field(registry, item.path, binding, desc) &&
                  binding && desc && desc->type == item.type,
              "viewer status field has exact path and type");
        CHECK(desc && (desc->flags & props::ReadOnly) != 0 &&
                  (desc->flags & props::NoSerialize) != 0,
              "every viewer status field is read-only and non-serializing");
    }

    const props::Desc& render_path = session_binding->schema().fields[0];
    CHECK(render_path.enum_count == 3 && render_path.enum_labels &&
              std::strcmp(render_path.enum_labels[0], "raster") == 0 &&
              std::strcmp(render_path.enum_labels[1], "native_rt") == 0 &&
              std::strcmp(render_path.enum_labels[2],
                          "native_rt_unavailable") == 0,
          "viewer render-path status labels are exact");

    const auto path_get = viewer::fifo_get_property(
        registry, "viewer.session.render_path");
    const auto serial_get = viewer::fifo_get_property(
        registry, "viewer.session.presented_frame_serial");
    const auto triple_get = viewer::fifo_get_property(
        registry,
        "viewer.atmosphere_status.atmospheric_direct_base_rgb");
    CHECK(path_get.success &&
              path_get.line == "get: viewer.session.render_path = native_rt",
          "FIFO get formats the render-path enum label exactly");
    CHECK(serial_get.success && serial_get.line ==
              "get: viewer.session.presented_frame_serial = 4294967297",
          "FIFO get formats UInt64 without truncation");
    CHECK(triple_get.success && triple_get.line ==
              "get: viewer.atmosphere_status.atmospheric_direct_base_rgb = (1, 2, 3)",
          "FIFO get formats status triples in the strict metrics grammar");

    const auto rejected = viewer::fifo_set_property(
        registry, "viewer.session.presented_frame_serial", "7");
    CHECK(!rejected.success && rejected.line ==
              "set: viewer.session.presented_frame_serial is read-only" &&
              session.presented_frame_serial == 4294967297ull,
          "FIFO setter rejects ReadOnly before parsing and leaves status intact");

    jsondoc::Value session_doc;
    props::save_scope(registry, props::Scope::Session, session_doc);
    jsondoc::Value world_doc;
    props::save_scope(registry, props::Scope::World, world_doc);
    const std::string session_json = jsondoc::write_json(session_doc);
    const std::string world_json = jsondoc::write_json(world_doc);
    CHECK(session_json.find("viewer.session") == std::string::npos &&
              session_json.find("viewer.atmosphere_status") == std::string::npos &&
              world_json.find("viewer.session") == std::string::npos &&
              world_json.find("viewer.atmosphere_status") == std::string::npos,
          "viewer status groups are absent from saved Session and World JSON");
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
    test_field_availability();
    test_lod_ring_round_trip();
    test_streaming_pacing_fields();
    test_fog_group_baseline_flow();
    test_dynamic_group_renders_generically();
    test_draw_override_rows();
    test_draw_override_baseline_rule();
    test_physical_atmosphere_quality_property_contract();
    test_physical_atmosphere_editor_source_contract();
    test_atmosphere_lighting_property_defaults_metadata_and_round_trip();
    test_viewer_status_groups_are_exact_read_only_session_state();
    return check_summary();
}
