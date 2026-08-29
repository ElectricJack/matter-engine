#pragma once

#include "hydrology/physx_fluid_types.h"
#include "hydrology/spillway_handoff.h"
#include "matter/gpu_visual_meshing.h"

#include <array>
#include <cstdint>

namespace hydrology {

struct WaterCutContourMetrics {
    std::uint32_t first_points = 0;
    std::uint32_t second_points = 0;
    std::uint32_t unmatched_open_edges = 0;
    std::uint32_t duplicate_coplanar_triangles = 0;
    float symmetric_hausdorff_m = 0.0f;
    float rms_distance_m = 0.0f;
    std::array<float, 3> first_height_quantiles_m{};
    std::array<float, 3> second_height_quantiles_m{};
    float minimum_normal_dot = 1.0f;
    float p95_normal_angle_degrees = 0.0f;
};

bool measure_water_cut_continuity(
    const gpu_meshing::MeshResult& first,
    const gpu_meshing::MeshResult& second,
    const SpillwayHandoffRecord& handoff,
    float signed_cut_m,
    float edge_match_tolerance_m,
    WaterCutContourMetrics& metrics,
    FluidBakeError& error);

bool water_cut_is_assertion_weldable(
    const WaterCutContourMetrics& metrics,
    float quantization_tolerance_m) noexcept;

}  // namespace hydrology
