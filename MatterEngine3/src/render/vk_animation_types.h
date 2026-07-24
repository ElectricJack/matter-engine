#pragma once

// These POD types are shared verbatim with shaders_vk/animation_skin.comp in
// C2. Keep this header free of Vulkan and engine implementation types: it is
// also compiled by the CPU-only ABI test target.

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace viewer {

struct alignas(16) VkSkinMatrix {
    // Column-major GLSL mat4 bytes. Conversion from Matter's row-major Mat4f
    // belongs at the renderer boundary, never in shader-visible storage.
    float elements[16]{};
};
static_assert(sizeof(VkSkinMatrix) == 64, "VkSkinMatrix must be one GLSL mat4");
static_assert(alignof(VkSkinMatrix) == 16, "VkSkinMatrix must preserve vec4 alignment");
static_assert(offsetof(VkSkinMatrix, elements) == 0, "VkSkinMatrix elements ABI");
static_assert(std::is_standard_layout<VkSkinMatrix>::value,
              "VkSkinMatrix must remain shader-copyable");

struct VkSkinInfluence {
    uint16_t joint[4]{};
    uint16_t weight[4]{};
};
static_assert(sizeof(VkSkinInfluence) == 16, "VkSkinInfluence shader ABI");
static_assert(alignof(VkSkinInfluence) == alignof(uint16_t),
              "VkSkinInfluence scalar alignment ABI");
static_assert(offsetof(VkSkinInfluence, joint) == 0, "VkSkinInfluence joints ABI");
static_assert(offsetof(VkSkinInfluence, weight) == 8, "VkSkinInfluence weights ABI");
static_assert(std::is_standard_layout<VkSkinInfluence>::value,
              "VkSkinInfluence must remain shader-copyable");

struct VkSkinJoint {
    VkSkinMatrix position;
    // Inverse-transpose of the position skin matrix. It is deliberately
    // uploaded rather than derived in the compute shader, avoiding malformed
    // normals when authored joints contain non-uniform scale.
    VkSkinMatrix normal;
};
static_assert(sizeof(VkSkinJoint) == 128, "VkSkinJoint shader ABI");
static_assert(alignof(VkSkinJoint) == 16, "VkSkinJoint std430 alignment ABI");
static_assert(offsetof(VkSkinJoint, position) == 0, "VkSkinJoint position ABI");
static_assert(offsetof(VkSkinJoint, normal) == 64, "VkSkinJoint normal ABI");
static_assert(std::is_standard_layout<VkSkinJoint>::value,
              "VkSkinJoint must remain shader-copyable");

struct VkSkinWorkItem {
    uint32_t source_vertex = 0;
    uint32_t influence = 0;
    uint32_t vertex_count = 0;
    uint32_t palette = 0;
    uint32_t output_current = 0;
    uint32_t output_previous = 0;
    uint32_t instance_slot = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(VkSkinWorkItem) == 32, "VkSkinWorkItem shader ABI");
static_assert(alignof(VkSkinWorkItem) == alignof(uint32_t),
              "VkSkinWorkItem scalar alignment ABI");
static_assert(offsetof(VkSkinWorkItem, source_vertex) == 0,
              "VkSkinWorkItem source vertex ABI");
static_assert(offsetof(VkSkinWorkItem, influence) == 4,
              "VkSkinWorkItem influence ABI");
static_assert(offsetof(VkSkinWorkItem, vertex_count) == 8,
              "VkSkinWorkItem vertex count ABI");
static_assert(offsetof(VkSkinWorkItem, palette) == 12,
              "VkSkinWorkItem palette ABI");
static_assert(offsetof(VkSkinWorkItem, output_current) == 16,
              "VkSkinWorkItem current output ABI");
static_assert(offsetof(VkSkinWorkItem, output_previous) == 20,
              "VkSkinWorkItem previous output ABI");
static_assert(offsetof(VkSkinWorkItem, instance_slot) == 24,
              "VkSkinWorkItem instance slot ABI");
static_assert(offsetof(VkSkinWorkItem, flags) == 28,
              "VkSkinWorkItem flags ABI");
static_assert(std::is_standard_layout<VkSkinWorkItem>::value,
              "VkSkinWorkItem must remain shader-copyable");

constexpr uint32_t kVkSkinHistoryInvalid = 1u << 0;
constexpr uint32_t kVkMaxSkinWorkItems = 256;
constexpr uint32_t kVkMaxSkinnedOutputVertices = 2000000;

inline float vk_skin_decode_weight(uint16_t weight) noexcept {
    return static_cast<float>(weight) / 65535.0f;
}

}  // namespace viewer
