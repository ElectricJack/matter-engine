#include <cstdlib>

namespace {
int local_alloc_calls = 0;
int local_free_calls = 0;

void* headless_local_alloc(std::size_t size) {
    ++local_alloc_calls;
    return std::malloc(size);
}

void headless_local_free(void* pointer) {
    ++local_free_calls;
    std::free(pointer);
}
}  // namespace

#define MATTER_VULKAN_ONLY
#define RL_MALLOC(size) headless_local_alloc(size)
#define RL_FREE(pointer) headless_local_free(pointer)
#include "mesh_memory.h"

bool headless_mesh_allocator_uses_local_hooks() {
    void* allocation = matter_surface::detail::mesh_alloc(32);
    matter_surface::detail::mesh_free(allocation);
    return allocation != nullptr && local_alloc_calls == 1 && local_free_calls == 1;
}
