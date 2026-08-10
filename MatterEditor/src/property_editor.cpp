#include "property_editor.h"

#include "editor_props.h"
#include "matter/world_definition.h"  // FogSettings, compact_clouds
#include "ui.h"  // ViewerStats readouts in the Lighting panel

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

template <class T>
void apply_registered_fields(Binding& binding, const T& target) {
    for (uint32_t i = 0; i < binding.schema().field_count; ++i) {
        if (binding.env_forced(i)) continue;  // layer 5 remains above a preset click
        const Desc& d = binding.schema().fields[i];
        const void* source = &target;
        switch (d.type) {
        case Type::Bool: set_bool(binding, d, get_bool(source, d)); break;
        case Type::Enum: set_enum(binding, d, get_enum(source, d)); break;
        case Type::Int: set_int(binding, d, get_int(source, d)); break;
        case Type::UInt: set_uint(binding, d, get_uint(source, d)); break;
        case Type::UInt64: set_uint64(binding, d, get_uint64(source, d)); break;
        case Type::Float: set_float(binding, d, get_float(source, d)); break;
        default: break;
        }
    }
}

void draw_volumetric_quality_presets(EditorProps& props) {
    Binding* vol_binding = props.volumetrics();
    Binding* shadow_binding = props.cloud_shadows();
    if (!vol_binding || !shadow_binding) return;

    const matter::VolumetricQualityPreset selected =
        matter::identify_volumetric_quality_preset(
            *static_cast<const matter::VulkanVolumetricsSettings*>(vol_binding->instance()),
            *static_cast<const matter::CloudShadowSettings*>(shadow_binding->instance()));
    struct Button { matter::VolumetricQualityPreset preset; const char* label; };
    const Button buttons[] = {
        {matter::VolumetricQualityPreset::CurrentCost, "Current cost"},
        {matter::VolumetricQualityPreset::Improved, "Improved"},
        {matter::VolumetricQualityPreset::High, "High"},
        {matter::VolumetricQualityPreset::Ultra, "Ultra"},
    };
    for (const Button& button : buttons) {
        if (&button != buttons) ImGui::SameLine();
        if (ImGui::Button(button.label)) {
            matter::VulkanVolumetricsSettings target_vol =
                *static_cast<const matter::VulkanVolumetricsSettings*>(vol_binding->instance());
            matter::CloudShadowSettings target_shadows =
                *static_cast<const matter::CloudShadowSettings*>(shadow_binding->instance());
            matter::apply_volumetric_quality_preset(button.preset, target_vol, target_shadows);
            apply_registered_fields(*vol_binding, target_vol);
            apply_registered_fields(*shadow_binding, target_shadows);
        }
    }
    const char* selected_name = selected == matter::VolumetricQualityPreset::CurrentCost ? "Current cost" :
                                selected == matter::VolumetricQualityPreset::Improved ? "Improved" :
                                selected == matter::VolumetricQualityPreset::High ? "High" :
                                selected == matter::VolumetricQualityPreset::Ultra ? "Ultra" : "Custom";
    ImGui::TextDisabled("Preset: %s", selected_name);
}

void draw_volumetric_readouts(const ViewerStats& stats) {
    ImGui::TextDisabled("Froxels requested/effective: %u x %u x %u / %u x %u x %u",
                        stats.requested_froxel.width, stats.requested_froxel.height,
                        stats.requested_froxel.depth, stats.effective_froxel.width,
                        stats.effective_froxel.height, stats.effective_froxel.depth);
    ImGui::TextDisabled("Froxel memory: %.2f MiB   Cloud shadows: %.2f MiB",
                        static_cast<double>(stats.froxel_bytes) / (1024.0 * 1024.0),
                        static_cast<double>(stats.cloud_shadow_bytes) / (1024.0 * 1024.0));
    ImGui::TextDisabled("Last allocation error: %s",
                        stats.last_volumetric_allocation_error.empty() ? "none" :
                        stats.last_volumetric_allocation_error.c_str());
}

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
        case Type::UInt64:
            std::snprintf(buf, sizeof(buf), "%llu",
                          static_cast<unsigned long long>(get_uint64(inst, d)));
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
bool draw_field(Binding& b, void* target, uint32_t index, const Desc& d,
                const char* unavailable) {
    const Group& g = b.schema();
    const bool env = b.env_forced(index);
    const bool live = target == b.instance();
    const bool modified = b.baseline() && !fields_equal(target, b.baseline(), d);
    // One decision, two consumers (badge text and the BeginDisabled bracket),
    // both from the pure helpers in the header so the tests exercise the same
    // rule this draw does.
    const bool locked = prop_field_locked(env, unavailable);
    const char* badge = prop_field_badge(env, unavailable, !live);

    ImGui::PushID(static_cast<int>(index));

    if (modified) ImGui::PushStyleColor(ImGuiCol_Text, kModifiedColor);
    ImGui::TextUnformatted(prop_display_label(d));
    if (modified) ImGui::PopStyleColor();
    const bool name_hovered = ImGui::IsItemHovered();
    if (ImGui::BeginPopupContextItem("##fieldctx")) {
        if (ImGui::MenuItem("Reset to world/default", nullptr, false,
                            modified && !locked && b.baseline())) {
            copy_field(target, b.baseline(), d);
            if (live) b.set_dirty(true);
        }
        if (ImGui::MenuItem("Copy path"))
            ImGui::SetClipboardText(prop_field_path(g, d).c_str());
        ImGui::EndPopup();
    }
    if (name_hovered && d.doc && *d.doc) ImGui::SetTooltip("%s", d.doc);
    // The badge carries the tooltip, not the widget: the widget below sits
    // inside BeginDisabled for the env and unavailable cases, and a disabled
    // ImGui item does not report hover, so a tooltip hung on it would never
    // appear in exactly the two states that most need explaining.
    if (badge) {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextDisabled("%s", badge);
        if (ImGui::IsItemHovered()) {
            if (env)
                ImGui::SetTooltip("forced by %s", d.env ? d.env : "?");
            else if (unavailable && *unavailable)
                ImGui::SetTooltip("Unavailable on this machine: %s",
                                  unavailable);
            else
                ImGui::SetTooltip(
                    "Consumed at world connect: the edit is held as a draft "
                    "until Apply & Reload.");
        }
    }

    ImGui::SameLine(kNameColumn);
    ImGui::SetNextItemWidth(-1.0f);

    const PropWidget widget = prop_widget_for(d);
    const std::string fmt = prop_format_for(d);
    const ImGuiSliderFlags slider_flags =
        (d.flags & Logarithmic) ? ImGuiSliderFlags_Logarithmic : 0;

    bool changed = false;
    if (locked) ImGui::BeginDisabled();
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
            // Sized past the current value so long strings are never truncated
            // by the edit buffer; headroom grows with the value as it is typed.
            const std::string cur = get_string(target, d);
            std::vector<char> buf(cur.size() + 256 < 512 ? 512 : cur.size() + 256);
            std::snprintf(buf.data(), buf.size(), "%s", cur.c_str());
            if (ImGui::InputText("##v", buf.data(), buf.size()))
                changed = set_string(target, d, std::string(buf.data()));
            break;
        }
    }
    if (locked) ImGui::EndDisabled();

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

bool draw_group_fields(Binding& b, const char* filter,
                       const PropFieldVeto* veto) {
    const Group& g = b.schema();
    // Field IDs are per-group indices; scope them by group path so panels that
    // call draw_group_fields directly for several groups in one window don't
    // collide (draw_group's own PushID nests harmlessly above this one).
    ImGui::PushID(g.path);
    void* target = prop_edit_target(b);
    bool changed = false;
    for (uint32_t i = 0; i < g.field_count; ++i) {
        const Desc& d = g.fields[i];
        if (!prop_filter_matches_field(filter, d)) continue;
        changed |= draw_field(b, target, i, d,
                              veto && *veto ? (*veto)(d) : nullptr);
    }
    ImGui::PopID();
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
                const std::function<void()>* on_apply,
                const PropFieldVeto* veto) {
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
        changed = draw_group_fields(b, filter, veto);
        static const std::function<void()> kNoApply;
        draw_draft_bar(b, on_apply ? *on_apply : kNoApply);
    }
    ImGui::PopID();
    return changed;
}

bool draw_draw_overrides_section(Binding& b, const char* filter) {
    const Group& g = b.schema();
    // Never a RequiresReload group: every one of these takes effect within a
    // frame or two (hide invalidates the instance expansion, distance and bias
    // ride a per-part GPU lane the cull dispatch re-reads every frame), so the
    // live instance IS the edit target.
    void* target = b.instance();
    const std::vector<DrawOverrideRow> rows = draw_override_rows(g);
    if (rows.empty()) {
        ImGui::TextDisabled("No modules in this world.");
        return false;
    }

    auto field_modified = [&](int index) {
        return index >= 0 && b.baseline() &&
               !fields_equal(target, b.baseline(), g.fields[index]);
    };

    ImGui::PushID(g.path);
    bool changed = false;

    size_t overridden = 0;
    for (const DrawOverrideRow& row : rows)
        if (field_modified(row.hide) || field_modified(row.max_dist) ||
            field_modified(row.lod_bias))
            ++overridden;
    if (overridden != 0) {
        ImGui::TextColored(kModifiedColor, "%zu module%s overridden",
                           overridden, overridden == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::Button("Reset all")) {
            reset_group(b);
            changed = true;
        }
    } else {
        ImGui::TextDisabled("No overrides - drawing everything as baked.");
    }
    ImGui::SetItemTooltip(
        "A view-time filter only: baked artifacts, content hashes and cache "
        "keys are untouched, so nothing here triggers a rebake.");

    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_ScrollY;
    // Bounded height so a 30-module world does not push the streaming section
    // off the panel; the table scrolls internally instead.
    const float row_h = ImGui::GetFrameHeightWithSpacing();
    const float body_h = std::min(rows.size() + 1.0f, 12.0f) * row_h;
    if (!ImGui::BeginTable("##drawoverrides", 4, flags, ImVec2(0.0f, body_h))) {
        ImGui::PopID();
        return changed;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Hide", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFrameHeight() + 4.0f);
    ImGui::TableSetupColumn("Max dist", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("LOD bias", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableHeadersRow();

    for (size_t r = 0; r < rows.size(); ++r) {
        const DrawOverrideRow& row = rows[r];
        if (filter && *filter && !prop_filter_matches(filter, row.module.c_str()))
            continue;
        const bool row_modified = field_modified(row.hide) ||
                                  field_modified(row.max_dist) ||
                                  field_modified(row.lod_bias);
        ImGui::PushID(static_cast<int>(r));
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (row_modified) ImGui::PushStyleColor(ImGuiCol_Text, kModifiedColor);
        ImGui::TextUnformatted(row.module.c_str());
        if (row_modified) ImGui::PopStyleColor();
        if (ImGui::BeginPopupContextItem("##rowctx")) {
            if (ImGui::MenuItem("Reset module", nullptr, false, row_modified)) {
                if (row.hide >= 0) reset_field(b, g.fields[row.hide]);
                if (row.max_dist >= 0) reset_field(b, g.fields[row.max_dist]);
                if (row.lod_bias >= 0) reset_field(b, g.fields[row.lod_bias]);
                changed = true;
            }
            if (ImGui::MenuItem("Copy path") && row.hide >= 0)
                ImGui::SetClipboardText(
                    prop_field_path(g, g.fields[row.hide]).c_str());
            ImGui::EndPopup();
        }

        ImGui::TableSetColumnIndex(1);
        if (row.hide >= 0) {
            bool hide = get_bool(target, g.fields[row.hide]);
            if (ImGui::Checkbox("##hide", &hide)) {
                if (set_bool(target, g.fields[row.hide], hide)) {
                    b.set_dirty(true);
                    changed = true;
                }
            }
            ImGui::SetItemTooltip("%s", g.fields[row.hide].doc);
        }

        ImGui::TableSetColumnIndex(2);
        if (row.max_dist >= 0) {
            const Desc& d = g.fields[row.max_dist];
            float v = get_float(target, d);
            ImGui::SetNextItemWidth(-1.0f);
            // Unranged drag, not a slider: 0 means "unlimited" and has to stay
            // one click away, which a logarithmic slider out to the far plane
            // cannot offer.
            if (ImGui::DragFloat("##dist", &v, 5.0f, 0.0f,
                                 matter::kDrawOverrideMaxDistance,
                                 v <= 0.0f ? "unlimited" : "%.0f m")) {
                if (v < 0.0f) v = 0.0f;
                if (set_float(target, d, v)) {
                    b.set_dirty(true);
                    changed = true;
                }
            }
            ImGui::SetItemTooltip("%s", d.doc);
        }

        ImGui::TableSetColumnIndex(3);
        if (row.lod_bias >= 0) {
            const Desc& d = g.fields[row.lod_bias];
            float v = get_float(target, d);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##bias", &v, d.min, d.max, "%.2fx",
                                   ImGuiSliderFlags_Logarithmic)) {
                if (set_float(target, d, v)) {
                    b.set_dirty(true);
                    changed = true;
                }
            }
            ImGui::SetItemTooltip("%s", d.doc);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::PopID();
    return changed;
}

bool draw_cloud_layers_section(Binding& b, const char* filter) {
    const Group& g = b.schema();
    const uint32_t layers = static_cast<uint32_t>(matter::kMaxCloudLayers);
    if (layers == 0 || g.field_count % layers != 0) {
        ImGui::TextDisabled("Cloud schema is not a whole number of layers.");
        return false;
    }
    const uint32_t per_layer = g.field_count / layers;
    // Index 0 within a layer is `enabled` — see CLOUD_LAYER_FIELDS in
    // editor_props.cpp. Asserted rather than searched by name because the
    // whole point of slicing by index is not to parse names.
    void* target = prop_edit_target(b);

    ImGui::PushID(g.path);
    bool changed = false;

    uint32_t live = 0;
    for (uint32_t l = 0; l < layers; ++l)
        if (get_bool(target, g.fields[l * per_layer])) ++live;

    if (live == 0) {
        ImGui::TextDisabled("No cloud decks - ground fog only.");
    } else {
        ImGui::Text("%u deck%s", live, live == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::Button("Clear all")) {
            reset_group(b);
            changed = true;
        }
    }
    ImGui::SetItemTooltip(
        "Each deck exists ONLY between its Min and Max height. Overlapping "
        "decks add their extinction. The enabled decks are compiled into the "
        "density shader as a specialization, so switching one on rebinds a "
        "different pipeline rather than costing a branch per froxel.");

    for (uint32_t l = 0; l < layers; ++l) {
        const uint32_t base = l * per_layer;
        const Desc& enabled_desc = g.fields[base];
        const bool on = get_bool(target, enabled_desc);

        // Any field in this layer differing from the world's baseline colours
        // the header, so a collapsed deck still advertises that it was edited.
        bool layer_modified = false;
        if (b.baseline()) {
            for (uint32_t f = 0; f < per_layer; ++f)
                if (!fields_equal(target, b.baseline(), g.fields[base + f]))
                    layer_modified = true;
        }

        ImGui::PushID(static_cast<int>(l));
        char header[96];
        std::snprintf(header, sizeof(header), "Layer %u%s###cloudlayer%u",
                      l, on ? "" : " (off)", l);
        if (layer_modified) ImGui::PushStyleColor(ImGuiCol_Text, kModifiedColor);
        const bool open = ImGui::CollapsingHeader(
            header, on ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        if (layer_modified) ImGui::PopStyleColor();

        if (open) {
            ImGui::Indent();
            for (uint32_t f = 0; f < per_layer; ++f) {
                const uint32_t index = base + f;
                const Desc& d = g.fields[index];
                if (!prop_filter_matches_field(filter, d)) continue;
                // Min height (f==1) and Max height (f==2) stay enabled while
                // the deck is off: they are the ONLY fields that can turn a
                // degenerate max_height<=min_height layer into one that CAN
                // be switched on, and disabling them was the deadlock in the
                // editor-cloud-deck-cannot-be-enabled issue — a fresh 0/0
                // layer had no field left to fix before the enable toggle
                // just bounced back off. Everything else here is cosmetic
                // while off (coverage, falloff shape, noise, wind) and stays
                // behind the disabled bracket: the values are still visible
                // (so a deck can be read without switching it on) but cannot
                // be edited into a state nothing renders.
                const bool row_disabled = !on && f != 0 && f != 1 && f != 2;
                if (row_disabled) ImGui::BeginDisabled();
                // No veto: PropFieldVeto carries a per-field "this machine
                // cannot" reason (render.gpu's RT/DLSS capability checks). A
                // cloud deck is pure CPU-authored state with no device
                // capability behind it, so there is nothing to veto here — the
                // only disable is the deck-off bracket above.
                changed |= draw_field(b, target, index, d, nullptr);
                if (row_disabled) ImGui::EndDisabled();
            }
            ImGui::Unindent();

            // A layer whose `enabled` just flipped false -> true this frame
            // may still be degenerate (a fresh CloudLayer{} default, or the
            // user only got as far as fixing one of Min/Max height before
            // ticking the box) — seed it so it is immediately visible rather
            // than bouncing back off next frame (fault A) or sitting there
            // enabled and invisible because max_density is still 0. Checked
            // BEFORE compact_clouds runs below so the freshly seeded values
            // are what compaction sees. A no-op on an already well-formed
            // layer, so this never stomps an authored deck.
            //
            // UI-toggle only: MATTER_CLOUD_LAYER<i> and the generic FIFO
            // `set render.clouds.layerN_enabled true` path both write
            // straight through the generic props setter and do not call
            // seed_default_cloud_layer, so enabling a degenerate layer
            // through either of those still leaves it degenerate.
            const bool on_now = get_bool(target, enabled_desc);
            if (!on && on_now && target == b.instance()) {
                matter::seed_default_cloud_layer(
                    static_cast<matter::FogSettings*>(target)->clouds[l]);
                b.set_dirty(true);
            }
        }
        ImGui::PopID();
    }

    // Compaction has to happen the instant a middle deck is switched off: the
    // density pipeline is chosen by a PREFIX count, so a hole would make the
    // shader render the deck behind it with the wrong index — or drop it. Only
    // on a real change, so this never fights a drag in progress.
    if (changed && target == b.instance())
        matter::compact_clouds(*static_cast<matter::FogSettings*>(target));

    ImGui::PopID();
    return changed;
}

void draw_lighting_contents(EditorProps& props) {
    // Neither group is RequiresReload, so the closure is inert today; passing
    // it anyway means a future reload-gated lighting field just works.
    const std::function<void()>& on_apply = props.reload_request();
    // Keep screenshot evidence legible at a 720 px desktop height: the preset
    // strip is drawn outside its group, while the long unrelated sections stay
    // collapsed. Interactive layouts are untouched.
    const bool compact_capture = std::getenv("MATTER_CAPTURE_LIGHTING_UI") != nullptr;

    // One reset for the whole panel rather than the old per-group button under
    // the lighting sliders: every group here is Scope::World, and their
    // baselines are the same layer-2 snapshot captured at connect (S4), so
    // "back to what the world authored" is one concept, not three.
    if (ImGui::Button("Reset to World")) {
        if (Binding* b = props.lighting()) reset_group(*b);
        if (Binding* b = props.atmosphere()) reset_group(*b);
        if (Binding* b = props.volumetrics()) reset_group(*b);
        if (Binding* b = props.fog()) reset_group(*b);
        if (Binding* b = props.cloud_shadows()) reset_group(*b);
        if (Binding* b = props.clouds()) reset_group(*b);
    }
    ImGui::SetItemTooltip(
        "Restores render.lighting, render.atmosphere, render.volumetrics, "
        "render.fog, render.cloud_shadows and render.clouds to the values "
        "the world script authored at connect.");
    ImGui::Separator();

    // note_panel_home: declared right at the draw call for each group, per
    // group, so a group added to (or dropped from) this panel automatically
    // updates what Tunables hides — see EditorProps::panel_home.
    if (Binding* b = props.lighting()) {
        props.note_panel_home(b->schema().path, "Lighting");
        draw_group(*b, nullptr, !compact_capture, &on_apply);
    }
    if (Binding* b = props.atmosphere()) {
        props.note_panel_home(b->schema().path, "Lighting");
        draw_group(*b, nullptr, true, &on_apply);
    }
    if (Binding* b = props.volumetrics()) {
        props.note_panel_home(b->schema().path, "Lighting");
        draw_volumetric_quality_presets(props);
        if (const ViewerStats* stats = props.viewer_stats())
            draw_volumetric_readouts(*stats);
        draw_group(*b, nullptr, !compact_capture, &on_apply);
    }
    // render.fog sits directly after render.volumetrics because the two are
    // read together: volumetrics says whether the froxel volume is marched at
    // all and how, fog says what is in it. They no longer SHARE any concept —
    // the five multipliers that used to shadow five fog fields are gone
    // (issue 80c66789), so each row below appears exactly once in this panel.
    if (Binding* b = props.fog()) {
        props.note_panel_home(b->schema().path, "Lighting");
        draw_group(*b, nullptr, !compact_capture, &on_apply);
    }
    if (Binding* b = props.cloud_shadows()) {
        props.note_panel_home(b->schema().path, "Lighting");
        draw_group(*b, nullptr, true, &on_apply);
    }
    // render.clouds last, and with its own renderer: it is 48 fields, and the
    // flat list draw_group would produce is unreadable. It follows render.fog
    // because a deck sits on top of the ground fog both literally and in the
    // arithmetic — extinction sums.
    if (Binding* b = props.clouds()) {
        props.note_panel_home(b->schema().path, "Lighting");
        ImGui::PushID(b->schema().path);
        char header[96];
        std::snprintf(header, sizeof(header), "%s###%s", b->schema().label,
                      b->schema().path);
        const bool open =
            ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]%s", prop_scope_name(b->scope()),
                            b->dirty() ? " *" : "");
        if (open) draw_cloud_layers_section(*b, nullptr);
        ImGui::PopID();
    }
}

void draw_tunables_contents(TunablesPanelState& state, EditorProps& props) {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Filter groups and fields...",
                             state.filter, sizeof(state.filter));

    const bool filtering = state.filter[0] != '\0';
    // Disabled (not skipped): the checkbox still shows its stored value while
    // filtering, it just cannot be clicked — ImGui::Checkbox does not write
    // through a disabled widget, so state.hide_duplicates survives a search
    // untouched and is exactly what re-applies once the filter is cleared.
    ImGui::BeginDisabled(filtering);
    ImGui::Checkbox("Hide properties shown in other panels",
                    &state.hide_duplicates);
    ImGui::EndDisabled();
    if (filtering && ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Search looks at every registered property regardless of this "
            "setting. Clear the filter to restore it.");
    const bool hide_duplicates =
        prop_effective_hide_duplicates(state.hide_duplicates, filtering);

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
        return prop_group_order_before(a->schema(), c->schema());
    });

    // What a RequiresReload group's Apply invokes. The panel does not know how
    // a reload is issued; EditorProps carries the closure main.cpp wired.
    const std::function<void()>& on_apply = props.reload_request();

    const char* filter = state.filter[0] ? state.filter : nullptr;
    // Category headers: the sort above already put same-category groups
    // together, so the header is emitted whenever the first path segment
    // changes AMONG THE ROWS THAT SURVIVED THE FILTER — a category whose every
    // group was filtered out must not leave a dangling header behind. A group
    // hidden by the de-duplication checkbox is skipped the SAME way (the
    // `continue` below runs before prev_path/any_drawn are touched), so a
    // category made up entirely of Performance/Lighting/Console/Viewer-Debug
    // groups produces no header either — see prop_group_is_duplicate_hidden.
    const char* prev_path = nullptr;
    bool any_drawn = false;
    bool any_hidden_as_duplicate = false;
    for (Binding* b : sorted) {
        const Group& g = b->schema();
        if (!prop_filter_matches_group(filter, g)) continue;
        if (prop_group_is_duplicate_hidden(hide_duplicates,
                                           props.panel_home(g.path) != nullptr)) {
            any_hidden_as_duplicate = true;
            continue;
        }
        if (prop_starts_new_category(prev_path, g.path)) {
            ImGui::SeparatorText(
                prop_category_label(prop_path_category(g.path)).c_str());
        }
        prev_path = g.path;
        any_drawn = true;
        // A group matched by its own path/label shows every field; one matched
        // only through a field name shows just the matching fields.
        const bool whole_group = !filter || prop_filter_matches(filter, g.path) ||
                                 prop_filter_matches(filter, g.label);
        // draw.overrides carries three fields per module; the generic renderer
        // would emit ~90 full-width rows for an alpine world. It gets the same
        // compact per-module table the Performance panel shows, under a normal
        // collapsing header so the panel still behaves like every other group.
        if (g.path && !std::strcmp(g.path, matter::kDrawOverridesPath)) {
            char header[192];
            std::snprintf(header, sizeof(header), "%s###%s",
                          g.label ? g.label : g.path, g.path);
            if (ImGui::CollapsingHeader(header)) {
                ImGui::SameLine();
                ImGui::TextDisabled("[%s]%s", prop_scope_name(b->scope()),
                                    b->dirty() ? " *" : "");
                draw_draw_overrides_section(*b, whole_group ? nullptr : filter);
            }
            continue;
        }
        draw_group(*b, whole_group ? nullptr : filter, true, &on_apply);
    }
    if (sorted.empty()) {
        ImGui::TextDisabled("No property groups registered.");
    } else if (!any_drawn) {
        // Distinguish "your filter matched nothing" from "your filter matched
        // things, but every one of them lives in another panel" — the second
        // is the empty state the de-duplication checkbox itself can produce,
        // and "no group or field matches" would send the user hunting for a
        // typo that is not there.
        if (any_hidden_as_duplicate)
            ImGui::TextDisabled(
                "Everything here is shown in another panel - untick \"Hide "
                "properties shown in other panels\" to see it.");
        else
            ImGui::TextDisabled("No group or field matches the filter.");
    }
}

}  // namespace viewer
