#pragma once

// MatterEditor/src/properties_panel.h
//
// The Properties panel is a pure VIEW: it holds no world state of its own and
// never includes flecs or WorldSession. Everything it can read or write goes
// through the std::function bundles declared here (FieldCommands,
// ComponentCommands from specialized_editors.h), which main.cpp fills in with
// closures over the live session. That is what lets the panel be compiled and
// reasoned about without the engine, and what lets a world switch change
// nothing here.
//
// Main/UI thread only — every entry point calls ImGui directly. All persistent
// state lives in PropertiesPanelState, owned by the Ui class, so the panel's
// value cache survives across frames but nothing survives across a run.

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
// component isn't present on that entity, the field's type doesn't match the
// accessor, or the descriptor marks it FieldReadOnly).
//
// Since the property system's Phase 3 the implementation is schema-driven:
// FieldDescriptor carries a byte offset, so main.cpp only fetches a component
// copy and stores it back — see the header comment there.
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
// Exactly ONE of the value members below is meaningful for any given entry:
// which one is decided by the field's WidgetKind (properties_registry.h), and
// the renderer that wrote the entry is the same one that reads it back. The
// others keep their zero-initialized values and must not be consulted.
//
// `mixed` is display-only and is cleared the moment an edit fans out, because
// an edit writes the same value to every selected entity and so ends the
// disagreement by construction.
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
// The cache is keyed by "component.field@primary_entity_id" (see
// make_cache_key in properties_panel.cpp): namespacing by the FIRST selected
// entity's id is what stops a selection change from briefly showing the
// previous entity's value. Entries are never evicted, so the map grows with
// the number of distinct (component, field, primary entity) triples inspected
// over the session — bounded in practice by how much a human clicks on.
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
