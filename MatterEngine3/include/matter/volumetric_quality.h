#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace matter {

struct CloudShadowSettings;

enum class FroxelXyScale : int32_t { X0_5 = 0, X0_75, X1_0, X1_5, X2_0 };
enum class FroxelDepthSlices : int32_t { D64 = 0, D96, D128, D192, D256 };

struct FroxelGridDimensions { uint32_t width, height, depth; };

struct VulkanVolumetricsSettings {
    bool enabled = false;
    float temporal_blend = 0.85f;
    float phase_g = 0.3f;
    float vol_debug_view = 0.0f;
    FroxelXyScale froxel_xy_scale = FroxelXyScale::X1_0;
    FroxelDepthSlices froxel_depth_slices = FroxelDepthSlices::D128;
    int32_t local_sun_march_steps = 8;
    float local_sun_march_distance_m = 250.0f;
    int32_t multiple_scattering_orders = 2;
    float multiple_scattering_strength = 0.55f;
    float powder_strength = 0.25f;
};

enum class VolumetricQualityPreset : int32_t {
    CurrentCost = 0, Improved, High, Ultra, Custom
};

inline FroxelGridDimensions resolve_froxel_grid(const VulkanVolumetricsSettings& settings) {
    constexpr float xy[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
    constexpr uint32_t depth[] = {64, 96, 128, 192, 256};
    const int raw_xy = static_cast<int>(settings.froxel_xy_scale);
    const int raw_depth = static_cast<int>(settings.froxel_depth_slices);
    const int xy_index = raw_xy >= 0 && raw_xy < 5 ? raw_xy : 2;
    const int depth_index = raw_depth >= 0 && raw_depth < 5 ? raw_depth : 2;
    return {static_cast<uint32_t>(std::lround(160.0f * xy[xy_index])),
            static_cast<uint32_t>(std::lround(90.0f * xy[xy_index])), depth[depth_index]};
}

inline uint64_t estimate_froxel_bytes(FroxelGridDimensions dimensions, bool enhanced_clouds) {
    const auto saturating_multiply = [](uint64_t left, uint64_t right) {
        constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
        return left != 0 && right > maximum / left ? maximum : left * right;
    };
    const uint64_t voxels = saturating_multiply(
        saturating_multiply(static_cast<uint64_t>(dimensions.width), dimensions.height), dimensions.depth);
    return saturating_multiply(voxels, 4ull * 8ull + (enhanced_clouds ? 2ull : 0ull));
}

bool enhanced_cloud_lighting(const VulkanVolumetricsSettings&, const CloudShadowSettings&);
void apply_volumetric_quality_preset(VolumetricQualityPreset,
                                     VulkanVolumetricsSettings&, CloudShadowSettings&);
VolumetricQualityPreset identify_volumetric_quality_preset(
    const VulkanVolumetricsSettings&, const CloudShadowSettings&);

} // namespace matter
