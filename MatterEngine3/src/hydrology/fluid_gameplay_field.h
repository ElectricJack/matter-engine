#pragma once

#include "hydrology/physx_fluid_types.h"
#include "hydrology/water_visual_products.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace hydrology {

// A section-local X/Z lattice.  Each sample is invalid unless it has water;
// callers must never treat an invalid sample as a stationary current.
struct GameplayFieldLayout {
    matter::Float3 origin_m{};
    float cell_size_m = 0.0f;
    std::uint32_t width = 0;
    std::uint32_t depth = 0;
};

using TerrainHeightSampler = std::function<bool(float x_m, float z_m,
                                                float& height_m)>;

bool build_fluid_gameplay_field(
    const std::vector<FluidParticle>& particles, float particle_radius_m,
    const GameplayFieldLayout& layout, const TerrainHeightSampler& terrain,
    std::vector<GameplaySample>& samples, std::string& error);

bool sample_fluid_gameplay_field(const GameplayFieldLayout& layout,
                                 const std::vector<GameplaySample>& samples,
                                 float x_m, float z_m,
                                 GameplaySample& sample) noexcept;

}  // namespace hydrology
