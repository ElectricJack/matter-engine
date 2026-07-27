#include "animation_panel.h"

#include "imgui.h"

#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

namespace viewer {
namespace {

void draw_status_banner(const AnimationPanelModel& model) {
    switch (model.status()) {
    case AnimationPanelStatus::QueryFailed:
        // Deliberately louder than the empty case: this one is a bug, not a
        // quiet scene, and it used to be indistinguishable from having nothing
        // selected.
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Animation data unavailable (the engine refused to "
                           "produce a consistent snapshot)");
        break;
    case AnimationPanelStatus::NoBindings:
        ImGui::TextDisabled("No live animation bindings");
        break;
    case AnimationPanelStatus::Ready:
        break;
    }
    if (model.rejected_snapshot_count() != 0)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "%u malformed snapshot(s) rejected at the draw boundary",
                           model.rejected_snapshot_count());
}

void draw_instance_selector(AnimationPanelModel& model) {
    if (model.instance_count() <= 1) return;
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("Animator",
                          ("instance " + std::to_string(model.selected_instance())).c_str())) {
        for (std::size_t i = 0; i < model.instance_count(); ++i) {
            const bool selected = i == model.selected_instance();
            if (ImGui::Selectable(("instance " + std::to_string(i)).c_str(), selected))
                model.select_instance(i);
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void draw_rig_tab(const AnimationPanelModel& model) {
    ImGui::Text("%zu joint(s), %zu socket(s)", model.joint_rows().size(),
                model.socket_rows().size());
    if (ImGui::BeginTable("##anim_joints", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Joint");
        ImGui::TableSetupColumn("Parent");
        ImGui::TableSetupColumn("Radius");
        ImGui::TableHeadersRow();
        for (const AnimationJointRow& row : model.joint_rows()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // Indent by depth so the hierarchy is readable without a tree node.
            ImGui::Text("%*s%s", static_cast<int>(row.depth) * 2, "", row.label.c_str());
            ImGui::TableNextColumn();
            if (row.parent == UINT16_MAX) ImGui::TextDisabled("-");
            else ImGui::Text("%u", static_cast<unsigned>(row.parent));
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", row.radius);
        }
        ImGui::EndTable();
    }
    if (!model.socket_rows().empty()) {
        ImGui::SeparatorText("Sockets");
        for (const AnimationSocketRow& row : model.socket_rows())
            ImGui::BulletText("%s -> %s", row.label.c_str(), row.joint_label.c_str());
    }
}

void draw_skin_tab(const AnimationPanelModel& model) {
    const AnimationSkinSummary& skin = model.skin_summary();
    if (!skin.has_skin && skin.rigid_part_count == 0) {
        ImGui::TextDisabled("This animator binds no skinned or rigid geometry.");
        return;
    }
    ImGui::Text("Skinned: %s", skin.has_skin ? "yes" : "no");
    ImGui::Text("LOD0 influences: %u", skin.influence_count);
    ImGui::Text("Palette joints: %u", skin.joint_count);
    ImGui::Text("Rigid segments: %u", skin.rigid_part_count);
}

// Inline editor for one externally-driven target. Seeded from the target's last
// EVALUATED transform, so opening it does not yank the target to identity; the
// author edits from where the target actually is.
//
// Every write goes through the engine and the engine's verdict is shown. A
// rejection here means one-driver arbitration or a non-finite value refused it,
// and silently swallowing that is precisely the failure this tab exists to
// prevent.
void draw_target_editor(const AnimationTargetRow& row,
                        matter::AnimatorInstanceHandle animator,
                        const AnimationTargetWriter& writer) {
    struct EditState {
        uint16_t target = UINT16_MAX;
        uint64_t animator_slot = UINT64_MAX;
        matter::AnimationTransform value{};
        bool last_write_rejected = false;
    };
    static EditState state;

    // Re-seed whenever the edited target (or the animator) changes, so a stale
    // buffer from a previous selection can never be written to a new target.
    const uint64_t slot = static_cast<uint64_t>(animator.slot_index);
    if (state.target != row.index || state.animator_slot != slot) {
        state.target = row.index;
        state.animator_slot = slot;
        state.value = row.evaluated;
        state.last_write_rejected = false;
    }

    ImGui::SetNextItemWidth(220.0f);
    bool edited = ImGui::DragFloat3("translation", &state.value.translation.x, 0.01f);
    ImGui::SetNextItemWidth(220.0f);
    edited |= ImGui::DragFloat4("rotation", &state.value.rotation.x, 0.01f);

    if (edited && writer.set_transform)
        state.last_write_rejected = !writer.set_transform(animator, row.label.c_str(), state.value);

    if (writer.snap && ImGui::SmallButton("snap")) {
        // Skip smoothing and adopt the desired transform on the next evaluation.
        state.last_write_rejected = !writer.snap(animator, row.label.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("reset to evaluated")) {
        state.value = row.evaluated;
        state.last_write_rejected = false;
    }
    if (state.last_write_rejected)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "the engine rejected that write");
}

void draw_targets_tab(AnimationPanelModel& model, const AnimationTargetWriter& writer) {
    if (model.target_rows().empty()) {
        ImGui::TextDisabled("This animator declares no targets.");
        return;
    }
    static uint16_t editing = UINT16_MAX;
    if (ImGui::BeginTable("##anim_targets", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Driver");
        ImGui::TableSetupColumn("Cadence");
        ImGui::TableSetupColumn("Chain");
        ImGui::TableSetupColumn("Weight");
        ImGui::TableSetupColumn("Gizmo");
        ImGui::TableHeadersRow();
        for (const AnimationTargetRow& row : model.target_rows()) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(row.index));
            ImGui::TableNextColumn();
            ImGui::Text("%s", row.label.c_str());
            if (!row.available) {
                ImGui::SameLine();
                ImGui::TextDisabled("(unavailable)");
            }
            ImGui::TableNextColumn();
            if (row.driver_is_controller) ImGui::Text("controller %s", row.controller.c_str());
            else ImGui::TextUnformatted("external");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.cadence_is_fixed ? "fixed" : "frame");
            ImGui::TableNextColumn();
            ImGui::Text("%u%s", row.chain_length, row.has_pole ? " +pole" : "");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", row.weight);
            ImGui::TableNextColumn();
            // The rule this column exists for: a controller owns the target, so
            // an external write is rejected. Show that, rather than offering a
            // drag that silently does nothing.
            if (!row.gizmo_enabled) {
                ImGui::TextDisabled("owned");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Driven by controller '%s'. External writes are "
                                      "rejected by one-driver arbitration.",
                                      row.controller.c_str());
            } else if (!writer.bound()) {
                ImGui::TextDisabled("read-only");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("No write path is bound to this panel.");
            } else if (ImGui::SmallButton("edit")) {
                editing = row.index;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // The editor sits below the table rather than inside a cell so the drag
    // fields get usable width.
    const matter::AnimatorInstanceHandle animator = model.selected_animator();
    if (editing == UINT16_MAX || !writer.bound() || !animator.valid()) return;
    for (const AnimationTargetRow& row : model.target_rows()) {
        if (row.index != editing) continue;
        // A live reload can turn an external target into a controller-driven
        // one. Re-check ownership here rather than trusting the click that
        // opened the editor.
        if (!row.gizmo_enabled) { editing = UINT16_MAX; return; }
        ImGui::SeparatorText(("Edit target: " + row.label).c_str());
        ImGui::PushID(static_cast<int>(row.index));
        draw_target_editor(row, animator, writer);
        ImGui::SameLine();
        if (ImGui::SmallButton("close")) editing = UINT16_MAX;
        ImGui::PopID();
        return;
    }
    editing = UINT16_MAX;  // the target disappeared (reload): drop the editor
}

void draw_render_tab(const AnimationPanelModel& model,
                     AnimationDebugOverlayOptions& overlay) {
    ImGui::Text("Visible: %s", model.selected_visible() ? "yes" : "no");
    ImGui::SeparatorText("Viewport overlay");
    // Same options struct the viewport draws with, so this cannot drift from
    // what is actually rendered. The model already carries authored joint
    // names, so the weight picker names its joints instead of numbering them.
    std::vector<std::string> joint_names;
    joint_names.reserve(model.joint_rows().size());
    for (const AnimationJointRow& row : model.joint_rows())
        joint_names.push_back(row.label);
    draw_animation_debug_overlay_controls(overlay, &joint_names);
}

void draw_diagnostics(const AnimationPanelModel& model) {
    if (model.diagnostics().empty()) return;
    ImGui::SeparatorText("Bake diagnostics");
    for (const matter::AnimationDiagnostic& diagnostic : model.diagnostics()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "[%s]", diagnostic.code.c_str());
        ImGui::SameLine();
        ImGui::TextWrapped("%s", diagnostic.message.c_str());
        const std::string where = diagnostic.source_location();
        if (!where.empty()) ImGui::TextDisabled("    %s", where.c_str());
    }
}

struct TabSpec {
    const char* label;
    AnimationTab tab;
};

} // namespace

void draw_animation_panel(AnimationPanelModel& model,
                          AnimationDebugOverlayOptions& overlay,
                          const AnimationTargetWriter& writer) {
    draw_status_banner(model);
    // Diagnostics are shown even with nothing live: a rig that FAILED to bake
    // produces no bindings, and its diagnostics are the only thing that explains
    // the empty panel.
    draw_diagnostics(model);
    if (model.status() != AnimationPanelStatus::Ready) return;

    draw_instance_selector(model);

    static const TabSpec kTabs[] = {
        {"Rig", AnimationTab::Rig},         {"Skin", AnimationTab::Skin},
        {"Clips", AnimationTab::Clips},     {"Graph", AnimationTab::Graph},
        {"Targets", AnimationTab::Targets}, {"Render", AnimationTab::Render},
    };
    if (!ImGui::BeginTabBar("##animation_tabs")) return;
    for (const TabSpec& spec : kTabs) {
        if (!ImGui::BeginTabItem(spec.label)) continue;
        model.set_tab(spec.tab);
        switch (spec.tab) {
        case AnimationTab::Rig:     draw_rig_tab(model); break;
        case AnimationTab::Skin:    draw_skin_tab(model); break;
        case AnimationTab::Targets: draw_targets_tab(model, writer); break;
        case AnimationTab::Render:  draw_render_tab(model, overlay); break;
        case AnimationTab::Clips:
            // Clip inventory is not in the debug snapshot yet; the snapshot
            // carries rig, skin, target, and pose data only.
            ImGui::TextDisabled("Clip inventory is not exposed by the debug snapshot yet.");
            break;
        case AnimationTab::Graph:
            ImGui::TextDisabled("Graph topology is not exposed by the debug snapshot yet.");
            break;
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

} // namespace viewer
