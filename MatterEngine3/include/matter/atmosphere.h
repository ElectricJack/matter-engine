#pragma once

#include "matter/math_types.h"

#include <algorithm>
#include <cmath>

namespace matter {

inline constexpr double kAtmospherePlanetRadiusM = 6360000.0;
inline constexpr double kAtmosphereTopRadiusM = 6460000.0;
inline constexpr double kRayleighScaleHeightM = 8000.0;
inline constexpr double kMieScaleHeightM = 1200.0;
inline constexpr double kOzoneCenterHeightM = 25000.0;
inline constexpr double kOzoneHalfWidthM = 15000.0;
inline constexpr double kRayleighScattering[3] =
    {5.802e-6, 13.558e-6, 33.100e-6};
inline constexpr double kMieScattering[3] =
    {3.996e-6, 3.996e-6, 3.996e-6};
inline constexpr double kMieExtinction[3] =
    {4.440e-6, 4.440e-6, 4.440e-6};
inline constexpr double kOzoneAbsorption[3] =
    {0.650e-6, 1.881e-6, 0.085e-6};
inline constexpr double kExtraterrestrialSolarRgb[3] = {1.0, 1.0, 1.0};

struct AtmosphereSettings {
    float sea_level_y = 0.0f;
    float rayleigh_scale = 1.0f;
    float mie_scale = 1.0f;
    float mie_anisotropy = 0.8f;
    float ozone_scale = 1.0f;
    float ground_albedo = 0.1f;
};

struct AtmosphereTransmittanceResult {
    Float3 transmittance{};
    bool valid = false;
};

inline AtmosphereSettings sanitize_atmosphere(const AtmosphereSettings& value) {
    const AtmosphereSettings defaults{};
    const auto finite_or = [](float candidate, float fallback) {
        return std::isfinite(candidate) ? candidate : fallback;
    };
    AtmosphereSettings result{};
    result.sea_level_y = finite_or(value.sea_level_y, defaults.sea_level_y);
    result.rayleigh_scale = std::clamp(finite_or(value.rayleigh_scale, defaults.rayleigh_scale), 0.0f, 4.0f);
    result.mie_scale = std::clamp(finite_or(value.mie_scale, defaults.mie_scale), 0.0f, 4.0f);
    result.mie_anisotropy = std::clamp(finite_or(value.mie_anisotropy, defaults.mie_anisotropy), -0.99f, 0.99f);
    result.ozone_scale = std::clamp(finite_or(value.ozone_scale, defaults.ozone_scale), 0.0f, 4.0f);
    result.ground_albedo = std::clamp(finite_or(value.ground_albedo, defaults.ground_albedo), 0.0f, 1.0f);
    return result;
}

namespace detail {

struct AtmosphereDouble3 {
    double x;
    double y;
    double z;
};

inline double dot(const AtmosphereDouble3& a, const AtmosphereDouble3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline bool normalize(AtmosphereDouble3& value) {
    const double length_squared = dot(value, value);
    if (!std::isfinite(length_squared) || length_squared <= 0.0) return false;
    const double inverse_length = 1.0 / std::sqrt(length_squared);
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

inline double ray_sphere_exit_distance(const AtmosphereDouble3& origin,
                                       const AtmosphereDouble3& direction,
                                       double radius) {
    const double b = dot(origin, direction);
    const double c = dot(origin, origin) - radius * radius;
    const double discriminant = b * b - c;
    if (!std::isfinite(discriminant) || discriminant < 0.0) return -1.0;
    const double root = std::sqrt(discriminant);
    const double near_distance = -b - root;
    const double far_distance = -b + root;
    if (far_distance <= 0.0) return -1.0;
    return near_distance > 0.0 ? near_distance : far_distance;
}

inline double ray_sphere_near_distance(const AtmosphereDouble3& origin,
                                       const AtmosphereDouble3& direction,
                                       double radius) {
    const double b = dot(origin, direction);
    const double c = dot(origin, origin) - radius * radius;
    const double discriminant = b * b - c;
    if (!std::isfinite(discriminant) || discriminant < 0.0) return -1.0;
    const double root = std::sqrt(discriminant);
    const double near_distance = -b - root;
    const double far_distance = -b + root;
    if (near_distance > 0.0) return near_distance;
    return far_distance > 0.0 ? far_distance : -1.0;
}

inline Float3 clear_transmittance() {
    return {1.0f, 1.0f, 1.0f};
}

} // namespace detail

inline AtmosphereTransmittanceResult atmosphere_transmittance_reference(
    const AtmosphereSettings& settings, double observer_world_y,
    const Float3& ray_direction, int sample_count = 256) {
    const AtmosphereSettings clean = sanitize_atmosphere(settings);
    if (!std::isfinite(observer_world_y)) return {detail::clear_transmittance(), false};

    detail::AtmosphereDouble3 direction{
        static_cast<double>(ray_direction.x), static_cast<double>(ray_direction.y), static_cast<double>(ray_direction.z)};
    if (!detail::normalize(direction)) return {detail::clear_transmittance(), false};

    const double height = std::clamp(observer_world_y - static_cast<double>(clean.sea_level_y), 0.0, 100000.0);
    const detail::AtmosphereDouble3 origin{0.0, kAtmospherePlanetRadiusM + height, 0.0};
    double distance = detail::ray_sphere_exit_distance(origin, direction, kAtmosphereTopRadiusM);
    if (!std::isfinite(distance) || distance <= 0.0) return {detail::clear_transmittance(), false};

    const double planet_distance = detail::ray_sphere_near_distance(origin, direction, kAtmospherePlanetRadiusM);
    if (planet_distance > 0.0) distance = std::min(distance, planet_distance);
    const int samples = sample_count > 0 ? sample_count : 256;
    const double step = distance / static_cast<double>(samples);
    double optical_depth[3] = {};

    for (int sample = 0; sample < samples; ++sample) {
        const double t = (static_cast<double>(sample) + 0.5) * step;
        const detail::AtmosphereDouble3 point{
            origin.x + direction.x * t, origin.y + direction.y * t, origin.z + direction.z * t};
        const double sample_height = std::max(0.0, std::sqrt(detail::dot(point, point)) - kAtmospherePlanetRadiusM);
        const double rayleigh_density = std::exp(-sample_height / kRayleighScaleHeightM);
        const double mie_density = std::exp(-sample_height / kMieScaleHeightM);
        const double ozone_density = std::max(0.0, 1.0 - std::abs(sample_height - kOzoneCenterHeightM) / kOzoneHalfWidthM);
        for (int channel = 0; channel < 3; ++channel) {
            const double extinction = kRayleighScattering[channel] * clean.rayleigh_scale * rayleigh_density +
                                      kMieExtinction[channel] * clean.mie_scale * mie_density +
                                      kOzoneAbsorption[channel] * clean.ozone_scale * ozone_density;
            optical_depth[channel] += extinction * step;
        }
    }

    for (double value : optical_depth) {
        if (!std::isfinite(value) || value < 0.0) return {detail::clear_transmittance(), false};
    }
    return {{static_cast<float>(std::exp(-optical_depth[0])),
             static_cast<float>(std::exp(-optical_depth[1])),
             static_cast<float>(std::exp(-optical_depth[2]))}, true};
}

inline Float3 atmosphere_to_sun_from_elevation_deg(double elevation_deg) {
    if (!std::isfinite(elevation_deg)) return {};
    constexpr double kPi = 3.14159265358979323846;
    const double radians = elevation_deg * kPi / 180.0;
    return {static_cast<float>(std::cos(radians)), static_cast<float>(std::sin(radians)), 0.0f};
}

inline Float3 atmosphere_direct_sun_transmittance(
    const AtmosphereSettings& settings, double observer_world_y,
    const Float3& to_sun, int sample_count = 256) {
    const AtmosphereSettings clean = sanitize_atmosphere(settings);
    if (!std::isfinite(observer_world_y)) return {};
    detail::AtmosphereDouble3 direction{
        static_cast<double>(to_sun.x), static_cast<double>(to_sun.y), static_cast<double>(to_sun.z)};
    if (!detail::normalize(direction)) return {};
    const double height = std::clamp(observer_world_y - static_cast<double>(clean.sea_level_y), 0.0, 100000.0);
    const detail::AtmosphereDouble3 origin{0.0, kAtmospherePlanetRadiusM + height, 0.0};
    const double planet_distance = detail::ray_sphere_near_distance(origin, direction, kAtmospherePlanetRadiusM);
    const double atmosphere_distance = detail::ray_sphere_exit_distance(origin, direction, kAtmosphereTopRadiusM);
    if ((height <= 0.0 && detail::dot(origin, direction) < 0.0) ||
        (planet_distance > 0.0 && planet_distance < atmosphere_distance)) return {};
    return atmosphere_transmittance_reference(clean, observer_world_y, to_sun, sample_count).transmittance;
}

inline Float3 atmosphere_direct_sun_transmittance(
    const AtmosphereSettings& settings, double observer_world_y,
    double elevation_deg, int sample_count = 256) {
    return atmosphere_direct_sun_transmittance(
        settings, observer_world_y, atmosphere_to_sun_from_elevation_deg(elevation_deg), sample_count);
}

inline Float3 atmosphere_direct_sun_rgb(
    const AtmosphereSettings& settings, double observer_world_y, const Float3& to_sun,
    const Float3& authored_modifier, const Float3& live_tint, float live_multiplier) {
    const Float3 transmittance = atmosphere_direct_sun_transmittance(settings, observer_world_y, to_sun);
    return {
        static_cast<float>(kExtraterrestrialSolarRgb[0]) * transmittance.x * authored_modifier.x * live_tint.x * live_multiplier,
        static_cast<float>(kExtraterrestrialSolarRgb[1]) * transmittance.y * authored_modifier.y * live_tint.y * live_multiplier,
        static_cast<float>(kExtraterrestrialSolarRgb[2]) * transmittance.z * authored_modifier.z * live_tint.z * live_multiplier};
}

} // namespace matter
