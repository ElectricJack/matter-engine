#pragma once

// MatterEngine3/src/render/vk_lighting_controls.h
//
// The gate every editor-authored lighting override passes through before the
// renderer sees it. VulkanLightingOverrides (matter/atmosphere_lighting.h) is
// filled by UI sliders, the property system and FIFO `set` commands, any of
// which can hand over a NaN or an out-of-range value; sanitize_* is what
// guarantees the renderer and the GPU uniform block only ever see finite,
// in-range numbers.
//
// Called from MatterEngine3/src/matter_engine.cpp on RenderOptions::
// vulkan_lighting at the top of each frame's lighting build, and exercised
// directly by MatterEngine3/tests/vulkan_smoke_tests.cpp. Free functions with
// no state — safe to call from anywhere, cheap enough to call per frame.
//
// A note on why this is separate from the renderer: the bounds are shared with
// the shaders (sun_shadow_samples is clamped to the same 16 rt_shadow.rgen
// clamps to) and with matter/sun_angles.h, so keeping them in one small
// translation unit is what lets a test assert them without a device.

#include "matter/atmosphere_lighting.h"

namespace viewer {
// Returns a copy with every field made finite and clamped into its documented
// range. Non-finite input falls back to a per-field default that is NOT
// necessarily the struct's own default (sun_multiplier falls back to 1.0, for
// instance) — the fallback is chosen to be visually neutral, not to restore
// the shipped look. Azimuth wraps instead of clamping; everything else clamps.
// Idempotent, so calling it twice is harmless.
matter::VulkanLightingOverrides sanitize_vulkan_lighting_overrides(
    const matter::VulkanLightingOverrides& value) noexcept;
// Linear exposure multiplier for an EV value: 2^ev, after the same clamping
// sanitize applies (so an absurd EV cannot produce an infinite scale).
float vulkan_exposure_scale(float exposure_ev) noexcept;
// True when a and b differ in a way that changes the LIGHT ITSELF rather than
// how it is displayed — multipliers, tints, ambient terms, sun aim, sun size,
// shadow sample count. Exposure and the debug-view selector are deliberately
// excluded: they are display-side, and a source change is what obliges the
// caller to drop accumulated GI history. Both sides are sanitized first, so
// two inputs that differ only outside the valid range compare equal.
bool vulkan_source_lighting_changed(
    const matter::VulkanLightingOverrides& a,
    const matter::VulkanLightingOverrides& b) noexcept;
}
