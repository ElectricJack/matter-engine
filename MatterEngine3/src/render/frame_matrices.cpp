#include "frame_matrices.h"

#include <cmath>

#include "matrix_math.h"

namespace viewer {
namespace {

float length_squared(matter::Float3 value) {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

matter::Float3 subtract(matter::Float3 a, matter::Float3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

matter::Float3 cross(matter::Float3 a, matter::Float3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

bool non_degenerate(matter::Float3 value) {
    const float squared = length_squared(value);
    return squared > 1e-12f && std::isfinite(squared);
}

} // namespace

bool build_frame_matrices(const matter::CameraDesc& camera, std::uint32_t width,
                          std::uint32_t height, FrameMatrices& frame,
                          std::string& error) {
    error.clear();
    if (width == 0 || height == 0) {
        error = "framebuffer extent must be non-zero";
        return false;
    }
    if (!(camera.near_plane > 0.0f) ||
        !(camera.far_plane > camera.near_plane) ||
        !std::isfinite(camera.near_plane) ||
        !std::isfinite(camera.far_plane)) {
        error = "camera planes must satisfy 0 < near < far";
        return false;
    }
    const float depth_scale =
        camera.far_plane / (camera.near_plane - camera.far_plane);
    if (!std::isfinite(depth_scale) || depth_scale == -1.0f) {
        error = "camera depth range is not representable in Mat4f";
        return false;
    }

    const matter::Float3 direction = subtract(camera.target, camera.position);
    if (!non_degenerate(direction)) {
        error = "camera direction must be non-degenerate";
        return false;
    }
    if (!non_degenerate(cross(direction, camera.up))) {
        error = "camera up vector must not be parallel to direction";
        return false;
    }

    FrameMatrices candidate{};
    candidate.world_to_view =
        look_at_rh(camera.position, camera.target, camera.up);
    candidate.view_to_clip = perspective_rh_zo(
        camera.vertical_fov_radians,
        static_cast<float>(width) / static_cast<float>(height),
        camera.near_plane, camera.far_plane);
    candidate.world_to_clip =
        mat4_mul(candidate.view_to_clip, candidate.world_to_view);
    if (!mat4_inverse(candidate.world_to_clip, candidate.clip_to_world)) {
        error = "world-to-clip matrix is singular";
        return false;
    }
    if (!extract_frustum_planes_zo(candidate.world_to_clip,
                                   candidate.frustum_planes)) {
        error = "frustum planes are degenerate";
        return false;
    }

    frame = candidate;
    return true;
}

} // namespace viewer
