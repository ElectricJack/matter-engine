#pragma once

// MatterEngine3/include/matter/render_debug.h
//
// Geometry-stage debug view selection, and the LOD debug colour palette that the
// host UI and the G-buffer fragment shader must agree on.
//
// The palette exists in two places by necessity — here for the editor's legend
// swatches, and again in `MatterEngine3/shaders_vk/gbuffer.frag` for the pixels.
// Everything below is `constexpr` and free of platform math calls (no `fmodf`,
// no `fabsf`, no table lookups) specifically so those two copies are the same
// arithmetic rather than two implementations that happen to look alike. If you
// change a constant or a helper here, change the shader's copy in the same
// commit — and remember that a shader edit only reaches the binary through the
// SPIR-V embedding step (see CLAUDE.md, "Shaders").
//
// Nothing here allocates, holds state, or touches the GPU; it is a pure
// header-only value mapping usable from any thread.

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

// A plain linear RGB triple in 0..1, used only for debug colours. Deliberately
// its own type rather than a MathLib vector so this header stays dependency-free
// and constexpr-friendly. The comparison operators are exact float equality,
// which is what a "did the debug colour change" check wants.
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

// Palette parameters. `lod` is taken modulo kLodDebugColorCount, so rung 16
// reuses rung 0's colour. The hue step is the golden-ratio conjugate: stepping
// hue by it and wrapping gives successive rungs maximally separated hues instead
// of the near-identical neighbours an even 1/16 split would produce.
inline constexpr uint32_t kLodDebugColorCount = 16;
inline constexpr float kLodDebugHueStep = 0.61803398875f;
inline constexpr float kLodDebugSaturation = 0.85f;
inline constexpr float kLodDebugValue = 1.0f;

// Internal helpers. Hand-rolled because <cmath> is neither constexpr nor
// guaranteed bit-identical to what GLSL computes; these three are the exact
// operations the shader-side copy performs. `hue` is 0..1 and is expected to be
// already wrapped.
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
