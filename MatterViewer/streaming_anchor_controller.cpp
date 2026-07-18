#include "streaming_anchor_controller.h"

#include "matter/ecs.h"

#include <array>
#include <atomic>
#include <cmath>
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

flecs::entity selected_anchor(flecs::world& world, flecs::entity_t selected) {
    return flecs::entity(world.c_ptr(), selected);
}

} // namespace

void CurrentFrameInputOrder::begin_ui() noexcept {
    if (stage_ == Stage::AwaitingUi) stage_ = Stage::UiBegun;
}

void CurrentFrameInputOrder::build_ui() noexcept {
    if (stage_ == Stage::UiBegun) stage_ = Stage::UiBuilt;
}

void CurrentFrameInputOrder::decide_capture(
    bool camera_input_allowed) noexcept {
    if (stage_ != Stage::UiBuilt) return;
    camera_input_allowed_ = camera_input_allowed;
    stage_ = Stage::CaptureDecided;
}

bool CurrentFrameInputOrder::camera_update_allowed() const noexcept {
    return stage_ == Stage::CaptureDecided && camera_input_allowed_;
}

flecs::entity_t create_anchor(StreamingAnchorState& state, flecs::world& world) {
    const flecs::entity anchor =
        world.entity().set<matter::ecs::LocalTransform>({});
    state.selected = anchor.id();
    state.follow_editor_camera = true;
    state.world_identity = identity_for(world);
    return anchor.id();
}

bool select_anchor(StreamingAnchorState& state, flecs::world& world,
                   flecs::entity_t anchor) {
    clear_anchor(state);
    if (anchor == 0 || !world.is_alive(anchor)) {
        return false;
    }
    const flecs::entity candidate = selected_anchor(world, anchor);
    if (!candidate.has<matter::ecs::LocalTransform>()) {
        return false;
    }
    state.selected = anchor;
    state.world_identity = identity_for(world);
    return true;
}

void clear_anchor(StreamingAnchorState& state) {
    state.selected = 0;
    state.follow_editor_camera = false;
    state.world_identity = 0;
}

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

bool attach_streaming(StreamingAnchorState& state, flecs::world& world) {
    validate_anchor(state, world);
    if (state.selected == 0) {
        return false;
    }
    flecs::entity anchor = selected_anchor(world, state.selected);
    if (!anchor.has<matter::ecs::LocalTransform>()) {
        return false;
    }
    if (!anchor.has<matter::streaming::SectorStreaming>()) {
        anchor.add<matter::streaming::SectorStreaming>();
    }
    return true;
}

bool remove_streaming(StreamingAnchorState& state, flecs::world& world) {
    validate_anchor(state, world);
    if (state.selected == 0) {
        return false;
    }
    flecs::entity anchor = selected_anchor(world, state.selected);
    if (!anchor.has<matter::streaming::SectorStreaming>()) {
        return false;
    }
    anchor.remove<matter::streaming::SectorStreaming>();
    return true;
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

bool gizmo_translation_allowed(StreamingAnchorState& state,
                               flecs::world& world) {
    validate_anchor(state, world);
    if (state.selected == 0 || state.follow_editor_camera) {
        return false;
    }
    return selected_anchor(world, state.selected)
        .has<matter::ecs::LocalTransform>();
}

matter::Mat4f local_transform_matrix(
    const matter::ecs::LocalTransform& transform) {
    double x = transform.rotation.x;
    double y = transform.rotation.y;
    double z = transform.rotation.z;
    double w = transform.rotation.w;
    const double length_squared = x * x + y * y + z * z + w * w;
    if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
        std::isfinite(w) && std::isfinite(length_squared) &&
        length_squared > 0.0) {
        const double inverse_length = 1.0 / std::sqrt(length_squared);
        x *= inverse_length;
        y *= inverse_length;
        z *= inverse_length;
        w *= inverse_length;
    } else {
        x = 0.0;
        y = 0.0;
        z = 0.0;
        w = 1.0;
    }
    const double xx = x * x;
    const double yy = y * y;
    const double zz = z * z;
    const double xy = x * y;
    const double xz = x * z;
    const double yz = y * z;
    const double xw = x * w;
    const double yw = y * w;
    const double zw = z * w;
    const float sx = transform.scale.x;
    const float sy = transform.scale.y;
    const float sz = transform.scale.z;

    matter::Mat4f matrix{};
    matrix.m[0] = static_cast<float>((1.0 - 2.0 * (yy + zz)) * sx);
    matrix.m[1] = static_cast<float>((2.0 * (xy - zw)) * sy);
    matrix.m[2] = static_cast<float>((2.0 * (xz + yw)) * sz);
    matrix.m[3] = transform.translation.x;
    matrix.m[4] = static_cast<float>((2.0 * (xy + zw)) * sx);
    matrix.m[5] = static_cast<float>((1.0 - 2.0 * (xx + zz)) * sy);
    matrix.m[6] = static_cast<float>((2.0 * (yz - xw)) * sz);
    matrix.m[7] = transform.translation.y;
    matrix.m[8] = static_cast<float>((2.0 * (xz - yw)) * sx);
    matrix.m[9] = static_cast<float>((2.0 * (yz + xw)) * sy);
    matrix.m[10] = static_cast<float>((1.0 - 2.0 * (xx + yy)) * sz);
    matrix.m[11] = transform.translation.z;
    matrix.m[15] = 1.0f;
    return matrix;
}

std::array<float, 16> to_imguizmo_matrix(const matter::Mat4f& matrix) {
    std::array<float, 16> result{};
    for (int row = 0; row != 4; ++row) {
        for (int column = 0; column != 4; ++column) {
            result[column * 4 + row] = matrix.m[row * 4 + column];
        }
    }
    return result;
}

matter::Mat4f from_imguizmo_matrix(const float matrix[16]) {
    matter::Mat4f result{};
    if (matrix == nullptr) {
        return result;
    }
    for (int row = 0; row != 4; ++row) {
        for (int column = 0; column != 4; ++column) {
            result.m[row * 4 + column] = matrix[column * 4 + row];
        }
    }
    return result;
}

bool frame_selected_anchor(StreamingAnchorState& state, flecs::world& world,
                           matter::CameraDesc& camera, float distance) {
    validate_anchor(state, world);
    if (state.selected == 0 || !(distance > 0.0f) || !std::isfinite(distance)) {
        return false;
    }
    const flecs::entity anchor = selected_anchor(world, state.selected);
    const matter::ecs::LocalTransform* transform =
        anchor.try_get<matter::ecs::LocalTransform>();
    if (transform == nullptr) {
        return false;
    }

    matter::Float3 forward{
        camera.target.x - camera.position.x,
        camera.target.y - camera.position.y,
        camera.target.z - camera.position.z};
    const float length_squared = forward.x * forward.x + forward.y * forward.y +
                                 forward.z * forward.z;
    if (length_squared > 1e-12f && std::isfinite(length_squared)) {
        const float inverse_length = 1.0f / std::sqrt(length_squared);
        forward.x *= inverse_length;
        forward.y *= inverse_length;
        forward.z *= inverse_length;
    } else {
        forward = {0.0f, 0.0f, -1.0f};
    }

    camera.target = transform->translation;
    camera.position = {
        camera.target.x - forward.x * distance,
        camera.target.y - forward.y * distance,
        camera.target.z - forward.z * distance};
    return true;
}

bool camera_input_allowed(bool want_capture_mouse, bool want_capture_keyboard,
                          bool gizmo_over, bool gizmo_using) {
    return !(want_capture_mouse || want_capture_keyboard || gizmo_over ||
             gizmo_using);
}

} // namespace matter_viewer
