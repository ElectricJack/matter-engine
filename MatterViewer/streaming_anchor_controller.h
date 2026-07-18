#pragma once

#include <cstdint>

#include "flecs.h"

namespace matter_viewer {

struct StreamingAnchorState {
    flecs::entity_t selected = 0;
    bool follow_editor_camera = true;

    // Unique private-world token; this controller never retains a Flecs world pointer.
    std::uint64_t world_identity = 0;
};

void validate_anchor(StreamingAnchorState& state, flecs::world& world);
void follow_camera(StreamingAnchorState& state, flecs::world& world,
                   const float camera_position[3]);
void detach_follow(StreamingAnchorState& state, flecs::world& world);
bool apply_gizmo_translation(StreamingAnchorState& state, flecs::world& world,
                             const float matrix[16]);
bool camera_input_allowed(bool imgui_capture, bool gizmo_using);

} // namespace matter_viewer
