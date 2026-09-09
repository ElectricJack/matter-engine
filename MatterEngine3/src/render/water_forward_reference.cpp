#include "water_forward_reference.h"

#include <algorithm>
#include <cmath>

namespace viewer {
namespace {

float finite_nonnegative(float value) noexcept {
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

}  // namespace

WaterScreenDepth water_screen_depth_reference(
    float baked_depth_m, float water_ray_distance_m,
    float center_opaque_ray_distance_m,
    float refracted_opaque_ray_distance_m,
    float discontinuity_limit_m) noexcept {
    WaterScreenDepth result{};
    result.distance_m = finite_nonnegative(baked_depth_m);

    if (!std::isfinite(water_ray_distance_m) ||
        !std::isfinite(center_opaque_ray_distance_m) ||
        !std::isfinite(refracted_opaque_ray_distance_m) ||
        !std::isfinite(discontinuity_limit_m) ||
        water_ray_distance_m < 0.0f || center_opaque_ray_distance_m <= 0.0f ||
        refracted_opaque_ray_distance_m <= 0.0f ||
        center_opaque_ray_distance_m <= water_ray_distance_m ||
        refracted_opaque_ray_distance_m <= water_ray_distance_m ||
        std::fabs(refracted_opaque_ray_distance_m -
                  center_opaque_ray_distance_m) >
            std::max(discontinuity_limit_m, 0.0f)) {
        return result;
    }

    const float maximum_screen_distance =
        std::max(result.distance_m * 2.0f, result.distance_m + 0.5f);
    result.distance_m = std::min(refracted_opaque_ray_distance_m -
                                     water_ray_distance_m,
                                 maximum_screen_distance);
    result.valid = true;
    return result;
}

matter::Float2 water_refraction_uv_reference(
    matter::Float2 source_uv, matter::Float2 normal_xz,
    float optical_distance_m, matter::Float2 viewport_px,
    float max_offset_px) noexcept {
    if (!std::isfinite(source_uv.x) || !std::isfinite(source_uv.y) ||
        !std::isfinite(normal_xz.x) || !std::isfinite(normal_xz.y) ||
        !std::isfinite(optical_distance_m) ||
        !std::isfinite(viewport_px.x) || !std::isfinite(viewport_px.y) ||
        !std::isfinite(max_offset_px) || viewport_px.x <= 0.0f ||
        viewport_px.y <= 0.0f) {
        return source_uv;
    }

    // Float2.y is world normal Z. Positive X and Z map directly to
    // increasing texture UV X and Y, matching water_screen_space.glsl.
    matter::Float2 offset_px{
        normal_xz.x * std::max(optical_distance_m, 0.0f),
        normal_xz.y * std::max(optical_distance_m, 0.0f)};
    const float offset_length =
        std::sqrt(offset_px.x * offset_px.x + offset_px.y * offset_px.y);
    const float maximum = std::max(max_offset_px, 0.0f);
    if (offset_length > maximum && offset_length > 0.0f) {
        const float scale = maximum / offset_length;
        offset_px.x *= scale;
        offset_px.y *= scale;
    }

    const matter::Float2 candidate{
        source_uv.x + offset_px.x / viewport_px.x,
        source_uv.y + offset_px.y / viewport_px.y};
    const matter::Float2 inset{0.5f / viewport_px.x,
                               0.5f / viewport_px.y};
    if (candidate.x < inset.x || candidate.y < inset.y ||
        candidate.x > 1.0f - inset.x ||
        candidate.y > 1.0f - inset.y) {
        return source_uv;
    }
    return candidate;
}

}  // namespace viewer
