#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "math_types.h"
#include "sun_angles.h"

namespace matter {

struct VulkanLightingOverrides {
    float sun_multiplier = 1.67f;
    float sky_multiplier = 0.77f;
    float emission_multiplier = 1.0f;
    float exposure_ev = -2.0f;
    float composite_debug_view = 0.0f;
    float sun_tint[3] = {1.0f, 1.0f, 1.0f};
    float sky_tint[3] = {1.0f, 1.0f, 1.0f};
    float day_ambient_multiplier = 0.25f;
    float twilight_ambient_multiplier = 1.0f;
    float sky_irradiance_multiplier = 1.0f;
    float sunset_direct_ratio = 0.25f;
    float sun_azimuth_deg = 127.874985f;
    float sun_elevation_deg = 54.525963f;
    float sun_angular_diameter_deg = kSunAngularDiameterDefaultDeg;
    int32_t sun_shadow_samples = 1;
};

struct AtmosphereLightingSources {
    Float3 atmospheric_direct_base_rgb{};
    Float3 atmospheric_noon_direct_base_rgb{};
    Float3 authored_display_sky_chroma_rgb{};
    Float3 authored_irradiance_chroma_rgb{};
    Float3 live_sun_tint_rgb{1.0f, 1.0f, 1.0f};
    Float3 live_sky_tint_rgb{1.0f, 1.0f, 1.0f};
    float sun_multiplier = 1.67f;
    float sky_multiplier = 0.77f;
    float sky_irradiance_multiplier = 1.0f;
    float day_ambient_multiplier = 0.25f;
    float twilight_ambient_multiplier = 1.0f;
    float sunset_direct_ratio = 0.25f;
    float elevation_deg = 54.525963f;
};

struct ResolvedAtmosphereLighting {
    Float3 atmospheric_direct_base_rgb{};
    Float3 direct_base_rgb{};
    Float3 direct_world_sun_rgb{};
    Float3 sun_disc_rgb{};
    Float3 sky_display_modifier_rgb{};
    Float3 sky_irradiance_modifier_rgb{};
    float direct_world_ratio = 0.0f;
    float sky_ambient_ratio = 0.0f;
    float resolved_elevation_deg = 0.0f;
};

inline float atmosphere_lighting_smoothstep(float a, float b,
                                             float x) noexcept {
    const float q = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return q * q * (3.0f - 2.0f * q);
}

inline float direct_world_ratio(float elevation_deg,
                                float sunset_direct_ratio) noexcept {
    if (!std::isfinite(elevation_deg) || !std::isfinite(sunset_direct_ratio))
        return 0.0f;
    const float sunset = std::clamp(sunset_direct_ratio, 0.0f, 1.0f);
    if (elevation_deg <= 0.0f) return 0.0f;
    if (elevation_deg < 5.0f)
        return sunset * atmosphere_lighting_smoothstep(0.0f, 5.0f,
                                                        elevation_deg);
    if (elevation_deg < 45.0f)
        return sunset + (1.0f - sunset) *
                            atmosphere_lighting_smoothstep(5.0f, 45.0f,
                                                           elevation_deg);
    return 1.0f;
}

inline float sky_twilight_mix(float elevation_deg) noexcept {
    if (!std::isfinite(elevation_deg)) return 0.0f;
    return 1.0f - atmosphere_lighting_smoothstep(-6.0f, 5.0f,
                                                  elevation_deg);
}

inline float sky_ambient_ratio(float elevation_deg,
                               float day_ambient_multiplier,
                               float twilight_ambient_multiplier) noexcept {
    if (!std::isfinite(day_ambient_multiplier) ||
        !std::isfinite(twilight_ambient_multiplier))
        return 0.0f;
    const float mix = sky_twilight_mix(elevation_deg);
    return day_ambient_multiplier +
           (twilight_ambient_multiplier - day_ambient_multiplier) * mix;
}

inline ResolvedAtmosphereLighting resolve_atmosphere_lighting(
    const AtmosphereLightingSources& source) noexcept {
    ResolvedAtmosphereLighting result{};
    result.atmospheric_direct_base_rgb = source.atmospheric_direct_base_rgb;
    result.resolved_elevation_deg =
        std::isfinite(source.elevation_deg) ? source.elevation_deg : 0.0f;
    result.direct_world_ratio = direct_world_ratio(
        result.resolved_elevation_deg, source.sunset_direct_ratio);
    result.sky_ambient_ratio = sky_ambient_ratio(
        result.resolved_elevation_deg, source.day_ambient_multiplier,
        source.twilight_ambient_multiplier);
    const auto product = [](Float3 a, Float3 b, float scale) noexcept {
        Float3 value{a.x * b.x * scale, a.y * b.y * scale,
                     a.z * b.z * scale};
        if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z))
            return Float3{};
        return value;
    };
    result.direct_base_rgb = product(source.atmospheric_direct_base_rgb,
                                     source.live_sun_tint_rgb,
                                     source.sun_multiplier);
    result.sun_disc_rgb = result.direct_base_rgb;
    const Float3 noon_direct_base_rgb = product(
        source.atmospheric_noon_direct_base_rgb, source.live_sun_tint_rgb,
        source.sun_multiplier);
    const auto luminance = [](Float3 value) noexcept {
        return 0.2126f * value.x + 0.7152f * value.y + 0.0722f * value.z;
    };
    const float current_luminance = luminance(result.direct_base_rgb);
    const float noon_luminance = luminance(noon_direct_base_rgb);
    if (result.direct_world_ratio > 0.0f &&
        std::isfinite(current_luminance) && current_luminance > 0.0f &&
        std::isfinite(noon_luminance) && noon_luminance > 0.0f) {
        result.direct_world_sun_rgb = product(
            result.direct_base_rgb, {1.0f, 1.0f, 1.0f},
            result.direct_world_ratio * noon_luminance / current_luminance);
    }
    result.sky_display_modifier_rgb = {
        source.authored_display_sky_chroma_rgb.x * source.sky_multiplier *
            source.live_sky_tint_rgb.x,
        source.authored_display_sky_chroma_rgb.y * source.sky_multiplier *
            source.live_sky_tint_rgb.y,
        source.authored_display_sky_chroma_rgb.z * source.sky_multiplier *
            source.live_sky_tint_rgb.z};
    result.sky_irradiance_modifier_rgb = {
        source.authored_irradiance_chroma_rgb.x *
            source.sky_irradiance_multiplier * result.sky_ambient_ratio,
        source.authored_irradiance_chroma_rgb.y *
            source.sky_irradiance_multiplier * result.sky_ambient_ratio,
        source.authored_irradiance_chroma_rgb.z *
            source.sky_irradiance_multiplier * result.sky_ambient_ratio};
    const auto finite_or_zero = [](Float3 value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
                       std::isfinite(value.z)
                   ? value
                   : Float3{};
    };
    result.sky_display_modifier_rgb =
        finite_or_zero(result.sky_display_modifier_rgb);
    result.sky_irradiance_modifier_rgb =
        finite_or_zero(result.sky_irradiance_modifier_rgb);
    return result;
}

enum AtmosphereLightingChange : uint32_t {
    kAtmosphereChangeNone = 0,
    kAtmosphereChangeDirect = 1u << 0,
    kAtmosphereChangeDisplay = 1u << 1,
    kAtmosphereChangeIrradiance = 1u << 2,
    kAtmosphereChangeEmission = 1u << 3,
    kAtmosphereChangeExposure = 1u << 4,
    kAtmosphereChangeDisc = 1u << 5,
    kAtmosphereChangeShadow = 1u << 6,
};

struct AtmosphereHistoryDecision {
    bool reset_diffuse_gi = false;
    bool reset_reflection_miss = false;
    bool reset_volumetric = false;
};

inline AtmosphereHistoryDecision atmosphere_history_decision(
    uint32_t change_mask, bool full_commit) noexcept {
    AtmosphereHistoryDecision result{};
    if (full_commit) {
        result.reset_diffuse_gi = true;
    }
    if ((change_mask & (kAtmosphereChangeDirect |
                        kAtmosphereChangeIrradiance)) != 0) {
        result.reset_diffuse_gi = true;
    }
    if ((change_mask & kAtmosphereChangeDisplay) != 0)
        result.reset_reflection_miss = true;
    if ((change_mask & (kAtmosphereChangeEmission |
                        kAtmosphereChangeDisc)) != 0) {
        result.reset_diffuse_gi = true;
        result.reset_reflection_miss = true;
    }
    return result;
}

}  // namespace matter
