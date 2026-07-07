#include "../include/object_allocator.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

// Test that objects are properly aligned to max_align_t
void test_object_alignment() {
    printf("Testing object alignment with objectSize=12...\n");

    // Create allocator with objectSize=12 (not a multiple of max_align_t)
    ObjectAllocator* allocator = oa_create(12, 10);
    assert(allocator != NULL);

    // Allocate several objects and verify alignment
    void* ptrs[5];
    for (int i = 0; i < 5; i++) {
        ptrs[i] = oa_alloc(allocator);
        assert(ptrs[i] != NULL);

        uintptr_t addr = (uintptr_t)ptrs[i];
        size_t alignment = _Alignof(max_align_t);

        printf("  Object %d: %p (alignment: %zu)\n", i, ptrs[i], alignment);

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
        oa_free(allocator, ptrs[i]);
    }

    // Clean up
    oa_destroy(allocator);

    printf("  All objects properly aligned!\n");
}

int main() {
    printf("Running ObjectAllocator tests...\n");
    printf("max_align_t alignment: %zu bytes\n\n", _Alignof(max_align_t));

    test_object_alignment();

    printf("\nAll tests passed!\n");
    return 0;
}
