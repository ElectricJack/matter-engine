#include "../include/memory.hpp"
#include <cassert>
#include <cstdio>
#include <utility>

struct Vec3 { float x, y, z; };

int main() {
    printf("Testing mem::Arena...\n");
    mem::Arena arena(1024);
    assert(arena.valid());

    Vec3* v = arena.allocArray<Vec3>(10);
    assert(v != nullptr);
    v[9].x = 1.0f; v[9].y = 2.0f; v[9].z = 3.0f;
    assert(arena.stats().totalAllocs == 1);
    assert(arena.stats().liveBytes == 10 * sizeof(Vec3));

    arena.reset();
    assert(arena.stats().liveBytes == 0);
    assert(arena.stats().peakBytes == 10 * sizeof(Vec3));

    mem::Arena moved(std::move(arena));
    assert(moved.valid() && !arena.valid());

    // overflow-sized count must fail cleanly, not wrap
    assert(moved.allocArray<Vec3>(static_cast<size_t>(-1) / 2) == nullptr);

    printf("Testing mem::Pool...\n");
    mem::Pool pool(sizeof(Vec3), 16);
    assert(pool.valid());
    void* p = pool.alloc();
    assert(p != nullptr);
    pool.free(p);
    assert(pool.stats().totalAllocs == 1);
    assert(pool.stats().freeObjects == 16);

    printf("All memory.hpp tests passed!\n");
    return 0;
}
