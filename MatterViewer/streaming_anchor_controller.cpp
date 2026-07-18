#include "streaming_anchor_controller.h"

#include "matter/ecs.h"

#include <atomic>
#include <cstdint>

namespace matter_viewer {
namespace {

struct WorldIdentity {
    std::uint64_t value = 0;
};

std::uint64_t identity_for(flecs::world& world) {
    if (const WorldIdentity* identity = world.try_get<WorldIdentity>()) {
        return identity->value;
    }

    static std::atomic<std::uint64_t> next_identity{1};
    const std::uint64_t identity = next_identity.fetch_add(1, std::memory_order_relaxed);
    world.set<WorldIdentity>({identity});
    return identity;
}

void clear_anchor(StreamingAnchorState& state) {
    state.selected = 0;
    state.follow_editor_camera = false;
    state.world_identity = 0;
}

flecs::entity selected_anchor(flecs::world& world, flecs::entity_t selected) {
    return flecs::entity(world.c_ptr(), selected);
}

} // namespace

void validate_anchor(StreamingAnchorState& state, flecs::world& world) {
    if (state.selected == 0) {
        return;
    }

    const std::uint64_t world_identity = identity_for(world);
    if ((state.world_identity != 0 && state.world_identity != world_identity) ||
        !world.is_alive(state.selected)) {
        clear_anchor(state);
        return;
    }

    state.world_identity = world_identity;
}

void follow_camera(StreamingAnchorState& state, flecs::world& world,
                   const float camera_position[3]) {
    validate_anchor(state, world);
    if (state.selected == 0 || !state.follow_editor_camera || camera_position == nullptr) {
        return;
    }

    flecs::entity anchor = selected_anchor(world, state.selected);
    const matter::ecs::LocalTransform* current =
        anchor.try_get<matter::ecs::LocalTransform>();
    if (current == nullptr) {
        return;
    }

    matter::ecs::LocalTransform updated = *current;
    updated.translation = {camera_position[0], camera_position[1], camera_position[2]};
    anchor.set<matter::ecs::LocalTransform>(updated);
    anchor.add<matter::ecs::TransformDirty>();
}

void detach_follow(StreamingAnchorState& state, flecs::world& world) {
    validate_anchor(state, world);
    state.follow_editor_camera = false;
}

bool apply_gizmo_translation(StreamingAnchorState& state, flecs::world& world,
                             const float matrix[16]) {
    validate_anchor(state, world);
    if (state.selected == 0 || matrix == nullptr) {
        return false;
    }

    flecs::entity anchor = selected_anchor(world, state.selected);
    const matter::ecs::LocalTransform* current =
        anchor.try_get<matter::ecs::LocalTransform>();
    if (current == nullptr) {
        return false;
    }

    matter::ecs::LocalTransform updated = *current;
    // Mat4f uses row-major storage with column-vector algebra.
    updated.translation = {matrix[3], matrix[7], matrix[11]};
    anchor.set<matter::ecs::LocalTransform>(updated);
    anchor.add<matter::ecs::TransformDirty>();
    return true;
}

bool camera_input_allowed(bool imgui_capture, bool gizmo_using) {
    return !imgui_capture && !gizmo_using;
}

} // namespace matter_viewer
