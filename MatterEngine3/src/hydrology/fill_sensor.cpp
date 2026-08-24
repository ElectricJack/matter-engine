#include "hydrology/fill_sensor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace hydrology {
namespace {

bool finite(matter::Float3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool valid_bounds(const matter::Aabb& bounds) {
    return finite(bounds.minimum) && finite(bounds.maximum) &&
           bounds.minimum.x < bounds.maximum.x &&
           bounds.minimum.y < bounds.maximum.y &&
           bounds.minimum.z < bounds.maximum.z;
}

bool fail(FluidBakeCode code, const char* message, FillSensorResult& result,
          FluidBakeError& error) {
    result = {};
    error = {code, message};
    return false;
}

}  // namespace

bool update_fill_sensor(const FluidFillSensor& sensor,
                        const std::vector<matter::Float3>& particle_positions,
                        std::uint32_t step,
                        FillSensorState& state,
                        FillSensorResult& result,
                        FluidBakeError& error) {
    result = {};
    error = {};
    const std::uint64_t horizontal_cells =
        static_cast<std::uint64_t>(sensor.resolution.x) *
        static_cast<std::uint64_t>(sensor.resolution.z);
    if (!valid_bounds(sensor.bounds_m) ||
        !valid_fluid_fill_sensor_frame(sensor) ||
        sensor.resolution.x == 0u ||
        sensor.resolution.y == 0u || sensor.resolution.z == 0u ||
        horizontal_cells > std::numeric_limits<std::size_t>::max() ||
        horizontal_cells > std::numeric_limits<std::uint32_t>::max() ||
        !std::isfinite(sensor.required_wet_fraction) ||
        !(sensor.required_wet_fraction > 0.0f) ||
        sensor.required_wet_fraction > 1.0f || sensor.stable_steps == 0u ||
        sensor.minimum_particles_per_cell == 0u ||
        (state.initialized && step <= state.last_step)) {
        return fail(FluidBakeCode::InvalidInput,
                    "fill sensor configuration or step is invalid", result,
                    error);
    }

    std::vector<std::uint32_t> contributions(
        static_cast<std::size_t>(horizontal_cells), 0u);
    for (matter::Float3 position : particle_positions) {
        if (!finite(position)) {
            return fail(FluidBakeCode::NonFinite,
                        "fill sensor received a non-finite particle", result,
                        error);
        }
        const matter::Float3 local =
            fluid_fill_sensor_local_position(sensor, position);
        if (local.x < 0.0f || local.x >= sensor.frame_extent_m.x ||
            local.y < 0.0f || local.y >= sensor.frame_extent_m.y ||
            local.z < 0.0f || local.z >= sensor.frame_extent_m.z) {
            continue;
        }
        const auto x = std::min(
            static_cast<std::uint32_t>(
                local.x / sensor.frame_extent_m.x *
                sensor.resolution.x),
            sensor.resolution.x - 1u);
        const auto z = std::min(
            static_cast<std::uint32_t>(
                local.z / sensor.frame_extent_m.z *
                sensor.resolution.z),
            sensor.resolution.z - 1u);
        std::uint32_t& count = contributions[
            static_cast<std::size_t>(z) * sensor.resolution.x + x];
        if (count < sensor.minimum_particles_per_cell) ++count;
    }

    std::uint64_t wet_cells = 0u;
    for (std::uint32_t count : contributions) {
        wet_cells += count >= sensor.minimum_particles_per_cell ? 1u : 0u;
    }
    return update_fill_sensor_counts(
        sensor, static_cast<std::uint32_t>(wet_cells),
        static_cast<std::uint32_t>(horizontal_cells), step, state, result,
        error);
}

bool update_fill_sensor_counts(const FluidFillSensor& sensor,
                               std::uint32_t wet_horizontal_cells,
                               std::uint32_t total_horizontal_cells,
                               std::uint32_t step,
                               FillSensorState& state,
                               FillSensorResult& result,
                               FluidBakeError& error) {
    result = {};
    error = {};
    const std::uint64_t expected_horizontal_cells =
        static_cast<std::uint64_t>(sensor.resolution.x) *
        static_cast<std::uint64_t>(sensor.resolution.z);
    if (!valid_bounds(sensor.bounds_m) ||
        !valid_fluid_fill_sensor_frame(sensor) ||
        sensor.resolution.x == 0u ||
        sensor.resolution.y == 0u || sensor.resolution.z == 0u ||
        expected_horizontal_cells != total_horizontal_cells ||
        wet_horizontal_cells > total_horizontal_cells ||
        !std::isfinite(sensor.required_wet_fraction) ||
        !(sensor.required_wet_fraction > 0.0f) ||
        sensor.required_wet_fraction > 1.0f || sensor.stable_steps == 0u ||
        sensor.minimum_particles_per_cell == 0u ||
        (state.initialized && step <= state.last_step)) {
        return fail(FluidBakeCode::InvalidInput,
                    "fill sensor counts or step are invalid", result,
                    error);
    }
    const float wet_fraction = static_cast<float>(
        static_cast<double>(wet_horizontal_cells) /
        static_cast<double>(total_horizontal_cells));

    FillSensorState candidate = state;
    if (candidate.initialized && step != candidate.last_step + 1u) {
        candidate.consecutive_wet_steps = 0u;
    }
    candidate.initialized = true;
    candidate.last_step = step;
    candidate.final_wet_fraction = wet_fraction;
    candidate.maximum_wet_fraction =
        std::max(candidate.maximum_wet_fraction, wet_fraction);
    if (wet_fraction >= sensor.required_wet_fraction) {
        if (!candidate.has_satisfied_step) {
            candidate.has_satisfied_step = true;
            candidate.first_satisfied_step = step;
        }
        ++candidate.consecutive_wet_steps;
        if (!candidate.complete &&
            candidate.consecutive_wet_steps >= sensor.stable_steps) {
            candidate.complete = true;
            candidate.completion_step = step;
            candidate.completion_wet_fraction = wet_fraction;
        }
    } else {
        candidate.consecutive_wet_steps = 0u;
    }

    state = candidate;
    result.wet_fraction = wet_fraction;
    result.stable_steps = candidate.consecutive_wet_steps;
    result.completion_step = candidate.completion_step;
    result.complete = candidate.complete;
    result.maximum_wet_fraction = candidate.maximum_wet_fraction;
    result.final_wet_fraction = candidate.final_wet_fraction;
    result.stable_window_wet_fraction =
        candidate.completion_wet_fraction;
    result.first_satisfied_step = candidate.first_satisfied_step;
    return true;
}

}  // namespace hydrology
