/*
 * spatial_hash_tests.c - TDD tests for SpatialQueryLib spatial hash bug fixes
 *
 * Tests cover:
 *  1. sh_query_radius dedup: hash-colliding cells must not produce duplicate results
 *  2. sh_query_first: returns non-NULL for an existing object; handles colliding cells
 *  3. Unsigned hash: coords near INT_MAX/4 must not trigger UBSan signed-overflow
 *  4. initialCapacity actually sizes the bucket table (not fixed 1024)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <math.h>

#include "../include/spatial_hash.h"

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() do { tests_passed++; printf("  PASS\n"); } while(0)
#define FAIL(msg) do { printf("  FAIL: %s\n", msg); } while(0)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("[TEST] %s\n", #name); \
    name(); \
} while(0)

/*
 * Recompute the hash function the same way the implementation should (unsigned).
 * Used to verify which grid coords collide.
 *
 * With cellSize=1.0:
 *   grid coord (cx, cy, cz) covers world [cx, cx+1) x [cy, cy+1) x [cz, cz+1).
 *
 * Pre-computed collision pair (within cellRange=1 of origin, bucket 139):
 *   cell (-1,-1,-1) and cell (-1,1,1)
 *   hash = ((unsigned)-1 * 73856093u) ^ ((unsigned)-1 * 19349663u) ^ ((unsigned)-1 * 83492791u)
 *        = (0xFFFFFFFF * 73856093u) ^ ... (all XOR'd) = some value & 1023 == 139
 *        Verified via Python: (-1*73856093)^(-1*19349663)^(-1*83492791) & 1023 == 139
 *        for both (-1,-1,-1) and (-1,1,1).
 *
 * World positions we insert into these cells:
 *   obj_a → (-0.5, -0.5, -0.5)  [center of cell (-1,-1,-1)]
 *   obj_b → (-0.5, 1.5, 1.5)    [center of cell (-1,1,1)]
 *
 * Query: center (0,0,0) radius 2.2 → cellRange=ceil(2.2/1)=3, covers both cells.
 */

/* Two distinct dummy objects */
static int obj_a_val = 1;
static int obj_b_val = 2;
static void* OBJ_A = &obj_a_val;
static void* OBJ_B = &obj_b_val;

/* --------------------------------------------------------------------------
 * Test 1: sh_query_radius does NOT return duplicates when cells hash-collide
 * -------------------------------------------------------------------------- */
static void test_query_radius_no_duplicates(void) {
    SpatialHash* sh = sh_create(1.0f, 64);
    assert(sh && "sh_create failed");

    /* Insert obj_a into cell (-1,-1,-1): world pos (-0.5, -0.5, -0.5) */
    assert(sh_insert(sh, -0.5f, -0.5f, -0.5f, OBJ_A));
    /* Insert obj_b into cell (-1,1,1): world pos (-0.5, 1.5, 1.5) */
    assert(sh_insert(sh, -0.5f, 1.5f, 1.5f, OBJ_B));

    /*
     * Both cells (-1,-1,-1) and (-1,1,1) hash to the same bucket (bucket 139
     * with unsigned arithmetic).  A radius query from (0,0,0) with r=2.2
     * covers both cells (cellRange=3).  Without the dedup fix the scan visits
     * bucket 139 twice, yielding a result array with duplicate entries.
     */
    void* results[16];
    int n = sh_query_radius(sh, 0.0f, 0.0f, 0.0f, 2.2f, results, 16);

    /* Both objects are within radius 2.2 of (0,0,0):
     *   dist(obj_a) = sqrt(3 * 0.5^2) = sqrt(0.75) ~= 0.866  <= 2.2  ✓
     *   dist(obj_b) = sqrt(0.25 + 2.25 + 2.25) = sqrt(4.75) ~= 2.179 <= 2.2  ✓
     */
    if (n != 2) {
        printf("  n=%d (expected 2 — likely %s)\n", n,
               n > 2 ? "DUPLICATE from hash collision" : "object missed");
        FAIL("expected exactly 2 results");
        sh_destroy(sh);
        return;
    }

    /* Verify no duplicates in results */
    int found_a = 0, found_b = 0;
    for (int i = 0; i < n; i++) {
        if (results[i] == OBJ_A) found_a++;
        if (results[i] == OBJ_B) found_b++;
    }
    if (found_a != 1 || found_b != 1) {
        printf("  found_a=%d found_b=%d\n", found_a, found_b);
        FAIL("each object must appear exactly once");
        sh_destroy(sh);
        return;
    }

    sh_destroy(sh);
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 2: sh_query_first returns correct object; no false positives from
 * colliding-cell foreign objects
 * -------------------------------------------------------------------------- */
static void test_query_first_basic(void) {
    SpatialHash* sh = sh_create(1.0f, 64);
    assert(sh && "sh_create failed");

    /* Insert obj_a only */
    assert(sh_insert(sh, 0.5f, 0.5f, 0.5f, OBJ_A));

    /* Query from same position: must find obj_a */
    void* result = sh_query_first(sh, 0.5f, 0.5f, 0.5f, 0.1f);
    if (result != OBJ_A) {
        printf("  result=%p expected=%p\n", result, OBJ_A);
        FAIL("sh_query_first returned wrong object");
        sh_destroy(sh);
        return;
    }

    sh_destroy(sh);
    PASS();
}

static void test_query_first_miss(void) {
    SpatialHash* sh = sh_create(1.0f, 64);
    assert(sh && "sh_create failed");

    /* Query empty table */
    void* result = sh_query_first(sh, 0.5f, 0.5f, 0.5f, 1.0f);
    if (result != NULL) {
        printf("  result=%p expected NULL\n", result);
        FAIL("sh_query_first must return NULL for empty table");
        sh_destroy(sh);
        return;
    }

    sh_destroy(sh);
    PASS();
}

/*
 * Test that sh_query_first, when colliding cells are in the scan range, does
 * not return a foreign-cell object that happens to be outside the radius.
 */
static void test_query_first_no_false_positive_from_collision(void) {
    SpatialHash* sh = sh_create(1.0f, 64);
    assert(sh && "sh_create failed");

    /*
     * Insert obj_b into cell (-1,1,1) which hash-collides with cell (-1,-1,-1).
     * Query from (-0.5,-0.5,-0.5) with a very small radius (0.01) so obj_b is
     * NOT within radius.  Cell (-1,-1,-1) is scanned first; due to the
     * collision obj_b lives in the same bucket.  Without coord filtering the
     * buggy code returns obj_b as a false positive IF it doesn't do the
     * distance check properly.  Actually the existing code does do the distance
     * check but NOT the coord check → the test verifies that behaviour doesn't
     * regress, and that with coord-filtering the implementation is still correct.
     */
    assert(sh_insert(sh, -0.5f, 1.5f, 1.5f, OBJ_B));  /* cell (-1,1,1) */

    void* result = sh_query_first(sh, -0.5f, -0.5f, -0.5f, 0.01f);
    if (result != NULL) {
        printf("  result=%p expected NULL\n", result);
        FAIL("sh_query_first must not return out-of-radius object from colliding bucket");
        sh_destroy(sh);
        return;
    }

    sh_destroy(sh);
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 3: unsigned hash — coords near INT_MAX/4 must not invoke UB
 * -------------------------------------------------------------------------- */
static void test_hash_no_signed_overflow(void) {
    /*
     * INT_MAX/4 ~= 536870911.  Inserting at that coordinate exercises the hash
     * multiply.  With the old signed-int multiply this is signed-integer
     * overflow (UB); UBSan will report it.  With the fixed unsigned multiply
     * it wraps silently.
     *
     * We just verify the insert/query round-trip works without crashing.
     */
    SpatialHash* sh = sh_create(1.0f, 64);
    assert(sh && "sh_create failed");

    /* Use world positions that map to grid coords near INT_MAX/4 */
    float big = (float)(INT_MAX / 4);
    assert(sh_insert(sh, big, big, big, OBJ_A));

    void* results[4];
    int n = sh_query_radius(sh, big, big, big, 1.0f, results, 4);
    if (n < 1 || results[0] != OBJ_A) {
        printf("  n=%d results[0]=%p\n", n, n > 0 ? results[0] : NULL);
        FAIL("object near INT_MAX/4 coords not found");
        sh_destroy(sh);
        return;
    }

    sh_destroy(sh);
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 4: initialCapacity actually affects bucket table size
 * -------------------------------------------------------------------------- */
static void test_initial_capacity_sizes_table(void) {
    /*
     * When initialCapacity=64, the brief specifies the bucket count is
     * next-power-of-two >= capacity/4, min 1024.  capacity/4 = 16, so
     * min(1024,16) → 1024.  That's the minimum floor.
     *
     * When initialCapacity=8192, capacity/4=2048 → next power-of-two >= 2048
     * = 2048 > 1024, so bucket count should be 2048.
     *
     * We verify via sh_get_stats that bucketCount reflects the capacity.
     */
    {
        SpatialHash* sh = sh_create(1.0f, 64);
        assert(sh && "sh_create(64) failed");
        int bc = 0;
        sh_get_stats(sh, &bc, NULL, NULL, NULL);
        if (bc < 1024) {
            printf("  capacity=64 → bucketCount=%d (expected >= 1024)\n", bc);
            FAIL("bucket count too small for capacity=64");
            sh_destroy(sh);
            return;
        }
        sh_destroy(sh);
    }

    {
        SpatialHash* sh = sh_create(1.0f, 8192);
        assert(sh && "sh_create(8192) failed");
        int bc = 0;
        sh_get_stats(sh, &bc, NULL, NULL, NULL);
        if (bc < 2048) {
            printf("  capacity=8192 → bucketCount=%d (expected >= 2048)\n", bc);
            FAIL("bucket count too small for capacity=8192");
            sh_destroy(sh);
            return;
        }
        sh_destroy(sh);
    }

    PASS();
}

/* --------------------------------------------------------------------------
 * Test 5: basic insert/query sanity (regression guard)
 * -------------------------------------------------------------------------- */
static void test_basic_insert_query(void) {
    SpatialHash* sh = sh_create(1.0f, 64);
    assert(sh && "sh_create failed");

    assert(sh_insert(sh, 0.5f, 0.5f, 0.5f, OBJ_A));
    assert(sh_insert(sh, 10.5f, 10.5f, 10.5f, OBJ_B));

    void* results[4];
    int n = sh_query_radius(sh, 0.5f, 0.5f, 0.5f, 0.1f, results, 4);
    if (n != 1 || results[0] != OBJ_A) {
        printf("  n=%d\n", n);
        FAIL("basic radius query failed");
        sh_destroy(sh);
        return;
    }

    sh_destroy(sh);
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 6: sh_query_radius_nearest (existing function — regression guard)
 * -------------------------------------------------------------------------- */
static void test_query_first_with_two_objects(void) {
    SpatialHash* sh = sh_create(1.0f, 64);
    assert(sh && "sh_create failed");

    assert(sh_insert(sh, 0.5f, 0.5f, 0.5f, OBJ_A));
    assert(sh_insert(sh, 0.5f, 0.5f, 10.5f, OBJ_B));

    /* Both within radius 20 */
    void* result = sh_query_first(sh, 0.5f, 0.5f, 0.5f, 20.0f);
    if (result == NULL) {
        FAIL("sh_query_first returned NULL for non-empty table");
        sh_destroy(sh);
        return;
    }

    sh_destroy(sh);
    PASS();
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(void) {
    printf("=== SpatialQueryLib: spatial_hash_tests ===\n\n");

    RUN_TEST(test_query_radius_no_duplicates);
    RUN_TEST(test_query_first_basic);
    RUN_TEST(test_query_first_miss);
    RUN_TEST(test_query_first_no_false_positive_from_collision);
    RUN_TEST(test_hash_no_signed_overflow);
    RUN_TEST(test_initial_capacity_sizes_table);
    RUN_TEST(test_basic_insert_query);
    RUN_TEST(test_query_first_with_two_objects);

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
