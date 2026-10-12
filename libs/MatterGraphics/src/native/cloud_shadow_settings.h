#pragma once

// MatterEngine3/include/matter/cloud_shadow_settings.h
//
// Cloud-shadow volume configuration and the pure math that turns it into a
// per-frame shadow volume. Header-only: everything here is `inline`, touches
// no Vulkan, allocates nothing and keeps no global state, so the renderer,
// the property editor and the headless suites all share one implementation
// (MatterEngine3/tests/cloud_shadow_tests.cpp,
// MatterEngine3/tests/volumetric_quality_tests.cpp).
//
// How it fits:
//   * `matter/volumetric_quality.h` holds the froxel-grid half of the same
//     settings block and only DECLARES `enhanced_cloud_lighting`,
//     `apply_volumetric_quality_preset` and
//     `identify_volumetric_quality_preset`; their definitions live at the
//     bottom of THIS header. Include cloud_shadow_settings.h — not just
//     volumetric_quality.h — wherever you call them.
//   * MatterEngine3/src/render/vk_cloud_shadows.cpp owns the GPU side. It
//     calls `resolve_cloud_shadow_levels` for the image dimensions,
//     `make_cloud_shadow_frame` once per level per frame, and
//     `cloud_shadow_requires_full_invalidation` to choose between a full
//     regeneration and an incremental refresh.
//   * MatterEditor/src/property_editor.cpp drives the same structs from the
//     property system.
//
// Conventions and gotchas:
//   * Distances are METRES. Light/sun vectors are world-space directions.
//     "UVW" is the normalized [0,1]^3 space of a shadow volume.
//   * The `*_resolution` / `*_depth_slices` fields are quality INDICES, not
//     pixel counts — see CloudShadowSettings below.
//   * Every function here is deliberately defensive: a non-finite or
//     non-positive input yields a documented default or an invalid frame
//     (`CloudShadowFrame::valid == false`) rather than letting a NaN reach
//     the shadow volume. Callers must check `.valid`.
//   * The two cascade levels are always ordered {near, far}; index 0 is near.

#include "matter/math_types.h"
#include "matter/volumetric_quality.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace matter {

// User-facing cloud-shadow tunables, as authored by the property system.
//
// The four resolution/slice fields are quality INDICES into the small tables
// in resolve_cloud_shadow_levels (0 = low, 1 = medium, 2 = high); anything
// outside 0..2 falls back to 1. Coverage is a world-space edge length in
// metres; non-finite or non-positive falls back to the default below.
//
// VkCloudShadows sanitizes two fields again on its own side: a non-finite or
// negative `filter_scale` becomes 1, and `update_fraction` is clamped into
// [0.0625, 1].
//
// `identify_volumetric_quality_preset` compares this struct FIELD BY FIELD,
// so a new field has to be added to both that comparison and every arm of
// `apply_volumetric_quality_preset` or the UI will report Custom forever.
struct CloudShadowSettings {
    bool enabled = true;
    int32_t near_resolution = 1;     // index 0/1/2 -> 128/256/512 texels (XY)
    int32_t near_depth_slices = 1;   // index 0/1/2 -> 16/32/48 slices
    float near_coverage_m = 1800.0f; // metres spanned by the near volume
    int32_t far_resolution = 1;      // index 0/1/2 -> 64/128/256 texels (XY)
    int32_t far_depth_slices = 1;    // index 0/1/2 -> 16/24/32 slices
    float far_coverage_m = 4000.0f;  // metres spanned by the far volume
    float filter_scale = 1.0f;       // spatial filter width multiplier
    float update_fraction = 0.25f;   // share of the volume refreshed per frame
};

// One resolved cascade level: the concrete voxel dimensions and the world
// extent they cover. Produced by resolve_cloud_shadow_levels, consumed by
// make_cloud_shadow_frame and by VkCloudShadows when it allocates images.
// `width == height` by construction. `coverage_m` is the same edge length
// along XY and along depth, so the depth voxel is coverage_m / depth and is
// generally NOT cubic with the XY voxel.
// A zeroed desc (any dimension 0) means "not resolved" and is rejected by
// make_cloud_shadow_frame.
struct CloudShadowLevelDesc {
    uint32_t width = 0, height = 0, depth = 0;  // voxels
    float coverage_m = 0.0f;                    // metres per volume edge
};

// The per-level, per-frame placement of a shadow volume: an orthonormal basis
// whose third axis is the incoming light direction, centred on a
// voxel-snapped point near the camera.
//
// Both matrices are `Mat4f` from matter/math_types.h — row-major storage,
// column-vector algebra — so the translation terms live in m[3], m[7], m[11].
// `world_to_uvw` maps world metres into [0,1]^3 with the snapped centre at
// 0.5; `uvw_to_world` is its inverse.
//
// A default-constructed frame is invalid. `valid == false` means the inputs
// were degenerate and nothing may be sampled from this frame; the GPU side
// checks it before using the volume.
struct CloudShadowFrame {
    Mat4f world_to_uvw{};          // world metres -> [0,1]^3 volume space
    Mat4f uvw_to_world{};          // inverse of world_to_uvw
    Float3 snapped_center{};       // world metres, quantized to whole voxels
    Float3 incoming_light_axis{};  // unit; == -sun_direction, the volume's z
    float voxel_xy_m = 0.0f;       // metres per voxel across the light
    float voxel_depth_m = 0.0f;    // metres per voxel along the light
    bool valid = false;            // false = degenerate input, do not sample
};

// Small finite-checked vector helpers, kept local so this header stays
// dependency-free (no MathLib, no raylib). They exist mainly so every entry
// point below can reject NaN/Inf before it can reach a shadow volume.
// `cloud_shadow_normalize` returns false for a degenerate or non-finite
// input, in which case `value` holds no usable result.
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

// Maps the quality indices in `settings` onto concrete voxel dimensions.
// Returns {near, far} in that fixed order. Cannot fail: an out-of-range index
// falls back to the medium entry and a non-finite or non-positive coverage
// falls back to the CloudShadowSettings default, so the result is always
// usable. Ignores `settings.enabled`.
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

// Approximate device memory for both levels, in bytes; 0 when disabled.
// Assumes three R16_SFLOAT volumes per level (VkCloudShadows allocates a
// density volume plus its cumulative pair), i.e. 3 x 2 bytes per voxel. It is
// a budget/HUD estimate, not what the allocator actually reserves — real
// allocations carry alignment and view overhead.
inline uint64_t estimate_cloud_shadow_bytes(const CloudShadowSettings& settings) {
    if (!settings.enabled) return 0;
    const auto levels = resolve_cloud_shadow_levels(settings);
    uint64_t voxels = 0;
    for (const auto& level : levels) {
        voxels += static_cast<uint64_t>(level.width) * level.height * level.depth;
    }
    return voxels * 3ull * 2ull;
}

// Builds the shadow-volume basis for one cascade level.
//
// The volume's third axis is `-sun_direction` (stored as
// `incoming_light_axis`); the other two are an arbitrary but stable
// perpendicular pair, with a fallback axis chosen when the light is within
// ~8 degrees of world up. `camera_world` is in metres.
//
// The centre is snapped to whole voxels along all three axes so the volume
// translates in texel steps as the camera moves — that is what keeps the
// contents reusable between frames.
//
// Returns a frame with `valid == false` for a zero-sized level, a non-finite
// input or a degenerate basis. ALWAYS check `.valid`; a false result is a
// normal outcome, not an error to report.
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

// True when the previous frame's volume contents cannot be reused and must be
// regenerated from scratch. That happens when either frame is invalid, when
// the sun moved more than 2 degrees, when the voxel size or the coverage
// changed, or when the snapped centre slid past a guard band of 8% of the
// coverage along any axis.
//
// `sun_angle_delta_deg` is in DEGREES since the previous frame. Any
// non-finite input returns true — the conservative answer is always "rebuild".
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

// ---------------------------------------------------------------------------
// Definitions of the three functions declared in matter/volumetric_quality.h
// ---------------------------------------------------------------------------

// True when any of the expensive cloud-lighting terms is switched on. This is
// the flag `estimate_froxel_bytes` takes to account for the extra per-froxel
// storage those terms need.
inline bool enhanced_cloud_lighting(const VulkanVolumetricsSettings& volumetrics,
                                    const CloudShadowSettings& shadows) {
    return volumetrics.local_sun_march_steps > 0 || volumetrics.multiple_scattering_orders > 1 ||
           volumetrics.powder_strength > 0.0f || shadows.enabled;
}

// Overwrites both settings structs with the named preset's values. `Custom`
// is deliberately a no-op — it means "keep whatever is already set".
//
// The set of fields written here must stay identical to the set compared in
// identify_volumetric_quality_preset below, or the round trip
// apply -> identify stops recognising its own output.
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

// Reverse lookup: the first preset whose applied values match the current
// settings exactly, else `Custom`. Floats are compared with ==, which is
// sound here only because the values can have come from nowhere but
// apply_volumetric_quality_preset or the UI's discrete widgets.
//
// Implemented by applying every preset to a scratch copy, so it is O(presets)
// struct copies — fine per UI frame, not something to call per render frame.
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
