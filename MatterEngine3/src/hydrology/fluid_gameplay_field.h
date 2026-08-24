#pragma once

#include "hydrology/physx_fluid_types.h"
#include "hydrology/water_visual_products.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace hydrology {

// A section-local X/Z lattice.  Each sample is invalid unless it has water;
// callers must never treat an invalid sample as a stationary current.
using TerrainHeightSampler = std::function<bool(float x_m, float z_m,
                                                float& height_m)>;

// Per-cell values derived in the same deterministic pass as GameplaySample.
// Presentation consumes these retained values and never rescans particles.
struct GameplayFieldStatistics {
    std::vector<float> velocity_variance_mps2;
};

bool build_fluid_gameplay_field(
    const std::vector<FluidParticle>& particles, float particle_radius_m,
    const GameplayFieldLayout& layout, const TerrainHeightSampler& terrain,
    std::vector<GameplaySample>& samples, std::string& error,
    GameplayFieldStatistics* statistics = nullptr);

bool sample_fluid_gameplay_field(const GameplayFieldLayout& layout,
                                 const std::vector<GameplaySample>& samples,
                                 float x_m, float z_m,
                                 GameplaySample& sample) noexcept;

}  // namespace hydrology
