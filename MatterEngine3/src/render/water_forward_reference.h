#pragma once

#include "matter/math_types.h"

namespace viewer {

struct WaterScreenDepth {
    float distance_m = 0.0f;
    bool valid = false;
};

WaterScreenDepth water_screen_depth_reference(
    float baked_depth_m, float water_ray_distance_m,
    float center_opaque_ray_distance_m,
    float refracted_opaque_ray_distance_m,
    float discontinuity_limit_m) noexcept;

matter::Float2 water_refraction_uv_reference(
    matter::Float2 source_uv, matter::Float2 normal_xz,
    float optical_distance_m, matter::Float2 viewport_px,
    float max_offset_px) noexcept;

}  // namespace viewer
