#include <cstdlib>

namespace {
int raylib_alloc_calls = 0;
int raylib_free_calls = 0;
int local_alloc_calls = 0;
int local_free_calls = 0;

void* graphical_local_alloc(std::size_t size) {
    ++local_alloc_calls;
    return std::malloc(size);
}

void graphical_local_free(void* pointer) {
    ++local_free_calls;
    std::free(pointer);
}
}  // namespace

#define RL_MALLOC(size) graphical_local_alloc(size)
#define RL_FREE(pointer) graphical_local_free(pointer)
#include "mesh_memory.h"

extern "C" void* MemAlloc(unsigned int size) {
    ++raylib_alloc_calls;
    return std::malloc(size);
}

extern "C" void MemFree(void* pointer) {
    ++raylib_free_calls;
    std::free(pointer);
}

bool graphical_mesh_allocator_uses_raylib_api() {
    void* allocation = matter_surface::detail::mesh_alloc(32);
    matter_surface::detail::mesh_free(allocation);
    return allocation != nullptr && raylib_alloc_calls == 1 && raylib_free_calls == 1 &&
           local_alloc_calls == 0 && local_free_calls == 0;
}
