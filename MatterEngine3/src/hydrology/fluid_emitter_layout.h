#pragma once

#include "hydrology/physx_fluid_types.h"

#include <cstdint>
#include <vector>

namespace hydrology {

bool build_emitter_offsets(
    FluidEmitterShape shape,
    float particle_spacing_m,
    float radius_m,
    matter::Float2 half_extent_m,
    std::uint32_t maximum_offsets,
    std::vector<matter::Float2>& offsets,
    FluidBakeError& error,
    float channel_depth_m = 0.0f,
    float channel_asymmetry = 0.0f);

}  // namespace hydrology
