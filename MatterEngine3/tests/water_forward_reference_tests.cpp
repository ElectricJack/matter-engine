#include "check.h"
#include "render/water_forward_reference.h"

#include <cmath>

namespace {

constexpr float kEpsilon = 1.0e-5f;

bool close(float actual, float expected, float epsilon = kEpsilon) {
    return std::fabs(actual - expected) <= epsilon;
}

void check_finite_nonnegative(const viewer::WaterScreenDepth& depth,
                              const char* message) {
    CHECK(std::isfinite(depth.distance_m) && depth.distance_m >= 0.0f,
          message);
}

void test_accepts_continuous_opaque_depth_behind_water() {
    const viewer::WaterScreenDepth depth =
        viewer::water_screen_depth_reference(2.0f, 10.0f, 12.0f, 12.5f,
                                             0.75f);
    CHECK(depth.valid && close(depth.distance_m, 2.5f),
          "continuous center/refracted depth behind water uses screen distance");
    CHECK(depth.distance_m <= 4.0f,
          "accepted screen distance stays within the baked-depth ceiling");
    check_finite_nonnegative(depth,
                             "accepted screen distance is finite and non-negative");
}

void test_clamps_accepted_screen_distance_to_baked_ceiling() {
    const viewer::WaterScreenDepth depth =
        viewer::water_screen_depth_reference(0.5f, 5.0f, 6.0f, 6.5f,
                                             0.75f);
    CHECK(depth.valid && close(depth.distance_m, 1.0f),
          "accepted screen distance clamps to max twice baked or baked plus half a metre");
    check_finite_nonnegative(depth,
                             "clamped screen distance is finite and non-negative");
}

void test_reversed_z_sky_uses_baked_depth() {
    const viewer::WaterScreenDepth depth =
        viewer::water_screen_depth_reference(1.25f, 8.0f, 0.0f, 0.0f,
                                             0.75f);
    CHECK(!depth.valid && close(depth.distance_m, 1.25f),
          "reversed-Z sky depth falls back to baked water depth");
    check_finite_nonnegative(depth,
                             "sky fallback distance is finite and non-negative");
}

void test_opaque_point_in_front_of_water_uses_baked_depth() {
    const viewer::WaterScreenDepth depth =
        viewer::water_screen_depth_reference(1.5f, 10.0f, 12.0f, 9.5f,
                                             0.75f);
    CHECK(!depth.valid && close(depth.distance_m, 1.5f),
          "opaque geometry reconstructed in front of water uses baked depth");
    check_finite_nonnegative(depth,
                             "front-point fallback distance is finite and non-negative");
}

void test_depth_discontinuity_uses_baked_depth() {
    const viewer::WaterScreenDepth depth =
        viewer::water_screen_depth_reference(2.0f, 10.0f, 11.0f, 11.751f,
                                             0.75f);
    CHECK(!depth.valid && close(depth.distance_m, 2.0f),
          "refracted depth discontinuity over three quarters of a metre uses baked depth");
    check_finite_nonnegative(depth,
                             "discontinuity fallback distance is finite and non-negative");
}

void test_refraction_rejects_uv_outside_half_texel_inset() {
    const matter::Float2 source{0.99f, 0.5f};
    const matter::Float2 refracted = viewer::water_refraction_uv_reference(
        source, {1.0f, 0.0f}, 1.0f, {100.0f, 80.0f}, 24.0f);
    CHECK(close(refracted.x, source.x) && close(refracted.y, source.y),
          "refraction leaving the half-texel inset returns undisplaced UV");
}

void test_refraction_offset_clamps_to_twenty_four_pixels() {
    const matter::Float2 refracted = viewer::water_refraction_uv_reference(
        {0.5f, 0.5f}, {1.0f, 0.0f}, 40.0f, {100.0f, 80.0f}, 24.0f);
    CHECK(close(refracted.x, 0.74f) && close(refracted.y, 0.5f),
          "requested refraction offset is clamped to twenty four pixels");
}

void test_flat_up_normal_has_zero_refraction_displacement() {
    const matter::Float2 source{0.35f, 0.65f};
    const matter::Float2 refracted = viewer::water_refraction_uv_reference(
        source, {0.0f, 0.0f}, 40.0f, {100.0f, 200.0f}, 24.0f);
    CHECK(close(refracted.x, source.x) && close(refracted.y, source.y),
          "flat up-normal xz projection has zero refraction displacement");
}

void test_refraction_direction_and_pixel_scale_are_explicit() {
    const matter::Float2 refracted = viewer::water_refraction_uv_reference(
        {0.5f, 0.5f}, {0.25f, -0.5f}, 4.0f,
        {100.0f, 200.0f}, 24.0f);
    CHECK(close(refracted.x, 0.51f) && close(refracted.y, 0.49f),
          "positive normal x moves UV right and negative normal z moves UV up at one pixel per metre");
}

}  // namespace

int main() {
    test_accepts_continuous_opaque_depth_behind_water();
    test_clamps_accepted_screen_distance_to_baked_ceiling();
    test_reversed_z_sky_uses_baked_depth();
    test_opaque_point_in_front_of_water_uses_baked_depth();
    test_depth_discontinuity_uses_baked_depth();
    test_refraction_rejects_uv_outside_half_texel_inset();
    test_refraction_offset_clamps_to_twenty_four_pixels();
    test_flat_up_normal_has_zero_refraction_displacement();
    test_refraction_direction_and_pixel_scale_are_explicit();
    return check_summary();
}
