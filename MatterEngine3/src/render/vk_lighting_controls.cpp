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
           x.emission_multiplier != y.emission_multiplier;
}
}
