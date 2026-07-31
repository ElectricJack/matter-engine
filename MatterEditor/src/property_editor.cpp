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

std::string value_text(const void* inst, const Desc& d) {
    char buf[128];
    const std::string fmt = prop_format_for(d);
    switch (d.type) {
        case Type::Float:
            std::snprintf(buf, sizeof(buf), fmt.c_str(), get_float(inst, d));
            break;
        case Type::Int:
            std::snprintf(buf, sizeof(buf), fmt.c_str(), get_int(inst, d));
            break;
        case Type::UInt:
            std::snprintf(buf, sizeof(buf), fmt.c_str(), get_uint(inst, d));
            break;
        case Type::Enum: {
            const int32_t v = get_enum(inst, d);
            if (d.enum_labels && v >= 0 && static_cast<uint32_t>(v) < d.enum_count)
                return d.enum_labels[v];
            std::snprintf(buf, sizeof(buf), "%d", v);
            break;
        }
        case Type::Bool:
            return get_bool(inst, d) ? "true" : "false";
        case Type::Float3:
        case Type::Color3: {
            float v[3];
            get_float3(inst, d, v);
            std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f", v[0], v[1], v[2]);
            break;
        }
        case Type::String:
            return get_string(inst, d);
    }
    return buf;
}

// `target` is the instance for a live group and the DRAFT for a RequiresReload
// one — the widgets are identical either way, which is the whole point of the
// draft being a full struct copy.
bool draw_field(Binding& b, void* target, uint32_t index, const Desc& d) {
    const Group& g = b.schema();
    const bool env = b.env_forced(index);
    const bool live = target == b.instance();
    const bool modified = b.baseline() && !fields_equal(target, b.baseline(), d);

    ImGui::PushID(static_cast<int>(index));

    if (modified) ImGui::PushStyleColor(ImGuiCol_Text, kModifiedColor);
    ImGui::TextUnformatted(prop_display_label(d));
    if (modified) ImGui::PopStyleColor();
    const bool name_hovered = ImGui::IsItemHovered();
    if (ImGui::BeginPopupContextItem("##fieldctx")) {
        if (ImGui::MenuItem("Reset to world/default", nullptr, false,
                            modified && !env && b.baseline())) {
            copy_field(target, b.baseline(), d);
            if (live) b.set_dirty(true);
        }
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
    } else if (!live) {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextDisabled("reload");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Consumed at world connect: the edit is held as a draft until "
                "Apply & Reload.");
    }

    ImGui::SameLine(kNameColumn);
    ImGui::SetNextItemWidth(-1.0f);

    const PropWidget widget = prop_widget_for(d);
    const std::string fmt = prop_format_for(d);
    const ImGuiSliderFlags slider_flags =
        (d.flags & Logarithmic) ? ImGuiSliderFlags_Logarithmic : 0;

    bool changed = false;
    if (env) ImGui::BeginDisabled();
    switch (widget) {
        case PropWidget::ReadOnlyText:
            ImGui::TextDisabled("%s", value_text(target, d).c_str());
            break;
        case PropWidget::FloatSlider: {
            float v = get_float(target, d);
            if (ImGui::SliderFloat("##v", &v, d.min, d.max, fmt.c_str(), slider_flags))
                changed = set_float(target, d, v);
            break;
        }
        case PropWidget::FloatDrag: {
            float v = get_float(target, d);
            if (ImGui::DragFloat("##v", &v, d.step > 0.0f ? d.step : 0.01f,
                                 0.0f, 0.0f, fmt.c_str()))
                changed = set_float(target, d, v);
            break;
        }
        case PropWidget::IntSlider: {
            int v = get_int(target, d);
            if (ImGui::SliderInt("##v", &v, static_cast<int>(d.min),
                                 static_cast<int>(d.max), fmt.c_str(), slider_flags))
                changed = set_int(target, d, v);
            break;
        }
        case PropWidget::IntDrag: {
            int v = get_int(target, d);
            if (ImGui::DragInt("##v", &v, d.step > 0.0f ? d.step : 1.0f, 0, 0,
                               fmt.c_str()))
                changed = set_int(target, d, v);
            break;
        }
        case PropWidget::UIntSlider: {
            uint32_t v = get_uint(target, d);
            const uint32_t lo = d.min <= 0.0f ? 0u : static_cast<uint32_t>(d.min);
            const uint32_t hi = d.max <= 0.0f ? 0u : static_cast<uint32_t>(d.max);
            if (ImGui::SliderScalar("##v", ImGuiDataType_U32, &v, &lo, &hi,
                                    fmt.c_str(), slider_flags))
                changed = set_uint(target, d, v);
            break;
        }
        case PropWidget::UIntDrag: {
            uint32_t v = get_uint(target, d);
            if (ImGui::DragScalar("##v", ImGuiDataType_U32, &v,
                                  d.step > 0.0f ? d.step : 1.0f, nullptr, nullptr,
                                  fmt.c_str()))
                changed = set_uint(target, d, v);
            break;
        }
        case PropWidget::Checkbox: {
            bool v = get_bool(target, d);
            if (ImGui::Checkbox("##v", &v)) changed = set_bool(target, d, v);
            break;
        }
        case PropWidget::EnumCombo: {
            int v = get_enum(target, d);
            if (d.enum_labels && d.enum_count > 0 &&
                ImGui::Combo("##v", &v, d.enum_labels, static_cast<int>(d.enum_count)))
                changed = set_enum(target, d, v);
            break;
        }
        case PropWidget::ColorEdit3: {
            float v[3];
            get_float3(target, d, v);
            if (ImGui::ColorEdit3("##v", v)) changed = set_float3(target, d, v);
            break;
        }
        case PropWidget::Float3Drag: {
            float v[3];
            get_float3(target, d, v);
            if (ImGui::DragFloat3("##v", v, d.step > 0.0f ? d.step : 0.01f,
                                  d.has_range ? d.min : 0.0f,
                                  d.has_range ? d.max : 0.0f, fmt.c_str()))
                changed = set_float3(target, d, v);
            break;
        }
        case PropWidget::TextInput: {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", get_string(target, d).c_str());
            if (ImGui::InputText("##v", buf, sizeof(buf)))
                changed = set_string(target, d, std::string(buf));
            break;
        }
    }
    if (env) ImGui::EndDisabled();

    // Only a LIVE edit is a persistable change; a draft edit becomes one when
    // Apply copies it into the instance.
    if (changed && live) b.set_dirty(true);

    ImGui::PopID();
    return changed;
}

}  // namespace

void* prop_edit_target(Binding& b) {
    if (!group_requires_reload(b.schema())) return b.instance();
    void* draft = ensure_draft(b);
    return draft ? draft : b.instance();
}

bool draw_group_fields(Binding& b, const char* filter) {
    const Group& g = b.schema();
    void* target = prop_edit_target(b);
    bool changed = false;
    for (uint32_t i = 0; i < g.field_count; ++i) {
        const Desc& d = g.fields[i];
        if (!prop_filter_matches_field(filter, d)) continue;
        changed |= draw_field(b, target, i, d);
    }
    return changed;
}

bool draw_draft_bar(Binding& b, const std::function<void()>& on_apply,
                    const char* apply_label) {
    if (!has_pending_draft(b)) return false;
    ImGui::PushID(b.schema().path);
    ImGui::TextColored(kModifiedColor, "Pending changes - not applied yet");
    bool applied = false;
    if (ImGui::Button(apply_label && *apply_label ? apply_label
                                                  : "Apply & Reload")) {
        apply_draft(b);
        applied = true;
        if (on_apply) on_apply();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) discard_draft(b);
    ImGui::PopID();
    return applied;
}

bool draw_group(Binding& b, const char* filter, bool default_open,
                const std::function<void()>* on_apply) {
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
    if (open) {
        changed = draw_group_fields(b, filter);
        static const std::function<void()> kNoApply;
        draw_draft_bar(b, on_apply ? *on_apply : kNoApply);
    }
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

    // What a RequiresReload group's Apply invokes. The panel does not know how
    // a reload is issued; EditorProps carries the closure main.cpp wired.
    const std::function<void()>& on_apply = props.reload_request();

    const char* filter = state.filter[0] ? state.filter : nullptr;
    for (Binding* b : sorted) {
        const Group& g = b->schema();
        if (!prop_filter_matches_group(filter, g)) continue;
        // A group matched by its own path/label shows every field; one matched
        // only through a field name shows just the matching fields.
        const bool whole_group = !filter || prop_filter_matches(filter, g.path) ||
                                 prop_filter_matches(filter, g.label);
        draw_group(*b, whole_group ? nullptr : filter, true, &on_apply);
    }
    if (sorted.empty()) ImGui::TextDisabled("No property groups registered.");
}

}  // namespace viewer
