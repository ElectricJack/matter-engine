#include "frame_matrices.h"

#include <cassert>
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
    // Reversed-Z equivalent of perspective_rh_zo_reversed's m[10]. That term
    // is mathematically guaranteed positive for any valid 0 < near < far, so
    // a non-finite or non-positive result here means near/far collapsed past
    // float32's representable range (e.g. near vanishingly small relative to
    // far underflows near/(far-near) to exactly 0.0f) rather than the old
    // formula's specific "-1.0f" cancellation artifact.
    const float depth_scale =
        camera.near_plane / (camera.far_plane - camera.near_plane);
    if (!std::isfinite(depth_scale) || depth_scale <= 0.0f) {
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
    candidate.view_to_clip = perspective_rh_zo_reversed(
        camera.vertical_fov_radians,
        static_cast<float>(width) / static_cast<float>(height),
        camera.near_plane, camera.far_plane);
    candidate.world_to_clip =
        mat4_mul(candidate.view_to_clip, candidate.world_to_view);
#ifndef NDEBUG
    // Reversed-Z recovery identities used by downstream consumers
    // (vk_scene_renderer / vk_volumetrics): m[11]/m[10] must recover far and
    // m[11]/(m[10]+1) must recover near. Guards against the projection and
    // its consumers drifting out of sync on the depth convention.
    {
        const float m10 = candidate.view_to_clip.m[10];
        const float m11 = candidate.view_to_clip.m[11];
        const float recovered_far = m11 / m10;
        const float recovered_near = m11 / (m10 + 1.0f);
        assert(std::fabs(recovered_far - camera.far_plane) <=
               1e-3f * camera.far_plane);
        assert(std::fabs(recovered_near - camera.near_plane) <=
               1e-3f * camera.near_plane);
    }
#endif
    if (!mat4_inverse(candidate.world_to_clip, candidate.clip_to_world)) {
        error = "world-to-clip matrix is singular";
        return false;
    }
    // NOTE (reversed-Z near/far plane labels): extract_frustum_planes_zo
    // derives planes[4] from the clip-space inequality z>=0 and planes[5]
    // from w-z>=0. Under standard ZO those are the near and far planes in
    // that order. Under reversed-Z (near->1, far->0) the same two
    // inequalities still bound exactly the same valid NDC range [0, w], but
    // the geometric plane each one represents swaps: z>=0 (NDC>=0) is now the
    // far-plane test and w-z>=0 (NDC<=1) is now the near-plane test. The
    // extractor's arithmetic does not need to change for this - it emits two
    // correctly-oriented half-space planes (sign/direction untouched, each
    // still a valid "point satisfies dot(plane,point)>=0 => inside" test);
    // only the near/far *label* of slots 4 and 5 swaps. Consumers
    // (gpu_culler.cpp / shaders_vk/cull.comp) test all 6 planes uniformly
    // with no near/far-specific indexing, so no plane-slot swap is required
    // at this call site either - verified via cull.comp's "for (plane in
    // 0..6) all_outside &= ..." loop, which never singles out planes[4] or
    // planes[5]. If a future consumer needs a specific near or far plane by
    // index, it must account for this label swap.
    if (!extract_frustum_planes_zo(candidate.world_to_clip,
                                   candidate.frustum_planes)) {
        error = "frustum planes are degenerate";
        return false;
    }

    frame = candidate;
    return true;
}

} // namespace viewer
