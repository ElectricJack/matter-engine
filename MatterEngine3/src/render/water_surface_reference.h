#pragma once

#include "matter/math_types.h"
#include "matter/river_network.h"
#include "water_field_vk.h"

#include <array>

namespace viewer {

constexpr int kWaterBacktraceSteps = 3;
constexpr int kWaterWaveBandCount = 3;

struct WaterSurfaceFieldSample {
    float surface_height_m = 0.0f;
    float depth_m = 0.0f;
    matter::Float3 velocity_mps{};
    matter::Float3 base_normal{0.0f, 1.0f, 0.0f};
    float turbulence = 0.0f;
    float aeration = 0.0f;
    float foam_potential = 0.0f;
    hydrology::RiverFeature feature = hydrology::RiverFeature::Calm;
    bool valid = false;
};

struct WaterDualPhase {
    float age_seconds[2]{};
    float weight[2]{};
};

struct WaterSurfaceEvaluation {
    matter::Float3 shading_normal{0.0f, 1.0f, 0.0f};
    float roughness = 0.0f;
    WaterSurfaceFieldSample field{};
    bool animated = false;
};

bool water_sample_field_reference(
    const PackedWaterField& field, WaterFieldBinding published_binding,
    WaterFieldBinding requested_binding, matter::Float2 world_xz,
    WaterSurfaceFieldSample& output) noexcept;

bool water_backtrace_rk2_reference(
    const PackedWaterField& field, WaterFieldBinding published_binding,
    WaterFieldBinding requested_binding, matter::Float2 world_xz,
    float age_seconds, float max_shading_speed_mps,
    matter::Float2& output) noexcept;

WaterDualPhase water_wrapped_phase_reference(float time_seconds,
                                              float period_seconds) noexcept;

std::array<float, kWaterWaveBandCount> water_band_responses_reference(
    const matter::WaterSurfaceDefinition& surface,
    const WaterSurfaceFieldSample& field) noexcept;

bool water_evaluate_surface_reference(
    const PackedWaterField& field, WaterFieldBinding published_binding,
    WaterFieldBinding requested_binding,
    const matter::WaterSurfaceDefinition& surface, matter::Float2 world_xz,
    matter::Float3 geometric_normal, float animation_time_seconds,
    float base_roughness, WaterSurfaceEvaluation& output) noexcept;

}  // namespace viewer
