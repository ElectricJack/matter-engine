#include "matter/cloud_shadow_settings.h"
#include "../src/render/vk_cloud_shadows.h"
#include "check.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

float dot(const matter::Float3& a, const matter::Float3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length(const matter::Float3& value) { return std::sqrt(dot(value, value)); }

matter::Float3 transform(const matter::Mat4f& m, const matter::Float3& value) {
    return {m.m[0] * value.x + m.m[1] * value.y + m.m[2] * value.z + m.m[3],
            m.m[4] * value.x + m.m[5] * value.y + m.m[6] * value.z + m.m[7],
            m.m[8] * value.x + m.m[9] * value.y + m.m[10] * value.z + m.m[11]};
}

void test_improved_clipmap_dimensions_and_persistent_memory() {
    matter::VulkanVolumetricsSettings volumetrics{};
    matter::CloudShadowSettings shadows{};
    matter::apply_volumetric_quality_preset(matter::VolumetricQualityPreset::Improved,
                                            volumetrics, shadows);
    const auto levels = matter::resolve_cloud_shadow_levels(shadows);
    CHECK(levels[0].width == 256 && levels[0].height == 256 && levels[0].depth == 32 &&
              levels[0].coverage_m == 1800.0f,
          "Improved resolves the pinned near clipmap");
    CHECK(levels[1].width == 128 && levels[1].height == 128 && levels[1].depth == 24 &&
              levels[1].coverage_m == 4000.0f,
          "Improved resolves the pinned far clipmap");
    const uint64_t expected = 3ull * 2ull *
        (256ull * 256ull * 32ull + 128ull * 128ull * 24ull);
    CHECK(matter::estimate_cloud_shadow_bytes(shadows) == expected,
          "persistent clipmap memory includes density scratch and cumulative ping-pong R16F images");
    matter::apply_volumetric_quality_preset(matter::VolumetricQualityPreset::CurrentCost,
                                            volumetrics, shadows);
    CHECK(!shadows.enabled && matter::estimate_cloud_shadow_bytes(shadows) == 0,
          "disabled Current cost clipmaps allocate no persistent cloud-shadow bytes");
}

void test_sun_frame_is_orthonormal_and_camera_center_maps_to_lateral_uv_center() {
    const matter::CloudShadowLevelDesc level{256, 256, 32, 1800.0f};
    const matter::Float3 camera{123.4f, 56.7f, -89.1f};
    const auto frame = matter::make_cloud_shadow_frame(level, camera, {0.2f, -0.9f, 0.3f});
    CHECK(frame.valid, "finite camera and sun direction produce a valid cloud-shadow frame");
    const matter::Float3 uvw = transform(frame.world_to_uvw, camera);
    CHECK(std::fabs(uvw.x - 0.5f) < 0.01f && std::fabs(uvw.y - 0.5f) < 0.01f,
          "camera center maps to lateral UV center after snapped frame construction");
    const matter::Float3 x{frame.uvw_to_world.m[0], frame.uvw_to_world.m[4], frame.uvw_to_world.m[8]};
    const matter::Float3 y{frame.uvw_to_world.m[1], frame.uvw_to_world.m[5], frame.uvw_to_world.m[9]};
    CHECK(std::fabs(length(frame.incoming_light_axis) - 1.0f) < 1.0e-5f &&
              std::fabs(dot(x, y)) < 1.0e-5f * length(x) * length(y) &&
              std::fabs(dot(x, frame.incoming_light_axis)) < 1.0e-5f * length(x) &&
              std::fabs(dot(y, frame.incoming_light_axis)) < 1.0e-5f * length(y),
          "incoming-light axis and lateral basis are orthonormal");
}

void test_history_invalidation_threshold_and_nonfinite_fail_closed() {
    const matter::CloudShadowLevelDesc level{256, 256, 32, 1800.0f};
    const auto previous = matter::make_cloud_shadow_frame(level, {0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    const auto next = matter::make_cloud_shadow_frame(level, {0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    CHECK(!matter::cloud_shadow_requires_full_invalidation(previous, next, 0.5f),
          "sub-threshold sun changes retain clipmap history");
    CHECK(matter::cloud_shadow_requires_full_invalidation(previous, next, 2.1f),
          "sun changes above two degrees invalidate clipmap history");
    const auto invalid = matter::make_cloud_shadow_frame(
        level, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    CHECK(!invalid.valid && matter::cloud_shadow_requires_full_invalidation(previous, invalid, 0.0f),
          "non-finite input produces an invalid frame and fails closed");
    auto nan_transform = previous;
    nan_transform.world_to_uvw.m[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(nan_transform.valid && matter::cloud_shadow_requires_full_invalidation(nan_transform, next, 0.0f),
          "non-finite stored transform fails closed even when its valid flag is stale");
    auto infinite_transform = next;
    infinite_transform.uvw_to_world.m[10] = std::numeric_limits<float>::infinity();
    CHECK(infinite_transform.valid && matter::cloud_shadow_requires_full_invalidation(previous, infinite_transform, 0.0f),
          "infinite stored transform fails closed even when its valid flag is stale");
}

void test_task10_sun_space_round_trip_and_stable_snapping() {
    matter::CloudShadowSettings shadows{};
    const auto levels = matter::resolve_cloud_shadow_levels(shadows);
    const matter::Float3 camera{41.25f, 73.5f, -119.75f};
    const matter::Float3 stored_sun_direction{0.31f, -0.91f, 0.27f};
    for (const auto& level : levels) {
        const auto frame = matter::make_cloud_shadow_frame(
            level, camera, stored_sun_direction);
        const matter::Float3 points[] = {
            camera,
            {camera.x + 111.0f, camera.y - 73.0f, camera.z + 29.0f},
            {camera.x - 203.0f, camera.y + 17.0f, camera.z - 91.0f},
        };
        for (const auto& point : points) {
            const auto round_trip = transform(
                frame.uvw_to_world, transform(frame.world_to_uvw, point));
            CHECK(length({round_trip.x - point.x, round_trip.y - point.y,
                          round_trip.z - point.z}) < 1.0e-3f,
                  "Task 10 sun-space world/UVW round trip stays below one millimetre");
        }

        matter::Float3 lateral{frame.uvw_to_world.m[0],
                               frame.uvw_to_world.m[4],
                               frame.uvw_to_world.m[8]};
        const float lateral_length = length(lateral);
        lateral = {lateral.x / lateral_length, lateral.y / lateral_length,
                   lateral.z / lateral_length};
        const matter::Float3 sub_voxel{
            frame.snapped_center.x + lateral.x * frame.voxel_xy_m * 0.49f,
            frame.snapped_center.y + lateral.y * frame.voxel_xy_m * 0.49f,
            frame.snapped_center.z + lateral.z * frame.voxel_xy_m * 0.49f};
        const auto stable = matter::make_cloud_shadow_frame(
            level, sub_voxel, stored_sun_direction);
        CHECK(std::memcmp(frame.world_to_uvw.m, stable.world_to_uvw.m,
                          sizeof(frame.world_to_uvw.m)) == 0,
              "sub-voxel lateral camera motion preserves the snapped transform bit-for-bit");

        const matter::Float3 crossed{
            frame.snapped_center.x + lateral.x * frame.voxel_xy_m * 0.51f,
            frame.snapped_center.y + lateral.y * frame.voxel_xy_m * 0.51f,
            frame.snapped_center.z + lateral.z * frame.voxel_xy_m * 0.51f};
        const auto advanced = matter::make_cloud_shadow_frame(
            level, crossed, stored_sun_direction);
        const auto old_center_in_new = transform(
            advanced.world_to_uvw, frame.snapped_center);
        CHECK(std::fabs(old_center_in_new.x -
                        (0.5f - 1.0f / static_cast<float>(level.width))) <
                  1.0e-6f &&
                  std::fabs(old_center_in_new.y - 0.5f) < 1.0e-6f,
              "crossing one lateral voxel advances the sun frame by exactly one texel");
    }
}

void test_task10_near_up_basis_and_engine_direction_convention() {
    const matter::CloudShadowLevelDesc level{256, 256, 32, 1800.0f};
    const matter::Float3 stored_sun_direction{0.0f, -1.0f, 1.0e-7f};
    const auto frame = matter::make_cloud_shadow_frame(
        level, {0.0f, 0.0f, 0.0f}, stored_sun_direction);
    const auto repeated = matter::make_cloud_shadow_frame(
        level, {0.0f, 0.0f, 0.0f}, stored_sun_direction);
    matter::Float3 x{frame.uvw_to_world.m[0] / level.coverage_m,
                     frame.uvw_to_world.m[4] / level.coverage_m,
                     frame.uvw_to_world.m[8] / level.coverage_m};
    matter::Float3 y{frame.uvw_to_world.m[1] / level.coverage_m,
                     frame.uvw_to_world.m[5] / level.coverage_m,
                     frame.uvw_to_world.m[9] / level.coverage_m};
    CHECK(frame.valid && std::isfinite(x.x) && std::isfinite(x.y) &&
              std::isfinite(x.z) && std::isfinite(y.x) &&
              std::isfinite(y.y) && std::isfinite(y.z) &&
              std::fabs(length(x) - 1.0f) < 1.0e-6f &&
              std::fabs(length(y) - 1.0f) < 1.0e-6f &&
              std::fabs(dot(x, y)) < 1.0e-6f &&
              std::memcmp(frame.world_to_uvw.m, repeated.world_to_uvw.m,
                          sizeof(frame.world_to_uvw.m)) == 0,
          "sun nearly parallel to world up keeps a finite stable orthonormal lateral basis");

    matter::Float3 expected{-stored_sun_direction.x,
                            -stored_sun_direction.y,
                            -stored_sun_direction.z};
    const float expected_length = length(expected);
    expected = {expected.x / expected_length, expected.y / expected_length,
                expected.z / expected_length};
    const matter::Float3 along_incoming{
        frame.snapped_center.x + expected.x * level.coverage_m * 0.25f,
        frame.snapped_center.y + expected.y * level.coverage_m * 0.25f,
        frame.snapped_center.z + expected.z * level.coverage_m * 0.25f};
    const auto uvw = transform(frame.world_to_uvw, along_incoming);
    CHECK(std::fabs(frame.incoming_light_axis.x - expected.x) < 1.0e-6f &&
              std::fabs(frame.incoming_light_axis.y - expected.y) < 1.0e-6f &&
              std::fabs(frame.incoming_light_axis.z - expected.z) < 1.0e-6f &&
              std::fabs(uvw.z - 0.75f) < 1.0e-6f,
          "stored incoming/from-sun direction is consumed exactly once without double negation");
}

void test_task10_edge_blend_filter_and_fail_closed_reference() {
    CHECK(viewer::cloud_shadow_outer_edge_fade({0.0f, 0.5f, 0.5f}) == 0.0f &&
              std::fabs(viewer::cloud_shadow_outer_edge_fade(
                            {0.04f, 0.5f, 0.5f}) - 0.5f) < 1.0e-6f &&
              viewer::cloud_shadow_outer_edge_fade({0.08f, 0.5f, 0.5f}) == 1.0f,
          "outer eight percent of each lateral edge fades optical depth to clear");
    CHECK(viewer::cloud_shadow_filter_radius_texels(
              0.01f, 1000.0f, 2.0f, 1.0f) == 4.0f &&
              viewer::cloud_shadow_filter_radius_texels(
                  0.01f, -1.0f, 2.0f, 1.0f) == 0.0f &&
              viewer::cloud_shadow_filter_radius_texels(
                  std::numeric_limits<float>::quiet_NaN(), 1000.0f,
                  2.0f, 1.0f) == 0.0f,
          "five-tap cross radius is finite and bounded to zero through four texels");

    const std::array<float, 5> clear{{0, 0, 0, 0, 0}};
    const std::array<float, 5> opaque{{100, 100, 100, 100, 100}};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array<float, 5> invalid{{nan, nan, nan, nan, nan}};
    const float clear_t = viewer::cloud_shadow_reference_transmittance(
        {0.5f, 0.5f, 0.5f}, clear);
    const float opaque_t = viewer::cloud_shadow_reference_transmittance(
        {0.5f, 0.5f, 0.5f}, opaque);
    const float invalid_t = viewer::cloud_shadow_reference_transmittance(
        {0.5f, 0.5f, 0.5f}, invalid);
    const float outside_t = viewer::cloud_shadow_reference_transmittance(
        {-0.01f, 0.5f, 0.5f}, opaque);
    const float boundary_t = viewer::cloud_shadow_reference_transmittance(
        {0.0f, 0.5f, 0.5f}, opaque);
    CHECK(clear_t == 1.0f && invalid_t == 1.0f && outside_t == 1.0f &&
              boundary_t == 1.0f && std::isfinite(opaque_t) &&
              opaque_t >= 0.0f && opaque_t <= 1.0f,
          "clear opaque non-finite outside and boundary samples stay finite in zero through one");

    const std::array<float, 5> near_tau{{4, 4, 4, 4, 4}};
    const std::array<float, 5> far_tau{{1, 1, 1, 1, 1}};
    const float center = viewer::cloud_shadow_reference_blended_transmittance(
        {0.5f, 0.5f, 0.5f}, near_tau,
        {0.5f, 0.5f, 0.5f}, far_tau);
    const float guard = viewer::cloud_shadow_reference_blended_transmittance(
        {0.04f, 0.5f, 0.5f}, near_tau,
        {0.5f, 0.5f, 0.5f}, far_tau);
    const float edge = viewer::cloud_shadow_reference_blended_transmittance(
        {0.0f, 0.5f, 0.5f}, near_tau,
        {0.5f, 0.5f, 0.5f}, far_tau);
    CHECK(std::fabs(center - std::exp(-4.0f)) < 1.0e-6f &&
              guard > center && guard < edge &&
              std::fabs(edge - std::exp(-1.0f)) < 1.0e-6f,
          "near clipmap blends into far transmittance across its lateral guard band");
}

} // namespace

int main() {
    test_improved_clipmap_dimensions_and_persistent_memory();
    test_sun_frame_is_orthonormal_and_camera_center_maps_to_lateral_uv_center();
    test_history_invalidation_threshold_and_nonfinite_fail_closed();
    test_task10_sun_space_round_trip_and_stable_snapping();
    test_task10_near_up_basis_and_engine_direction_convention();
    test_task10_edge_blend_filter_and_fail_closed_reference();
    return check_summary();
}
