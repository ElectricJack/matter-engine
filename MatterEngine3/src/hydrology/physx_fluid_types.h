#pragma once

#include "hydrology/river_geometry.h"
#include "matter/bounds.h"
#include "matter/river_network.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
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
    InsufficientAnimationHistory,
    ProductFailure,
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

struct FluidQuarantinedParticle {
    matter::Float3 position_m{};
    std::uint64_t id = 0;
};

inline std::uint32_t fluid_escape_budget(
    std::uint32_t emitted_particle_count,
    const matter::HydrologyEscapePolicy& policy) noexcept {
    const auto proportional = static_cast<std::uint64_t>(std::ceil(
        static_cast<double>(emitted_particle_count) *
        static_cast<double>(policy.ratio)));
    const auto budget = std::max<std::uint64_t>(policy.absolute_count,
                                                proportional);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        budget, std::numeric_limits<std::uint32_t>::max()));
}

inline std::uint32_t fluid_escape_budget(
    std::uint32_t emitted_particle_count) noexcept {
    return fluid_escape_budget(emitted_particle_count,
                               matter::HydrologyEscapePolicy{});
}

enum class FluidEmitterShape : std::uint8_t { Disc, Ribbon };

struct FluidEmitter {
    std::uint32_t id = 0;
    FluidEmitterShape shape = FluidEmitterShape::Disc;
    matter::Float3 position_m{};
    matter::Float3 direction{};
    matter::Float3 lateral_axis{};
    matter::Float3 up_axis{};
    matter::Float3 initial_velocity_mps{};
    float flow_m3s = 0.0f;
    float radius_m = 0.0f;
    matter::Float2 half_extent_m{};
    std::uint32_t start_step = 0;
    std::uint32_t stop_step = 0;
    // A positive channel depth clips a ribbon's rectangular grid against the
    // rounded-V bed used by the river terrain overlay. Zero preserves a plain
    // rectangular ribbon for non-channel callers.
    float channel_depth_m = 0.0f;
    float channel_asymmetry = 0.0f;
};

inline bool valid_fluid_emitter(const FluidEmitter& emitter) noexcept {
    const auto finite3 = [](matter::Float3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    };
    const auto length_squared = [](matter::Float3 value) {
        return value.x * value.x + value.y * value.y + value.z * value.z;
    };
    const auto dot = [](matter::Float3 a, matter::Float3 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    if (!finite3(emitter.position_m) || !finite3(emitter.direction) ||
        !finite3(emitter.initial_velocity_mps) ||
        !std::isfinite(emitter.flow_m3s) || emitter.flow_m3s <= 0.0f ||
        emitter.start_step >= emitter.stop_step)
        return false;
    const float direction_length = length_squared(emitter.direction);
    if (!std::isfinite(direction_length) || direction_length <= 0.0f)
        return false;
    if (emitter.shape == FluidEmitterShape::Disc)
        return std::isfinite(emitter.radius_m) && emitter.radius_m > 0.0f;
    if (emitter.shape != FluidEmitterShape::Ribbon ||
        !finite3(emitter.lateral_axis) || !finite3(emitter.up_axis) ||
        !std::isfinite(emitter.half_extent_m.x) ||
        !std::isfinite(emitter.half_extent_m.y) ||
        !std::isfinite(emitter.channel_depth_m) ||
        !std::isfinite(emitter.channel_asymmetry) ||
        emitter.half_extent_m.x <= 0.0f || emitter.half_extent_m.y <= 0.0f ||
        emitter.channel_depth_m < 0.0f ||
        std::fabs(emitter.channel_asymmetry) > 1.0f)
        return false;
    constexpr float tolerance = 1.0e-4f;
    const float lateral_length = length_squared(emitter.lateral_axis);
    const float up_length = length_squared(emitter.up_axis);
    return std::fabs(direction_length - 1.0f) <= tolerance &&
           std::fabs(lateral_length - 1.0f) <= tolerance &&
           std::fabs(up_length - 1.0f) <= tolerance &&
           std::fabs(dot(emitter.direction, emitter.lateral_axis)) <= tolerance &&
           std::fabs(dot(emitter.direction, emitter.up_axis)) <= tolerance &&
           std::fabs(dot(emitter.lateral_axis, emitter.up_axis)) <= tolerance;
}

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
    std::uint32_t minimum_particles_per_cell = 1;
    // The authored sensor is a channel-aligned rectangular prism. bounds_m is
    // its enclosing world AABB for validation and diagnostics only; occupancy
    // is evaluated in this local longitudinal/vertical/lateral frame.
    matter::Float3 frame_origin_m{};
    matter::Float2 longitudinal_axis_xz{1.0f, 0.0f};
    matter::Float2 lateral_axis_xz{0.0f, 1.0f};
    matter::Float3 frame_extent_m{};
};

inline bool valid_fluid_fill_sensor_frame(
    const FluidFillSensor& sensor) noexcept {
    const auto finite2 = [](matter::Float2 value) {
        return std::isfinite(value.x) && std::isfinite(value.y);
    };
    const auto finite3 = [](matter::Float3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    };
    if (!finite3(sensor.frame_origin_m) ||
        !finite2(sensor.longitudinal_axis_xz) ||
        !finite2(sensor.lateral_axis_xz) ||
        !finite3(sensor.frame_extent_m) ||
        !(sensor.frame_extent_m.x > 0.0f) ||
        !(sensor.frame_extent_m.y > 0.0f) ||
        !(sensor.frame_extent_m.z > 0.0f)) {
        return false;
    }
    const float longitudinal_length_sq =
        sensor.longitudinal_axis_xz.x * sensor.longitudinal_axis_xz.x +
        sensor.longitudinal_axis_xz.y * sensor.longitudinal_axis_xz.y;
    const float lateral_length_sq =
        sensor.lateral_axis_xz.x * sensor.lateral_axis_xz.x +
        sensor.lateral_axis_xz.y * sensor.lateral_axis_xz.y;
    const float dot =
        sensor.longitudinal_axis_xz.x * sensor.lateral_axis_xz.x +
        sensor.longitudinal_axis_xz.y * sensor.lateral_axis_xz.y;
    constexpr float tolerance = 1.0e-4f;
    return std::abs(longitudinal_length_sq - 1.0f) <= tolerance &&
           std::abs(lateral_length_sq - 1.0f) <= tolerance &&
           std::abs(dot) <= tolerance;
}

inline matter::Float3 fluid_fill_sensor_local_position(
    const FluidFillSensor& sensor, matter::Float3 position_m) noexcept {
    const float delta_x = position_m.x - sensor.frame_origin_m.x;
    const float delta_z = position_m.z - sensor.frame_origin_m.z;
    return {
        delta_x * sensor.longitudinal_axis_xz.x +
            delta_z * sensor.longitudinal_axis_xz.y,
        position_m.y - sensor.frame_origin_m.y,
        delta_x * sensor.lateral_axis_xz.x +
            delta_z * sensor.lateral_axis_xz.y};
}

struct FluidPbdSettings {
    float particle_spacing_m = 0.0f;
    float rest_density_kg_m3 = 0.0f;
    float fixed_step_seconds = 0.0f;
    std::uint32_t solver_iterations = 0;
    std::uint32_t max_neighbors = 0;
    std::uint32_t batch_steps = 0;
    std::uint32_t max_steps = 0;
    std::uint32_t max_particles = 0;
    matter::HydrologyEscapePolicy escape_policy{};
};

struct FillSensorResult {
    float wet_fraction = 0.0f;
    std::uint32_t stable_steps = 0;
    std::uint32_t completion_step = 0;
    bool complete = false;
    float maximum_wet_fraction = 0.0f;
    float final_wet_fraction = 0.0f;
    float stable_window_wet_fraction = 0.0f;
    std::uint32_t first_satisfied_step = 0;
};

struct FluidBakeStats {
    std::uint32_t simulated_steps = 0;
    std::uint32_t active_particles = 0;
    std::uint32_t peak_particles = 0;
    std::uint32_t escaped_particles = 0;
    std::uint32_t non_finite_particles = 0;
    double wall_seconds = 0.0;
    std::uint32_t emitted_particles = 0;
    std::uint32_t escape_budget = 0;
    std::uint32_t retired_particles = 0;
    matter::HydrologyEscapePolicy escape_policy{};
};

struct FluidParticleAnimationFrame {
    std::uint32_t simulation_step = 0;
    std::vector<matter::Float3> positions_m;
};

struct FluidParticleAnimationCapture {
    std::uint32_t frames_per_second = 0;
    std::uint32_t phase_offset_frames = 0;
    double host_readback_ms = 0.0;
    std::uint64_t device_storage_bytes = 0u;
    std::vector<FluidParticleAnimationFrame> frames;
};

struct FluidBakeOutput {
    std::vector<FluidParticle> particles;
    std::vector<FluidQuarantinedParticle> quarantined_particles;
    FillSensorResult sensor{};
    FluidBakeStats stats{};
    std::optional<FluidParticleAnimationCapture> animation_capture;
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
