#pragma once

// MatterEditor/src/specialized_editors.h
//
// The declarative half of the editor's "specialized component editors": for
// three ComponentKinds, the auto-generated property fields are not enough and
// the Properties panel draws extra widgets (a part picker, physics impulse
// buttons, streaming attach/regenerate). This header declares WHAT those
// editors can do; it deliberately contains no ImGui and no drawing.
//
// The three pieces:
//   - Command structs (PartEditorCommands, PhysicsEditorCommands,
//     StreamingEditorCommands) — std::function hooks the application fills in
//     so this layer never has to know about the engine session.
//   - UI-only state (PartPickerState, StreamingEditorState) that has to live
//     across frames because ImGui itself is stateless.
//   - SpecializedEditors, which just holds one of each.
//
// Wiring: `MatterEditor/src/main.cpp` constructs one SpecializedEditors at
// startup, assigns every command it can service, and passes it by reference
// into the Properties panel every frame. The drawing lives in
// `MatterEditor/src/properties_panel.cpp`
// (draw_part_instance_editor / draw_rigidbody_editor / draw_streaming_editor,
// dispatched by draw_specialized_editor).
//
// EVERY COMMAND MAY BE EMPTY. main.cpp assigns the ones it can, and the panel
// null-checks each std::function before calling it, so a build or a session
// that cannot service an action simply draws a button that does nothing. Do
// not assume a command is set.
//
// Threading: main/UI thread only. The std::functions capture editor-session
// state and are invoked synchronously from the panel draw.
//
// Tests: `MatterEngine3/tests/specialized_editors_tests.cpp`
// (`make -C MatterEngine3/tests run-specialized-editors`).

#include "properties_registry.h"
#include "matter/scene.h"
#include "matter/ecs.h"
#include "matter/streaming.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace viewer {

// --- Part Instance Editor ---
// Resolves part_hash to display name, provides "pick part" action.
struct PartPickerState {
    uint64_t selected_hash = 0;
    bool picker_open = false;
};

// Hooks for the part-instance editor. `assign_part` returns false when the
// assignment was refused (unknown hash, entity gone); `list_available_parts`
// returns (part_hash, display name) pairs for the picker popup and is called
// while the popup is open, so it should not be O(world) if it can help it.
//
// `current_part_hash` reads the entity's PartInstance.part_hash at FULL 64-bit
// width, which the generic FieldCommands::get_uint accessor cannot do — that
// family is 32-bit and truncates (scene_registry.cpp documents the truncation
// on field_get_uint). Returns false when the entity has no PartInstance.
//
// All three may be empty — check before calling.
struct PartEditorCommands {
    std::function<bool(matter::scene::SceneEntityId, uint64_t new_hash)> assign_part;
    std::function<std::vector<std::pair<uint64_t, std::string>>()> list_available_parts;
    std::function<bool(matter::scene::SceneEntityId, uint64_t& out_hash)> current_part_hash;
};

// --- Physics Editor ---
// Commands for runtime physics operations beyond property editing.
struct PhysicsEditorCommands {
    std::function<bool(matter::scene::SceneEntityId, matter::Float3 velocity)> set_linear_velocity;
    std::function<bool(matter::scene::SceneEntityId, matter::Float3 impulse)> apply_impulse;
    std::function<bool(matter::scene::SceneEntityId)> wake;
    std::function<bool(matter::scene::SceneEntityId, matter::Float3 position)> teleport;
};

// --- Sector Streaming Editor ---
// Replaces the standalone sector streaming panel with a Properties-integrated editor.
struct StreamingEditorState {
    bool follow_camera = false;
    uint64_t seed = 0;
    // UI-only for now: no StreamingEditorCommands entry point exists yet to
    // apply a per-anchor radius (sector streaming config is currently global).
    // Kept here so the Properties panel has somewhere to store the drag value.
    float radius = 32.0f;
};

struct StreamingEditorCommands {
    std::function<bool(matter::scene::SceneEntityId)> attach_streaming;
    std::function<bool(matter::scene::SceneEntityId)> remove_streaming;
    std::function<void(bool follow)> set_follow_camera;
    std::function<void(uint64_t seed)> regenerate;
};

// Holder for the command tables and the cross-frame UI state above. It owns no
// engine object and has no behaviour beyond `has_specialized_editor`; the
// accessors exist so the application can fill the commands in at startup and
// the panel can read them each frame.
//
// Lifetime: one instance, a stack local in main.cpp, alive for the whole
// process. Copyable only in the trivial sense — don't; the panel takes it by
// reference and the picker/streaming state must be the same instance frame to
// frame or the widgets reset.
//
// Registry of specialized editors keyed by ComponentKind.
class SpecializedEditors {
public:
    bool has_specialized_editor(matter::scene::ComponentKind kind) const;

    // Accessors for the command structs (set by the application at startup).
    PartEditorCommands& part_commands() { return part_commands_; }
    PhysicsEditorCommands& physics_commands() { return physics_commands_; }
    StreamingEditorCommands& streaming_commands() { return streaming_commands_; }

    const PartEditorCommands& part_commands() const { return part_commands_; }
    const PhysicsEditorCommands& physics_commands() const { return physics_commands_; }
    const StreamingEditorCommands& streaming_commands() const { return streaming_commands_; }

    PartPickerState& part_picker_state() { return part_picker_; }
    StreamingEditorState& streaming_state() { return streaming_state_; }

private:
    PartEditorCommands part_commands_;
    PhysicsEditorCommands physics_commands_;
    StreamingEditorCommands streaming_commands_;
    PartPickerState part_picker_;
    StreamingEditorState streaming_state_;
};

} // namespace viewer
