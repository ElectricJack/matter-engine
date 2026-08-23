#pragma once

#include "hydrology/physx_fluid_types.h"

#include <cstdint>
#include <vector>

namespace hydrology {

struct FillSensorState {
    float maximum_wet_fraction = 0.0f;
    float final_wet_fraction = 0.0f;
    float completion_wet_fraction = 0.0f;
    std::uint32_t first_satisfied_step = 0;
    std::uint32_t consecutive_wet_steps = 0;
    std::uint32_t completion_step = 0;
    std::uint32_t last_step = 0;
    bool has_satisfied_step = false;
    bool complete = false;
    bool initialized = false;
};

// CPU reference for the device occupancy reduction. A horizontal X/Z column
// is wet once it contains minimum_particles_per_cell particles anywhere in
// the sensor's vertical span.
bool update_fill_sensor(const FluidFillSensor& sensor,
                        const std::vector<matter::Float3>& particle_positions,
                        std::uint32_t step,
                        FillSensorState& state,
                        FillSensorResult& result,
                        FluidBakeError& error);

// Applies the temporal completion rule to a bounded occupancy reduction.
// The GPU path returns only these counts; particle positions remain on device
// until the accepted final snapshot.
bool update_fill_sensor_counts(const FluidFillSensor& sensor,
                               std::uint32_t wet_horizontal_cells,
                               std::uint32_t total_horizontal_cells,
                               std::uint32_t step,
                               FillSensorState& state,
                               FillSensorResult& result,
                               FluidBakeError& error);

}  // namespace hydrology
