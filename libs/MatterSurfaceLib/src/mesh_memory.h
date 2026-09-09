#pragma once

#include "raylib.h"

#include <cstddef>

namespace matter_surface {
namespace detail {

static inline void* mesh_alloc(std::size_t size) {
#ifdef MATTER_VULKAN_ONLY
    return RL_MALLOC(size);
#else
    return MemAlloc(static_cast<unsigned int>(size));
#endif
}

static inline void mesh_free(void* pointer) {
#ifdef MATTER_VULKAN_ONLY
    RL_FREE(pointer);
#else
    MemFree(pointer);
#endif
}

}  // namespace detail
}  // namespace matter_surface
