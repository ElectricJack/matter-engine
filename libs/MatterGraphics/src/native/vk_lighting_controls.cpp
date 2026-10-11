// MatterEngine3/src/render/vk_lighting_controls.cpp
//
// Implementation of the lighting-override gate declared in
// vk_lighting_controls.h. Two properties the inline comments below defend and
// that any edit here has to preserve:
//
//   1. IDEMPOTENCE AND BIT-IDENTITY. An already-valid value must come back
//      unchanged, bit for bit — not merely equal-ish. The engine decides
//      "has anyone moved the sun?" with a float comparison against the
//      authored value, so a sanitize pass that renormalized an in-range angle
//      would make an untouched Lighting panel look like an edit.
//   2. SHARED BOUNDS. The limits are not local taste: sun_shadow_samples
//      matches the clamp in shaders_vk/rt_shadow.rgen, and the sun angular
//      diameter bounds come from matter/sun_angles.h so the world-authored
//      path (which never calls this) clamps to the same numbers.

#include "vk_lighting_controls.h"
#include <algorithm>
#include <cmath>

namespace viewer {
namespace {
float finite_or(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}
}

// Field-by-field clamp + NaN replacement. EVERY non-finite fallback is the
// field's own struct default, read off a default-constructed `d` rather than
// spelled out again -- which is what the panel means by "invalid override uses
// default". sun_multiplier and sky_multiplier used to be the exception,
// hard-coded to 1.0 while the struct ships 1.67 and 0.77, so one NaN silently
// re-lit the scene at a brightness nothing had authored. `out` starts
// default-constructed, so any field this function forgets to assign silently
// keeps the struct default instead of the caller's value.
matter::VulkanLightingOverrides sanitize_vulkan_lighting_overrides(
    const matter::VulkanLightingOverrides& value) noexcept {
    const matter::VulkanLightingOverrides d{};
    matter::VulkanLightingOverrides out{};
    out.sun_multiplier =
        std::clamp(finite_or(value.sun_multiplier, d.sun_multiplier), 0.0f, 4.0f);
    out.sky_multiplier =
        std::clamp(finite_or(value.sky_multiplier, d.sky_multiplier), 0.0f, 4.0f);
    out.emission_multiplier = std::clamp(
        finite_or(value.emission_multiplier, d.emission_multiplier), 0.0f, 4.0f);
    out.exposure_ev =
        std::clamp(finite_or(value.exposure_ev, d.exposure_ev), -6.0f, 6.0f);
    out.composite_debug_view = std::clamp(
        finite_or(value.composite_debug_view, d.composite_debug_view), 0.0f, 10.0f);
    // Tints share the multipliers' range and NaN policy, per channel. The
    // default is 1.0 (white), so a corrupt channel drops the tint out of the
    // way instead of blacking the light out.
    for (int i = 0; i < 3; ++i) {
        out.sun_tint[i] =
            std::clamp(finite_or(value.sun_tint[i], d.sun_tint[i]), 0.0f, 4.0f);
        out.sky_tint[i] =
            std::clamp(finite_or(value.sky_tint[i], d.sky_tint[i]), 0.0f, 4.0f);
    }
    out.day_ambient_multiplier = std::clamp(
        finite_or(value.day_ambient_multiplier, d.day_ambient_multiplier),
        0.0f, 4.0f);
    out.twilight_ambient_multiplier = std::clamp(
        finite_or(value.twilight_ambient_multiplier,
                  d.twilight_ambient_multiplier), 0.0f, 4.0f);
    out.sky_irradiance_multiplier = std::clamp(
        finite_or(value.sky_irradiance_multiplier, d.sky_irradiance_multiplier),
        0.0f, 4.0f);
    out.sunset_direct_ratio = std::clamp(
        finite_or(value.sunset_direct_ratio, d.sunset_direct_ratio), 0.0f, 1.0f);
    // Sun orientation. Azimuth WRAPS rather than clamps -- it is a bearing, and
    // a slider that sticks at 180 while the sun is one degree past the seam is
    // the wrong behaviour. Elevation clamps, because the poles are real ends.
    // The fallbacks are the compiled defaults, i.e. the angles of the engine's
    // default light vector.
    {
        float azimuth = finite_or(value.sun_azimuth_deg, d.sun_azimuth_deg);
        // Only touch it if it is actually outside the range. An in-range value
        // -- which is everything sun_angles_from_direction ever produces,
        // including exactly +/-180 at the seam -- must come back BIT-IDENTICAL,
        // or the engine's "has anyone moved the sun?" float comparison would
        // fire on a sun nobody moved.
        if (!(azimuth >= -180.0f && azimuth <= 180.0f)) {
            azimuth = std::fmod(azimuth + 180.0f, 360.0f);
            if (azimuth < 0.0f) azimuth += 360.0f;
            azimuth -= 180.0f;
        }
        out.sun_azimuth_deg = azimuth;
        out.sun_elevation_deg =
            std::clamp(finite_or(value.sun_elevation_deg, d.sun_elevation_deg),
                       -90.0f, 90.0f);
        // Bounds live in sun_angles.h so the world-authored path (which never
        // passes through here) is clamped by the same numbers.
        out.sun_angular_diameter_deg = std::clamp(
            finite_or(value.sun_angular_diameter_deg, d.sun_angular_diameter_deg),
            matter::kSunAngularDiameterMinDeg, matter::kSunAngularDiameterMaxDeg);
        // rt_shadow.rgen clamps to 16 too; matching it here keeps the value the
        // panel shows equal to the value the GPU used.
        out.sun_shadow_samples = std::clamp(value.sun_shadow_samples, 1, 16);
    }
    return out;
}

// Clamps through the full sanitizer, then 2^ev.
//
// The field is assigned BY NAME. This used to aggregate-initialize positionally
// as `{1.0f, 1.0f, 1.0f, exposure_ev}`, relying on exposure_ev being the fourth
// declared member of VulkanLightingOverrides -- inserting a field above it
// would have routed the EV into emission_multiplier with no compile error. The
// result is unchanged either way: only exposure_ev is read back, and the
// sanitizer clamps each field independently of the others.
float vulkan_exposure_scale(float exposure_ev) noexcept {
    matter::VulkanLightingOverrides request{};
    request.exposure_ev = exposure_ev;
    const auto clean = sanitize_vulkan_lighting_overrides(request);
    return std::exp2(clean.exposure_ev);
}

bool vulkan_source_lighting_changed(
    const matter::VulkanLightingOverrides& a,
    const matter::VulkanLightingOverrides& b) noexcept {
    const auto x = sanitize_vulkan_lighting_overrides(a);
    const auto y = sanitize_vulkan_lighting_overrides(b);
    for (int i = 0; i < 3; ++i) {
        // A tint scales the same sun/sky colour the multipliers do, so it is a
        // SOURCE change (GI history must be dropped), not a display one.
        if (x.sun_tint[i] != y.sun_tint[i]) return true;
        if (x.sky_tint[i] != y.sky_tint[i]) return true;
    }
    return x.sun_multiplier != y.sun_multiplier ||
           x.sky_multiplier != y.sky_multiplier ||
           x.emission_multiplier != y.emission_multiplier ||
           x.day_ambient_multiplier != y.day_ambient_multiplier ||
           x.twilight_ambient_multiplier !=
               y.twilight_ambient_multiplier ||
           x.sky_irradiance_multiplier != y.sky_irradiance_multiplier ||
           x.sunset_direct_ratio != y.sunset_direct_ratio ||
           // Aiming or resizing the sun moves every shadow, every bounce and
           // the disc itself: as SOURCE a change as a colour change, and the
           // one that would look worst if stale GI history survived it.
           // (VkSceneRenderer::set_lighting independently resets GI history on
           // the direction it actually receives; this is the viewer-side half,
           // which sees the edit one step earlier -- while it is still an
           // angle.)
           x.sun_azimuth_deg != y.sun_azimuth_deg ||
           x.sun_elevation_deg != y.sun_elevation_deg ||
           x.sun_angular_diameter_deg != y.sun_angular_diameter_deg ||
           x.sun_shadow_samples != y.sun_shadow_samples;
}
}
