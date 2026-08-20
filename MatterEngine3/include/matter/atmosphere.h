#pragma once

// MatterEngine3/include/matter/atmosphere.h
//
// The authored atmosphere parameters and the CPU REFERENCE evaluation of
// atmospheric transmittance. Header-only, allocation-free, no Vulkan and no
// engine state — which is what lets the same arithmetic run in the renderer,
// in the world loader and in tests.
//
// HOW IT FITS
//
//   - `AtmosphereSettings` is authored per world (parsed in
//     src/script/world_definition_loader.cpp, stored on WorldDefinition and
//     surfaced by WorldSession::world_atmosphere) and exposed as a property
//     group in MatterEditor/src/editor_props.cpp.
//   - `viewer::VkAtmosphere` (src/render/vk_atmosphere.h/.cpp) owns the GPU
//     lookup textures and calls `atmosphere_direct_sun_rgb` /
//     `atmosphere_direct_sun_transmittance` here for the CPU-side direct sun
//     colour. It caches: settings, sun direction and observer altitude are
//     compared before a rebuild.
//   - MatterEngine3/tests/atmosphere_tests.cpp pins this file.
//
// MODEL AND UNITS
//
// A spherical planet of radius `kAtmospherePlanetRadiusM` with the top of the
// atmosphere 100 km above it. All lengths are METRES; scattering/extinction
// coefficients are PER METRE at sea level, per RGB channel. The observer is
// placed on the +Y axis at `observer_world_y - settings.sea_level_y`, clamped
// to [0, 100000] m — so `sea_level_y` is the world Y that means "ground".
// The integral runs in double and returns linear-RGB `Float3`.
//
// DIRECTION CONVENTION
//
// A `sun_direction` in this engine points FROM the sun TOWARD the scene — the
// direction light travels (matter/sun_angles.h spells this out). The
// transmittance walker marches the other way, so
// `atmosphere_direct_sun_transmittance` negates it before integrating. The
// `ray_direction` taken by `atmosphere_transmittance_reference`, in contrast,
// is the direction to march, i.e. already pointing away from the observer.
//
// FAILURE CONVENTION — worth knowing because the two are NOT the same value:
//
//   - `atmosphere_transmittance_reference` returns CLEAR (1,1,1) with
//     `valid = false` for non-finite input or a degenerate ray. Clear, not
//     black, so a bad input cannot silently darken a scene.
//   - `atmosphere_direct_sun_transmittance` returns ZERO when the sun is at or
//     below the horizon or the planet occludes it. That is a normal night-time
//     outcome, not an error.
//
// Nothing here ever returns NaN: every entry point sanitizes its settings
// first and re-checks finiteness on the way out.
//
// COST. `atmosphere_transmittance_reference` evaluates `sample_count`
// (default 256) sample points, each with three exp() calls. It is a reference
// integrator meant for LUT construction and a handful of per-frame queries —
// not something to call per pixel or per particle.

#include "matter/math_types.h"

#include <algorithm>
#include <cmath>

namespace matter {

// Earth-like reference values, in metres and per-metre coefficients. Rayleigh
// and Mie densities fall off exponentially with their scale heights; ozone is
// a TENT centred at kOzoneCenterHeightM and reaching zero kOzoneHalfWidthM
// either side, and it absorbs without scattering.
//
// Rayleigh does not absorb, so kRayleighScattering doubles as its extinction
// and is what the integral below uses. Mie does, which is why kMieExtinction
// exists separately.
//
// kMieScattering has NO reader anywhere in the repo, and that is expected
// rather than an oversight: only extinction terms enter a transmittance
// integral, and this file computes nothing else. It is kept because these are
// one published reference table and the pair carries the Mie single-scattering
// albedo (scattering / extinction ~= 0.9), which is what an in-scattering or
// multiple-scattering pass would need. Being `inline constexpr` it costs
// nothing to keep. Do not delete it as dead code without also deciding the
// table is no longer the reference.
//
// kExtraterrestrialSolarRgb is deliberately (1,1,1): the absolute solar
// magnitude and colour are left to the caller's authored modifier and live
// tint (see atmosphere_direct_sun_rgb), so this header stays a pure
// transmittance model.
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

// The authored, per-world atmosphere dials. Small and copyable by design: it
// is compared for equality to decide whether the GPU LUTs need rebuilding
// (see `same_settings` in src/render/vk_atmosphere.cpp), so adding a field
// here means teaching that comparison about it.
//
// Ranges below are the ones `sanitize_atmosphere` enforces; nothing downstream
// re-checks, so pass authored values through it first (every function in this
// header does so itself).
struct AtmosphereSettings {
    // World-space Y, in metres, that maps to planet-surface altitude 0.
    // Checked for finiteness but deliberately NOT clamped — worlds sit at
    // arbitrary altitudes.
    float sea_level_y = 0.0f;
    // Dimensionless multipliers on the built-in coefficients above; clamped
    // to [0, 4]. 0 removes that term entirely.
    float rayleigh_scale = 1.0f;
    float mie_scale = 1.0f;
    // Henyey-Greenstein asymmetry g, clamped to [-0.99, 0.99]. Forward
    // scattering is positive. Used by the sky/volumetric PHASE function
    // downstream — it does not enter the transmittance integral in this file.
    float mie_anisotropy = 0.8f;
    float ozone_scale = 1.0f;
    // Ground reflectance for the multiple-scattering term, 0-1. Also unused
    // by the transmittance integral here.
    float ground_albedo = 0.1f;
};

struct AtmosphereTransmittanceResult {
    Float3 transmittance{};
    bool valid = false;
};

// Fold any authored or scripted settings into the legal ranges. A NON-FINITE
// field falls back to the DEFAULT rather than being clamped, because clamping
// a NaN yields a NaN — that is the whole reason this is a separate pass and
// not a set of clamps at the use sites.
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

// Transmittance along a ray leaving the observer in `ray_direction` (the
// direction to MARCH, need not be normalized), by uniform ray marching to the
// top of the atmosphere — or to the planet surface, whichever comes first, so
// a downward ray integrates only to the ground.
//
// "Reference" means exactly that: it is the ground truth the GPU LUTs are
// built from, at `sample_count` samples x 3 exp() per sample. Sanitizes
// `settings` itself.
//
// Returns `{(1,1,1), false}` — CLEAR, not black — for a non-finite observer
// height, a degenerate direction, a ray that never exits, or a non-finite
// optical depth.
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

// Elevation in degrees -> a normalized TO-SUN direction in the XY plane
// (z = 0). Azimuth is not a parameter because it cannot matter: the model is
// spherically symmetric about the observer's Y axis, so transmittance depends
// only on the angle above the horizon. Returns (0,0,0) for a non-finite
// elevation.
inline Float3 atmosphere_to_sun_from_elevation_deg(double elevation_deg) {
    if (!std::isfinite(elevation_deg)) return {};
    constexpr double kPi = 3.14159265358979323846;
    const double radians = elevation_deg * kPi / 180.0;
    return {static_cast<float>(std::cos(radians)), static_cast<float>(std::sin(radians)), 0.0f};
}

// Transmittance of direct sunlight reaching the observer. `sun_direction` is
// the engine's FROM-sun-TOWARD-scene vector (matter/sun_angles.h), which is
// why the body negates it.
//
// Returns ZERO when the sun is at or below the horizon for an observer at or
// below sea level, or when the planet occludes it before the ray leaves the
// atmosphere — i.e. black is the night-time answer, distinct from the clear
// (1,1,1) that atmosphere_transmittance_reference returns for BAD input.
inline Float3 atmosphere_direct_sun_transmittance(
    const AtmosphereSettings& settings, double observer_world_y,
    const Float3& sun_direction, int sample_count = 256) {
    const AtmosphereSettings clean = sanitize_atmosphere(settings);
    if (!std::isfinite(observer_world_y)) return {};
    // The engine stores a ray travelling from the sun toward the scene; the
    // atmosphere integral instead marches from the observer toward the sun.
    detail::AtmosphereDouble3 direction{
        -static_cast<double>(sun_direction.x), -static_cast<double>(sun_direction.y), -static_cast<double>(sun_direction.z)};
    if (!detail::normalize(direction)) return {};
    const double height = std::clamp(observer_world_y - static_cast<double>(clean.sea_level_y), 0.0, 100000.0);
    const detail::AtmosphereDouble3 origin{0.0, kAtmospherePlanetRadiusM + height, 0.0};
    const double planet_distance = detail::ray_sphere_near_distance(origin, direction, kAtmospherePlanetRadiusM);
    const double atmosphere_distance = detail::ray_sphere_exit_distance(origin, direction, kAtmosphereTopRadiusM);
    if ((height <= 0.0 && detail::dot(origin, direction) < 0.0) ||
        (planet_distance > 0.0 && planet_distance < atmosphere_distance)) return {};
    return atmosphere_transmittance_reference(
        clean, observer_world_y, {static_cast<float>(direction.x), static_cast<float>(direction.y),
                                   static_cast<float>(direction.z)}, sample_count).transmittance;
}

// Convenience overload taking a sun ELEVATION in degrees instead of a vector.
// The double negation is deliberate and not redundant: the helper above
// produces a to-sun vector, this converts it into the engine's from-sun
// convention, and the overload it calls negates it back to march toward the
// sun. Keeping the conversion explicit means both overloads agree by
// construction.
inline Float3 atmosphere_direct_sun_transmittance(
    const AtmosphereSettings& settings, double observer_world_y,
    double elevation_deg, int sample_count = 256) {
    const Float3 to_sun = atmosphere_to_sun_from_elevation_deg(elevation_deg);
    const Float3 sun_direction{-to_sun.x, -to_sun.y, -to_sun.z};
    return atmosphere_direct_sun_transmittance(settings, observer_world_y, sun_direction, sample_count);
}

// The final linear-RGB direct sun colour, as a per-channel product of four
// things: the extraterrestrial solar colour (currently (1,1,1) — see the
// constants block), the atmospheric transmittance, the world's authored
// modifier, and the editor's live tint and multiplier (`sun_tint` /
// `sun_multiplier` on VulkanLightingOverrides, matter/atmosphere_lighting.h).
//
// Nothing is clamped or normalized here, and the result is zero whenever the
// transmittance is — see the overload above for when that happens.
inline Float3 atmosphere_direct_sun_rgb(
    const AtmosphereSettings& settings, double observer_world_y, const Float3& sun_direction,
    const Float3& authored_modifier, const Float3& live_tint, float live_multiplier) {
    const Float3 transmittance = atmosphere_direct_sun_transmittance(settings, observer_world_y, sun_direction);
    return {
        static_cast<float>(kExtraterrestrialSolarRgb[0]) * transmittance.x * authored_modifier.x * live_tint.x * live_multiplier,
        static_cast<float>(kExtraterrestrialSolarRgb[1]) * transmittance.y * authored_modifier.y * live_tint.y * live_multiplier,
        static_cast<float>(kExtraterrestrialSolarRgb[2]) * transmittance.z * authored_modifier.z * live_tint.z * live_multiplier};
}

} // namespace matter
