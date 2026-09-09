// libs/SpatialQueryLib/main.c
//
// Smoke-test driver for SpatialQueryLib. This file IS the project's
// executable: `make -C libs/SpatialQueryLib` links it with `src/spatial_hash.c`
// and MemoryLib's `src/mem_pool.c` into `build/spatialquerylib`, which
// `build-all.sh` builds as part of the repo-wide sweep. There is no library
// archive target — the consumers of this project (`libs/MatterSurfaceLib`,
// `MatterEngine3`) compile `src/spatial_hash.c` directly from here, per the
// "Code Sharing Between Projects" rule in the root CLAUDE.md.
//
// Scope: coarse coverage that the pieces fit together — the MemoryLib
// integration plus create/insert/query/remove/box/stats/clear on the spatial
// hash. It deliberately does not cover the sharp edges (hash collisions
// between distinct grid cells, signed-overflow-free hashing at extreme
// coordinates, `initialCapacity` actually sizing the bucket table, or
// `sh_query_first`). Those live in `tests/spatial_hash_tests.c`, and the
// BVH/TLAS/analyzer regressions live in `tests/bvh_tests.cpp`; `make -C
// libs/SpatialQueryLib test` builds and runs both (with AddressSanitizer +
// UBSan where the toolchain has them — mingw-w64 does not). Add new regression
// coverage there, not here.
//
// Convention: each test returns bool and prints via the TEST_PASSED /
// TEST_FAILED macros below; every failure path must destroy the hash it
// created before returning, since the suite is also run under sanitizers.
// Exit status is 0 only when every test passes.

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include "mem_pool.h"
#include "include/spatial_hash.h"

#define TEST_PASSED printf("PASSED: %s\n", __func__)
#define TEST_FAILED printf("FAILED: %s (line %d)\n", __func__, __LINE__)

typedef struct TestPoint {
    float x, y, z;
    int data;
} TestPoint;

// Test that MemPool works with our project setup
bool test_mem_pool_integration() {
    MemPool* allocator = mem_pool_create(sizeof(TestPoint), 10);
    if (!allocator) {
        TEST_FAILED;
        return false;
    }
    
    // Allocate a test point
    TestPoint* point = (TestPoint*)mem_pool_alloc(allocator);
    if (!point) {
        TEST_FAILED;
        mem_pool_destroy(allocator);
        return false;
    }
    
    // Initialize and verify the point
    point->x = 1.0f;
    point->y = 2.0f;
    point->z = 3.0f;
    point->data = 42;
    
    if (point->x != 1.0f || point->y != 2.0f || point->z != 3.0f || point->data != 42) {
        TEST_FAILED;
        mem_pool_destroy(allocator);
        return false;
    }
    
    // Free the point
    mem_pool_free(allocator, point);
    mem_pool_destroy(allocator);
    TEST_PASSED;
    return true;
}

// Test SpatialHash creation and destruction
bool test_spatial_hash_create_destroy() {
    SpatialHash* hash = sh_create(10.0f, 100);
    if (!hash) {
        TEST_FAILED;
        return false;
    }
    
    sh_destroy(hash);
    TEST_PASSED;
    return true;
}

// Test basic insertion and querying
bool test_spatial_hash_insert_query() {
    SpatialHash* hash = sh_create(10.0f, 100);
    if (!hash) {
        TEST_FAILED;
        return false;
    }
    
    // Create test objects
    TestPoint point1 = {0.0f, 0.0f, 0.0f, 1};
    TestPoint point2 = {5.0f, 5.0f, 5.0f, 2};
    TestPoint point3 = {20.0f, 20.0f, 20.0f, 3};
    
    // Insert objects
    if (!sh_insert(hash, point1.x, point1.y, point1.z, &point1)) {
        TEST_FAILED;
        sh_destroy(hash);
        return false;
    }
    
    if (!sh_insert(hash, point2.x, point2.y, point2.z, &point2)) {
        TEST_FAILED;
        sh_destroy(hash);
        return false;
    }
    
    if (!sh_insert(hash, point3.x, point3.y, point3.z, &point3)) {
        TEST_FAILED;
        sh_destroy(hash);
        return false;
    }
    
    // Query near first point - should find point1 and point2
    void* results[10];
    int found = sh_query_radius(hash, 0.0f, 0.0f, 0.0f, 10.0f, results, 10);
    
    if (found != 2) {
        printf("Expected 2 objects near origin, found %d\n", found);
        TEST_FAILED;
        sh_destroy(hash);
        return false;
    }
    
    sh_destroy(hash);
    TEST_PASSED;
    return true;
}

// Test removal of objects
bool test_spatial_hash_remove() {
    SpatialHash* hash = sh_create(10.0f, 100);
    TestPoint point1 = {0.0f, 0.0f, 0.0f, 1};
    TestPoint point2 = {5.0f, 5.0f, 5.0f, 2};
    
    // Insert objects
    sh_insert(hash, point1.x, point1.y, point1.z, &point1);
    sh_insert(hash, point2.x, point2.y, point2.z, &point2);
    
    // Remove one object
    if (!sh_remove(hash, point1.x, point1.y, point1.z, &point1)) {
        TEST_FAILED;
        sh_destroy(hash);
        return false;
    }
    
    // Query should now find only one object
    void* results[10];
    int found = sh_query_radius(hash, 0.0f, 0.0f, 0.0f, 10.0f, results, 10);
    
    if (found != 1) {
        printf("Expected 1 object after removal, found %d\n", found);
        TEST_FAILED;
        sh_destroy(hash);
        return false;
    }
    
    sh_destroy(hash);
    TEST_PASSED;
    return true;
}

// Test bounding box queries
bool test_spatial_hash_box_query() {
    SpatialHash* hash = sh_create(10.0f, 100);
    TestPoint point1 = {0.0f, 0.0f, 0.0f, 1};
    TestPoint point2 = {5.0f, 5.0f, 5.0f, 2};
    TestPoint point3 = {15.0f, 15.0f, 15.0f, 3};
    
    // Insert objects
    sh_insert(hash, point1.x, point1.y, point1.z, &point1);
    sh_insert(hash, point2.x, point2.y, point2.z, &point2);
    sh_insert(hash, point3.x, point3.y, point3.z, &point3);
    
    // Query box that should contain only first two points
    void* results[10];
    int found = sh_query_box(hash, -1.0f, -1.0f, -1.0f, 10.0f, 10.0f, 10.0f, results, 10);
    
    if (found != 2) {
        printf("Expected 2 objects in box query, found %d\n", found);
        TEST_FAILED;
        sh_destroy(hash);
        return false;
    }
    
    sh_destroy(hash);
    TEST_PASSED;
    return true;
}

// Test statistics
bool test_spatial_hash_stats() {
    SpatialHash* hash = sh_create(10.0f, 100);
    int bucketCount, objectCount, maxBucketSize;
    float loadFactor;
    
    // Check initial stats
    sh_get_stats(hash, &bucketCount, &objectCount, &maxBucketSize, &loadFactor);
    
    if (objectCount != 0) {
        printf("Expected 0 initial objects, found %d\n", objectCount);
        TEST_FAILED;
        sh_destroy(hash);
        return false;
    }
    
    // Add some objects
    TestPoint points[5];
    for (int i = 0; i < 5; i++) {
        points[i].x = i * 10.0f;
        points[i].y = i * 10.0f; 
        points[i].z = i * 10.0f;
        points[i].data = i;
        sh_insert(hash, points[i].x, points[i].y, points[i].z, &points[i]);
    }
    
    // Check stats after insertion
    sh_get_stats(hash, &bucketCount, &objectCount, &maxBucketSize, &loadFactor);
    
    if (objectCount != 5) {
        printf("Expected 5 objects after insertion, found %d\n", objectCount);
        TEST_FAILED;
        sh_destroy(hash);
        return false;
    }
    
    sh_destroy(hash);
    TEST_PASSED;
    return true;
}

// Test clearing the hash table
bool test_spatial_hash_clear() {
    SpatialHash* hash = sh_create(10.0f, 100);
    TestPoint point1 = {0.0f, 0.0f, 0.0f, 1};
    
    // Insert object
    sh_insert(hash, point1.x, point1.y, point1.z, &point1);
    
    // Clear hash
    sh_clear(hash);
    
    // Verify it's empty
    int bucketCount, objectCount, maxBucketSize;
    float loadFactor;
    sh_get_stats(hash, &bucketCount, &objectCount, &maxBucketSize, &loadFactor);
    
    if (objectCount != 0) {
        printf("Expected 0 objects after clear, found %d\n", objectCount);
        TEST_FAILED;
        sh_destroy(hash);
        return false;
    }
    
    sh_destroy(hash);
    TEST_PASSED;
    return true;
}

// Run all tests
// `total` below is a hand-maintained count, not derived from the call list —
// bump it whenever you add or remove a test or the pass/fail line lies.
int main() {
    printf("=== SpatialQueryLib Tests ===\n");
    
    int passed = 0;
    int total = 7;
    
    if (test_mem_pool_integration()) passed++;
    if (test_spatial_hash_create_destroy()) passed++;
    if (test_spatial_hash_insert_query()) passed++;
    if (test_spatial_hash_remove()) passed++;
    if (test_spatial_hash_box_query()) passed++;
    if (test_spatial_hash_stats()) passed++;
    if (test_spatial_hash_clear()) passed++;
    
    printf("\n%d/%d tests passed\n", passed, total);
    
    return (passed == total) ? 0 : 1;
}