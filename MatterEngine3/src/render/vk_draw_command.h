#pragma once

#include <cstdint>
#include <type_traits>

namespace viewer {

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
