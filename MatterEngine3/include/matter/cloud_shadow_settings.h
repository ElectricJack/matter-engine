#pragma once

#include "matter/math_types.h"
#include "matter/volumetric_quality.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace matter {

struct CloudShadowSettings {
    bool enabled = true;
    int32_t near_resolution = 1;
    int32_t near_depth_slices = 1;
    float near_coverage_m = 1800.0f;
    int32_t far_resolution = 1;
    int32_t far_depth_slices = 1;
    float far_coverage_m = 4000.0f;
    float filter_scale = 1.0f;
    float update_fraction = 0.25f;
};

struct CloudShadowLevelDesc {
    uint32_t width = 0, height = 0, depth = 0;
    float coverage_m = 0.0f;
};

struct CloudShadowFrame {
    Mat4f world_to_uvw{};
    Mat4f uvw_to_world{};
    Float3 snapped_center{};
    Float3 incoming_light_axis{};
    float voxel_xy_m = 0.0f;
    float voxel_depth_m = 0.0f;
    bool valid = false;
};

namespace detail {

inline bool cloud_shadow_finite(float value) { return std::isfinite(value); }
inline bool cloud_shadow_finite(const Float3& value) {
    return cloud_shadow_finite(value.x) && cloud_shadow_finite(value.y) && cloud_shadow_finite(value.z);
}
inline float cloud_shadow_dot(const Float3& a, const Float3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Float3 cloud_shadow_cross(const Float3& a, const Float3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline bool cloud_shadow_normalize(Float3& value) {
    const float length_squared = cloud_shadow_dot(value, value);
    if (!cloud_shadow_finite(length_squared) || length_squared <= 0.0f) return false;
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
    return cloud_shadow_finite(value);
}
inline bool cloud_shadow_finite(const Mat4f& value) {
    for (float component : value.m) if (!cloud_shadow_finite(component)) return false;
    return true;
}
inline bool cloud_shadow_frame_is_valid(const CloudShadowFrame& frame) {
    return frame.valid && cloud_shadow_finite(frame.world_to_uvw) && cloud_shadow_finite(frame.uvw_to_world) &&
           cloud_shadow_finite(frame.snapped_center) && cloud_shadow_finite(frame.incoming_light_axis) &&
           cloud_shadow_finite(frame.voxel_xy_m) && cloud_shadow_finite(frame.voxel_depth_m) &&
           frame.voxel_xy_m > 0.0f && frame.voxel_depth_m > 0.0f;
}

} // namespace detail

inline std::array<CloudShadowLevelDesc, 2> resolve_cloud_shadow_levels(
    const CloudShadowSettings& settings) {
    constexpr uint32_t near_resolution[] = {128, 256, 512};
    constexpr uint32_t near_depth[] = {16, 32, 48};
    constexpr uint32_t far_resolution[] = {64, 128, 256};
    constexpr uint32_t far_depth[] = {16, 24, 32};
    const auto index = [](int32_t value) { return value >= 0 && value < 3 ? value : 1; };
    const int near_resolution_index = index(settings.near_resolution);
    const int near_depth_index = index(settings.near_depth_slices);
    const int far_resolution_index = index(settings.far_resolution);
    const int far_depth_index = index(settings.far_depth_slices);
    const float near_coverage = detail::cloud_shadow_finite(settings.near_coverage_m) &&
        settings.near_coverage_m > 0.0f ? settings.near_coverage_m : 1800.0f;
    const float far_coverage = detail::cloud_shadow_finite(settings.far_coverage_m) &&
        settings.far_coverage_m > 0.0f ? settings.far_coverage_m : 4000.0f;
    return {{{near_resolution[near_resolution_index], near_resolution[near_resolution_index],
              near_depth[near_depth_index], near_coverage},
             {far_resolution[far_resolution_index], far_resolution[far_resolution_index],
              far_depth[far_depth_index], far_coverage}}};
}

inline uint64_t estimate_cloud_shadow_bytes(const CloudShadowSettings& settings) {
    if (!settings.enabled) return 0;
    const auto levels = resolve_cloud_shadow_levels(settings);
    uint64_t voxels = 0;
    for (const auto& level : levels) {
        voxels += static_cast<uint64_t>(level.width) * level.height * level.depth;
    }
    return voxels * 3ull * 2ull;
}

inline CloudShadowFrame make_cloud_shadow_frame(
    const CloudShadowLevelDesc& level, const Float3& camera_world, const Float3& sun_direction) {
    CloudShadowFrame frame{};
    if (level.width == 0 || level.height == 0 || level.depth == 0 ||
        !detail::cloud_shadow_finite(level.coverage_m) || level.coverage_m <= 0.0f ||
        !detail::cloud_shadow_finite(camera_world) || !detail::cloud_shadow_finite(sun_direction)) return frame;

    Float3 incoming{-sun_direction.x, -sun_direction.y, -sun_direction.z};
    if (!detail::cloud_shadow_normalize(incoming)) return frame;
    const Float3 world_up{0.0f, 1.0f, 0.0f};
    const Float3 fallback = std::fabs(detail::cloud_shadow_dot(incoming, world_up)) > 0.99f
        ? Float3{1.0f, 0.0f, 0.0f} : world_up;
    Float3 x = detail::cloud_shadow_cross(fallback, incoming);
    if (!detail::cloud_shadow_normalize(x)) return frame;
    Float3 y = detail::cloud_shadow_cross(incoming, x);
    if (!detail::cloud_shadow_normalize(y)) return frame;

    frame.voxel_xy_m = level.coverage_m / static_cast<float>(level.width);
    frame.voxel_depth_m = level.coverage_m / static_cast<float>(level.depth);
    if (!detail::cloud_shadow_finite(frame.voxel_xy_m) || !detail::cloud_shadow_finite(frame.voxel_depth_m) ||
        frame.voxel_xy_m <= 0.0f || frame.voxel_depth_m <= 0.0f) return CloudShadowFrame{};
    const float cx = std::round(detail::cloud_shadow_dot(camera_world, x) / frame.voxel_xy_m) * frame.voxel_xy_m;
    const float cy = std::round(detail::cloud_shadow_dot(camera_world, y) / frame.voxel_xy_m) * frame.voxel_xy_m;
    const float cz = std::round(detail::cloud_shadow_dot(camera_world, incoming) / frame.voxel_depth_m) * frame.voxel_depth_m;
    frame.snapped_center = {x.x * cx + y.x * cy + incoming.x * cz,
                            x.y * cx + y.y * cy + incoming.y * cz,
                            x.z * cx + y.z * cy + incoming.z * cz};
    if (!detail::cloud_shadow_finite(frame.snapped_center)) return CloudShadowFrame{};

    const float inv_coverage = 1.0f / level.coverage_m;
    frame.world_to_uvw.m[0] = x.x * inv_coverage;
    frame.world_to_uvw.m[1] = x.y * inv_coverage;
    frame.world_to_uvw.m[2] = x.z * inv_coverage;
    frame.world_to_uvw.m[3] = 0.5f - detail::cloud_shadow_dot(x, frame.snapped_center) * inv_coverage;
    frame.world_to_uvw.m[4] = y.x * inv_coverage;
    frame.world_to_uvw.m[5] = y.y * inv_coverage;
    frame.world_to_uvw.m[6] = y.z * inv_coverage;
    frame.world_to_uvw.m[7] = 0.5f - detail::cloud_shadow_dot(y, frame.snapped_center) * inv_coverage;
    frame.world_to_uvw.m[8] = incoming.x * inv_coverage;
    frame.world_to_uvw.m[9] = incoming.y * inv_coverage;
    frame.world_to_uvw.m[10] = incoming.z * inv_coverage;
    frame.world_to_uvw.m[11] = 0.5f - detail::cloud_shadow_dot(incoming, frame.snapped_center) * inv_coverage;
    frame.world_to_uvw.m[15] = 1.0f;

    frame.uvw_to_world.m[0] = x.x * level.coverage_m;
    frame.uvw_to_world.m[1] = y.x * level.coverage_m;
    frame.uvw_to_world.m[2] = incoming.x * level.coverage_m;
    frame.uvw_to_world.m[3] = frame.snapped_center.x - 0.5f * level.coverage_m * (x.x + y.x + incoming.x);
    frame.uvw_to_world.m[4] = x.y * level.coverage_m;
    frame.uvw_to_world.m[5] = y.y * level.coverage_m;
    frame.uvw_to_world.m[6] = incoming.y * level.coverage_m;
    frame.uvw_to_world.m[7] = frame.snapped_center.y - 0.5f * level.coverage_m * (x.y + y.y + incoming.y);
    frame.uvw_to_world.m[8] = x.z * level.coverage_m;
    frame.uvw_to_world.m[9] = y.z * level.coverage_m;
    frame.uvw_to_world.m[10] = incoming.z * level.coverage_m;
    frame.uvw_to_world.m[11] = frame.snapped_center.z - 0.5f * level.coverage_m * (x.z + y.z + incoming.z);
    frame.uvw_to_world.m[15] = 1.0f;
    frame.incoming_light_axis = incoming;
    frame.valid = detail::cloud_shadow_finite(frame.world_to_uvw) && detail::cloud_shadow_finite(frame.uvw_to_world);
    return frame;
}

inline bool cloud_shadow_requires_full_invalidation(
    const CloudShadowFrame& previous, const CloudShadowFrame& next, float sun_angle_delta_deg) {
    if (!detail::cloud_shadow_frame_is_valid(previous) || !detail::cloud_shadow_frame_is_valid(next) ||
        !detail::cloud_shadow_finite(sun_angle_delta_deg)) return true;
    if (sun_angle_delta_deg > 2.0f) return true;
    const float dimensions_epsilon = 1.0e-5f;
    if (std::fabs(previous.voxel_xy_m - next.voxel_xy_m) > dimensions_epsilon ||
        std::fabs(previous.voxel_depth_m - next.voxel_depth_m) > dimensions_epsilon) return true;
    const float inverse_coverage = std::sqrt(previous.world_to_uvw.m[0] * previous.world_to_uvw.m[0] +
        previous.world_to_uvw.m[1] * previous.world_to_uvw.m[1] + previous.world_to_uvw.m[2] * previous.world_to_uvw.m[2]);
    const float next_inverse_coverage = std::sqrt(next.world_to_uvw.m[0] * next.world_to_uvw.m[0] +
        next.world_to_uvw.m[1] * next.world_to_uvw.m[1] + next.world_to_uvw.m[2] * next.world_to_uvw.m[2]);
    if (!detail::cloud_shadow_finite(inverse_coverage) || !detail::cloud_shadow_finite(next_inverse_coverage) ||
        inverse_coverage <= 0.0f || next_inverse_coverage <= 0.0f ||
        std::fabs(inverse_coverage - next_inverse_coverage) > 1.0e-8f) return true;
    const float guard_band_m = 0.08f / inverse_coverage;
    const Float3 delta{next.snapped_center.x - previous.snapped_center.x,
                       next.snapped_center.y - previous.snapped_center.y,
                       next.snapped_center.z - previous.snapped_center.z};
    const Float3 x{previous.uvw_to_world.m[0] * inverse_coverage,
                   previous.uvw_to_world.m[4] * inverse_coverage,
                   previous.uvw_to_world.m[8] * inverse_coverage};
    const Float3 y{previous.uvw_to_world.m[1] * inverse_coverage,
                   previous.uvw_to_world.m[5] * inverse_coverage,
                   previous.uvw_to_world.m[9] * inverse_coverage};
    const Float3 z = previous.incoming_light_axis;
    return std::fabs(detail::cloud_shadow_dot(delta, x)) > guard_band_m ||
           std::fabs(detail::cloud_shadow_dot(delta, y)) > guard_band_m ||
           std::fabs(detail::cloud_shadow_dot(delta, z)) > guard_band_m;
}

inline bool enhanced_cloud_lighting(const VulkanVolumetricsSettings& volumetrics,
                                    const CloudShadowSettings& shadows) {
    return volumetrics.local_sun_march_steps > 0 || volumetrics.multiple_scattering_orders > 1 ||
           volumetrics.powder_strength > 0.0f || shadows.enabled;
}

inline void apply_volumetric_quality_preset(VolumetricQualityPreset preset,
                                            VulkanVolumetricsSettings& volumetrics,
                                            CloudShadowSettings& shadows) {
    switch (preset) {
    case VolumetricQualityPreset::CurrentCost:
        volumetrics.froxel_xy_scale = FroxelXyScale::X1_0;
        volumetrics.froxel_depth_slices = FroxelDepthSlices::D128;
        volumetrics.local_sun_march_steps = 0;
        volumetrics.local_sun_march_distance_m = 250.0f;
        volumetrics.multiple_scattering_orders = 1;
        volumetrics.multiple_scattering_strength = 0.0f;
        volumetrics.powder_strength = 0.0f;
        shadows.enabled = false; shadows.near_resolution = 1; shadows.near_depth_slices = 1;
        shadows.near_coverage_m = 1800.0f; shadows.far_resolution = 1; shadows.far_depth_slices = 1;
        shadows.far_coverage_m = 4000.0f; shadows.filter_scale = 1.0f; shadows.update_fraction = 0.25f;
        break;
    case VolumetricQualityPreset::Improved:
        volumetrics.froxel_xy_scale = FroxelXyScale::X1_0;
        volumetrics.froxel_depth_slices = FroxelDepthSlices::D128;
        volumetrics.local_sun_march_steps = 8; volumetrics.local_sun_march_distance_m = 250.0f;
        volumetrics.multiple_scattering_orders = 2; volumetrics.multiple_scattering_strength = 0.55f;
        volumetrics.powder_strength = 0.25f;
        shadows.enabled = true; shadows.near_resolution = 1; shadows.near_depth_slices = 1;
        shadows.near_coverage_m = 1800.0f; shadows.far_resolution = 1; shadows.far_depth_slices = 1;
        shadows.far_coverage_m = 4000.0f; shadows.filter_scale = 1.0f; shadows.update_fraction = 0.25f;
        break;
    case VolumetricQualityPreset::High:
        volumetrics.froxel_xy_scale = FroxelXyScale::X1_5;
        volumetrics.froxel_depth_slices = FroxelDepthSlices::D192;
        volumetrics.local_sun_march_steps = 12; volumetrics.local_sun_march_distance_m = 350.0f;
        volumetrics.multiple_scattering_orders = 3; volumetrics.multiple_scattering_strength = 0.70f;
        volumetrics.powder_strength = 0.35f;
        shadows.enabled = true; shadows.near_resolution = 2; shadows.near_depth_slices = 1;
        shadows.near_coverage_m = 2200.0f; shadows.far_resolution = 2; shadows.far_depth_slices = 1;
        shadows.far_coverage_m = 4500.0f; shadows.filter_scale = 1.0f; shadows.update_fraction = 0.50f;
        break;
    case VolumetricQualityPreset::Ultra:
        volumetrics.froxel_xy_scale = FroxelXyScale::X2_0;
        volumetrics.froxel_depth_slices = FroxelDepthSlices::D256;
        volumetrics.local_sun_march_steps = 24; volumetrics.local_sun_march_distance_m = 500.0f;
        volumetrics.multiple_scattering_orders = 4; volumetrics.multiple_scattering_strength = 0.85f;
        volumetrics.powder_strength = 0.50f;
        shadows.enabled = true; shadows.near_resolution = 2; shadows.near_depth_slices = 2;
        shadows.near_coverage_m = 2500.0f; shadows.far_resolution = 2; shadows.far_depth_slices = 2;
        shadows.far_coverage_m = 5000.0f; shadows.filter_scale = 1.0f; shadows.update_fraction = 1.00f;
        break;
    case VolumetricQualityPreset::Custom: break;
    }
}

inline VolumetricQualityPreset identify_volumetric_quality_preset(
    const VulkanVolumetricsSettings& volumetrics, const CloudShadowSettings& shadows) {
    for (const auto preset : {VolumetricQualityPreset::CurrentCost, VolumetricQualityPreset::Improved,
                              VolumetricQualityPreset::High, VolumetricQualityPreset::Ultra}) {
        VulkanVolumetricsSettings expected_volumetrics = volumetrics;
        CloudShadowSettings expected_shadows = shadows;
        apply_volumetric_quality_preset(preset, expected_volumetrics, expected_shadows);
        if (expected_volumetrics.froxel_xy_scale == volumetrics.froxel_xy_scale &&
            expected_volumetrics.froxel_depth_slices == volumetrics.froxel_depth_slices &&
            expected_volumetrics.local_sun_march_steps == volumetrics.local_sun_march_steps &&
            expected_volumetrics.local_sun_march_distance_m == volumetrics.local_sun_march_distance_m &&
            expected_volumetrics.multiple_scattering_orders == volumetrics.multiple_scattering_orders &&
            expected_volumetrics.multiple_scattering_strength == volumetrics.multiple_scattering_strength &&
            expected_volumetrics.powder_strength == volumetrics.powder_strength &&
            expected_shadows.enabled == shadows.enabled && expected_shadows.near_resolution == shadows.near_resolution &&
            expected_shadows.near_depth_slices == shadows.near_depth_slices && expected_shadows.near_coverage_m == shadows.near_coverage_m &&
            expected_shadows.far_resolution == shadows.far_resolution && expected_shadows.far_depth_slices == shadows.far_depth_slices &&
            expected_shadows.far_coverage_m == shadows.far_coverage_m && expected_shadows.filter_scale == shadows.filter_scale &&
            expected_shadows.update_fraction == shadows.update_fraction) return preset;
    }
    return VolumetricQualityPreset::Custom;
}

} // namespace matter
