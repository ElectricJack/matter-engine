#include "check.h"

#include "hydrology/fluid_gameplay_field.h"
#include "hydrology/river_presentation_field.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kWidth = 5u;
constexpr std::uint32_t kDepth = 5u;

std::size_t index_of(std::uint32_t x, std::uint32_t z) {
    return static_cast<std::size_t>(z) * kWidth + x;
}

void test_derives_repeatable_bounded_presentation_field() {
    const hydrology::GameplayFieldLayout layout{{0.0f, 0.0f, 0.0f}, 1.0f,
                                                kWidth, kDepth};
    std::vector<hydrology::GameplaySample> gameplay(kWidth * kDepth);
    std::vector<float> terrain(kWidth * kDepth, 0.0f);
    std::vector<float> wake_distance(kWidth * kDepth, 100.0f);
    std::vector<hydrology::PresentationMarkers> markers(kWidth * kDepth);
    std::vector<hydrology::PresentationLocalOverride> overrides(kWidth *
                                                                  kDepth);
    hydrology::GameplayFieldStatistics statistics{};
    statistics.velocity_variance_mps2.assign(kWidth * kDepth, 0.25f);

    for (std::uint32_t z = 1u; z != 4u; ++z) {
        for (std::uint32_t x = 1u; x != 4u; ++x) {
            const std::size_t index = index_of(x, z);
            gameplay[index] = {4.0f + static_cast<float>(x), 2.0f,
                               static_cast<float>(x), 0.0f, 0.0f, true};
        }
    }

    const std::size_t rapid = index_of(2u, 2u);
    const std::size_t fall = index_of(3u, 2u);
    const std::size_t pool = index_of(1u, 2u);
    const std::size_t wake = index_of(2u, 3u);
    const std::size_t calm = index_of(1u, 1u);
    gameplay[rapid].velocity_x_mps = 3.0f;
    gameplay[fall].velocity_y_mps = -4.0f;
    gameplay[pool].depth_m = 0.15f;
    markers[fall].waterfall = true;
    markers[pool].pool = true;
    wake_distance[wake] = 0.0f;
    overrides[wake].foam_multiplier = 1.5f;

    hydrology::PresentationDerivationInput input{};
    input.layout = layout;
    input.gameplay = &gameplay;
    input.gameplay_statistics = &statistics;
    input.terrain_heights_m = &terrain;
    input.wake_distances_m = &wake_distance;
    input.markers = &markers;
    input.local_overrides = &overrides;

    hydrology::PresentationDerivationSettings settings{};
    settings.rapid_speed_mps = 2.5f;
    settings.current_speed_mps = 0.5f;
    settings.shallow_depth_m = 0.25f;
    std::vector<hydrology::PresentationSample> first;
    std::vector<hydrology::PresentationSample> second;
    std::string error;
    CHECK(hydrology::build_river_presentation_field(input, settings, first,
                                                     error),
          error.c_str());
    CHECK(hydrology::build_river_presentation_field(input, settings, second,
                                                     error),
          error.c_str());
    CHECK(first == second, "presentation field is exactly repeatable");
    CHECK(first[rapid].feature == hydrology::RiverFeature::Rapid,
          "speed classifies a rapid");
    CHECK(first[fall].feature == hydrology::RiverFeature::Waterfall,
          "waterfall marker takes precedence");
    CHECK(first[pool].feature == hydrology::RiverFeature::Pool,
          "pool marker takes precedence over speed");
    CHECK(first[wake].foam_potential > first[calm].foam_potential,
          "boulder wake raises foam potential");

    for (const hydrology::PresentationSample& sample : first) {
        if (!sample.wet_valid) continue;
        const float normal_y_squared =
            1.0f - sample.normal_x * sample.normal_x -
            sample.normal_z * sample.normal_z;
        CHECK(std::isfinite(sample.normal_x) && std::isfinite(sample.normal_z) &&
                  normal_y_squared >= 0.0f,
              "wet presentation normal is finite and positive-hemisphere");
        CHECK(sample.turbulence >= 0.0f && sample.turbulence <= 1.0f &&
                  sample.aeration >= 0.0f && sample.aeration <= 1.0f &&
                  sample.foam_potential >= 0.0f &&
                  sample.foam_potential <= 1.0f,
              "wet presentation channels are bounded");
    }

    hydrology::PresentationSample sampled{};
    CHECK(!hydrology::sample_river_presentation_field(layout, first, 0.5f,
                                                       0.5f, sampled),
          "presentation sampling rejects dry cells");
}

void test_gameplay_retains_known_velocity_variance() {
    const hydrology::GameplayFieldLayout layout{{0.0f, 0.0f, 0.0f}, 1.0f,
                                                1u, 1u};
    const std::vector<hydrology::FluidParticle> particles = {
        {{0.25f, 2.0f, 0.25f}, {1.0f, 0.0f, 0.0f}, 1u},
        {{0.75f, 2.0f, 0.75f}, {3.0f, 0.0f, 0.0f}, 2u},
    };
    std::vector<hydrology::GameplaySample> field;
    hydrology::GameplayFieldStatistics statistics{};
    std::string error;
    CHECK(hydrology::build_fluid_gameplay_field(
              particles, 0.5f, layout,
              [](float, float, float& height) { height = 0.0f; return true; },
              field, error, &statistics), error.c_str());
    CHECK(statistics.velocity_variance_mps2.size() == 1u &&
              std::fabs(statistics.velocity_variance_mps2[0] - 1.0f) < 1e-6f,
          "gameplay extraction retains the population velocity variance");
}

void test_gameplay_retains_stable_large_velocity_variance() {
    const hydrology::GameplayFieldLayout layout{{0.0f, 0.0f, 0.0f}, 1.0f,
                                                1u, 1u};
    const std::vector<hydrology::FluidParticle> particles = {
        {{0.25f, 2.0f, 0.25f}, {10000.0f, 0.0f, 0.0f}, 1u},
        {{0.75f, 2.0f, 0.75f}, {10002.0f, 0.0f, 0.0f}, 2u},
    };
    std::vector<hydrology::GameplaySample> field;
    hydrology::GameplayFieldStatistics statistics{};
    std::string error;
    CHECK(hydrology::build_fluid_gameplay_field(
              particles, 0.5f, layout,
              [](float, float, float& height) { height = 0.0f; return true; },
              field, error, &statistics), error.c_str());
    CHECK(std::fabs(statistics.velocity_variance_mps2[0] - 1.0f) < 1e-6f,
          "retained variance is stable for large nearby velocities");
}

void test_gameplay_uses_checked_stable_means_and_depths() {
    const hydrology::GameplayFieldLayout layout{{0.0f, 0.0f, 0.0f}, 1.0f,
                                                1u, 1u};
    const float maximum = std::numeric_limits<float>::max();
    const std::vector<hydrology::FluidParticle> particles = {
        {{0.25f, 2.0f, 0.25f}, {maximum, 0.0f, 0.0f}, 1u},
        {{0.75f, 2.0f, 0.75f}, {maximum, 0.0f, 0.0f}, 2u},
    };
    std::vector<hydrology::GameplaySample> field;
    std::string error;
    CHECK(hydrology::build_fluid_gameplay_field(
              particles, 0.5f, layout,
              [](float, float, float& height) { height = 0.0f; return true; },
              field, error) && field.size() == 1u && field[0].wet_valid &&
              field[0].velocity_x_mps == maximum,
          "finite equal near-FLT_MAX velocities retain their representable mean");

    const std::vector<hydrology::FluidParticle> tall = {
        {{0.25f, 2.0e38f, 0.25f}, {0.0f, 0.0f, 0.0f}, 1u},
    };
    CHECK(!hydrology::build_fluid_gameplay_field(
              tall, 0.5f, layout,
              [](float, float, float& height) { height = -2.0e38f; return true; },
              field, error) && field.empty() && !error.empty(),
          "an unrepresentable required gameplay depth fails closed");
}

void test_presentation_rejects_overflowing_intermediates() {
    const hydrology::GameplayFieldLayout layout{{0.0f, 0.0f, 0.0f}, 1.0f,
                                                2u, 1u};
    std::vector<hydrology::GameplaySample> gameplay = {
        {-3.0e38f, 1.0f, 1.0f, 0.0f, 0.0f, true},
        {3.0e38f, 1.0f, 1.0f, 0.0f, 0.0f, true},
    };
    hydrology::GameplayFieldStatistics statistics{{0.0f, 0.0f}};
    std::vector<float> terrain(2u, 0.0f), wake(2u, 1.0f);
    std::vector<hydrology::PresentationMarkers> markers(2u);
    std::vector<hydrology::PresentationLocalOverride> overrides(2u);
    hydrology::PresentationDerivationInput input{
        layout, &gameplay, &statistics, &terrain, &wake, &markers, &overrides};
    hydrology::PresentationDerivationSettings settings{};
    std::vector<hydrology::PresentationSample> output;
    std::string error;
    CHECK(hydrology::build_river_presentation_field(input, settings, output, error) &&
              output.size() == 2u && output[0].wet_valid &&
              std::isfinite(output[0].normal_x),
          "finite huge gradients retain a bounded representable normal");

    gameplay = {{2.0f, 1.0f, 1.0f, 0.0f, 0.0f, true},
                {2.0f, 1.0f, 1.0f, 0.0f, 0.0f, true}};
    statistics.velocity_variance_mps2 = {1.0f, 1.0f};
    settings.velocity_variance_weight = std::numeric_limits<float>::max();
    settings.divergence_weight = std::numeric_limits<float>::max();
    CHECK(hydrology::build_river_presentation_field(input, settings, output, error) &&
              output[0].turbulence >= 0.0f && output[0].turbulence <= 1.0f,
          "large finite weights use stable arithmetic without overflow");
}

void test_presentation_keeps_huge_finite_metrics_bounded() {
    const hydrology::GameplayFieldLayout layout{{0.0f, 0.0f, 0.0f}, 1.0f,
                                                1u, 1u};
    const float maximum = std::numeric_limits<float>::max();
    std::vector<hydrology::GameplaySample> gameplay = {
        {2.0f, maximum, maximum, 0.0f, maximum, true}};
    hydrology::GameplayFieldStatistics statistics{{maximum}};
    std::vector<float> terrain(1u, 0.0f), wake(1u, maximum);
    std::vector<hydrology::PresentationMarkers> markers(1u);
    std::vector<hydrology::PresentationLocalOverride> overrides(1u);
    hydrology::PresentationDerivationInput input{
        layout, &gameplay, &statistics, &terrain, &wake, &markers, &overrides};
    hydrology::PresentationDerivationSettings settings{};
    settings.velocity_variance_scale_mps2 = std::numeric_limits<float>::denorm_min();
    settings.shallow_depth_m = std::numeric_limits<float>::denorm_min();
    settings.wake_distance_scale_m = std::numeric_limits<float>::denorm_min();
    settings.rapid_speed_mps = 1.0f;
    std::vector<hydrology::PresentationSample> output;
    std::string error;
    CHECK(hydrology::build_river_presentation_field(input, settings, output, error) &&
              output.size() == 1u && output[0].wet_valid &&
              output[0].turbulence > 0.0f &&
              output[0].feature == hydrology::RiverFeature::Rapid,
          "huge finite variance and speed use double ratios and hypot rather than zeroing");
}

void test_pool_weight_is_calm_evidence() {
    const hydrology::GameplayFieldLayout layout{{0.0f, 0.0f, 0.0f}, 1.0f,
                                                1u, 1u};
    std::vector<hydrology::GameplaySample> gameplay = {
        {2.0f, 0.1f, 3.0f, 1.0f, 0.0f, true}};
    hydrology::GameplayFieldStatistics statistics{{1.0f}};
    std::vector<float> terrain(1u, 0.0f), wake(1u, 0.0f);
    std::vector<hydrology::PresentationMarkers> markers(1u, {false, false, false, true});
    std::vector<hydrology::PresentationLocalOverride> overrides(1u);
    hydrology::PresentationDerivationInput input{
        layout, &gameplay, &statistics, &terrain, &wake, &markers, &overrides};
    hydrology::PresentationDerivationSettings settings{};
    std::vector<hydrology::PresentationSample> weak, strong;
    std::string error;
    settings.pool_weight = 0.0f;
    CHECK(hydrology::build_river_presentation_field(input, settings, weak, error), error.c_str());
    settings.pool_weight = 10.0f;
    CHECK(hydrology::build_river_presentation_field(input, settings, strong, error), error.c_str());
    CHECK(strong[0].turbulence < weak[0].turbulence &&
              strong[0].aeration < weak[0].aeration &&
              strong[0].foam_potential < weak[0].foam_potential &&
              strong[0].feature == hydrology::RiverFeature::Pool,
          "pool weight adds calm evidence without changing pool precedence");
}

void test_presentation_sampling_is_strict_and_bilinear() {
    const hydrology::GameplayFieldLayout layout{{0.0f, 0.0f, 0.0f}, 1.0f,
                                                2u, 2u};
    std::vector<hydrology::PresentationSample> field = {
        {0.0f, 0.0f, 0.0f, 0.1f, 0.2f, hydrology::RiverFeature::Calm, true},
        {0.2f, 0.0f, 0.2f, 0.3f, 0.4f, hydrology::RiverFeature::Current, true},
        {0.0f, 0.2f, 0.4f, 0.5f, 0.6f, hydrology::RiverFeature::Rapid, true},
        {0.2f, 0.2f, 0.6f, 0.7f, 0.8f, hydrology::RiverFeature::Spillway, true},
    };
    hydrology::PresentationSample sample{};
    CHECK(hydrology::sample_river_presentation_field(layout, field, 1.0f, 1.0f,
                                                       sample),
          "wet presentation contributors bilinearly filter");
    CHECK(std::fabs(sample.turbulence - 0.3f) < 1e-6f &&
              std::fabs(sample.aeration - 0.4f) < 1e-6f &&
              std::fabs(sample.foam_potential - 0.5f) < 1e-6f &&
              sample.feature == hydrology::RiverFeature::Spillway,
          "continuous presentation values blend while feature chooses nearest cell");
    CHECK(!hydrology::sample_river_presentation_field(layout, field, -0.01f,
                                                       0.5f, sample) &&
              sample == hydrology::PresentationSample{},
          "presentation sampling rejects out of bounds coordinates and clears output");

    field[3].foam_potential = std::numeric_limits<float>::quiet_NaN();
    CHECK(!hydrology::sample_river_presentation_field(layout, field, 1.0f, 1.0f,
                                                       sample) &&
              sample == hydrology::PresentationSample{},
          "non-finite nonzero-weight presentation contributors are rejected");
    field[3].foam_potential = 0.8f;
    field[2].normal_x = 2.0f;
    CHECK(!hydrology::sample_river_presentation_field(layout, field, 1.0f, 1.0f,
                                                       sample) &&
              sample == hydrology::PresentationSample{},
          "out-of-contract nonzero-weight presentation contributors are rejected");
    field[2].normal_x = 0.0f;
    field[1].normal_x = 2.0f;
    CHECK(hydrology::sample_river_presentation_field(layout, field, 0.5f, 0.5f,
                                                      sample),
          "zero-weight invalid presentation contributors are skipped");
}

void test_feature_markers_have_explicit_precedence() {
    hydrology::PresentationDerivationSettings settings{};
    settings.current_speed_mps = 1.0f;
    settings.rapid_speed_mps = 2.0f;
    CHECK(hydrology::classify_river_feature({true, true, true, true}, 3.0f,
                                            settings) ==
              hydrology::RiverFeature::Waterfall,
          "waterfall precedes impact spillway pool and rapid");
    CHECK(hydrology::classify_river_feature({false, true, true, true}, 3.0f,
                                            settings) == hydrology::RiverFeature::Impact,
          "impact precedes spillway pool and rapid");
    CHECK(hydrology::classify_river_feature({false, false, true, true}, 3.0f,
                                            settings) == hydrology::RiverFeature::Spillway,
          "spillway precedes pool and rapid");
    CHECK(hydrology::classify_river_feature({false, false, false, true}, 3.0f,
                                            settings) == hydrology::RiverFeature::Pool,
          "pool precedes rapid");
    CHECK(hydrology::classify_river_feature({}, 3.0f, settings) ==
              hydrology::RiverFeature::Rapid,
          "rapid precedes current and calm");
    CHECK(hydrology::classify_river_feature({}, 1.0f, settings) ==
              hydrology::RiverFeature::Current,
          "current precedes calm");
    CHECK(hydrology::classify_river_feature({}, 0.0f, settings) ==
              hydrology::RiverFeature::Calm,
          "calm is the final classification rung");
}

}  // namespace

int main() {
    test_derives_repeatable_bounded_presentation_field();
    test_gameplay_retains_known_velocity_variance();
    test_gameplay_retains_stable_large_velocity_variance();
    test_gameplay_uses_checked_stable_means_and_depths();
    test_presentation_rejects_overflowing_intermediates();
    test_presentation_keeps_huge_finite_metrics_bounded();
    test_pool_weight_is_calm_evidence();
    test_presentation_sampling_is_strict_and_bilinear();
    test_feature_markers_have_explicit_precedence();
    return check_summary();
}
