#include "../include/mem_pool.h"
#include "../include/mem_arena.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

// Test that objects are properly aligned to max_align_t
void test_object_alignment() {
    printf("Testing object alignment with objectSize=12...\n");

    // Create allocator with objectSize=12 (not a multiple of max_align_t)
    MemPool* allocator = mem_pool_create(12, 10);
    assert(allocator != NULL);

    size_t alignment = _Alignof(max_align_t);

    // Allocate several objects and verify alignment
    void* ptrs[5];
    for (int i = 0; i < 5; i++) {
        ptrs[i] = mem_pool_alloc(allocator);
        assert(ptrs[i] != NULL);

        uintptr_t addr = (uintptr_t)ptrs[i];

        printf("  Object %d: %p (addr %% alignment = %zu)\n", i, ptrs[i], addr % alignment);

        // Check that address is aligned to max_align_t
        if ((addr % alignment) != 0) {
            fprintf(stderr, "FAILED: Object %d at %p is not aligned to %zu bytes\n",
                    i, ptrs[i], alignment);
            fprintf(stderr, "  Offset from alignment: %zu bytes\n", addr % alignment);
            assert(0 && "Object alignment check failed");
        }
    }

    // Free objects
    for (int i = 0; i < 5; i++) {
        mem_pool_free(allocator, ptrs[i]);
    }

    printf("  All objects properly aligned!\n");

    // Test alloc-after-free reuse: verify freed slots are reused and still aligned
    printf("Testing alloc-after-free reuse...\n");
    void* reuse_ptrs[5];
    for (int i = 0; i < 5; i++) {
        reuse_ptrs[i] = mem_pool_alloc(allocator);
        assert(reuse_ptrs[i] != NULL);

        uintptr_t addr = (uintptr_t)reuse_ptrs[i];
        printf("  Reused Object %d: %p (addr %% alignment = %zu)\n", i, reuse_ptrs[i], addr % alignment);

        // Verify alignment after reuse from free list
        if ((addr % alignment) != 0) {
            fprintf(stderr, "FAILED: Reused Object %d at %p is not aligned to %zu bytes\n",
                    i, reuse_ptrs[i], alignment);
            fprintf(stderr, "  Offset from alignment: %zu bytes\n", addr % alignment);
            assert(0 && "Reused object alignment check failed");
        }
    }

    // Free the reused objects
    for (int i = 0; i < 5; i++) {
        mem_pool_free(allocator, reuse_ptrs[i]);
    }

    printf("  Alloc-after-free reuse verified!\n");

    // Clean up
    mem_pool_destroy(allocator);

    printf("  All objects properly aligned!\n");
}

// Test that objects stay aligned across multiple pages
void test_multi_page_alignment() {
    printf("Testing alignment across multiple pages (allocate 25 objects with objectsPerPage=10)...\n");

    // Create allocator with objectsPerPage=10, so 25 objects will span multiple pages
    MemPool* allocator = mem_pool_create(12, 10);
    assert(allocator != NULL);

    size_t alignment = _Alignof(max_align_t);
    void* ptrs[25];

    // Allocate 25 objects (more than one page of 10 objects)
    for (int i = 0; i < 25; i++) {
        ptrs[i] = mem_pool_alloc(allocator);
        assert(ptrs[i] != NULL);

        uintptr_t addr = (uintptr_t)ptrs[i];
        printf("  Object %d: %p (addr %% alignment = %zu)\n", i, ptrs[i], addr % alignment);

        // Check that address is aligned to max_align_t
        if ((addr % alignment) != 0) {
            fprintf(stderr, "FAILED: Multi-page Object %d at %p is not aligned to %zu bytes\n",
                    i, ptrs[i], alignment);
            fprintf(stderr, "  Offset from alignment: %zu bytes\n", addr % alignment);
            assert(0 && "Multi-page object alignment check failed");
        }
    }

    // Free all objects
    for (int i = 0; i < 25; i++) {
        mem_pool_free(allocator, ptrs[i]);
    }

    // Clean up
    mem_pool_destroy(allocator);

    printf("  All multi-page objects properly aligned!\n");
}

void test_stats() {
    printf("Testing MemStats...\n");
    MemPool* pool = mem_pool_create(32, 4);
    assert(pool != NULL);

    MemStats st;
    mem_pool_get_stats(pool, &st);
    assert(st.totalAllocs == 0);
    assert(st.liveBytes == 0);
    assert(st.pageCount == 0);

    void* a = mem_pool_alloc(pool);
    void* b = mem_pool_alloc(pool);
    assert(a && b);
    mem_pool_get_stats(pool, &st);
    assert(st.totalAllocs == 2);
    assert(st.pageCount == 1);
    assert(st.totalObjects == 4);
    assert(st.freeObjects == 2);
    assert(st.liveBytes > 0);
    size_t peakAfterTwo = st.peakBytes;
    assert(peakAfterTwo == st.liveBytes);

    mem_pool_free(pool, a);
    mem_pool_get_stats(pool, &st);
    assert(st.peakBytes == peakAfterTwo);   /* peak survives frees */
    assert(st.liveBytes < peakAfterTwo);
    assert(st.totalAllocs == 2);            /* allocs only count up */

    mem_pool_destroy(pool);
    printf("  MemStats tests passed!\n");
}

void test_arena_basic() {
    printf("Testing arena basic alloc + alignment...\n");
    MemArena* a = mem_arena_create(1024);
    assert(a != NULL);

    int* x = (int*)mem_arena_alloc(a, sizeof(int) * 10);   /* 40 -> 40 */
    assert(x != NULL);
    assert(((uintptr_t)x % 8) == 0);
    for (int i = 0; i < 10; i++) x[i] = i;

    char* c = (char*)mem_arena_alloc(a, 3);                /* 3 -> 8 */
    assert(c != NULL);
    assert(((uintptr_t)c % 8) == 0);

    for (int i = 0; i < 10; i++) assert(x[i] == i);        /* no overlap */

    MemStats st;
    mem_arena_get_stats(a, &st);
    assert(st.totalAllocs == 2);
    assert(st.pageCount == 1);
    assert(st.liveBytes == 48);
    assert(st.peakBytes == 48);

    assert(mem_arena_alloc(a, 0) == NULL);
    mem_arena_destroy(a);
    printf("  Arena basic tests passed!\n");
}

void test_arena_chaining_and_reset() {
    printf("Testing arena block chaining + reset...\n");
    MemArena* a = mem_arena_create(64);
    assert(a != NULL);

    void* big = mem_arena_alloc(a, 256);   /* > 64: forces a new 256-byte block */
    assert(big != NULL);
    MemStats st;
    mem_arena_get_stats(a, &st);
    assert(st.pageCount == 2);
    assert(st.liveBytes == 256);

    mem_arena_reset(a);
    mem_arena_get_stats(a, &st);
    assert(st.pageCount == 1);             /* largest (256) block retained */
    assert(st.liveBytes == 0);
    assert(st.peakBytes == 256);           /* peak survives reset */

    /* steady state: refill fits in the retained block — no new blocks */
    for (int i = 0; i < 4; i++) assert(mem_arena_alloc(a, 64) != NULL);
    mem_arena_get_stats(a, &st);
    assert(st.pageCount == 1);
    assert(st.liveBytes == 256);

    mem_arena_destroy(a);
    printf("  Arena chaining/reset tests passed!\n");
}

int main() {
    printf("Running MemPool tests...\n");
    printf("max_align_t alignment: %zu bytes\n\n", _Alignof(max_align_t));

    test_object_alignment();
    printf("\n");
    test_multi_page_alignment();
    printf("\n");
    test_stats();
    printf("\n");
    test_arena_basic();
    printf("\n");
    test_arena_chaining_and_reset();

    printf("\nAll tests passed!\n");
    return 0;
}
