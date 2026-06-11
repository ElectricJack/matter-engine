#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include "include/object_allocator.h"
#include "include/spatial_hash.h"
#include "include/bvh.h"

#define TEST_PASSED printf("PASSED: %s\n", __func__)
#define TEST_FAILED printf("FAILED: %s (line %d)\n", __func__, __LINE__)

typedef struct TestPoint {
    float x, y, z;
    int data;
} TestPoint;

// Test that ObjectAllocator works with our project setup
bool test_object_allocator_integration() {
    ObjectAllocator* allocator = oa_create(sizeof(TestPoint), 10);
    if (!allocator) {
        TEST_FAILED;
        return false;
    }
    
    // Allocate a test point
    TestPoint* point = (TestPoint*)oa_alloc(allocator);
    if (!point) {
        TEST_FAILED;
        oa_destroy(allocator);
        return false;
    }
    
    // Initialize and verify the point
    point->x = 1.0f;
    point->y = 2.0f;
    point->z = 3.0f;
    point->data = 42;
    
    if (point->x != 1.0f || point->y != 2.0f || point->z != 3.0f || point->data != 42) {
        TEST_FAILED;
        oa_destroy(allocator);
        return false;
    }
    
    // Free the point
    oa_free(allocator, point);
    oa_destroy(allocator);
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

// Test BVH creation and basic operations
bool test_bvh_creation() {
    // Create test triangles
    Triangle triangles[3];
    
    // Triangle 1
    triangles[0].v0 = (Vec3){-1, -1, 0};
    triangles[0].v1 = (Vec3){1, -1, 0};
    triangles[0].v2 = (Vec3){0, 1, 0};
    triangles[0].normal = (Vec3){0, 0, 1};
    triangles[0].material_id = 0;
    
    // Triangle 2
    triangles[1].v0 = (Vec3){2, -1, 0};
    triangles[1].v1 = (Vec3){4, -1, 0};
    triangles[1].v2 = (Vec3){3, 1, 0};
    triangles[1].normal = (Vec3){0, 0, 1};
    triangles[1].material_id = 1;
    
    // Triangle 3
    triangles[2].v0 = (Vec3){-1, -1, 2};
    triangles[2].v1 = (Vec3){1, -1, 2};
    triangles[2].v2 = (Vec3){0, 1, 2};
    triangles[2].normal = (Vec3){0, 0, 1};
    triangles[2].material_id = 0;
    
    BVH* bvh = bvh_create(triangles, 3, 1);
    if (!bvh) {
        TEST_FAILED;
        return false;
    }
    
    if (bvh->triangle_count != 3) {
        TEST_FAILED;
        bvh_destroy(bvh);
        return false;
    }
    
    // Test flattening for GPU
    BVHNode* node_buffer;
    int* index_buffer;
    int node_count, index_count;
    
    bvh_flatten_for_gpu(bvh, &node_buffer, &index_buffer, &node_count, &index_count);
    
    if (!node_buffer || !index_buffer || node_count <= 0 || index_count != 3) {
        TEST_FAILED;
        free(node_buffer);
        free(index_buffer);
        bvh_destroy(bvh);
        return false;
    }
    
    free(node_buffer);
    free(index_buffer);
    bvh_destroy(bvh);
    TEST_PASSED;
    return true;
}

// Test ray-triangle intersection
bool test_ray_triangle_intersection() {
    Triangle triangle;
    triangle.v0 = (Vec3){-1, -1, 0};
    triangle.v1 = (Vec3){1, -1, 0};
    triangle.v2 = (Vec3){0, 1, 0};
    triangle.normal = (Vec3){0, 0, 1};
    triangle.material_id = 0;
    
    Vec3 ray_origin = (Vec3){0, 0, -1};
    Vec3 ray_direction = (Vec3){0, 0, 1};
    
    float t, u, v;
    bool hit = ray_triangle_intersect(ray_origin, ray_direction, triangle, &t, &u, &v);
    
    if (!hit || t != 1.0f) {
        printf("Expected hit at t=1.0, got hit=%d, t=%f\n", hit, t);
        TEST_FAILED;
        return false;
    }
    
    TEST_PASSED;
    return true;
}

// Run all tests
int main() {
    printf("=== SpatialQueryLib Tests ===\n");
    
    int passed = 0;
    int total = 9;
    
    if (test_object_allocator_integration()) passed++;
    if (test_spatial_hash_create_destroy()) passed++;
    if (test_spatial_hash_insert_query()) passed++;
    if (test_spatial_hash_remove()) passed++;
    if (test_spatial_hash_box_query()) passed++;
    if (test_spatial_hash_stats()) passed++;
    if (test_spatial_hash_clear()) passed++;
    if (test_bvh_creation()) passed++;
    if (test_ray_triangle_intersection()) passed++;
    
    printf("\n%d/%d tests passed\n", passed, total);
    
    return (passed == total) ? 0 : 1;
}