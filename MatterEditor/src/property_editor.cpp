#include "property_editor.h"

#include "editor_props.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#include "imgui.h"

namespace viewer {
namespace {

using namespace matter::props;

// Column the widget starts at, so the name column can carry the
// modified/env decoration without fighting the widget's own label.
constexpr float kNameColumn = 178.0f;
// The Part Workbench params panel's "differs from default" amber
// (part_workbench.cpp draw_params_panel) — same visual language.
const ImVec4 kModifiedColor(1.0f, 0.85f, 0.35f, 1.0f);

std::string value_text(const Binding& b, const Desc& d) {
    char buf[128];
    const std::string fmt = prop_format_for(d);
    switch (d.type) {
        case Type::Float:
            std::snprintf(buf, sizeof(buf), fmt.c_str(), get_float(b, d));
            break;
        case Type::Int:
            std::snprintf(buf, sizeof(buf), fmt.c_str(), get_int(b, d));
            break;
        case Type::UInt:
            std::snprintf(buf, sizeof(buf), fmt.c_str(), get_uint(b, d));
            break;
        case Type::Enum: {
            const int32_t v = get_enum(b, d);
            if (d.enum_labels && v >= 0 && static_cast<uint32_t>(v) < d.enum_count)
                return d.enum_labels[v];
            std::snprintf(buf, sizeof(buf), "%d", v);
            break;
        }
        case Type::Bool:
            return get_bool(b, d) ? "true" : "false";
        case Type::Float3:
        case Type::Color3: {
            float v[3];
            get_float3(b, d, v);
            std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f", v[0], v[1], v[2]);
            break;
        }
        case Type::String:
            return get_string(b, d);
    }
    return buf;
}

bool draw_field(Binding& b, uint32_t index, const Desc& d) {
    const Group& g = b.schema();
    const bool env = b.env_forced(index);
    // RequiresReload draft flow is Stage 3: until then such a field is shown
    // read-only rather than silently editing a value nothing will consume.
    const bool locked = (d.flags & RequiresReload) != 0;
    const bool modified = !is_field_default(b, d);

    ImGui::PushID(static_cast<int>(index));

    if (modified) ImGui::PushStyleColor(ImGuiCol_Text, kModifiedColor);
    ImGui::TextUnformatted(prop_display_label(d));
    if (modified) ImGui::PopStyleColor();
    const bool name_hovered = ImGui::IsItemHovered();
    if (ImGui::BeginPopupContextItem("##fieldctx")) {
        if (ImGui::MenuItem("Reset to world/default", nullptr, false,
                            modified && !env && !locked))
            reset_field(b, d);
        if (ImGui::MenuItem("Copy path"))
            ImGui::SetClipboardText(prop_field_path(g, d).c_str());
        ImGui::EndPopup();
    }
    if (name_hovered && d.doc && *d.doc) ImGui::SetTooltip("%s", d.doc);
    if (env) {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextDisabled("env");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("forced by %s", d.env ? d.env : "?");
    } else if (locked) {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextDisabled("reload");
    }

    ImGui::SameLine(kNameColumn);
    ImGui::SetNextItemWidth(-1.0f);

    const PropWidget widget = prop_widget_for(d);
    const std::string fmt = prop_format_for(d);
    const ImGuiSliderFlags slider_flags =
        (d.flags & Logarithmic) ? ImGuiSliderFlags_Logarithmic : 0;

    bool changed = false;
    if (env || locked) ImGui::BeginDisabled();
    switch (widget) {
        case PropWidget::ReadOnlyText:
            ImGui::TextDisabled("%s", value_text(b, d).c_str());
            break;
        case PropWidget::FloatSlider: {
            float v = get_float(b, d);
            if (ImGui::SliderFloat("##v", &v, d.min, d.max, fmt.c_str(), slider_flags))
                changed = set_float(b, d, v);
            break;
        }
        case PropWidget::FloatDrag: {
            float v = get_float(b, d);
            if (ImGui::DragFloat("##v", &v, d.step > 0.0f ? d.step : 0.01f,
                                 0.0f, 0.0f, fmt.c_str()))
                changed = set_float(b, d, v);
            break;
        }
        case PropWidget::IntSlider: {
            int v = get_int(b, d);
            if (ImGui::SliderInt("##v", &v, static_cast<int>(d.min),
                                 static_cast<int>(d.max), fmt.c_str(), slider_flags))
                changed = set_int(b, d, v);
            break;
        }
        case PropWidget::IntDrag: {
            int v = get_int(b, d);
            if (ImGui::DragInt("##v", &v, d.step > 0.0f ? d.step : 1.0f, 0, 0,
                               fmt.c_str()))
                changed = set_int(b, d, v);
            break;
        }
        case PropWidget::UIntSlider: {
            uint32_t v = get_uint(b, d);
            const uint32_t lo = d.min <= 0.0f ? 0u : static_cast<uint32_t>(d.min);
            const uint32_t hi = d.max <= 0.0f ? 0u : static_cast<uint32_t>(d.max);
            if (ImGui::SliderScalar("##v", ImGuiDataType_U32, &v, &lo, &hi,
                                    fmt.c_str(), slider_flags))
                changed = set_uint(b, d, v);
            break;
        }
        case PropWidget::UIntDrag: {
            uint32_t v = get_uint(b, d);
            if (ImGui::DragScalar("##v", ImGuiDataType_U32, &v,
                                  d.step > 0.0f ? d.step : 1.0f, nullptr, nullptr,
                                  fmt.c_str()))
                changed = set_uint(b, d, v);
            break;
        }
        case PropWidget::Checkbox: {
            bool v = get_bool(b, d);
            if (ImGui::Checkbox("##v", &v)) changed = set_bool(b, d, v);
            break;
        }
        case PropWidget::EnumCombo: {
            int v = get_enum(b, d);
            if (d.enum_labels && d.enum_count > 0 &&
                ImGui::Combo("##v", &v, d.enum_labels, static_cast<int>(d.enum_count)))
                changed = set_enum(b, d, v);
            break;
        }
        case PropWidget::ColorEdit3: {
            float v[3];
            get_float3(b, d, v);
            if (ImGui::ColorEdit3("##v", v)) changed = set_float3(b, d, v);
            break;
        }
        case PropWidget::Float3Drag: {
            float v[3];
            get_float3(b, d, v);
            if (ImGui::DragFloat3("##v", v, d.step > 0.0f ? d.step : 0.01f,
                                  d.has_range ? d.min : 0.0f,
                                  d.has_range ? d.max : 0.0f, fmt.c_str()))
                changed = set_float3(b, d, v);
            break;
        }
        case PropWidget::TextInput: {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", get_string(b, d).c_str());
            if (ImGui::InputText("##v", buf, sizeof(buf)))
                changed = set_string(b, d, buf);
            break;
        }
    }
    if (env || locked) ImGui::EndDisabled();

    ImGui::PopID();
    return changed;
}

}  // namespace

bool draw_group_fields(Binding& b, const char* filter) {
    const Group& g = b.schema();
    bool changed = false;
    for (uint32_t i = 0; i < g.field_count; ++i) {
        const Desc& d = g.fields[i];
        if (!prop_filter_matches_field(filter, d)) continue;
        changed |= draw_field(b, i, d);
    }
    return changed;
}

bool draw_group(Binding& b, const char* filter, bool default_open) {
    const Group& g = b.schema();
    ImGui::PushID(g.path);
    char header[192];
    // ###path: the visible label can change without resetting the open state.
    std::snprintf(header, sizeof(header), "%s###%s",
                  g.label && *g.label ? g.label : (g.path ? g.path : "?"),
                  g.path ? g.path : "?");
    const bool open = ImGui::CollapsingHeader(
        header, default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]%s", prop_scope_name(b.scope()),
                        b.dirty() ? " *" : "");
    bool changed = false;
    if (open) changed = draw_group_fields(b, filter);
    ImGui::PopID();
    return changed;
}

void draw_tunables_contents(TunablesPanelState& state, EditorProps& props) {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Filter groups and fields...",
                             state.filter, sizeof(state.filter));

    const bool dirty = props.world_dirty();
    if (dirty) ImGui::PushStyleColor(ImGuiCol_Text, kModifiedColor);
    ImGui::Text("World%s", dirty ? " *" : "");
    if (dirty) ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginDisabled(!props.persisting() || props.world_path().empty());
    if (ImGui::Button("Save World")) props.save_world_now();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", props.persisting()
                                  ? "User scope autosaves"
                                  : "replay: persistence disabled");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("World: %s\nUser:  %s",
                          props.world_path().empty() ? "(none)"
                                                     : props.world_path().c_str(),
                          props.user_path().c_str());
    }
    ImGui::Separator();

    Registry& registry = props.registry();
    std::vector<Binding*> sorted;
    sorted.reserve(registry.size());
    for (size_t i = 0; i < registry.size(); ++i) sorted.push_back(&registry.at(i));
    std::sort(sorted.begin(), sorted.end(), [](const Binding* a, const Binding* c) {
        const char* pa = a->schema().path ? a->schema().path : "";
        const char* pc = c->schema().path ? c->schema().path : "";
        return std::strcmp(pa, pc) < 0;
    });

    const char* filter = state.filter[0] ? state.filter : nullptr;
    for (Binding* b : sorted) {
        const Group& g = b->schema();
        if (!prop_filter_matches_group(filter, g)) continue;
        // A group matched by its own path/label shows every field; one matched
        // only through a field name shows just the matching fields.
        const bool whole_group = !filter || prop_filter_matches(filter, g.path) ||
                                 prop_filter_matches(filter, g.label);
        draw_group(*b, whole_group ? nullptr : filter);
    }
    if (sorted.empty()) ImGui::TextDisabled("No property groups registered.");
}

}  // namespace viewer
