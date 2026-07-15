#pragma once

#include <cstdint>

#include "matter/math_types.h"

namespace viewer {

struct VulkanCosineSample {
    matter::Float3 direction{};
    float pdf = 0.0f;
};

uint32_t vulkan_gi_pcg_hash(uint32_t value) noexcept;
VulkanCosineSample vulkan_cosine_sample(matter::Float3 normal, float u1,
                                        float u2) noexcept;

}  // namespace viewer
