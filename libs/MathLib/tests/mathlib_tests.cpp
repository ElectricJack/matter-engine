// mathlib_tests.cpp - tests for libs/MathLib/include/matter_math.h
//
// Covers (per Phase 1 of
// docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md):
//   1. round-trip M * M^-1 == I within tolerance
//   2. singular input hitting the documented policy (inverse() -> false,
//      inverse_or_identity() -> identity)
//   3. TRS composition matches MatterEngine3/src/ecs/transform_math.h's
//      trs_matrix() formula
//   4. row-major layout, explicitly: translation must land at m[3]/m[7]/m[11]
//      (a transposed implementation that put it at m[12]/m[13]/m[14] would
//      still pass a naive "does it translate a point" test)
//
// No engine dependencies: this only includes matter_math.h itself, plus (for
// test 3 only) MatterEngine3/include/matter/math_types.h, which is a
// standalone POD header with no further includes. It deliberately does NOT
// include MatterEngine3/src/ecs/transform_math.h directly, because that
// header pulls in matter/ecs.h -> third_party/flecs/flecs.h (~39k lines) and
// linking against it requires flecs.c's globals (EcsSparse, EcsChildOf, ...)
// even though trs_matrix() itself never calls into flecs. Pulling a full ECS
// runtime into a math library's unit tests was judged not worth it; instead
// reference_trs_matrix() below is a byte-for-byte copy of trs_matrix()'s
// body (verified against MatterEngine3/src/ecs/transform_math.h:9-55 at the
// time this was written) so the two can be diffed directly.

#include "../include/matter_math.h"
#include "matter/math_types.h"

#include <cmath>
#include <cstdio>

namespace {

int tests_run = 0;
int tests_passed = 0;

} // namespace

#define PASS() \
    do { \
        tests_passed++; \
        std::printf("  PASS\n"); \
    } while (0)
#define FAIL(msg) \
    do { \
        std::printf("  FAIL: %s\n", msg); \
        return; \
    } while (0)
#define RUN_TEST(name) \
    do { \
        tests_run++; \
        std::printf("[TEST] %s\n", #name); \
        name(); \
    } while (0)

namespace {

bool nearly_equal(float a, float b, float epsilon) {
    return std::fabs(a - b) <= epsilon;
}

bool mat4_nearly_equal(const mm::Mat4& a, const mm::Mat4& b, float epsilon) {
    for (int i = 0; i < 16; ++i) {
        if (!nearly_equal(a.m[i], b.m[i], epsilon)) {
            return false;
        }
    }
    return true;
}

// Literal copy of MatterEngine3/src/ecs/transform_math.h's trs_matrix(), see
// the file header comment above for why this is a copy rather than a call.
matter::Mat4f reference_trs_matrix(const matter::Float3& translation,
                                    const matter::Quaternion& rotation,
                                    const matter::Float3& scale) {
    double x = rotation.x;
    double y = rotation.y;
    double z = rotation.z;
    double w = rotation.w;
    const double length_squared = x * x + y * y + z * z + w * w;
    if (std::isfinite(x) && std::isfinite(y) &&
        std::isfinite(z) && std::isfinite(w) &&
        std::isfinite(length_squared) && length_squared > 0.0) {
        const double inverse_length = 1.0 / std::sqrt(length_squared);
        x *= inverse_length;
        y *= inverse_length;
        z *= inverse_length;
        w *= inverse_length;
    } else {
        x = 0.0;
        y = 0.0;
        z = 0.0;
        w = 1.0;
    }

    const double xx = x * x;
    const double yy = y * y;
    const double zz = z * z;
    const double xy = x * y;
    const double xz = x * z;
    const double yz = y * z;
    const double xw = x * w;
    const double yw = y * w;
    const double zw = z * w;

    matter::Mat4f result{};
    result.m[0] = static_cast<float>((1.0 - 2.0 * (yy + zz)) * scale.x);
    result.m[1] = static_cast<float>((2.0 * (xy - zw)) * scale.y);
    result.m[2] = static_cast<float>((2.0 * (xz + yw)) * scale.z);
    result.m[3] = translation.x;
    result.m[4] = static_cast<float>((2.0 * (xy + zw)) * scale.x);
    result.m[5] = static_cast<float>((1.0 - 2.0 * (xx + zz)) * scale.y);
    result.m[6] = static_cast<float>((2.0 * (yz - xw)) * scale.z);
    result.m[7] = translation.y;
    result.m[8] = static_cast<float>((2.0 * (xz - yw)) * scale.x);
    result.m[9] = static_cast<float>((2.0 * (yz + xw)) * scale.y);
    result.m[10] = static_cast<float>((1.0 - 2.0 * (xx + yy)) * scale.z);
    result.m[11] = translation.z;
    result.m[15] = 1.0f;
    return result;
}

// ---------------------------------------------------------------------------
// Test 1: round-trip M * M^-1 == I within tolerance
// ---------------------------------------------------------------------------
void test_inverse_round_trip() {
    const mm::Mat4 m = mm::multiply(
        mm::multiply(mm::translation({3.0f, -2.0f, 5.0f}),
                     mm::rotation_axis(mm::normalize({1.0f, 2.0f, 3.0f}), 0.8f)),
        mm::scale({2.0f, 0.5f, 1.5f}));

    mm::Mat4 inv{};
    if (!mm::inverse(m, inv)) {
        FAIL("inverse() reported a well-conditioned TRS matrix as singular");
    }

    const mm::Mat4 product = mm::multiply(m, inv);
    if (!mat4_nearly_equal(product, mm::identity(), 1e-4f)) {
        FAIL("M * inverse(M) did not converge to identity within tolerance");
    }

    const mm::Mat4 product_reversed = mm::multiply(inv, m);
    if (!mat4_nearly_equal(product_reversed, mm::identity(), 1e-4f)) {
        FAIL("inverse(M) * M did not converge to identity within tolerance");
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Test 2: singular input hits the documented policy
// ---------------------------------------------------------------------------
void test_singular_matrix_policy() {
    // Zero scale on the Y axis collapses the matrix: row 1 (and column 1)
    // become all-zero, so the determinant is zero.
    const mm::Mat4 singular = mm::scale({1.0f, 0.0f, 1.0f});

    mm::Mat4 out = mm::translation({9.0f, 9.0f, 9.0f}); // sentinel, must survive untouched
    const bool ok = mm::inverse(singular, out);
    if (ok) {
        FAIL("inverse() reported success on a matrix with zero determinant");
    }
    // Policy: false means "did not modify out". Verify the sentinel is intact.
    if (!nearly_equal(out.m[3], 9.0f, 1e-6f) || !nearly_equal(out.m[7], 9.0f, 1e-6f) ||
        !nearly_equal(out.m[11], 9.0f, 1e-6f)) {
        FAIL("inverse() modified `out` despite returning false");
    }

    const mm::Mat4 lenient = mm::inverse_or_identity(singular);
    if (!mat4_nearly_equal(lenient, mm::identity(), 1e-6f)) {
        FAIL("inverse_or_identity() did not return identity on singular input");
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Test 3: TRS composition matches transform_math.h's trs_matrix()
// ---------------------------------------------------------------------------
void test_trs_matches_transform_math_reference() {
    struct Case {
        mm::Vec3 t;
        mm::Quat r;
        mm::Vec3 s;
    };
    const Case cases[] = {
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
        {{1.0f, 2.0f, 3.0f}, mm::quat_from_axis_angle(mm::normalize({0.0f, 1.0f, 0.0f}), 0.5f), {1.0f, 1.0f, 1.0f}},
        {{-4.0f, 0.5f, 2.0f}, mm::quat_from_axis_angle(mm::normalize({1.0f, 1.0f, 1.0f}), 1.234f), {2.0f, 0.5f, 3.0f}},
        // Non-finite rotation: both implementations must fall back to identity rotation.
        {{5.0f, 5.0f, 5.0f}, {NAN, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        // Zero-length rotation quaternion: same fallback.
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
    };

    for (const Case& c : cases) {
        const mm::Mat4 actual = mm::from_trs(c.t, c.r, c.s);

        const matter::Float3 ref_t{c.t.x, c.t.y, c.t.z};
        const matter::Quaternion ref_r{c.r.x, c.r.y, c.r.z, c.r.w};
        const matter::Float3 ref_s{c.s.x, c.s.y, c.s.z};
        const matter::Mat4f expected = reference_trs_matrix(ref_t, ref_r, ref_s);

        for (int i = 0; i < 16; ++i) {
            if (!nearly_equal(actual.m[i], expected.m[i], 1e-5f)) {
                std::printf("    element %d: mm::from_trs=%f reference=%f\n", i,
                            static_cast<double>(actual.m[i]), static_cast<double>(expected.m[i]));
                FAIL("mm::from_trs diverged from the transform_math.h reference formula");
            }
        }
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Test 4: row-major layout, explicit indices
// ---------------------------------------------------------------------------
void test_row_major_translation_indices() {
    const mm::Mat4 t = mm::translation({10.0f, 20.0f, 30.0f});

    // Translation must be at m[3]/m[7]/m[11] (row*4+col form), NOT at
    // m[12]/m[13]/m[14] (which is where a column-major implementation, or a
    // row/column-transposed one, would put it).
    if (!nearly_equal(t.m[3], 10.0f, 1e-6f) || !nearly_equal(t.m[7], 20.0f, 1e-6f) ||
        !nearly_equal(t.m[11], 30.0f, 1e-6f)) {
        FAIL("translation() did not place translation at m[3]/m[7]/m[11]");
    }
    if (!nearly_equal(t.m[12], 0.0f, 1e-6f) || !nearly_equal(t.m[13], 0.0f, 1e-6f) ||
        !nearly_equal(t.m[14], 0.0f, 1e-6f)) {
        FAIL("translation() wrote nonzero values into the bottom row "
             "(m[12]/m[13]/m[14]) -- looks transposed");
    }

    // operator()(row, col) must agree: translation lives in column 3 of
    // rows 0-2.
    if (!nearly_equal(t(0, 3), 10.0f, 1e-6f) || !nearly_equal(t(1, 3), 20.0f, 1e-6f) ||
        !nearly_equal(t(2, 3), 30.0f, 1e-6f)) {
        FAIL("operator()(row,col) disagrees with the m[row*4+col] convention");
    }

    // transform_point must apply the translation once, not append it
    // transposed: a point at the origin maps to exactly the translation.
    const mm::Vec3 origin_mapped = mm::transform_point(t, {0.0f, 0.0f, 0.0f});
    if (!nearly_equal(origin_mapped.x, 10.0f, 1e-6f) ||
        !nearly_equal(origin_mapped.y, 20.0f, 1e-6f) ||
        !nearly_equal(origin_mapped.z, 30.0f, 1e-6f)) {
        FAIL("transform_point() did not apply translation() consistently "
             "with its own m[] layout");
    }

    // transform_vector must ignore translation (w=0).
    const mm::Vec3 dir_mapped = mm::transform_vector(t, {1.0f, 0.0f, 0.0f});
    if (!nearly_equal(dir_mapped.x, 1.0f, 1e-6f) || !nearly_equal(dir_mapped.y, 0.0f, 1e-6f) ||
        !nearly_equal(dir_mapped.z, 0.0f, 1e-6f)) {
        FAIL("transform_vector() picked up translation (w=0 not respected)");
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Extra: rotation_axis on a cardinal axis must reduce exactly to
// rotation_x/y/z. This is a cross-check of the row-major/column-vector sign
// convention chosen for rotation_axis against the three named builders.
// ---------------------------------------------------------------------------
void test_rotation_axis_matches_cardinal_builders() {
    const float angle = 0.7f;

    if (!mat4_nearly_equal(mm::rotation_axis({1.0f, 0.0f, 0.0f}, angle),
                            mm::rotation_x(angle), 1e-6f)) {
        FAIL("rotation_axis(X, a) != rotation_x(a)");
    }
    if (!mat4_nearly_equal(mm::rotation_axis({0.0f, 1.0f, 0.0f}, angle),
                            mm::rotation_y(angle), 1e-6f)) {
        FAIL("rotation_axis(Y, a) != rotation_y(a)");
    }
    if (!mat4_nearly_equal(mm::rotation_axis({0.0f, 0.0f, 1.0f}, angle),
                            mm::rotation_z(angle), 1e-6f)) {
        FAIL("rotation_axis(Z, a) != rotation_z(a)");
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Extra: multiply() composition order. combined = multiply(parent, child)
// must transform a point the same as applying child first, then parent --
// matching MatterEngine3/src/world_tracer.cpp's mul16(world_xf, ci.transform,
// combined) usage.
// ---------------------------------------------------------------------------
void test_multiply_composition_order() {
    const mm::Mat4 parent = mm::translation({100.0f, 0.0f, 0.0f});
    const mm::Mat4 child = mm::translation({0.0f, 10.0f, 0.0f});
    const mm::Mat4 combined = mm::multiply(parent, child);

    const mm::Vec3 p{1.0f, 1.0f, 1.0f};
    const mm::Vec3 expected = mm::transform_point(parent, mm::transform_point(child, p));
    const mm::Vec3 actual = mm::transform_point(combined, p);

    if (!nearly_equal(actual.x, expected.x, 1e-4f) || !nearly_equal(actual.y, expected.y, 1e-4f) ||
        !nearly_equal(actual.z, expected.z, 1e-4f)) {
        FAIL("multiply(parent, child) did not match applying child then parent");
    }

    PASS();
}

} // namespace

int main() {
    std::printf("=== MathLib: mathlib_tests ===\n\n");

    RUN_TEST(test_inverse_round_trip);
    RUN_TEST(test_singular_matrix_policy);
    RUN_TEST(test_trs_matches_transform_math_reference);
    RUN_TEST(test_row_major_translation_indices);
    RUN_TEST(test_rotation_axis_matches_cardinal_builders);
    RUN_TEST(test_multiply_composition_order);

    std::printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
