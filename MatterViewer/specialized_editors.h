#pragma once

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

struct PartEditorCommands {
    std::function<bool(matter::scene::SceneEntityId, uint64_t new_hash)> assign_part;
    std::function<std::vector<std::pair<uint64_t, std::string>>()> list_available_parts;
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
};

struct StreamingEditorCommands {
    std::function<bool(matter::scene::SceneEntityId)> attach_streaming;
    std::function<bool(matter::scene::SceneEntityId)> remove_streaming;
    std::function<void(bool follow)> set_follow_camera;
    std::function<void(uint64_t seed)> regenerate;
};

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
