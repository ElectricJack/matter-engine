#pragma once

#include <cstdint>
#include <type_traits>

namespace viewer {

struct DrawCommand {
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
};
static_assert(std::is_standard_layout<DrawCommand>::value,
              "indirect command must be standard layout");
static_assert(sizeof(DrawCommand) == 4 * sizeof(uint32_t),
              "VkDrawIndirectCommand-compatible layout");

inline bool operator==(const DrawCommand& a, const DrawCommand& b) {
    return a.vertex_count == b.vertex_count &&
           a.instance_count == b.instance_count &&
           a.first_vertex == b.first_vertex &&
           a.first_instance == b.first_instance;
}

}  // namespace viewer
