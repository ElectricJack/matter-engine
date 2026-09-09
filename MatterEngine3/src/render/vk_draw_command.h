#pragma once

// MatterEngine3/src/render/vk_draw_command.h
//
// One indirect draw record. The layout is deliberately identical to
// VkDrawIndexedIndirectCommand so an array of these can be uploaded and handed
// straight to vkCmdDrawIndexedIndirect / vkCmdDrawIndexedIndirectCount — the
// two static_asserts below are what keep that true, and neither may be
// relaxed. The same 20-byte record is produced by the Vulkan compute cull
// (MatterEngine3/shaders_vk/cull.comp) and by the CPU-side command building in
// vk_scene_renderer.cpp, so a change here is a shader change too.
//
// Used by MatterEngine3/src/render/vk_scene_renderer.{h,cpp}, which builds a
// command template per part and lets the cull pass compact it.

#include <cstdint>
#include <type_traits>

namespace viewer {

// All offsets are ABSOLUTE into the shared part-store index/vertex buffers, not
// relative to a part or a LOD — that is what lets one indirect buffer draw
// every part without rebinding. `instance_count` 0 is the normal way the cull
// pass says "drawn nothing this frame"; `first_instance` indexes the instance
// SSBO the vertex shader reads its transform from.
struct DrawCommand {                 // VkDrawIndexedIndirectCommand-compatible
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;            // GLOBAL (part index_start + lod.first_index)
    int32_t  vertex_offset;          // part's global vertex_start
    uint32_t first_instance;
};
static_assert(std::is_standard_layout<DrawCommand>::value,
              "indirect command must be standard layout");
static_assert(sizeof(DrawCommand) == 5 * sizeof(uint32_t),
              "VkDrawIndexedIndirectCommand-compatible layout");

inline bool operator==(const DrawCommand& a, const DrawCommand& b) {
    return a.index_count == b.index_count &&
           a.instance_count == b.instance_count &&
           a.first_index == b.first_index &&
           a.vertex_offset == b.vertex_offset &&
           a.first_instance == b.first_instance;
}

}  // namespace viewer
