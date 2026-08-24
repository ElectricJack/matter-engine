#include "check.h"

#include "hydrology/fluid_gameplay_field.h"
#include "hydrology/river_presentation_field.h"

#include <cmath>
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
    std::vector<hydrology::FluidParticle> particles;

    for (std::uint32_t z = 1u; z != 4u; ++z) {
        for (std::uint32_t x = 1u; x != 4u; ++x) {
            const std::size_t index = index_of(x, z);
            gameplay[index] = {4.0f + static_cast<float>(x), 2.0f,
                               static_cast<float>(x), 0.0f, 0.0f, true};
            particles.push_back({{static_cast<float>(x) + 0.25f, 3.5f,
                                  static_cast<float>(z) + 0.25f},
                                 {static_cast<float>(x), 0.0f, 0.0f},
                                 static_cast<std::uint64_t>(particles.size())});
            particles.push_back({{static_cast<float>(x) + 0.75f, 3.5f,
                                  static_cast<float>(z) + 0.75f},
                                 {static_cast<float>(x) + 1.0f, 0.0f, 0.0f},
                                 static_cast<std::uint64_t>(particles.size())});
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
    input.particles = &particles;
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

}  // namespace

int main() {
    test_derives_repeatable_bounded_presentation_field();
    return check_summary();
}
