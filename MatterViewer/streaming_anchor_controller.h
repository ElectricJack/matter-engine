#pragma once

#include <array>
#include <cstdint>

#include "flecs.h"
#include "matter/camera.h"
#include "matter/ecs.h"

namespace matter_viewer {

struct StreamingAnchorState {
    flecs::entity_t selected = 0;
    bool follow_editor_camera = true;

    // Unique private-world token; this controller never retains a Flecs world pointer.
    std::uint64_t world_identity = 0;
};

flecs::entity_t create_anchor(StreamingAnchorState& state, flecs::world& world);
bool select_anchor(StreamingAnchorState& state, flecs::world& world,
                   flecs::entity_t anchor);
void clear_anchor(StreamingAnchorState& state);
void validate_anchor(StreamingAnchorState& state, flecs::world& world);
bool attach_streaming(StreamingAnchorState& state, flecs::world& world);
bool remove_streaming(StreamingAnchorState& state, flecs::world& world);
void follow_camera(StreamingAnchorState& state, flecs::world& world,
                   const float camera_position[3]);
void detach_follow(StreamingAnchorState& state, flecs::world& world);
bool apply_gizmo_translation(StreamingAnchorState& state, flecs::world& world,
                             const float matrix[16]);
matter::Mat4f local_transform_matrix(const matter::ecs::LocalTransform& transform);
std::array<float, 16> to_imguizmo_matrix(const matter::Mat4f& matrix);
matter::Mat4f from_imguizmo_matrix(const float matrix[16]);
bool frame_selected_anchor(StreamingAnchorState& state, flecs::world& world,
                           matter::CameraDesc& camera, float distance);
bool camera_input_allowed(bool want_capture_mouse, bool want_capture_keyboard,
                          bool gizmo_over, bool gizmo_using);

} // namespace matter_viewer
