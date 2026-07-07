/*
 * bvh_tests.c - TDD tests for SpatialQueryLib BVH matrix_inverse fix
 *
 * Tests cover:
 *  1. matrix_inverse: M·M⁻¹ ≈ I for translation+rotation+scale matrix (epsilon 1e-4)
 *  2. matrix_inverse: near-zero determinant → returns identity (no NaN/Inf)
 *  3. instance-transformed ray intersection: ray in world space hits a
 *     known translated+rotated+scaled triangle by transforming the ray via
 *     the instance inv_transform and intersecting in local space.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <float.h>

#include "../include/bvh.h"

/* --------------------------------------------------------------------------
 * Test harness helpers
 * -------------------------------------------------------------------------- */

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() do { tests_passed++; printf("  PASS\n"); } while(0)
#define FAIL(msg) do { printf("  FAIL: %s\n", (msg)); } while(0)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("[TEST] %s\n", #name); \
    name(); \
} while(0)

#define EPSILON 1e-4f

static int approx_equal(float a, float b) {
    return fabsf(a - b) < EPSILON;
}

/* Multiply M * M_inv and check that result ≈ identity (epsilon 1e-4 per element) */
static int is_identity(const Matrix4x4* m) {
    Matrix4x4 expected = matrix_identity();
    for (int i = 0; i < 16; i++) {
        if (!approx_equal(m->m[i], expected.m[i])) {
            return 0;
        }
    }
    return 1;
}

static void print_matrix(const char* label, const Matrix4x4* m) {
    printf("  %s:\n", label);
    for (int row = 0; row < 4; row++) {
        printf("    [");
        for (int col = 0; col < 4; col++) {
            printf(" %8.4f", m->m[col * 4 + row]);
        }
        printf(" ]\n");
    }
}

/* --------------------------------------------------------------------------
 * Test 1: M·M⁻¹ ≈ I for a combined translation+rotation+scale matrix
 *
 * Build a non-trivial TRS matrix:
 *   scale(2, 3, 0.5) * rotation_y(30°) * translation(5, -3, 7)
 *
 * This exercises all code paths: the stub returns identity for any non-identity
 * input, so M * identity ≠ I → RED. After fix → GREEN.
 * -------------------------------------------------------------------------- */
static void test_matrix_inverse_trs(void) {
    /* Build: T * Ry * S (right-to-left application: scale first, then rotate, then translate) */
    float angle = 0.5235987756f; /* 30 degrees in radians */

    Matrix4x4 T = matrix_translation(5.0f, -3.0f, 7.0f);
    Matrix4x4 Ry = matrix_rotation_y(angle);
    Matrix4x4 S = matrix_scale(2.0f, 3.0f, 0.5f);

    /* Combined: M = T * Ry * S */
    Matrix4x4 TR = matrix_multiply(&T, &Ry);
    Matrix4x4 M  = matrix_multiply(&TR, &S);

    /* Compute inverse */
    Matrix4x4 Minv = matrix_inverse(&M);

    /* M * M^-1 should be identity */
    Matrix4x4 product = matrix_multiply(&M, &Minv);

    int ok = is_identity(&product);

    if (!ok) {
        print_matrix("M", &M);
        print_matrix("M_inv", &Minv);
        print_matrix("M * M_inv", &product);
        FAIL("M * M_inv is not identity (epsilon 1e-4)");
        return;
    }

    PASS();
}

/* --------------------------------------------------------------------------
 * Test 2: M⁻¹·M ≈ I (also check left-multiplication gives identity)
 * -------------------------------------------------------------------------- */
static void test_matrix_inverse_left_multiply(void) {
    float angle = 1.0f; /* ~57 degrees */

    Matrix4x4 T  = matrix_translation(-2.0f, 4.0f, 1.5f);
    Matrix4x4 Rz = matrix_rotation_z(angle);
    Matrix4x4 S  = matrix_scale(0.5f, 2.0f, 1.0f);

    Matrix4x4 TR = matrix_multiply(&T, &Rz);
    Matrix4x4 M  = matrix_multiply(&TR, &S);

    Matrix4x4 Minv    = matrix_inverse(&M);
    Matrix4x4 product = matrix_multiply(&Minv, &M);

    int ok = is_identity(&product);

    if (!ok) {
        print_matrix("M", &M);
        print_matrix("M_inv", &Minv);
        print_matrix("M_inv * M", &product);
        FAIL("M_inv * M is not identity (epsilon 1e-4)");
        return;
    }

    PASS();
}

/* --------------------------------------------------------------------------
 * Test 3: near-zero determinant → matrix_inverse returns identity, not NaN/Inf
 * -------------------------------------------------------------------------- */
static void test_matrix_inverse_singular(void) {
    /* A singular matrix: two identical rows → det = 0 */
    Matrix4x4 singular;
    memset(singular.m, 0, sizeof(singular.m));
    /* Column-major: set row 0 and row 1 to same values */
    singular.m[0] = 1.0f; singular.m[4] = 2.0f; singular.m[8]  = 3.0f; singular.m[12] = 4.0f;
    singular.m[1] = 1.0f; singular.m[5] = 2.0f; singular.m[9]  = 3.0f; singular.m[13] = 4.0f;
    singular.m[2] = 0.0f; singular.m[6] = 1.0f; singular.m[10] = 0.0f; singular.m[14] = 0.0f;
    singular.m[3] = 0.0f; singular.m[7] = 0.0f; singular.m[11] = 0.0f; singular.m[15] = 1.0f;

    Matrix4x4 result = matrix_inverse(&singular);

    /* Result should be identity, not NaN or Inf */
    Matrix4x4 identity = matrix_identity();
    int ok = 1;
    for (int i = 0; i < 16; i++) {
        if (isnan(result.m[i]) || isinf(result.m[i])) {
            printf("  element[%d] = %f (NaN or Inf!)\n", i, result.m[i]);
            ok = 0;
            break;
        }
        if (!approx_equal(result.m[i], identity.m[i])) {
            printf("  element[%d]: got %f expected %f\n", i, result.m[i], identity.m[i]);
            ok = 0;
            break;
        }
    }

    if (!ok) {
        FAIL("singular matrix_inverse did not return identity");
        return;
    }

    PASS();
}

/* --------------------------------------------------------------------------
 * Test 4: pure translation inverse is correct
 *
 * T(tx,ty,tz)^-1 = T(-tx,-ty,-tz). Trivial case, good sanity check.
 * -------------------------------------------------------------------------- */
static void test_matrix_inverse_pure_translation(void) {
    Matrix4x4 T    = matrix_translation(3.0f, -7.0f, 2.5f);
    Matrix4x4 Tinv = matrix_inverse(&T);
    Matrix4x4 prod = matrix_multiply(&T, &Tinv);

    if (!is_identity(&prod)) {
        print_matrix("T * T_inv", &prod);
        FAIL("pure translation M * M_inv is not identity");
        return;
    }

    PASS();
}

/* --------------------------------------------------------------------------
 * Test 5: instance-transformed ray intersection
 *
 * Scenario:
 *   - Unit triangle in local space: vertices at (0,0,0), (1,0,0), (0,1,0).
 *   - Instance transform: translate (10, 0, 0) → world-space triangle at
 *     (10,0,0), (11,0,0), (10,1,0).
 *   - World-space ray: origin=(10, 0.25, -1), direction=(0, 0, 1).
 *     This ray hits the world-space triangle at z=0.
 *   - To test via inv_transform: transform ray origin and direction to local
 *     space using inst->inv_transform, then call ray_triangle_intersect.
 *
 * With the STUB: inv_transform = identity → local-space ray origin is
 *   (10, 0.25, -1), which does NOT hit the unit triangle (x=10 is outside
 *   [0,1]) → test FAILS (RED).
 *
 * With the FIX: inv_transform = T(-10,0,0) → local-space ray origin is
 *   (0, 0.25, -1), direction=(0,0,1) → hits the unit triangle at t=1 (GREEN).
 * -------------------------------------------------------------------------- */
static void test_instance_transformed_ray_intersect(void) {
    /* Local-space triangle: unit triangle in XY plane at z=0 */
    Triangle local_tri;
    local_tri.v0 = (Vec3){0.0f, 0.0f, 0.0f};
    local_tri.v1 = (Vec3){1.0f, 0.0f, 0.0f};
    local_tri.v2 = (Vec3){0.0f, 1.0f, 0.0f};
    local_tri.n0 = local_tri.n1 = local_tri.n2 = (Vec3){0.0f, 0.0f, -1.0f};
    local_tri.centroid  = (Vec3){1.0f/3.0f, 1.0f/3.0f, 0.0f};
    local_tri.normal    = (Vec3){0.0f, 0.0f, -1.0f};
    local_tri.material_id = 0;

    /* Build a minimal BLAS (single-triangle, no SAH needed) */
    BLAS* blas = blas_create(&local_tri, 1, 1);
    assert(blas && "blas_create failed");
    blas_build(blas);

    /* Create instance with a pure translation: translate (10, 0, 0) */
    BVHInstance* inst = bvh_instance_create(blas, 0);
    assert(inst && "bvh_instance_create failed");

    Matrix4x4 transform = matrix_translation(10.0f, 0.0f, 0.0f);
    bvh_instance_set_transform(inst, &transform);

    /*
     * World-space ray: origin=(10, 0.25, -1), dir=(0, 0, 1).
     * Transform to local space via inv_transform.
     */
    Vec3 world_origin    = {10.0f, 0.25f, -1.0f};
    Vec3 world_direction = { 0.0f,  0.0f,  1.0f};

    Vec3 local_origin    = matrix_transform_point(&inst->inv_transform, world_origin);
    Vec3 local_direction = matrix_transform_vector(&inst->inv_transform, world_direction);

    float t = 0.0f, u = 0.0f, v = 0.0f;
    bool hit = ray_triangle_intersect(local_origin, local_direction, local_tri, &t, &u, &v);

    if (!hit) {
        printf("  local_origin = (%.4f, %.4f, %.4f)\n",
               local_origin.x, local_origin.y, local_origin.z);
        printf("  local_direction = (%.4f, %.4f, %.4f)\n",
               local_direction.x, local_direction.y, local_direction.z);
        printf("  (stub returns identity inv_transform → local_origin.x=10, misses triangle)\n");
        FAIL("ray did not hit the instance-transformed triangle (inv_transform broken)");
        bvh_instance_destroy(inst);
        blas_destroy(blas);
        return;
    }

    /* Verify hit parameter: t should be ~1.0 (ray travels from z=-1 to z=0) */
    if (!approx_equal(t, 1.0f)) {
        printf("  t=%.4f (expected ~1.0)\n", t);
        FAIL("hit t-value incorrect");
        bvh_instance_destroy(inst);
        blas_destroy(blas);
        return;
    }

    bvh_instance_destroy(inst);
    blas_destroy(blas);
    PASS();
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(void) {
    printf("=== SpatialQueryLib: bvh_tests ===\n\n");

    RUN_TEST(test_matrix_inverse_pure_translation);
    RUN_TEST(test_matrix_inverse_trs);
    RUN_TEST(test_matrix_inverse_left_multiply);
    RUN_TEST(test_matrix_inverse_singular);
    RUN_TEST(test_instance_transformed_ray_intersect);

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
