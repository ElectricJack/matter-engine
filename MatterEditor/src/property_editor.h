#pragma once

// Generic ImGui renderer over matter::props bindings (property-system design
// S6.1). One switch on Desc::type produces the widget; no panel ever hand-wires
// a slider to a settings-struct member again.
//
// This header is deliberately ImGui-free: the widget-kind / format / path
// decisions are pure functions of the Desc and are unit-testable without an
// ImGui context. Only the draw_* entry points need a live context.

#include "matter/props.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace viewer {

class EditorProps;

enum class PropWidget : uint8_t {
    ReadOnlyText,
    FloatSlider,
    FloatDrag,
    IntSlider,
    IntDrag,
    UIntSlider,
    UIntDrag,
    Checkbox,
    EnumCombo,
    ColorEdit3,
    Float3Drag,
    TextInput,
};

// has_range -> Slider, else Drag (properties_registry.cpp's widget_for_field
// policy). ReadOnly short-circuits everything: the value is printed, not edited.
inline PropWidget prop_widget_for(const matter::props::Desc& d) {
    using matter::props::Type;
    if (d.flags & matter::props::ReadOnly) return PropWidget::ReadOnlyText;
    switch (d.type) {
        case Type::Float:  return d.has_range ? PropWidget::FloatSlider : PropWidget::FloatDrag;
        case Type::Int:    return d.has_range ? PropWidget::IntSlider : PropWidget::IntDrag;
        case Type::UInt:   return d.has_range ? PropWidget::UIntSlider : PropWidget::UIntDrag;
        case Type::Bool:   return PropWidget::Checkbox;
        case Type::Enum:   return PropWidget::EnumCombo;
        case Type::Color3: return PropWidget::ColorEdit3;
        case Type::Float3: return PropWidget::Float3Drag;
        case Type::String: return PropWidget::TextInput;
    }
    return PropWidget::ReadOnlyText;
}

// Decimals follow the RANGE, not the value: a 0..0.5 relief cap needs three of
// them, a 5..10241 metre reach needs none. Unranged floats get three.
inline const char* prop_float_precision(const matter::props::Desc& d) {
    if (!d.has_range) return "%.3f";
    const float span = d.max - d.min;
    if (span >= 200.0f) return "%.0f";
    if (span >= 2.0f) return "%.2f";
    return "%.3f";
}

// "%.2f EV" — Desc::units folded into the printf format the widget uses.
inline std::string prop_format_for(const matter::props::Desc& d) {
    const bool integral = d.type == matter::props::Type::Int ||
                          d.type == matter::props::Type::UInt;
    std::string fmt = integral ? "%d" : prop_float_precision(d);
    if (d.type == matter::props::Type::UInt) fmt = "%u";
    if (d.units && *d.units) {
        fmt += ' ';
        fmt += d.units;
    }
    return fmt;
}

inline const char* prop_display_label(const matter::props::Desc& d) {
    return d.label && *d.label ? d.label : (d.name ? d.name : "?");
}

// "render.pom.steps" — what the context menu's "Copy path" yields and what a
// later generic FIFO `set` will accept.
inline std::string prop_field_path(const matter::props::Group& g,
                                   const matter::props::Desc& d) {
    std::string out = g.path ? g.path : "";
    out += '.';
    out += d.name ? d.name : "";
    return out;
}

// Field lookup by name, for panels that mix hand-written widgets with schema
// fields on the same struct (the LOD panel's ring strings). Null when unknown.
inline const matter::props::Desc* prop_find_field(const matter::props::Group& g,
                                                  const char* name) {
    if (!name) return nullptr;
    for (uint32_t i = 0; i < g.field_count; ++i)
        if (g.fields[i].name && !std::strcmp(g.fields[i].name, name))
            return &g.fields[i];
    return nullptr;
}

inline const char* prop_scope_name(matter::props::Scope s) {
    switch (s) {
        case matter::props::Scope::User:    return "User";
        case matter::props::Scope::Project: return "Project";
        case matter::props::Scope::World:   return "World";
        case matter::props::Scope::Session: return "Session";
    }
    return "?";
}

// Case-insensitive substring test used by the Tunables filter box. Kept here
// (rather than ImGuiTextFilter) so this header stays ImGui-free and the filter
// semantics are testable. An empty/null needle matches everything.
inline bool prop_filter_matches(const char* needle, const char* haystack) {
    if (!needle || !*needle) return true;
    if (!haystack) return false;
    const size_t nlen = std::strlen(needle);
    const size_t hlen = std::strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i + nlen <= hlen; ++i) {
        size_t k = 0;
        for (; k < nlen; ++k) {
            const unsigned char a = static_cast<unsigned char>(haystack[i + k]);
            const unsigned char n = static_cast<unsigned char>(needle[k]);
            if (std::tolower(a) != std::tolower(n)) break;
        }
        if (k == nlen) return true;
    }
    return false;
}

inline bool prop_filter_matches_field(const char* needle, const matter::props::Desc& d) {
    if (!needle || !*needle) return true;
    return prop_filter_matches(needle, d.name) || prop_filter_matches(needle, d.label);
}

// True when the group path/label matches, or any field name/label does.
inline bool prop_filter_matches_group(const char* needle, const matter::props::Group& g) {
    if (!needle || !*needle) return true;
    if (prop_filter_matches(needle, g.path)) return true;
    if (prop_filter_matches(needle, g.label)) return true;
    for (uint32_t i = 0; i < g.field_count; ++i)
        if (prop_filter_matches_field(needle, g.fields[i])) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Tunables categories. Groups are listed sorted by path, which already makes
// every group sharing a first path segment ("render.lighting",
// "render.volumetrics", ...) contiguous — so a category header is purely a
// question of "did the first segment change since the previous row". Kept
// ImGui-free, and next to the other pure Desc/Group decisions, for the same
// reason prop_filter_matches is.
// ---------------------------------------------------------------------------

// "render.lighting" -> "render". A path with no '.' IS its own category;
// null/empty yields an empty category (which still groups consistently).
inline std::string prop_path_category(const char* path) {
    if (!path) return std::string();
    const char* dot = std::strchr(path, '.');
    return dot ? std::string(path, static_cast<size_t>(dot - path))
               : std::string(path);
}

// Display text for a category segment. Two-letter segments are acronyms in
// this codebase ("vt" -> "VT"); anything longer is title-cased on its first
// character only, so "render" -> "Render" and a hand-cased segment keeps its
// own interior capitals.
inline std::string prop_category_label(const std::string& segment) {
    if (segment.empty()) return std::string("Other");
    std::string out = segment;
    if (out.size() <= 2) {
        for (char& c : out)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// The Tunables listing order: by group path. Categories fall out of it for
// free, which is why the panel needs no separate bucketing pass.
inline bool prop_group_order_before(const matter::props::Group& a,
                                    const matter::props::Group& b) {
    return std::strcmp(a.path ? a.path : "", b.path ? b.path : "") < 0;
}

// True when `path` opens a new category relative to the previously drawn
// `prev_path`. A null `prev_path` means "nothing drawn yet", which always
// opens one.
inline bool prop_starts_new_category(const char* prev_path, const char* path) {
    if (!prev_path) return true;
    return prop_path_category(prev_path) != prop_path_category(path);
}

// ---------------------------------------------------------------------------
// Renderers (need a live ImGui context)
// ---------------------------------------------------------------------------

// Where a panel's widgets must write: the live instance for a normal group,
// the (lazily allocated) draft for a RequiresReload one. Hand-written widgets
// that edit undescribed members of the same struct MUST go through this too,
// or an Apply would overwrite them from a stale draft.
void* prop_edit_target(matter::props::Binding& binding);

// Draws every field of the group inline — no Begin/End, no header. `filter`,
// when non-null and non-empty, hides fields whose name/label does not match.
// Returns true when any field changed this frame.
bool draw_group_fields(matter::props::Binding& binding, const char* filter = nullptr);

// "Pending changes" bar with Apply / Discard, drawn only while the group has a
// draft that differs from the live instance. Apply pushes the draft and then
// calls `on_apply` — the caller wires that to the reload command. Returns true
// on the frame Apply was pressed.
bool draw_draft_bar(matter::props::Binding& binding,
                    const std::function<void()>& on_apply,
                    const char* apply_label = "Apply & Reload");

// CollapsingHeader (label + scope tag + dirty marker) wrapping the fields, plus
// the draft bar for RequiresReload groups.
bool draw_group(matter::props::Binding& binding, const char* filter = nullptr,
                bool default_open = true,
                const std::function<void()>* on_apply = nullptr);

// Lighting window body (call inside Begin/End). Draws render.lighting —
// including the sun/sky tints — and render.volumetrics, plus the
// "Reset to World" baseline restore that used to sit under the lighting
// sliders in Viewer Debug.
//
// WORKSTREAM-2 SEAM: `render.fog`, once that group exists, belongs here
// directly after render.volumetrics (and in the reset button's group list
// below). Nothing else needs to change — see the marked spot in the body.
void draw_lighting_contents(EditorProps& props);

struct TunablesPanelState {
    char filter[128] = {};
};

// Tunables window body (call inside Begin/End) — the catch-all panel every
// registered group appears in, sorted by path.
void draw_tunables_contents(TunablesPanelState& state, EditorProps& props);

}  // namespace viewer
