#pragma once

#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/river_runtime.h"

#include "flecs.h"

#include <array>
#include <cstdint>
#include <memory>

namespace matter::river_float {

enum class RiverSampleStatus : std::uint8_t { Wet, Dry, Invalid };

using RiverSampleCallback = RiverSampleStatus (*)(
    const void*, Float3, RiverFieldSample&) noexcept;

struct RiverSampleFunction {
    const void* context = nullptr;
    RiverSampleCallback callback = nullptr;
};

struct RiverFloatProbeForce {
    Float3 force_n{};
    Float3 world_point_m{};
};

struct RiverFloatForceBuffer {
    std::array<RiverFloatProbeForce, 64> rows{};
    std::uint32_t count = 0;
};

struct RiverFloatDiagnostics {
    std::uint64_t binding_generation = 0;
    std::uint64_t sample_checksum = 0;
    std::uint64_t force_checksum = 0;
    std::uint32_t wet_probe_count = 0;
    std::uint32_t dry_probe_count = 0;
    bool hard_invalid = false;
    float reference_mass_kg = 0.0f;
    float equilibrium_submerged_fraction = 0.0f;
};

struct RiverFloatState {
    std::uint64_t binding_generation = 0;
    std::uint32_t consecutive_invalid = 0;
    bool disabled = false;
    bool diagnostic_emitted = false;
    std::uint64_t sample_checksum = 0;
    std::uint64_t force_checksum = 0;
};

using RiverBindingAcquire = std::shared_ptr<const RiverRuntimeBinding> (*)(
    const void*) noexcept;

struct RiverFloatMeasurementHook {
    void* context = nullptr;
    void (*begin)(void*) noexcept = nullptr;
    void (*end)(void*) noexcept = nullptr;
};

bool valid_river_float_body(const RiverFloatBody& value) noexcept;

bool compute_river_float_forces(
    const RiverFloatBody& settings,
    const physics::BoxCollider& box,
    const ecs::LocalTransform& transform,
    const physics::PhysicsVelocity& velocity,
    const RiverSampleFunction& sample,
    float gravity_mps2,
    RiverFloatForceBuffer& output,
    RiverFloatDiagnostics& diagnostics) noexcept;

void register_river_float_systems(flecs::world& world);
void install_test_binding(flecs::world& world, std::uint64_t generation,
                          RiverSampleFunction sample) noexcept;
void install_runtime_binding(flecs::world& world, const void* context,
                             RiverBindingAcquire acquire) noexcept;
void clear_runtime_binding(flecs::world& world) noexcept;
void install_measurement_hook(flecs::world& world,
                              RiverFloatMeasurementHook hook) noexcept;

std::uint64_t checksum_transform(const ecs::LocalTransform& transform) noexcept;

} // namespace matter::river_float
