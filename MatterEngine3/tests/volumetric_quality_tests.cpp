#include "matter/volumetric_quality.h"
#include "matter/cloud_shadow_settings.h"
#include "check.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>

namespace {

bool equal(matter::FroxelGridDimensions a, matter::FroxelGridDimensions b) {
    return a.width == b.width && a.height == b.height && a.depth == b.depth;
}

bool equal(const matter::VulkanVolumetricsSettings& a,
           const matter::VulkanVolumetricsSettings& b) {
    return a.enabled == b.enabled && a.temporal_blend == b.temporal_blend &&
           a.phase_g == b.phase_g && a.vol_debug_view == b.vol_debug_view &&
           a.froxel_xy_scale == b.froxel_xy_scale &&
           a.froxel_depth_slices == b.froxel_depth_slices &&
           a.local_sun_march_steps == b.local_sun_march_steps &&
           a.local_sun_march_distance_m == b.local_sun_march_distance_m &&
           a.multiple_scattering_orders == b.multiple_scattering_orders &&
           a.multiple_scattering_strength == b.multiple_scattering_strength &&
           a.powder_strength == b.powder_strength;
}

bool equal(const matter::CloudShadowSettings& a,
           const matter::CloudShadowSettings& b) {
    return a.enabled == b.enabled && a.near_resolution == b.near_resolution &&
           a.near_depth_slices == b.near_depth_slices && a.near_coverage_m == b.near_coverage_m &&
           a.far_resolution == b.far_resolution && a.far_depth_slices == b.far_depth_slices &&
           a.far_coverage_m == b.far_coverage_m && a.filter_scale == b.filter_scale &&
           a.update_fraction == b.update_fraction;
}

std::string read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

float slice_to_depth(float slice, float depth_slices) {
    return 0.1f * std::pow(30000.0f, slice / depth_slices);
}

float depth_to_slice_n(float depth, float depth_slices) {
    const float clamped = std::fmax(0.1f, std::fmin(3000.0f, depth));
    return std::log(clamped / 0.1f) / std::log(30000.0f);
}

void test_dimension_independent_slice_mapping_and_shader_contract() {
    const uint32_t depths[] = {64, 96, 128, 192, 256};
    for (uint32_t depth_count : depths) {
        CHECK(slice_to_depth(0.0f, static_cast<float>(depth_count)) >= 0.1f &&
                  slice_to_depth(static_cast<float>(depth_count), static_cast<float>(depth_count)) <= 3000.0f,
              "slice endpoints remain within the 3000m froxel range");
        float prior_depth = 0.0f;
        float prior_transmittance = 1.0f;
        for (uint32_t i = 0; i < depth_count; ++i) {
            const float midpoint = slice_to_depth(static_cast<float>(i) + 0.5f,
                                                  static_cast<float>(depth_count));
            CHECK(midpoint > prior_depth, "froxel depths are strictly increasing");
            CHECK(std::fabs(depth_to_slice_n(midpoint, static_cast<float>(depth_count)) -
                                (static_cast<float>(i) + 0.5f) / static_cast<float>(depth_count)) <= 1e-5f,
                  "slice/depth mapping round-trips at every supported depth count");
            const float transmittance = std::exp(-0.01f * midpoint);
            CHECK(transmittance <= prior_transmittance,
                  "integrated transmittance is non-increasing for nonnegative extinction");
            prior_depth = midpoint;
            prior_transmittance = transmittance;
        }
    }
    const std::string common = read_file("../shaders_vk/vol_common.glsl");
    CHECK(common.find("const uint VOL_W") == std::string::npos &&
              common.find("const uint VOL_H") == std::string::npos &&
              common.find("const uint VOL_D") == std::string::npos &&
              common.find("slice_to_depth(float slice_index, float depth_slices)") != std::string::npos &&
              common.find("depth_to_slice_n(float depth, float depth_slices)") != std::string::npos,
          "volumetric shader helpers derive slice mapping from runtime depth dimensions");
}

void test_froxel_grid_resolves_every_discrete_combination() {
    const matter::FroxelGridDimensions expected_xy[5] = {
        {80, 45, 128}, {120, 68, 128}, {160, 90, 128},
        {240, 135, 128}, {320, 180, 128}
    };
    const uint32_t expected_depth[5] = {64, 96, 128, 192, 256};
    for (int xy = 0; xy != 5; ++xy) {
        for (int depth = 0; depth != 5; ++depth) {
            matter::VulkanVolumetricsSettings settings{};
            settings.froxel_xy_scale = static_cast<matter::FroxelXyScale>(xy);
            settings.froxel_depth_slices = static_cast<matter::FroxelDepthSlices>(depth);
            const auto got = matter::resolve_froxel_grid(settings);
            CHECK(equal(got, {expected_xy[xy].width, expected_xy[xy].height, expected_depth[depth]}),
                  "each XY/depth enum pair resolves to its pinned discrete froxel grid");
        }
    }
}

void test_froxel_grid_sanitizes_invalid_enums_to_current_cost() {
    matter::VulkanVolumetricsSettings settings{};
    settings.froxel_xy_scale = static_cast<matter::FroxelXyScale>(-9);
    settings.froxel_depth_slices = static_cast<matter::FroxelDepthSlices>(99);
    CHECK(equal(matter::resolve_froxel_grid(settings), {160, 90, 128}),
          "invalid low/high enum values sanitize to the Current cost grid");
    settings.froxel_xy_scale = static_cast<matter::FroxelXyScale>(99);
    settings.froxel_depth_slices = static_cast<matter::FroxelDepthSlices>(-9);
    CHECK(equal(matter::resolve_froxel_grid(settings), {160, 90, 128}),
          "invalid high/low enum values sanitize independently to Current cost");
}

void test_froxel_memory_accounts_for_four_rgba16f_images_and_optional_density() {
    const matter::FroxelGridDimensions base{160, 90, 128};
    CHECK(matter::estimate_froxel_bytes(base, false) == 58982400ull,
          "base grid owns exactly four RGBA16F froxel images");
    CHECK(matter::estimate_froxel_bytes(base, true) == 62668800ull,
          "enhanced cloud lighting adds exactly one R16F cloud-density image");

    for (int xy = 0; xy != 5; ++xy) {
        uint64_t prior = 0;
        for (int depth = 0; depth != 5; ++depth) {
            matter::VulkanVolumetricsSettings settings{};
            settings.froxel_xy_scale = static_cast<matter::FroxelXyScale>(xy);
            settings.froxel_depth_slices = static_cast<matter::FroxelDepthSlices>(depth);
            const uint64_t bytes = matter::estimate_froxel_bytes(
                matter::resolve_froxel_grid(settings), true);
            CHECK(bytes > 0, "every valid grid has positive persistent memory");
            if (depth != 0) CHECK(bytes >= prior,
                "memory grows monotonically as depth slices increase");
            prior = bytes;
        }
    }
    for (int depth = 0; depth != 5; ++depth) {
        uint64_t prior = 0;
        for (int xy = 0; xy != 5; ++xy) {
            matter::VulkanVolumetricsSettings settings{};
            settings.froxel_xy_scale = static_cast<matter::FroxelXyScale>(xy);
            settings.froxel_depth_slices = static_cast<matter::FroxelDepthSlices>(depth);
            const uint64_t bytes = matter::estimate_froxel_bytes(
                matter::resolve_froxel_grid(settings), true);
            if (xy != 0) CHECK(bytes >= prior,
                "memory grows monotonically as XY scale increases");
            prior = bytes;
        }
    }
}

void test_froxel_memory_saturates_instead_of_wrapping_for_unrepresentable_dimensions() {
    const matter::FroxelGridDimensions enormous{
        std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(),
        std::numeric_limits<uint32_t>::max()};
    CHECK(matter::estimate_froxel_bytes(enormous, true) == std::numeric_limits<uint64_t>::max(),
          "unrepresentable enhanced froxel memory saturates instead of wrapping");
}

struct ExpectedPreset {
    matter::VolumetricQualityPreset preset;
    matter::VulkanVolumetricsSettings volumetrics;
    matter::CloudShadowSettings shadows;
};

void test_presets_apply_and_identify_exact_fixed_table_values() {
    const ExpectedPreset expected[] = {
        {matter::VolumetricQualityPreset::CurrentCost,
         {false, 0.85f, 0.3f, 0.0f, matter::FroxelXyScale::X1_0, matter::FroxelDepthSlices::D128,
          0, 250.0f, 1, 0.0f, 0.0f},
         {false, 1, 1, 1800.0f, 1, 1, 4000.0f, 1.0f, 0.25f}},
        {matter::VolumetricQualityPreset::Improved,
         {false, 0.85f, 0.3f, 0.0f, matter::FroxelXyScale::X1_0, matter::FroxelDepthSlices::D128,
          8, 250.0f, 2, 0.55f, 0.25f},
         {true, 1, 1, 1800.0f, 1, 1, 4000.0f, 1.0f, 0.25f}},
        {matter::VolumetricQualityPreset::High,
         {false, 0.85f, 0.3f, 0.0f, matter::FroxelXyScale::X1_5, matter::FroxelDepthSlices::D192,
          12, 350.0f, 3, 0.70f, 0.35f},
         {true, 2, 1, 2200.0f, 2, 1, 4500.0f, 1.0f, 0.50f}},
        {matter::VolumetricQualityPreset::Ultra,
         {false, 0.85f, 0.3f, 0.0f, matter::FroxelXyScale::X2_0, matter::FroxelDepthSlices::D256,
          24, 500.0f, 4, 0.85f, 0.50f},
         {true, 2, 2, 2500.0f, 2, 2, 5000.0f, 1.0f, 1.00f}},
    };
    for (const auto& item : expected) {
        matter::VulkanVolumetricsSettings volumetrics{};
        matter::CloudShadowSettings shadows{};
        matter::apply_volumetric_quality_preset(item.preset, volumetrics, shadows);
        CHECK(equal(volumetrics, item.volumetrics) && equal(shadows, item.shadows),
              "named preset writes every fixed table field exactly");
        CHECK(matter::identify_volumetric_quality_preset(volumetrics, shadows) == item.preset,
              "named preset is recognized after application");
        ++volumetrics.local_sun_march_steps;
        CHECK(matter::identify_volumetric_quality_preset(volumetrics, shadows) ==
                  matter::VolumetricQualityPreset::Custom,
              "one changed preset field is recognized as Custom");
    }
}

void test_enhanced_lighting_derives_from_any_enhanced_feature() {
    matter::VulkanVolumetricsSettings volumetrics{};
    matter::CloudShadowSettings shadows{};
    matter::apply_volumetric_quality_preset(matter::VolumetricQualityPreset::CurrentCost,
                                            volumetrics, shadows);
    CHECK(!matter::enhanced_cloud_lighting(volumetrics, shadows),
          "only Current cost leaves enhanced cloud lighting disabled");
    volumetrics.local_sun_march_steps = 1;
    CHECK(matter::enhanced_cloud_lighting(volumetrics, shadows),
          "local march steps enable enhanced cloud lighting");
    volumetrics.local_sun_march_steps = 0;
    volumetrics.multiple_scattering_orders = 2;
    CHECK(matter::enhanced_cloud_lighting(volumetrics, shadows),
          "multiple scattering orders enable enhanced cloud lighting");
    volumetrics.multiple_scattering_orders = 1;
    volumetrics.powder_strength = 0.01f;
    CHECK(matter::enhanced_cloud_lighting(volumetrics, shadows),
          "powder strength enables enhanced cloud lighting");
    volumetrics.powder_strength = 0.0f;
    shadows.enabled = true;
    CHECK(matter::enhanced_cloud_lighting(volumetrics, shadows),
          "cloud shadows enable enhanced cloud lighting");
}

void test_froxel_capture_uses_the_reproducible_current_cost_baseline() {
    const std::string harness =
        read_file("../tools/atmosphere_cloud_shots.sh");
    const size_t froxel = harness.find("  froxel)\n");
    const size_t suite_end =
        froxel == std::string::npos ? std::string::npos : harness.find("    ;;", froxel);
    const std::string body = suite_end == std::string::npos
        ? std::string{} : harness.substr(froxel, suite_end - froxel);
    CHECK(body.find("set render.volumetrics.local_sun_march_steps 0") !=
              std::string::npos &&
              body.find("set render.volumetrics.multiple_scattering_orders 1") !=
              std::string::npos &&
              body.find("set render.volumetrics.multiple_scattering_strength 0") !=
              std::string::npos &&
              body.find("set render.volumetrics.powder_strength 0") !=
              std::string::npos &&
              body.find("set render.cloud_shadows.enabled false") !=
              std::string::npos &&
              body.find("set render.lighting.exposure_ev 0") !=
              std::string::npos,
          "froxel capture pins Task 7's Current-cost and daylight baseline");
}

} // namespace

int main() {
    test_dimension_independent_slice_mapping_and_shader_contract();
    test_froxel_grid_resolves_every_discrete_combination();
    test_froxel_grid_sanitizes_invalid_enums_to_current_cost();
    test_froxel_memory_accounts_for_four_rgba16f_images_and_optional_density();
    test_froxel_memory_saturates_instead_of_wrapping_for_unrepresentable_dimensions();
    test_presets_apply_and_identify_exact_fixed_table_values();
    test_enhanced_lighting_derives_from_any_enhanced_feature();
    test_froxel_capture_uses_the_reproducible_current_cost_baseline();
    return check_summary();
}
