#pragma once

// Phase 5 Task 7 — Properties inspector panel: auto-generates ImGui widgets
// for the components/fields on the current selection, driven entirely by
// PropertiesRegistry (see properties_registry.h).
//
// Task 8 adds component-specific UI (part picker, physics actions, sector
// streaming controls) rendered inline after a component's auto-generated
// fields, driven by SpecializedEditors (see specialized_editors.h).

#include "properties_registry.h"
#include "specialized_editors.h"
#include "editor_model.h"
#include "selection_set.h"
#include "matter/scene.h"
#include "matter/math_types.h"
#include "part_graph_snapshot.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace viewer {

// Get/set individual field values on ECS entities, keyed by SceneEntityId +
// component name + field name (names match ComponentDescriptor/
// FieldDescriptor from ecs/scene_registry.h). Getters/setters return false
// when the entity, component, or field cannot be resolved (e.g. the
// component isn't present on that entity). There is no generic reflection
// API for ECS components yet, so the engine side (main.cpp) hardcodes field
// access per ComponentKind.
struct FieldCommands {
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, float&)> get_float;
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, float)> set_float;
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, int&)> get_int;
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, int)> set_int;
    // UInt fields (e.g. PartInstance.part_hash, ConvexHullCollider.point_count)
    // need real uint32_t storage for ImGui::DragScalar(ImGuiDataType_U32, ...).
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, uint32_t&)> get_uint;
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, uint32_t)> set_uint;
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, bool&)> get_bool;
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, bool)> set_bool;
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, matter::Float3&)> get_float3;
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, matter::Float3)> set_float3;
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, matter::Quaternion&)> get_quat;
    std::function<bool(matter::scene::SceneEntityId, const char*, const char*, matter::Quaternion)> set_quat;
};

// A single field's last-known value(s), used both as the live display value
// and as the frozen snapshot shown (disabled) while SimulationMode::Play is
// active, so the panel does not re-query the ECS every frame during Play.
struct CachedFieldValue {
    bool valid = false;  // false => field could not be resolved (skip drawing)
    bool mixed = false;  // true => selected entities disagree on this value
    float f = 0.0f;
    int i = 0;
    uint32_t u = 0;
    bool b = false;
    matter::Float3 f3{};
    matter::Quaternion q{};
};

// Per-frame UI state for the Properties panel, owned by the Ui class
// (analogous to SceneTreeState / ConsolePanelState).
struct PropertiesPanelState {
    std::unordered_map<std::string, CachedFieldValue> cache;
    // Set the first time a property edit occurs while SimulationMode::Pause
    // is active; suppresses re-showing the "changes are lost on Stop" hint
    // for the rest of the session (see draw_properties_contents()).
    bool pause_edit_hint_shown = false;
};

// Draw the Properties panel contents (call inside an ImGui::Begin/End pair).
//
// Renders one ImGui::CollapsingHeader per component present on the selected
// entity/entities, in PropertiesRegistry order, auto-generating a widget per
// field from its WidgetKind. With multiple entities selected, only
// components common to every selected entity are shown; fields with
// differing values are labelled "(mixed)" and editing fans the new value out
// to every selected entity. All widgets (including Add/Remove Component) are
// disabled while `mode` is SimulationMode::Play, and field values are not
// re-read from the ECS during Play — the last values read before Play
// started stay pinned until Pause/Stop.
// `snapshot` is the cached part_graph_snapshot::Snapshot (see
// WorldSession::graph_snapshot), used to render baked-root info (Task 9)
// when the selection contains SelectedObject::Kind::BakedRoot items.
// Nullable — pass nullptr when no snapshot is available yet (e.g. before the
// first bake completes); baked-root selections then show a "no data" message.
//
// `specialized` (Task 8) supplies the command callbacks (part picker, physics
// actions, sector streaming controls) for the three component kinds where
// SpecializedEditors::has_specialized_editor() is true; the corresponding
// controls are appended after that component's auto-generated fields, inside
// the same CollapsingHeader. `camera_position` is used by RigidBody's
// "Teleport To Camera" action.
void draw_properties_contents(PropertiesPanelState& state,
                              const SelectionSet& selection, EditorModel& editor,
                              const PropertiesRegistry& registry,
                              const FieldCommands& fields,
                              const ComponentCommands& components,
                              matter::scene::SimulationMode mode,
                              const part_graph_snapshot::Snapshot* snapshot,
                              SpecializedEditors& specialized,
                              const matter::Float3& camera_position);

} // namespace viewer
