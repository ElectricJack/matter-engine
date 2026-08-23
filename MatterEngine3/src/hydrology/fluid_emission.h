#pragma once

#include "hydrology/physx_fluid_types.h"

#include <cstdint>
#include <vector>

namespace hydrology {

struct FluidParticleActivation {
    std::uint64_t id = 0;
    std::uint32_t emitter_id = 0;
    matter::Float3 position_m{};
    matter::Float3 velocity_mps{};
};

struct FluidEmissionState {
    std::vector<std::uint32_t> emitter_ids;
    std::vector<double> fractional_carry;
    std::uint64_t next_particle_id = 0;
    std::uint32_t next_step = 0;
    bool initialized = false;
};

// Matches the particle volume used by SnippetPBF's mass derivation.
float physx_particle_volume_m3(float particle_spacing_m) noexcept;

// Computes only deterministic activation data. It contains no force,
// integration, pressure, neighbor-search, or fluid-constraint mathematics.
bool schedule_fluid_emission_step(
    const std::vector<FluidEmitter>& emitters,
    const FluidPbdSettings& settings,
    std::uint32_t step,
    std::uint32_t active_particle_count,
    FluidEmissionState& state,
    std::vector<FluidParticleActivation>& activations,
    FluidBakeError& error);

}  // namespace hydrology
