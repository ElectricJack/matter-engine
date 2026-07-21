#include "scene_tree_panel.h"

#include "matter/world_session.h"
#include "camera_focus.h"
#include "os_open.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace viewer {
namespace {

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool has_component(const std::vector<std::string>& names, const char* name) {
    for (const auto& n : names) {
        if (n == name) return true;
    }
    return false;
}

// Task 13: bundles the extra command/selection/camera plumbing the row
// context menus need, so draw_baked_roots/draw_entities don't grow an
// unwieldy parameter list of their own.
struct TreeContext {
    SceneCommands* commands = nullptr;
    matter::scene::SimulationMode mode = matter::scene::SimulationMode::Edit;
    matter::CameraDesc* camera = nullptr;
    SelectionSet* selection = nullptr;
    const FieldCommands* fields = nullptr;
    ConsoleLog* console_log = nullptr;
};

const char* edit_error_message(matter::scene::SceneEditError error) {
    switch (error) {
        case matter::scene::SceneEditError::None: return nullptr;
        case matter::scene::SceneEditError::EntityNotFound: return "entity not found";
        case matter::scene::SceneEditError::CycleDetected: return "cycle detected";
        case matter::scene::SceneEditError::InvalidTarget: return "invalid target";
    }
    return "unknown error";
}

void log_error(ConsoleLog* console_log, const char* action,
               matter::scene::SceneEditError error) {
    if (!console_log || error == matter::scene::SceneEditError::None) return;
    console_log->push(LogSeverity::Error,
                      std::string(action) + ": " + edit_error_message(error));
}

void focus_on(TreeContext& ctx) {
    if (ctx.camera && ctx.selection && ctx.fields) {
        focus_camera_on_selection(*ctx.camera, *ctx.selection, *ctx.fields);
    }
}

void draw_baked_roots(SceneTreeState& state, EditorModel& editor,
                      TreeContext& ctx, const std::string& filter_lower) {
    for (const auto& [module, node] : state.cached_snapshot.nodes) {
        if (!node.is_root) continue;
        if (!filter_lower.empty() &&
            to_lower(module).find(filter_lower) == std::string::npos) {
            continue;
        }

        const bool selected = editor.selection().id.value == 0 &&
                              state.selected_root_hash == node.resolved_hash;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                                   ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;

        const std::string label = module + " [Baked]";
        ImGui::TreeNodeEx(label.c_str(), flags);
        if (ImGui::IsItemClicked()) {
            state.selected_root_hash = node.resolved_hash;
            editor.clear_selection();
            if (ctx.selection) {
                ctx.selection->replace(
                    SelectedObject{SelectedObject::BakedRoot, node.resolved_hash});
            }
        }

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Focus", "F")) {
                state.selected_root_hash = node.resolved_hash;
                editor.clear_selection();
                if (ctx.selection) {
                    ctx.selection->replace(
                        SelectedObject{SelectedObject::BakedRoot, node.resolved_hash});
                }
                focus_on(ctx);
            }
            const bool has_source = !node.source_path.empty();
            if (ImGui::MenuItem("Open Source", nullptr, false, has_source)) {
                os_open_file(node.source_path);
            }
            ImGui::EndPopup();
        }
    }
}

void draw_entities(SceneTreeState& state, EditorModel& editor, TreeContext& ctx,
                   const std::unordered_set<uint64_t>* authored_entity_ids) {
    constexpr float kIndentWidth = 16.0f;
    const bool play_mode = ctx.mode == matter::scene::SimulationMode::Play;

    for (const auto& row : editor.rows()) {
        if (row.depth > 0) ImGui::Indent(row.depth * kIndentWidth);

        const bool is_runtime = authored_entity_ids != nullptr &&
                                authored_entity_ids->find(row.id.value) ==
                                    authored_entity_ids->end();
        const bool has_part = has_component(row.component_names, "PartInstance");

        const bool selected = editor.selection().id.value == row.id.value;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
        if (row.child_count == 0 && !has_part) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;

        const std::string label = row.name + (is_runtime ? " [Runtime]" : " [Entity]");
        const bool opened = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<uintptr_t>(row.id.value)), flags,
            "%s", label.c_str());
        if (ImGui::IsItemClicked()) {
            editor.select(row.id);
            state.selected_root_hash = 0;
            if (ctx.selection) {
                ctx.selection->replace(SelectedObject{SelectedObject::Entity, row.id.value});
            }
        }

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Focus", "F")) {
                editor.select(row.id);
                state.selected_root_hash = 0;
                if (ctx.selection) {
                    ctx.selection->replace(SelectedObject{SelectedObject::Entity, row.id.value});
                }
                focus_on(ctx);
            }
            if (ImGui::MenuItem("Add Child Entity", nullptr, false,
                                ctx.commands != nullptr)) {
                if (ctx.commands->create_empty) {
                    matter::scene::SceneEditResult created =
                        ctx.commands->create_empty("Entity");
                    if (created.error == matter::scene::SceneEditError::None) {
                        if (ctx.commands->reparent) {
                            matter::scene::SceneEditResult reparented =
                                ctx.commands->reparent(created.created_id, row.id);
                            log_error(ctx.console_log, "Add Child Entity", reparented.error);
                        }
                        editor.select(created.created_id);
                        state.selected_root_hash = 0;
                        if (ctx.selection) {
                            ctx.selection->replace(SelectedObject{
                                SelectedObject::Entity, created.created_id.value});
                        }
                    } else {
                        log_error(ctx.console_log, "Add Child Entity", created.error);
                    }
                }
            }
            if (ImGui::MenuItem("Duplicate", nullptr, false,
                                !play_mode && ctx.commands != nullptr)) {
                editor.select(row.id);
                matter::scene::SceneEditResult duplicated =
                    editor.duplicate_selected(*ctx.commands);
                if (duplicated.error == matter::scene::SceneEditError::None) {
                    if (ctx.fields && ctx.fields->get_float3 && ctx.fields->set_float3) {
                        matter::Float3 translation{};
                        if (ctx.fields->get_float3(duplicated.created_id, "LocalTransform",
                                                   "translation", translation)) {
                            translation.x += 0.5f;
                            ctx.fields->set_float3(duplicated.created_id, "LocalTransform",
                                                   "translation", translation);
                        }
                    }
                    state.selected_root_hash = 0;
                    if (ctx.selection) {
                        ctx.selection->replace(SelectedObject{
                            SelectedObject::Entity, duplicated.created_id.value});
                    }
                } else {
                    log_error(ctx.console_log, "Duplicate", duplicated.error);
                }
            }
            if (ImGui::MenuItem("Delete", nullptr, false,
                                !play_mode && ctx.commands != nullptr)) {
                editor.select(row.id);
                matter::scene::SceneEditResult deleted =
                    editor.delete_selected(*ctx.commands);
                log_error(ctx.console_log, "Delete", deleted.error);
            }
            ImGui::EndPopup();
        }

        if (opened) {
            if (has_part) {
                ImGui::TreeNodeEx("Part", ImGuiTreeNodeFlags_Leaf |
                                              ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                              ImGuiTreeNodeFlags_Bullet,
                                  "%s", "[Part]");
            }
            if (row.child_count != 0 || has_part) {
                ImGui::TreePop();
            }
        }

        if (row.depth > 0) ImGui::Unindent(row.depth * kIndentWidth);
    }
}

} // namespace

void draw_scene_tree(SceneTreeState& state, EditorModel& editor,
                     matter::WorldSession* session,
                     SceneCommands* commands,
                     matter::scene::SimulationMode mode,
                     matter::CameraDesc* camera,
                     SelectionSet* selection,
                     const FieldCommands* fields,
                     ConsoleLog* console_log,
                     const std::unordered_set<uint64_t>* authored_entity_ids) {
    if (session) {
        const uint64_t generation = session->graph_generation();
        if (generation != state.cached_graph_gen) {
            if (session->graph_snapshot(state.cached_snapshot)) {
                state.cached_graph_gen = generation;
            }
        }
    }

    if (ImGui::InputTextWithHint("##filter", "Filter...", state.filter_text,
                                 sizeof(state.filter_text))) {
        editor.set_filter(state.filter_text);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::Combo("##mode", &state.filter_mode, "All\0Entities\0Roots\0");

    ImGui::SeparatorText("World");

    const std::string filter_lower = to_lower(state.filter_text);

    TreeContext ctx{commands, mode, camera, selection, fields, console_log};

    if (state.filter_mode != 1) {  // All or Roots
        draw_baked_roots(state, editor, ctx, filter_lower);
    }
    if (state.filter_mode != 2) {  // All or Entities
        draw_entities(state, editor, ctx, authored_entity_ids);
    }
}

} // namespace viewer
