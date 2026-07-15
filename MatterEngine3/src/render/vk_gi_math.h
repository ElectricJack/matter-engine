#pragma once

#include <cstdint>

#include "matter/math_types.h"

namespace viewer {

struct VulkanCosineSample {
    matter::Float3 direction{};
    float pdf = 0.0f;
};

struct VulkanGiUv {
    float x = 0.0f;
    float y = 0.0f;
};

uint32_t vulkan_gi_pcg_hash(uint32_t value) noexcept;
uint32_t vulkan_gi_seed(uint32_t pixel_x, uint32_t pixel_y,
                        uint32_t presented_frame_index,
                        uint32_t bounce) noexcept;
VulkanGiUv vulkan_gi_source_uv(uint32_t raw_x, uint32_t raw_y,
                               uint32_t raw_width, uint32_t raw_height,
                               uint32_t source_width,
                               uint32_t source_height) noexcept;
VulkanCosineSample vulkan_cosine_sample(matter::Float3 normal, float u1,
                                        float u2) noexcept;
matter::Float3 vulkan_schlick_fresnel(matter::Float3 f0,
                                      float view_half_cosine) noexcept;
float vulkan_ggx_reflection_pdf(float normal_half_cosine,
                                float view_half_cosine,
                                float roughness) noexcept;
float vulkan_clearcoat_selection_probability(float clearcoat) noexcept;

}  // namespace viewer
