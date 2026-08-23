#pragma once

#include "hydrology/river_geometry.h"
#include "matter/bounds.h"
#include "matter/river_network.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hydrology {

enum class FluidBakeCode : std::uint8_t {
    Ready = 0,
    InvalidInput,
    BackendUnavailable,
    BackendFailure,
    Cancelled,
    CapacityExceeded,
    Escaped,
    NonFinite,
    SensorNotReached,
    DeviceLost,
};

struct FluidBakeError {
    FluidBakeCode code = FluidBakeCode::Ready;
    std::string message;
};

struct FluidParticle {
    matter::Float3 position_m{};
    matter::Float3 velocity_mps{};
    std::uint64_t id = 0;
};

struct FluidEmitter {
    std::uint32_t id = 0;
    matter::Float3 position_m{};
    matter::Float3 direction{};
    matter::Float3 initial_velocity_mps{};
    float flow_m3s = 0.0f;
    float radius_m = 0.0f;
    std::uint32_t start_step = 0;
    std::uint32_t stop_step = 0;
};

struct FluidCollisionMesh {
    std::vector<matter::Float3> vertices;
    std::vector<std::uint32_t> indices;
};

struct FluidGridResolution {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;
};

struct FluidFillSensor {
    matter::Aabb bounds_m{};
    FluidGridResolution resolution{};
    float required_wet_fraction = 0.0f;
    std::uint32_t stable_steps = 0;
};

struct FluidPbdSettings {
    float particle_spacing_m = 0.0f;
    float rest_density_kg_m3 = 0.0f;
    float fixed_step_seconds = 0.0f;
    std::uint32_t solver_iterations = 0;
    std::uint32_t max_neighbors = 0;
    std::uint32_t batch_steps = 0;
    std::uint32_t max_steps = 0;
    std::uint32_t max_particles = 0;
};

struct FillSensorResult {
    float wet_fraction = 0.0f;
    std::uint32_t stable_steps = 0;
    std::uint32_t completion_step = 0;
    bool complete = false;
};

struct FluidBakeStats {
    std::uint32_t simulated_steps = 0;
    std::uint32_t active_particles = 0;
    std::uint32_t peak_particles = 0;
    std::uint32_t escaped_particles = 0;
    std::uint32_t non_finite_particles = 0;
    double wall_seconds = 0.0;
};

struct FluidBakeOutput {
    std::vector<FluidParticle> particles;
    FillSensorResult sensor{};
    FluidBakeStats stats{};
};

struct FluidBakeProgress {
    std::uint32_t completed_steps = 0;
    std::uint32_t total_steps = 0;
    std::uint32_t active_particles = 0;
    float sensor_wet_fraction = 0.0f;
};

struct FluidBakeCallbacks {
    std::function<bool()> cancelled;
    std::function<void(const FluidBakeProgress&)> progress;
};

struct FluidBakeInput {
    matter::RiverNetworkDefinition network;
    RiverGeometry geometry;
    FluidCollisionMesh collision;
    std::vector<FluidEmitter> emitters;
    FluidFillSensor sensor{};
    FluidPbdSettings settings{};
    matter::Aabb dry_collar_bounds_m{};
};

}  // namespace hydrology
