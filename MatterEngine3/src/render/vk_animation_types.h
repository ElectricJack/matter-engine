#pragma once
// MatterEngine3/src/render/vk_animation_types.h
//
// The shader-visible POD ABI for GPU skinning. This header is the single
// contract shared by the CPU queue builder (`vk_animation_skinning.h`), the
// animated-bounds/cull path (`vk_animation_bounds.h`) and the compute shader
// `MatterEngine3/shaders_vk/animation_skin.comp`. Every struct here carries
// `static_assert`s pinning its size, alignment and member offsets; if one of
// them fires, the GLSL declaration and this header have drifted apart because
// one of the two was edited alone.
//
// Deliberately free of Vulkan and engine implementation types -- the same
// header is compiled by a CPU-only ABI test target, so it must stay
// includable without a Vulkan SDK or any renderer header.
//
// Conventions that hold throughout:
//  - Matrices are stored column-major, in GLSL `mat4` byte order. Matter's
//    own `Mat4f` is row-major, so the transpose belongs at the renderer
//    boundary and never in shader-visible storage.
//  - Skinning happens before the instance transform: everything here is in
//    the mesh's own object space.
//  - "Palette" means the flattened per-frame joint array. A work item's
//    `palette` field is an offset into that array, not a per-asset index.
//  - All offsets in these structs are element indices, never byte offsets.
//  - Bone weights are uint16 fixed point; decode with
//    `vk_skin_decode_weight` (weight / 65535). A lane of weight 0 is an
//    unused influence slot, not a zero-weight joint.
//
// `vk_skin_vertex_cpu` at the bottom is the CPU mirror of the shader: the
// executable form of this contract and a readback oracle for tests. It is
// never on the production dispatch path.
#include "animation/animation_budget.h"

// These POD types are shared verbatim with shaders_vk/animation_skin.comp in
// C2. Keep this header free of Vulkan and engine implementation types: it is
// also compiled by the CPU-only ABI test target.

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace viewer {

// One 4x4 skinning matrix in exactly the byte layout a GLSL `mat4` expects.
// It is a struct rather than a bare array so the size/alignment assertions
// below travel with it into every array and every struct that stores it.
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

// Up to four bone influences for one source vertex. These live in the packed
// per-renderer influence arena (`VkAnimationSkinning::influences()`), one
// entry per source vertex, addressed as `VkSkinWorkItem::influence + i`.
//
// `joint` is an index into THIS work item's palette slice (0 ..
// palette_count-1), not a global joint id; an out-of-range joint on a
// non-zero-weight lane rejects the whole submission. `weight` is fixed point
// where 65535 == 1.0; a zero weight marks an unused lane and its `joint` is
// ignored. The four weights are not required to sum to 1 -- the shader and
// its CPU mirror normalize the skinned normal but not the skinned position,
// so unnormalized weights scale the vertex.
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

// One palette entry: both matrices needed to skin a position and a normal
// with the same joint. Palettes are uploaded as flat arrays of these, one
// contiguous slice per work item.
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

// One dispatch unit: skin `vertex_count` consecutive source vertices with one
// joint palette. The queue builder emits exactly one of these per accepted
// submission and the compute shader reads them as a std430 array. Every
// field is an element index, never a byte offset:
//
//  - `source_vertex`   first vertex in the renderer-global source arena.
//  - `influence`       first entry in the packed influence arena, i.e. the
//                      asset's base offset plus its asset-local vertex.
//  - `palette`         first joint of this item's slice of the flattened
//                      `palette_current` / `palette_previous` arenas; the
//                      slice length is the joint count packed into the high
//                      bits of `flags`.
//  - `output_current`  first vertex of this item's slice of the frame's
//  - `output_previous` skinned output. The queue builder currently gives
//                      both the same offset, because current and previous
//                      positions live in the one `VkSkinVertex` record.
//  - `instance_slot`   dynamic instance slot; supplies the instance
//                      transform and matches this work to cull records and
//                      raster draws. The owning generation is NOT here -- it
//                      is carried CPU-side in
//                      `VkSkinFrameArenas::work_instance_generations` so the
//                      shader ABI can stay at 32 bytes.
//  - `flags`           see the bit layout documented below.
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

// `VkSkinWorkItem::flags` bit layout:
//   bit  0      kVkSkinHistoryInvalid -- there was no usable previous pose
//               this frame. The queue then copies the current palette into
//               the previous stream, so the previous skinned position equals
//               the current one and the deformation motion vector is zero
//               rather than a difference against unrelated joints.
//   bits 1-4    cull-selected LOD      (kVkSkinLodShift, kVkSkinLodBits)
//   bits 5-15   cull-selected cluster  (kVkSkinClusterShift)
//   bits 16-31  palette joint count    (kVkSkinPaletteCountShift)
// `kVkSkinWorkFlagsMask` covers bits 0-15, i.e. everything except the joint
// count.
constexpr uint32_t kVkSkinHistoryInvalid = 1u << 0;
// The work ABI is deliberately fixed at 32 bytes.  Palette size is carried in
// the high bits of flags. The low bits carry the cull-selected LOD/cluster so
// CPU queue auditing can retain that identity without changing shader stride.
constexpr uint32_t kVkSkinPaletteCountShift = 16u;
constexpr uint32_t kVkSkinPaletteCountMax = 0xffffu;
constexpr uint32_t kVkSkinLodShift = 1u;
constexpr uint32_t kVkSkinLodBits = 4u;
constexpr uint32_t kVkSkinLodMax = (1u << kVkSkinLodBits) - 1u;
constexpr uint32_t kVkSkinClusterShift = kVkSkinLodShift + kVkSkinLodBits;
constexpr uint32_t kVkSkinClusterBits = kVkSkinPaletteCountShift - kVkSkinClusterShift;
constexpr uint32_t kVkSkinClusterMax = (1u << kVkSkinClusterBits) - 1u;
constexpr uint32_t kVkSkinWorkFlagsMask = (1u << kVkSkinPaletteCountShift) - 1u;
constexpr uint32_t vk_skin_pack_cull_identity(uint32_t lod, uint32_t cluster) noexcept {
    return (lod & kVkSkinLodMax) << kVkSkinLodShift |
           (cluster & kVkSkinClusterMax) << kVkSkinClusterShift;
}
constexpr uint32_t vk_skin_work_lod(uint32_t flags) noexcept {
    return (flags >> kVkSkinLodShift) & kVkSkinLodMax;
}
constexpr uint32_t vk_skin_work_cluster(uint32_t flags) noexcept {
    return (flags >> kVkSkinClusterShift) & kVkSkinClusterMax;
}
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

// Compute output, consumed directly as the skinned raster vertex buffer.
// `previous_position` is the previous frame's skinned position of the SAME
// vertex, which is what makes deformation motion vectors possible; when
// history is invalid it ends up equal to `position` (see
// kVkSkinHistoryInvalid above). `tint`, `surface` and `material_index` are
// copied through from the source vertex unchanged; only position and normal
// are actually skinned.
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
