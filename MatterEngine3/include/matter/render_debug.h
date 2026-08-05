#pragma once

#include <cstdint>

namespace matter {

// Geometry-stage debug visualizations. Unlike the composite debug views
// (VulkanLightingOverrides::composite_debug_view) these are applied while the
// G-buffer is being written, because the thing being visualized -- the rung
// cull.comp actually selected for each draw -- only exists there.
//
// M2.5 (object impostors) adds a second member here for the terminal-impostor
// tint. It is deliberately absent until there is geometry to draw with it.
enum class GeometryDebugView : uint8_t { None, LodTint };

struct DebugRgb {
    float r;
    float g;
    float b;

    constexpr bool operator==(const DebugRgb& other) const {
        return r == other.r && g == other.g && b == other.b;
    }
    constexpr bool operator!=(const DebugRgb& other) const {
        return !(*this == other);
    }
};

inline constexpr uint32_t kLodDebugColorCount = 16;
inline constexpr float kLodDebugHueStep = 0.61803398875f;
inline constexpr float kLodDebugSaturation = 0.85f;
inline constexpr float kLodDebugValue = 1.0f;

constexpr float debug_abs(float value) { return value < 0.0f ? -value : value; }
constexpr float debug_hue_component(float hue, float offset) {
    float wrapped = hue * 6.0f + offset;
    if (wrapped >= 6.0f) wrapped -= 6.0f;
    const float component = debug_abs(wrapped - 3.0f) - 1.0f;
    return component < 0.0f ? 0.0f : (component > 1.0f ? 1.0f : component);
}

constexpr DebugRgb debug_hsv_to_rgb(float hue, float saturation, float value) {
    const float chroma = value * saturation;
    const float m = value - chroma;
    return {m + chroma * debug_hue_component(hue, 0.0f),
            m + chroma * debug_hue_component(hue, 4.0f),
            m + chroma * debug_hue_component(hue, 2.0f)};
}

// Deliberately constexpr and free of platform math calls so the host UI legend
// and gbuffer.frag's copy of this rotation stay a stable, inspectable mapping:
// the swatch the editor draws for rung N is the colour the fragment shader
// computes for rung N, by construction rather than by convention.
constexpr DebugRgb lod_debug_color(uint32_t lod) {
    const float unwrapped_hue =
        static_cast<float>(lod % kLodDebugColorCount) * kLodDebugHueStep;
    const float hue = unwrapped_hue - static_cast<float>(
                                          static_cast<uint32_t>(unwrapped_hue));
    return debug_hsv_to_rgb(hue, kLodDebugSaturation, kLodDebugValue);
}

}  // namespace matter
