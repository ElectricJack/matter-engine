#pragma once
#include "animation/animation_budget.h"

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
// The work ABI is deliberately fixed at 32 bytes.  Palette size is carried in
// the otherwise spare high bits of flags so the shader can reject a corrupt
// influence before indexing either palette stream.  C1's asset contract caps
// skeletons far below this representable maximum.
constexpr uint32_t kVkSkinPaletteCountShift = 16u;
constexpr uint32_t kVkSkinPaletteCountMax = 0xffffu;
constexpr uint32_t kVkSkinWorkFlagsMask = (1u << kVkSkinPaletteCountShift) - 1u;
constexpr uint32_t kVkMaxSkinWorkItems =
    matter::animation::AnimationBudgetConfig::kHardMaxSkinWorkItems;
constexpr uint32_t kVkMaxSkinnedOutputVertices =
    matter::animation::AnimationBudgetConfig::kHardMaxSkinnedVertices;

// Compute input/output ABI. Source vertices deliberately retain every raster
// attribute; output adds only the previous skinned position used for true
// deformation motion vectors. Both structs use vec4-aligned arrays so the
// matching GLSL std430 declarations remain unambiguous.
struct alignas(16) VkSkinSourceVertex {
    float position[4]{};
    float normal[4]{};
    float tint[4]{};
    float surface[4]{};
    uint32_t material_index = 0;
    uint32_t pad[3]{};
};
static_assert(sizeof(VkSkinSourceVertex) == 80, "VkSkinSourceVertex std430 ABI");

struct alignas(16) VkSkinVertex {
    float position[4]{};
    float previous_position[4]{};
    float normal[4]{};
    float tint[4]{};
    float surface[4]{};
    uint32_t material_index = 0;
    uint32_t pad[3]{};
};
static_assert(sizeof(VkSkinVertex) == 96, "VkSkinVertex std430 ABI");

inline float vk_skin_decode_weight(uint16_t weight) noexcept {
    return static_cast<float>(weight) / 65535.0f;
}

// CPU mirror of animation_skin.comp. It is both an executable ABI contract
// and a validation/readback oracle; production dispatch never calls it.
bool vk_skin_vertex_cpu(const VkSkinSourceVertex& source,
                        const VkSkinInfluence& influence,
                        const VkSkinJoint* current_palette,
                        const VkSkinJoint* previous_palette,
                        uint32_t palette_count,
                        VkSkinVertex& output) noexcept;

}  // namespace viewer
