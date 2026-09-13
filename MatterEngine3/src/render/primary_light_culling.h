#pragma once

// Portable reference for the primary-receiver tile mask. Keep the float
// rejection tests in sync with shaders_vk/primary_light_cull.comp.
#include "../world_lights.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace world_lights {

constexpr std::uint32_t kPrimaryLightTileSize = 16u;
constexpr std::uint64_t kPrimaryLightMetadataBytes = 48u;
constexpr std::uint64_t kPrimaryLightStorageBudgetBytes = 64u * 1024u * 1024u;

struct PrimaryLightMaskLayout {
    std::uint32_t tiles_x = 0, tiles_y = 0, words_per_tile = 0;
    std::uint64_t byte_size = kPrimaryLightMetadataBytes;
    bool enabled = false;
};

// Disabled means use the spatial-index fallback, never a truncated light set.
// After the metadata come all tile masks, then fixed-stride compact lists:
// one count plus light_count IDs per tile. Both arrays consume the budget.
// The shader uses 32-bit word addressing, independently of the device limits.
inline PrimaryLightMaskLayout primary_light_mask_layout(
    std::uint32_t width, std::uint32_t height, std::uint32_t light_count,
    std::uint64_t max_storage_bytes, std::uint64_t max_buffer_bytes) noexcept {
    PrimaryLightMaskLayout result;
    if (!width || !height || !light_count) return result;
    const std::uint64_t tx = (std::uint64_t(width) + 15u) / 16u;
    const std::uint64_t ty = (std::uint64_t(height) + 15u) / 16u;
    const std::uint64_t words = (std::uint64_t(light_count) + 31u) / 32u;
    const std::uint64_t tile_words = words + std::uint64_t(light_count) + 1u;
    const std::uint64_t limit = std::min(kPrimaryLightStorageBudgetBytes,
        std::min(max_storage_bytes, max_buffer_bytes));
    if (limit < kPrimaryLightMetadataBytes) return result;
    const std::uint64_t word_budget = std::min(
        (limit - kPrimaryLightMetadataBytes) / 4u,
        std::uint64_t(std::numeric_limits<std::uint32_t>::max()) - 12u);
    if (tx > word_budget / ty || tx * ty > word_budget / tile_words)
        return result;
    result.tiles_x = static_cast<std::uint32_t>(tx);
    result.tiles_y = static_cast<std::uint32_t>(ty);
    result.words_per_tile = static_cast<std::uint32_t>(words);
    result.byte_size += tx * ty * tile_words * 4u;
    result.enabled = true;
    return result;
}

struct PrimaryLightAabb { float min[3], max[3]; };

inline bool primary_light_bounds_may_intersect(
    const LocalLight& light, const PrimaryLightAabb& receivers) noexcept {
    float scale = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(receivers.min[i]) ||
            !std::isfinite(receivers.max[i]) ||
            receivers.min[i] > receivers.max[i] ||
            !std::isfinite(light.position[i])) return true;
        scale = std::max(scale, std::max(std::fabs(receivers.min[i]),
                                         std::fabs(receivers.max[i])));
    }
    if (!std::isfinite(light.range) || light.range <= 0.0f) return true;
    const float padding = std::max(1.0e-4f, scale * 2.0e-5f);
    float lo[3], hi[3], distance2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        lo[i] = receivers.min[i] - padding;
        hi[i] = receivers.max[i] + padding;
        if (!std::isfinite(lo[i]) || !std::isfinite(hi[i])) return true;
        const float closest = std::max(lo[i], std::min(hi[i], light.position[i]));
        const float d = (closest - light.position[i]) / light.range;
        distance2 += d * d;
    }
    if (!std::isfinite(distance2)) return true;
    // Source radius affects the BRDF denominator, never its finite cutoff.
    if (distance2 > 1.0f + 4.0e-5f) return false;
    if (light.kind != static_cast<std::uint32_t>(LocalLightKind::Spot)) return true;
    if (!std::isfinite(light.cos_outer) || !std::isfinite(light.cos_inner))
        return true;
    float axis2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(light.direction[i])) return true;
        axis2 += light.direction[i] * light.direction[i];
    }
    if (!std::isfinite(axis2) || axis2 <= 0.0f) return true;
    const float axis_length = std::sqrt(axis2);
    float axis[3];
    for (int i = 0; i < 3; ++i) axis[i] = light.direction[i] / axis_length;
    // The BRDF compares its original axis dot product. Adjust its threshold
    // when normalizing so even a slightly nonunit published axis stays safe.
    float c = std::min(light.cos_outer, light.cos_inner) / axis_length;
    if (!std::isfinite(c) || c <= -1.0f) return true;
    c = std::min(c, 1.0f);
    float v[3], radius2 = 0.0f, q = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float center = lo[i] * 0.5f + hi[i] * 0.5f;
        const float extent = hi[i] * 0.5f - lo[i] * 0.5f;
        v[i] = center - light.position[i];
        radius2 += extent * extent;
        q += axis[i] * v[i];
    }
    float rho2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float perpendicular = v[i] - axis[i] * q;
        rho2 += perpendicular * perpendicular;
    }
    const float radius = std::sqrt(radius2);
    const float rho = std::sqrt(rho2);
    const float s = std::sqrt(std::max(0.0f, 1.0f - c * c));
    const float separation = c * rho - s * q;
    const float epsilon = std::max(1.0e-4f,
        std::max(radius, std::max(std::fabs(v[0]),
            std::max(std::fabs(v[1]), std::fabs(v[2])))) * 2.0e-5f);
    if (!std::isfinite(radius) || !std::isfinite(rho) ||
        !std::isfinite(separation) || !std::isfinite(epsilon)) return true;
    return separation <= radius + epsilon;
}

} // namespace world_lights
