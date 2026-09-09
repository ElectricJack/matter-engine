#pragma once

// MatterEngine3/include/matter/display_dither.h
//
// The 8x8 ordered dither used at the very end of the display transform, to
// break up the banding that 8-bit quantisation leaves in smooth gradients
// (sky, fog, distant terrain).
//
// This is a MIRROR, not the render path. The renderer's copy is the `ranks`
// array hard-coded in MatterEngine3/shaders_vk/display_transform.frag, and the
// two must stay byte-identical: the shader does the same
// (rank - 31.5) / 31.5 * (0.5/255) offset on the same pixel indexing. The
// version here exists so tests and CPU-side tone-map checks can predict the
// exact pixel the GPU will write.
//
// Guarantees the table is expected to keep, all asserted by
// `test_periodic_sky_uv_and_display_dither_oracles` in
// MatterEngine3/tests/atmosphere_tests.cpp:
//   * it is a permutation of 0..63 (every rank appears exactly once);
//   * one complete 8x8 tile has zero mean, so dithering does not shift the
//     average image brightness;
//   * the extremes are exactly +/- half an 8-bit code step;
//   * `display_dither_fnv1a32()` hashes to 0xdc0d948b. Editing any byte of
//     the table trips that oracle on purpose — if you really mean to change
//     it, update the shader and the oracle in the same commit.
//
// THE ORACLE ONLY GUARDS THIS SIDE. It hashes kDisplayDitherRanks, not the
// GLSL, and nothing anywhere hashes or cross-checks the shader's literal. So:
//   * editing THIS table alone -> atmosphere_tests.cpp goes red immediately;
//   * editing the SHADER's `ranks[64]` alone -> every test stays green and the
//     two silently disagree, and the symptom (a shifted dither pattern in
//     8-bit gradients) is invisible in anything but a pixel-exact diff.
// Any change to the pattern is therefore a THREE-part edit in one commit: this
// table, the `ranks[64]` initializer in
// MatterEngine3/shaders_vk/display_transform.frag, and the 0xdc0d948b oracle
// in MatterEngine3/tests/atmosphere_tests.cpp. Remember that a shader edit
// only reaches the binary through the SPIR-V embed step (see CLAUDE.md).
//
// Units and space: offsets are in DISPLAY CODE units normalized to [0,1]
// (0.5/255 is half a code step at 8 bits). They apply to an already
// tone-mapped, sRGB-ENCODED colour — never to linear HDR radiance, where a
// fixed offset would be perceptually wrong.
//
// Everything here is constexpr/inline and stateless.

#include <algorithm>
#include <array>
#include <cstdint>

#include "matter/math_types.h"

namespace matter {

// Row-major 8x8 rank table, indexed as (y & 7) * 8 + (x & 7). These 64 values
// appear again, in the same order, as the `const int ranks[64]` initializer
// inside main() in MatterEngine3/shaders_vk/display_transform.frag, indexed
// there as `ranks[(pixel.y & 7) * 8 + (pixel.x & 7)]` with
// `pixel = ivec2(floor(gl_FragCoord.xy))`. Verified identical 2026-08-19.
// Nothing checks that at build time — see the header comment above.
inline constexpr std::array<uint8_t, 64> kDisplayDitherRanks{{
    37, 12, 54, 1, 46, 27, 61, 8,
    18, 43, 5, 58, 31, 50, 14, 40,
    63, 22, 35, 10, 48, 3, 56, 29,
    16, 45, 7, 60, 25, 52, 11, 38,
    33, 0, 47, 20, 57, 15, 42, 30,
    9, 53, 24, 62, 4, 36, 19, 51,
    41, 13, 55, 28, 59, 6, 44, 21,
    26, 49, 2, 39, 17, 34, 23, 32,
}};

// FNV-1a over the 64 table bytes. Not used at render time — it exists purely
// so a test can pin the exact table contents with one number.
constexpr uint32_t display_dither_fnv1a32() noexcept {
    uint32_t hash = 2166136261u;
    for (uint8_t rank : kDisplayDitherRanks) {
        hash ^= rank;
        hash *= 16777619u;
    }
    return hash;
}

constexpr uint8_t display_dither_rank(uint32_t pixel_x,
                                      uint32_t pixel_y) noexcept {
    return kDisplayDitherRanks[(pixel_y & 7u) * 8u + (pixel_x & 7u)];
}

// Signed offset for this pixel, in display code units normalized to [0,1]:
// exactly -0.5/255 at rank 0 and +0.5/255 at rank 63, zero mean over a tile.
// The tile repeats every 8 pixels, so any pixel coordinate is acceptable.
constexpr float display_dither_code_offset(uint32_t pixel_x,
                                           uint32_t pixel_y) noexcept {
    const uint8_t rank = display_dither_rank(pixel_x, pixel_y);
    return ((static_cast<float>(rank) - 31.5f) / 31.5f) * (0.5f / 255.0f);
}

// Adds this pixel's offset to an already sRGB-encoded colour and clamps back
// into [0,1]. `encoded_code` is display code, NOT linear radiance. Because of
// the clamp, values at 0 or 1 receive a one-sided offset — that matches
// display_transform.frag, which clamps the same way.
inline Float3 apply_display_dither_code(Float3 encoded_code,
                                        uint32_t pixel_x,
                                        uint32_t pixel_y) noexcept {
    const float d = display_dither_code_offset(pixel_x, pixel_y);
    return {std::clamp(encoded_code.x + d, 0.0f, 1.0f),
            std::clamp(encoded_code.y + d, 0.0f, 1.0f),
            std::clamp(encoded_code.z + d, 0.0f, 1.0f)};
}

}  // namespace matter
