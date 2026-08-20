#pragma once

// MatterEngine3/include/matter/atmosphere_lighting.h
//
// Turns "what the atmosphere produced" plus "what the author wrote" plus
// "what the user is dragging in the UI right now" into the small set of RGB
// terms the renderer uploads. Pure functions over plain structs: header-only,
// allocation-free, every function `noexcept`, no Vulkan types despite the
// `Vulkan` in the first struct's name.
//
// HOW IT FITS
//
//   - `VulkanLightingOverrides` is the LIVE, editor-editable layer, edited in
//     MatterEditor/src/ui_lighting_controls.cpp and carried through
//     src/render/vk_lighting_controls.h.
//   - `AtmosphereLightingSources` is the assembled INPUT: atmospheric bases
//     come from viewer::VkAtmosphere's committed state
//     (src/render/vk_atmosphere.h), authored chroma from the world, live tints
//     from the overrides above.
//   - `resolve_atmosphere_lighting()` is the single entry point;
//     `ResolvedAtmosphereLighting` is what src/render/vk_scene_renderer.cpp
//     consumes.
//   - `AtmosphereLightingChange` + `atmosphere_history_decision()` answer the
//     separate question of which temporal accumulation buffers a lighting edit
//     invalidates.
//   - MatterEngine3/tests/atmosphere_tests.cpp pins the curves.
//
// CONVENTIONS
//
//   - Colours are LINEAR RGB multipliers, not absolute units. Ratios are
//     normalized 0-1. Angles are DEGREES.
//   - Elevation is the sun's angle above the horizon (matter/sun_angles.h);
//     negative is below.
//   - Non-finite input never propagates: it collapses to 0, to black, or to
//     the default, depending on the function. Nothing here clamps to a
//     display range, though — the results are HDR.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "math_types.h"
#include "sun_angles.h"

namespace matter {

// The live lighting layer the user edits — one per session, sitting on top of
// whatever the world authored. Plain data, no Vulkan handles.
//
// Several defaults here (sun_multiplier, sky_multiplier, the ambient
// multipliers, sunset_direct_ratio, elevation) are SPELLED OUT AGAIN on
// `AtmosphereLightingSources` below. Change one and change the other, or an
// untouched session and an edited one resolve from different starting points.
struct VulkanLightingOverrides {
    // Scalar gains on the resolved direct/sky terms.
    float sun_multiplier = 1.67f;
    float sky_multiplier = 0.77f;
    float emission_multiplier = 1.0f;
    // Exposure in EV stops (negative darkens).
    float exposure_ev = -2.0f;
    // Debug-view selector carried as a float so it can ride the same
    // uniform block; 0 = normal composite.
    float composite_debug_view = 0.0f;
    // Linear-RGB tints, multiplied in; (1,1,1) is neutral.
    float sun_tint[3] = {1.0f, 1.0f, 1.0f};
    float sky_tint[3] = {1.0f, 1.0f, 1.0f};
    // Sky ambient gains at full day and at twilight; sky_ambient_ratio()
    // below blends between them by elevation.
    float day_ambient_multiplier = 0.25f;
    float twilight_ambient_multiplier = 1.0f;
    float sky_irradiance_multiplier = 1.0f;
    // Fraction of full direct light that survives at a 5-degree sun; see
    // direct_world_ratio() for the curve this anchors.
    float sunset_direct_ratio = 0.25f;
    // Sun aim in degrees. These defaults are the engine's default sun
    // direction {-0.45, -0.80, -0.35} expressed as angles — see
    // matter/sun_angles.h, which owns that conversion.
    float sun_azimuth_deg = 127.874985f;
    float sun_elevation_deg = 54.525963f;
    // Angular DIAMETER of the sun disc, degrees (matter/sun_angles.h).
    float sun_angular_diameter_deg = kSunAngularDiameterDefaultDeg;
    // Shadow rays per shaded point for the sun's penumbra.
    int32_t sun_shadow_samples = 1;
};

// Everything resolve_atmosphere_lighting() reads, gathered in one place so
// the resolve stays a pure function of its argument. Assembled per frame from
// the committed atmosphere state, the world's authored chroma, and the live
// overrides above.
struct AtmosphereLightingSources {
    // Direct sun colour out of the atmosphere model, linear RGB, before any
    // tint or multiplier.
    Float3 atmospheric_direct_base_rgb{};
    // The same quantity evaluated with the sun at NOON. Used only as a
    // luminance reference: direct_world_sun_rgb below is renormalized to it
    // so the world keeps roughly noon brightness while taking the current
    // sun's chroma. Leave it zero and that renormalization is skipped.
    Float3 atmospheric_noon_direct_base_rgb{};
    // World-authored chroma for the visible sky and for the sky's
    // contribution to ambient irradiance. Separate because a world may want
    // the sky to LOOK one colour and LIGHT with another.
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

// The resolve output — what the renderer uploads. All linear RGB.
//
//   atmospheric_direct_base_rgb — the input, passed through unchanged.
//   direct_base_rgb             — that, times the live sun tint and
//                                 sun_multiplier.
//   direct_world_sun_rgb        — the term that LIGHTS the world: scaled by
//                                 direct_world_ratio and renormalized toward
//                                 noon luminance. Stays BLACK whenever the
//                                 ratio is 0 (sun at or below the horizon) or
//                                 either luminance is non-positive.
//   sun_disc_rgb                — the VISIBLE disc, which is exactly
//                                 direct_base_rgb: it keeps the current
//                                 brightness rather than the renormalized
//                                 one, so a low sun dims the scene without
//                                 dimming the sun you can see.
//   sky_*_modifier_rgb          — authored sky chroma times its multiplier
//                                 (and, for irradiance, sky_ambient_ratio);
//                                 forced to black if non-finite.
//   direct_world_ratio          — 0-1, from direct_world_ratio().
//   sky_ambient_ratio           — day/twilight blend, from
//                                 sky_ambient_ratio().
//   resolved_elevation_deg      — the input elevation, or 0 if it was
//                                 non-finite.
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

// Hermite smoothstep, clamped to [0,1]. NOTE it does not guard a == b: the
// division would produce inf/NaN and std::clamp passes NaN straight through.
// Every call site in this file passes distinct literal edges; keep it that
// way, or guard at the call site.
inline float atmosphere_lighting_smoothstep(float a, float b,
                                             float x) noexcept {
    const float q = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return q * q * (3.0f - 2.0f * q);
}

// How much of the direct sun term reaches the world, 0-1, as a piecewise
// smooth function of sun elevation in degrees:
//
//   <= 0 deg   -> 0            (sun at or below the horizon)
//   0..5 deg   -> 0 .. sunset_direct_ratio
//   5..45 deg  -> sunset_direct_ratio .. 1
//   >= 45 deg  -> 1
//
// `sunset_direct_ratio` is clamped to [0,1] first; a non-finite argument
// yields 0 rather than propagating.
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

// How "twilight" the sky is, 1 at and below -6 degrees (civil twilight) down
// to 0 at and above +5 degrees. Only used to blend the two ambient
// multipliers below.
inline float sky_twilight_mix(float elevation_deg) noexcept {
    if (!std::isfinite(elevation_deg)) return 0.0f;
    return 1.0f - atmosphere_lighting_smoothstep(-6.0f, 5.0f,
                                                  elevation_deg);
}

// Linear blend from the day ambient multiplier to the twilight one by
// sky_twilight_mix(). The result is NOT clamped to [0,1] — it is whatever the
// two authored multipliers span, and twilight_ambient_multiplier defaults
// above 1.
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

// The single resolve entry point: pure, allocation-free, safe from any
// thread, and total — every product is finiteness-checked and collapses to
// black rather than propagating a NaN into the renderer's uniforms.
//
// Luminance is the Rec.709 weighting (0.2126/0.7152/0.0722).
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

// Bit flags describing WHAT changed in a lighting edit, OR-ed into a mask and
// handed to atmosphere_history_decision(). A plain unscoped enum because it is
// used as a bit mask.
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

// Which temporally accumulated buffers a lighting edit invalidates. Resetting
// history costs the accumulated samples, so this exists to reset only what a
// given change actually falsifies.
struct AtmosphereHistoryDecision {
    bool reset_diffuse_gi = false;
    bool reset_reflection_miss = false;
    bool reset_volumetric = false;
};

// Map a change mask onto history resets. `full_commit` forces the diffuse GI
// history to reset regardless of the mask.
//
// Read the body before relying on this: as written, `reset_volumetric` is
// never set by any branch, and `kAtmosphereChangeExposure` and
// `kAtmosphereChangeShadow` are not tested at all — a mask containing only
// those bits produces an all-false decision.
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
