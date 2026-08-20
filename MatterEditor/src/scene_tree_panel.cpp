// MatterEditor/src/scene_tree_panel.cpp
//
// The Scene panel's tree: the ImGui half of `draw_scene_tree` (contract and
// parameter meanings in scene_tree_panel.h). It renders ONE list out of two
// unrelated sources —
//
//  - Baked roots, from `state.cached_snapshot` (the session's part-graph
//    snapshot, refreshed by generation in `sync_scene_tree_graph_cache`).
//    Labelled "[Baked]". These are content, not entities: they carry a
//    resolved hash and no SceneEntityId.
//  - ECS entities, from `editor.rows()` (EditorModel, delta-fed by
//    scene_model_adapter.*). Labelled "[Entity]", or "[Runtime]" when the id
//    is absent from the caller's authored-entity set (a play-mode spawn).
//
// Selection. A click has to land in three places and they are kept consistent
// by hand, not by a single owner: `editor.select()` / `editor.clear_selection()`
// (the EditorModel's entity selection, which the Properties panel reads),
// `state.selected_root_hash` (the baked-root highlight), and the app-wide
// `SelectionSet` via `ctx.selection` (which the gizmo and the selection
// outline read). Selecting a baked root clears the entity selection and vice
// versa — that mutual exclusion is why the baked-root "is selected" test also
// requires `editor.selection().id.value == 0`. Any new click site added here
// must update all three.
//
// Layout. The entity list is drawn FLAT. `editor.rows()` already arrives in
// PREORDER hierarchy order with a `depth` per row, so nesting is faked with
// Indent/Unindent by `depth * kIndentWidth` rather than by nesting ImGui tree
// nodes. A row with children or a Part is still a non-leaf node (so it pushes,
// and the matching TreePop runs), but its children are drawn by later
// iterations of the same flat loop rather than inside that push — only the
// synthetic "[Part]" bullet is genuinely nested.
//
// Collapsing still works despite that, because preorder makes a subtree a
// contiguous run: a closed row records its depth and the loop skips every
// following row deeper than it (see draw_entities). Rows with children are
// DefaultOpen so the panel looks the same on first sight as the flat list
// always did. The skip is turned off while a text filter is active, because
// the filtered row list is a match list rather than a whole tree.
//
// Filtering. The text box drives two different filters: `editor.set_filter`
// (model-side, for entities) and a local lowercase substring test against the
// module name (for baked roots). `filter_mode` then decides which of the two
// sections is drawn at all: 0 = both, 1 = entities only, 2 = roots only.
//
// Threading. UI thread, inside an ImGui frame. Every mutation goes out through
// the nullable `SceneCommands` / `FieldCommands` callbacks main.cpp binds to
// the live session; failures are reported to the Console (`log_error`) rather
// than asserting, and the destructive menu items are disabled during Play.

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

// Frame the camera on whatever is currently in the SelectionSet. A silent
// no-op unless the caller supplied all three of camera/selection/fields — the
// context-menu "Focus" item is drawn regardless, so a panel driven without
// them does nothing instead of crashing.
void focus_on(TreeContext& ctx) {
    if (ctx.camera && ctx.selection && ctx.fields) {
        focus_camera_on_selection(*ctx.camera, *ctx.selection, *ctx.fields);
    }
}

// The "[Baked]" section: one leaf row per ROOT node in the cached graph
// snapshot, filtered by a case-insensitive substring of the module name
// (`filter_lower` must already be lowercased by the caller). Non-root nodes
// are skipped — they only exist composed inside other parts and have no world
// instance to select, the same rule reveal_part.cpp applies.
//
// A click records `SelectedObject{BakedRoot, resolved_hash}` — the identical
// selection viewer.reveal_part produces — and clears the ECS entity selection
// so the two channels are never both live. "Open Source" shells out to the OS
// handler for the node's authoring file (os_open.h) and is greyed when the
// node carries no source path.
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

// The "[Entity]" / "[Runtime]" section: `EditorModel::rows()` drawn flat with
// manual indentation (see the file header). `authored_entity_ids`, when
// non-null, is the last Edit-mode simulation snapshot's id set; a row missing
// from it is a play-mode spawn and is tagged [Runtime]. Null means "no
// snapshot yet" and every row reads as [Entity].
//
// The context menu mutates through `ctx.commands`, whose individual
// std::functions may each be empty and are tested before use; Duplicate and
// Delete are additionally greyed during Play. Duplicate nudges the copy's
// LocalTransform.translation by +0.5 on x so it is not coincident with the
// original, and only when the FieldCommands get/set float3 pair is available.
// Every failure path logs to the Console via `log_error` rather than
// asserting.
void draw_entities(SceneTreeState& state, EditorModel& editor, TreeContext& ctx,
                   const std::unordered_set<uint64_t>* authored_entity_ids) {
    constexpr float kIndentWidth = 16.0f;
    const bool play_mode = ctx.mode == matter::scene::SimulationMode::Play;

    // Rows are a FLAT preorder list with a depth, drawn with manual
    // indentation, so closing a parent's tree node does not by itself remove
    // its descendants from the loop — they are separate iterations. Preorder
    // is what makes that fixable: a node's subtree is exactly the run of
    // following rows whose depth is GREATER than its own, so a closed node
    // records its depth here and every deeper row is skipped until the list
    // comes back out to that level.
    //
    // Disabled while a filter is active: `rows()` is then a match list, not a
    // contiguous tree, and a match whose parent happens to be closed must not
    // disappear from the search results.
    const bool collapsing = editor.filter().empty();
    int collapsed_at_depth = -1;  // -1 => nothing collapsed

    for (const auto& row : editor.rows()) {
        const int depth = static_cast<int>(row.depth);
        if (collapsed_at_depth >= 0) {
            if (depth > collapsed_at_depth) continue;
            collapsed_at_depth = -1;
        }

        if (row.depth > 0) ImGui::Indent(row.depth * kIndentWidth);

        const bool is_runtime = authored_entity_ids != nullptr &&
                                authored_entity_ids->find(row.id.value) ==
                                    authored_entity_ids->end();
        const bool has_part = has_component(row.component_names, "PartInstance");

        const bool selected = editor.selection().id.value == row.id.value;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
        if (row.child_count == 0 && !has_part) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        } else if (row.child_count != 0) {
            // Default OPEN for real hierarchy, so the tree still shows every
            // entity on first sight the way the flat list always did; the
            // "[Part]" pseudo-child stays default-closed as before.
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
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
        } else if (collapsing && row.child_count != 0) {
            collapsed_at_depth = depth;
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
    // Refresh the baked-root cache before anything reads it: the snapshot is
    // pulled here, once per draw, never by the row loops below. A null
    // `session` leaves the cache untouched, so a disconnected editor keeps
    // showing the last world's roots until the world-switch seam resets it
    // (reset_scene_tree_graph_cache).
    sync_scene_tree_graph_cache(state, session);

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
