#include "vk_lighting_controls.h"
#include <algorithm>
#include <cmath>

namespace viewer {
namespace {
float finite_or(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}
}

matter::VulkanLightingOverrides sanitize_vulkan_lighting_overrides(
    const matter::VulkanLightingOverrides& value) noexcept {
    matter::VulkanLightingOverrides out{};
    out.sun_multiplier = std::clamp(finite_or(value.sun_multiplier, 1.0f), 0.0f, 4.0f);
    out.sky_multiplier = std::clamp(finite_or(value.sky_multiplier, 1.0f), 0.0f, 4.0f);
    out.emission_multiplier =
        std::clamp(finite_or(value.emission_multiplier, 1.0f), 0.0f, 4.0f);
    out.exposure_ev = std::clamp(finite_or(value.exposure_ev, -2.0f), -6.0f, 6.0f);
    out.composite_debug_view = std::clamp(finite_or(value.composite_debug_view, 0.0f), 0.0f, 10.0f);
    // Tints share the multipliers' range and NaN policy, per channel. The
    // fallback is 1.0 (white) rather than 0, so a corrupt channel drops the
    // tint out of the way instead of blacking the light out.
    for (int i = 0; i < 3; ++i) {
        out.sun_tint[i] = std::clamp(finite_or(value.sun_tint[i], 1.0f), 0.0f, 4.0f);
        out.sky_tint[i] = std::clamp(finite_or(value.sky_tint[i], 1.0f), 0.0f, 4.0f);
    }
    // Sun orientation. Azimuth WRAPS rather than clamps -- it is a bearing, and
    // a slider that sticks at 180 while the sun is one degree past the seam is
    // the wrong behaviour. Elevation clamps, because the poles are real ends.
    // The fallbacks are the compiled defaults, i.e. the angles of the engine's
    // default light vector.
    {
        const matter::VulkanLightingOverrides d{};
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

float vulkan_exposure_scale(float exposure_ev) noexcept {
    const auto clean = sanitize_vulkan_lighting_overrides(
        {1.0f, 1.0f, 1.0f, exposure_ev});
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
