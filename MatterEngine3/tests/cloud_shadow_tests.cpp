#include "matter/cloud_shadow_settings.h"
#include "check.h"

#include <cmath>
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
}

} // namespace

int main() {
    test_improved_clipmap_dimensions_and_persistent_memory();
    test_sun_frame_is_orthonormal_and_camera_center_maps_to_lateral_uv_center();
    test_history_invalidation_threshold_and_nonfinite_fail_closed();
    return check_summary();
}
