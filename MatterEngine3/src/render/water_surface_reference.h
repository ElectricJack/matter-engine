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
    float local_foam_multiplier = 1.0f;
    float local_threshold_offset = 0.0f;
    float local_wave_multiplier = 1.0f;
    hydrology::RiverFeature feature = hydrology::RiverFeature::Calm;
    bool valid = false;
};

struct WaterOpticalState {
    matter::Float3 transmittance{1.0f, 1.0f, 1.0f};
    matter::Float3 scattering_color{};
    float bottom_visibility = 1.0f;
    float reflection_weight = 0.0f;
    float coherent_transmission_weight = 1.0f;
    float diffuse_scattering_weight = 0.0f;
};

struct WaterFoamState {
    float macro_mask = 0.0f;
    float breakup_detail = 0.0f;
    float coverage = 0.0f;
    float local_multiplier = 1.0f;
    float threshold_offset = 0.0f;
    float wave_multiplier = 1.0f;
};

struct WaterDualPhase {
    float age_seconds[2]{};
    float weight[2]{};
};

struct WaterSurfaceEvaluation {
    matter::Float3 shading_normal{0.0f, 1.0f, 0.0f};
    float roughness = 0.0f;
    WaterSurfaceFieldSample field{};
    WaterOpticalState optics{};
    WaterFoamState foam{};
    float reactivity = 0.0f;
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
