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

    printf("Testing mem::Pool move-construction...\n");
    mem::Pool pool(sizeof(Vec3), 16);
    assert(pool.valid());
    void* p = pool.alloc();
    assert(p != nullptr);
    pool.free(p);
    assert(pool.stats().totalAllocs == 1);
    assert(pool.stats().freeObjects == 16);

    mem::Pool pool2(std::move(pool));
    assert(!pool.valid());          /* moved-from is invalidated */
    assert(pool2.valid());
    assert(pool2.stats().totalAllocs == 1);  /* state transferred */

    printf("Testing mem::Pool move-assignment (including self-assignment)...\n");
    mem::Pool pool3(sizeof(int), 8);
    assert(pool3.valid());
    pool3.alloc();  /* allocate one so the old resource has live state */
    pool3 = std::move(pool2);       /* assign into valid object — old pool3 resource must be freed cleanly */
    assert(!pool2.valid());
    assert(pool3.valid());
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
    pool3 = std::move(pool3);       /* self-assignment must remain valid */
#pragma GCC diagnostic pop
    assert(pool3.valid());

    printf("Testing mem::Arena move-assignment (including self-assignment)...\n");
    mem::Arena arena2(512);
    assert(arena2.valid());
    arena2.alloc(64);               /* give it live state before overwrite */
    mem::Arena arena3(256);
    assert(arena3.valid());
    arena3 = std::move(arena2);     /* old arena3 resource must be freed cleanly */
    assert(!arena2.valid());
    assert(arena3.valid());
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
    arena3 = std::move(arena3);     /* self-assignment must remain valid */
#pragma GCC diagnostic pop
    assert(arena3.valid());

    printf("All memory.hpp tests passed!\n");
    return 0;
}
