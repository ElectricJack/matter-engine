#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "matter/math_types.h"

namespace matter {

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

constexpr float display_dither_code_offset(uint32_t pixel_x,
                                           uint32_t pixel_y) noexcept {
    const uint8_t rank = display_dither_rank(pixel_x, pixel_y);
    return ((static_cast<float>(rank) - 31.5f) / 31.5f) * (0.5f / 255.0f);
}

inline Float3 apply_display_dither_code(Float3 encoded_code,
                                        uint32_t pixel_x,
                                        uint32_t pixel_y) noexcept {
    const float d = display_dither_code_offset(pixel_x, pixel_y);
    return {std::clamp(encoded_code.x + d, 0.0f, 1.0f),
            std::clamp(encoded_code.y + d, 0.0f, 1.0f),
            std::clamp(encoded_code.z + d, 0.0f, 1.0f)};
}

}  // namespace matter
