#include "check.h"

#include <cmath>
#include <string>

#include "matter/camera.h"
#include "render/frame_matrices.h"
#include "render/gpu_matrix_pack.h"
#include "render/matrix_math.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool closef(float a, float b, float epsilon) {
    return std::fabs(a - b) <= epsilon;
}

bool close3(matter::Float3 a, matter::Float3 b, float epsilon) {
    return closef(a.x, b.x, epsilon) && closef(a.y, b.y, epsilon) &&
           closef(a.z, b.z, epsilon);
}

void test_matrix_translation_uses_3_7_11() {
    const auto translation = viewer::mat4_translation({3.0f, 4.0f, 5.0f});
    CHECK(closef(translation.m[3], 3.0f, 1e-6f) &&
              closef(translation.m[7], 4.0f, 1e-6f) &&
              closef(translation.m[11], 5.0f, 1e-6f),
          "translation indices");
    CHECK(close3(viewer::transform_point(translation, {1.0f, 2.0f, 3.0f}),
                 {4.0f, 6.0f, 8.0f}, 1e-6f),
          "translated point");
    CHECK(close3(viewer::transform_vector(translation, {1.0f, 2.0f, 3.0f}),
                 {1.0f, 2.0f, 3.0f}, 1e-6f),
          "translation does not affect vectors");
}

void test_vulkan_projection_maps_near_far_to_zero_one() {
    const auto projection = viewer::perspective_rh_zo(
        60.0f * kPi / 180.0f, 16.0f / 9.0f, 1.0f, 5000.0f);
    CHECK(closef(viewer::project_ndc(projection, {0.0f, 0.0f, -1.0f}).z,
                  0.0f, 1e-5f),
          "near maps to zero");
    CHECK(closef(viewer::project_ndc(projection, {0.0f, 0.0f, -5000.0f}).z,
                  1.0f, 1e-5f),
          "far maps to one");
}

void test_composition_is_projection_times_view() {
    matter::CameraDesc camera{{0.0f, 0.0f, 5.0f},
                              {0.0f, 0.0f, 0.0f},
                              {0.0f, 1.0f, 0.0f},
                              60.0f * kPi / 180.0f,
                              0.1f,
                              100.0f};
    viewer::FrameMatrices frame{};
    std::string error;
    CHECK(viewer::build_frame_matrices(camera, 1280, 720, frame, error),
          "build frame matrices");
    // Reversed-Z: the target sits 5 units ahead with near=0.1/far=100, so
    // NDC depth = near*(far-d)/((far-near)*d) = 0.1*95/(99.9*5) = 0.019019
    // (the complement of the pre-migration standard-Z value 0.980981).
    CHECK(close3(viewer::project_ndc(frame.world_to_clip, {0.0f, 0.0f, 0.0f}),
                 {0.0f, 0.0f, 0.019019f}, 1e-5f),
          "projection times view");
}

void test_glsl_pack_is_explicit() {
    const auto packed =
        viewer::pack_glsl_mat4(viewer::mat4_translation({3.0f, 4.0f, 5.0f}));
    CHECK(closef(packed.elements[12], 3.0f, 1e-6f) &&
              closef(packed.elements[13], 4.0f, 1e-6f) &&
              closef(packed.elements[14], 5.0f, 1e-6f),
          "explicit GLSL pack");
}

void test_inverse_and_unproject_round_trip() {
    const auto matrix = viewer::mat4_mul(
        viewer::mat4_translation({3.0f, 4.0f, 5.0f}),
        viewer::mat4_rotation_y(0.5f));
    matter::Mat4f inverse{};
    CHECK(viewer::mat4_inverse(matrix, inverse), "invert affine matrix");
    const matter::Float3 point{1.25f, -2.0f, 7.5f};
    CHECK(close3(viewer::transform_point(inverse,
                                         viewer::transform_point(matrix, point)),
                 point, 1e-5f),
          "inverse round trip");

    const auto projection =
        viewer::perspective_rh_zo(0.9f, 1.5f, 0.25f, 200.0f);
    CHECK(viewer::mat4_inverse(projection, inverse), "invert projection matrix");
    const matter::Float3 view_point{0.7f, -0.4f, -9.0f};
    CHECK(close3(viewer::unproject_ndc(
                     inverse, viewer::project_ndc(projection, view_point)),
                 view_point, 1e-4f),
          "project unproject round trip");
}

void test_frustum_planes_use_vulkan_zero_to_one_rows() {
    const auto projection = viewer::perspective_rh_zo(90.0f * kPi / 180.0f,
                                                       1.0f, 1.0f, 10.0f);
    float planes[6][4]{};
    CHECK(viewer::extract_frustum_planes_zo(projection, planes),
          "extract frustum planes");
    CHECK(closef(planes[4][0], 0.0f, 1e-6f) &&
              closef(planes[4][1], 0.0f, 1e-6f) &&
              closef(planes[4][2], -1.0f, 1e-6f) &&
              closef(planes[4][3], -1.0f, 1e-6f),
          "near plane is normalized row two");
    CHECK(closef(planes[5][2], 1.0f, 1e-6f) &&
              closef(planes[5][3], 10.0f, 1e-5f),
          "far plane is row three minus row two");
}

void test_frame_matrix_errors_are_specific() {
    viewer::FrameMatrices frame{};
    std::string error;
    matter::CameraDesc camera{};

    CHECK(!viewer::build_frame_matrices(camera, 0, 720, frame, error) &&
              error == "framebuffer extent must be non-zero",
          "zero extent error");

    camera.near_plane = 5.0f;
    camera.far_plane = 5.0f;
    CHECK(!viewer::build_frame_matrices(camera, 1280, 720, frame, error) &&
              error == "camera planes must satisfy 0 < near < far",
          "invalid plane error");

    camera = {};
    camera.target = camera.position;
    CHECK(!viewer::build_frame_matrices(camera, 1280, 720, frame, error) &&
              error == "camera direction must be non-degenerate",
          "degenerate direction error");

    camera = {};
    camera.up = {camera.target.x - camera.position.x,
                 camera.target.y - camera.position.y,
                 camera.target.z - camera.position.z};
    CHECK(!viewer::build_frame_matrices(camera, 1280, 720, frame, error) &&
              error == "camera up vector must not be parallel to direction",
          "degenerate up error");

    camera = {};
    camera.vertical_fov_radians = 0.0f;
    CHECK(!viewer::build_frame_matrices(camera, 1280, 720, frame, error) &&
              error == "world-to-clip matrix is singular",
          "singular inverse error");
}

void test_singular_matrix_inverse_fails() {
    matter::Mat4f inverse{};
    CHECK(!viewer::mat4_inverse({}, inverse), "singular inverse fails");
}

void test_small_invertible_pivot_is_not_called_singular() {
    matter::Mat4f matrix = viewer::mat4_identity();
    matrix.m[15] = 1e-13f;
    matter::Mat4f inverse{};
    CHECK(viewer::mat4_inverse(matrix, inverse) &&
              closef(inverse.m[15], 1e13f, 1e7f),
          "small invertible pivot");
}

void test_unrepresentable_depth_range_has_specific_error() {
    // Under the old (non-reversed) formula, near=1e-13/far=1.0 rounded the
    // depth-scale term to exactly -1.0f (catastrophic cancellation). The
    // reversed formula (near/(far-near)) does not collapse for that same
    // pair - near/(far-near) is a tiny-but-representable positive float, so
    // that camera is no longer degenerate. The reversed formula's actual
    // failure mode is near underflowing to nothing *relative to* far: with
    // near=1e-30/far=1e30, far-near == far in float32 (near is 60 orders of
    // magnitude below far's ULP), so depth_scale = 1e-30/1e30 = 1e-60, far
    // below float32's smallest subnormal (~1.4e-45) and rounds to exactly
    // 0.0f.
    matter::CameraDesc camera{};
    camera.near_plane = 1e-30f;
    camera.far_plane = 1e30f;
    viewer::FrameMatrices frame{};
    std::string error;
    CHECK(!viewer::build_frame_matrices(camera, 1280, 720, frame, error) &&
              error == "camera depth range is not representable in Mat4f",
          "unrepresentable depth range error");
}

void test_reversed_projection_maps_near_far_to_one_zero() {
    const auto projection = viewer::perspective_rh_zo_reversed(
        60.0f * kPi / 180.0f, 16.0f / 9.0f, 1.0f, 5000.0f);
    CHECK(closef(viewer::project_ndc(projection, {0.0f, 0.0f, -1.0f}).z,
                  1.0f, 1e-5f),
          "reversed near maps to one");
    CHECK(closef(viewer::project_ndc(projection, {0.0f, 0.0f, -5000.0f}).z,
                  0.0f, 1e-5f),
          "reversed far maps to zero");
}

void test_reversed_projection_mid_point_is_between_and_monotonic() {
    const auto projection = viewer::perspective_rh_zo_reversed(
        60.0f * kPi / 180.0f, 16.0f / 9.0f, 1.0f, 5000.0f);
    const float depth_near_side =
        viewer::project_ndc(projection, {0.0f, 0.0f, -100.0f}).z;
    const float depth_far_side =
        viewer::project_ndc(projection, {0.0f, 0.0f, -1000.0f}).z;
    CHECK(depth_near_side > 0.0f && depth_near_side < 1.0f,
          "reversed mid-distance depth is strictly between zero and one");
    CHECK(depth_far_side > 0.0f && depth_far_side < 1.0f,
          "reversed farther mid-distance depth is strictly between zero and one");
    CHECK(depth_far_side < depth_near_side,
          "reversed depth decreases as distance increases");
}

void test_reversed_frustum_planes_reject_behind_accept_mid_frustum() {
    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 5.0f};
    camera.target = {0.0f, 0.0f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 60.0f * kPi / 180.0f;
    camera.near_plane = 1.0f;
    camera.far_plane = 100.0f;
    viewer::FrameMatrices frame{};
    std::string error;
    CHECK(viewer::build_frame_matrices(camera, 1280, 720, frame, error),
          "build reversed frame matrices");

    auto inside_all_planes = [&](matter::Float3 point) {
        for (int plane = 0; plane < 6; ++plane) {
            const float side =
                frame.frustum_planes[plane][0] * point.x +
                frame.frustum_planes[plane][1] * point.y +
                frame.frustum_planes[plane][2] * point.z +
                frame.frustum_planes[plane][3];
            if (side < 0.0f) {
                return false;
            }
        }
        return true;
    };

    // Behind the camera, opposite the view direction: must be rejected.
    CHECK(!inside_all_planes({0.0f, 0.0f, 10.0f}),
          "point behind camera is culled");
    // On the boresight, mid-frustum (well within [near, far]): must be kept.
    CHECK(inside_all_planes({0.0f, 0.0f, -20.0f}),
          "point mid-frustum is kept");
}

void test_reversed_frustum_planes_near_far_boundaries() {
    matter::CameraDesc camera{};
    camera.position = {0.0f, 0.0f, 5.0f};
    camera.target = {0.0f, 0.0f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 60.0f * kPi / 180.0f;
    camera.near_plane = 1.0f;
    camera.far_plane = 100.0f;
    viewer::FrameMatrices frame{};
    std::string error;
    CHECK(viewer::build_frame_matrices(camera, 1280, 720, frame, error),
          "build reversed frame matrices for boundary test");

    auto inside_all_planes = [&](matter::Float3 point) {
        for (int plane = 0; plane < 6; ++plane) {
            const float side =
                frame.frustum_planes[plane][0] * point.x +
                frame.frustum_planes[plane][1] * point.y +
                frame.frustum_planes[plane][2] * point.z +
                frame.frustum_planes[plane][3];
            if (side < 0.0f) {
                return false;
            }
        }
        return true;
    };

    // Camera looks down -Z from z=5; world point along the boresight at
    // distance d is {0, 0, 5 - d}. This isolates the near/far test: with
    // x == y == 0 the left/right/top/bottom planes are always satisfied.
    const matter::Float3 just_inside_near{0.0f, 0.0f, 5.0f - 1.01f};
    const matter::Float3 just_outside_near{0.0f, 0.0f, 5.0f - 0.99f};
    const matter::Float3 just_inside_far{0.0f, 0.0f, 5.0f - 99.9f};
    const matter::Float3 just_outside_far{0.0f, 0.0f, 5.0f - 100.1f};

    // This is the test that catches a near/far plane-slot swap bug: if the
    // extractor (or a call-site fix) incorrectly relabeled slots 4/5 instead
    // of leaving the half-spaces as-is, one of these four would flip.
    CHECK(inside_all_planes(just_inside_near), "just inside near is kept");
    CHECK(!inside_all_planes(just_outside_near),
          "just outside (closer than) near is culled");
    CHECK(inside_all_planes(just_inside_far), "just inside far is kept");
    CHECK(!inside_all_planes(just_outside_far),
          "just outside (farther than) far is culled");
}

} // namespace

int main() {
    test_matrix_translation_uses_3_7_11();
    test_vulkan_projection_maps_near_far_to_zero_one();
    test_composition_is_projection_times_view();
    test_glsl_pack_is_explicit();
    test_inverse_and_unproject_round_trip();
    test_frustum_planes_use_vulkan_zero_to_one_rows();
    test_frame_matrix_errors_are_specific();
    test_singular_matrix_inverse_fails();
    test_small_invertible_pivot_is_not_called_singular();
    test_unrepresentable_depth_range_has_specific_error();
    test_reversed_projection_maps_near_far_to_one_zero();
    test_reversed_projection_mid_point_is_between_and_monotonic();
    test_reversed_frustum_planes_reject_behind_accept_mid_frustum();
    test_reversed_frustum_planes_near_far_boundaries();
    // test_active_raygen_kernels_use_vulkan_ndc_depth was removed: it read
    // src/render/shaders_rt/*.cu, which was deleted along with the whole
    // CUDA/OptiX RT path in commit d23da337.
    return check_summary();
}
