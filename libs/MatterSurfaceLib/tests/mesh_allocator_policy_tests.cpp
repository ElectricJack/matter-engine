#include "check.h"

bool graphical_mesh_allocator_uses_raylib_api();
bool headless_mesh_allocator_uses_local_hooks();

int main() {
    CHECK(graphical_mesh_allocator_uses_raylib_api(),
          "graphical mesh allocation preserves MemAlloc/MemFree dispatch");
    CHECK(headless_mesh_allocator_uses_local_hooks(),
          "headless mesh allocation uses local allocator hooks");
    return check_summary();
}
