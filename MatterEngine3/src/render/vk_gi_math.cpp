#include "vk_gi_math.h"

#include <algorithm>
#include <cmath>

namespace viewer {

uint32_t vulkan_gi_pcg_hash(uint32_t value) noexcept {
    const uint32_t state = value * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) *
                          277803737u;
    return (word >> 22u) ^ word;
}

uint32_t vulkan_gi_seed(uint32_t pixel_x, uint32_t pixel_y,
                        uint32_t presented_frame_index,
                        uint32_t bounce) noexcept {
    uint32_t seed = pixel_x ^ (pixel_y * 0x9e3779b9u) ^
                    (presented_frame_index * 0x85ebca6bu) ^
                    (bounce * 0xc2b2ae35u);
    return vulkan_gi_pcg_hash(seed);
}

VulkanCosineSample vulkan_cosine_sample(matter::Float3 normal, float u1,
                                        float u2) noexcept {
    const auto normalize = [](matter::Float3 v) {
        const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return length > 1e-20f
                   ? matter::Float3{v.x / length, v.y / length, v.z / length}
                   : matter::Float3{0.0f, 1.0f, 0.0f};
    };
    const matter::Float3 n = normalize(normal);
    const matter::Float3 helper = std::fabs(n.z) < 0.999f
                                      ? matter::Float3{0.0f, 0.0f, 1.0f}
                                      : matter::Float3{1.0f, 0.0f, 0.0f};
    const matter::Float3 tangent = normalize(
        {helper.y * n.z - helper.z * n.y,
         helper.z * n.x - helper.x * n.z,
         helper.x * n.y - helper.y * n.x});
    const matter::Float3 bitangent{
        n.y * tangent.z - n.z * tangent.y,
        n.z * tangent.x - n.x * tangent.z,
        n.x * tangent.y - n.y * tangent.x};
    const float r = std::sqrt(std::clamp(u1, 0.0f, 1.0f));
    constexpr float tau = 6.28318530717958647692f;
    const float phi = tau * u2;
    const float x = r * std::cos(phi);
    const float y = r * std::sin(phi);
    const float z = std::sqrt(std::max(0.0f, 1.0f - r * r));
    const matter::Float3 direction = normalize(
        {tangent.x * x + bitangent.x * y + n.x * z,
         tangent.y * x + bitangent.y * y + n.y * z,
         tangent.z * x + bitangent.z * y + n.z * z});
    const float cosine = std::max(0.0f, direction.x * n.x +
                                            direction.y * n.y +
                                            direction.z * n.z);
    return {direction, cosine * 0.31830988618379067154f};
}

}  // namespace viewer
