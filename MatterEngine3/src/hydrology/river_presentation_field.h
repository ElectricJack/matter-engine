#pragma once

#include "hydrology/fluid_gameplay_field.h"

#include <string>
#include <vector>

namespace hydrology {

struct PresentationDerivationInput {
    GameplayFieldLayout layout{};
    const std::vector<GameplaySample>* gameplay = nullptr;
    const std::vector<FluidParticle>* particles = nullptr;
    const std::vector<float>* terrain_heights_m = nullptr;
    const std::vector<float>* wake_distances_m = nullptr;
    const std::vector<PresentationMarkers>* markers = nullptr;
    const std::vector<PresentationLocalOverride>* local_overrides = nullptr;
};

bool build_river_presentation_field(
    const PresentationDerivationInput& input,
    const PresentationDerivationSettings& settings,
    std::vector<PresentationSample>& samples, std::string& error);

bool sample_river_presentation_field(
    const GameplayFieldLayout& layout,
    const std::vector<PresentationSample>& samples, float x_m, float z_m,
    PresentationSample& sample) noexcept;

}  // namespace hydrology
