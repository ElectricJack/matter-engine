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
//   5. rotation_axis(cardinal axis) matches rotation_x/y/z, AND (separately)
//      rotation_x/y/z match the independent quaternion path
//      (from_trs(quat_from_axis_angle(axis, theta))) -- pins handedness
//      against a second implementation, since #5 alone can't detect all
//      four builders being transposed together
//   6. multiply(a, b) composition order, using NON-commuting operands
//      (translation composed with a rotation) so a reversed argument order
//      inside multiply() actually changes the result
//   7. inverse() rejects non-finite input up front (policy point 3) and
//      leaves `out` untouched
//   8. rotation_axis() degrades to identity() on a non-unit (including
//      zero-length-collapsed-by-normalize()) axis instead of silently
//      producing a uniform-scale matrix
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
#include <cstring>

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

    // Sentinel: every one of the 16 elements is distinguishable (translation
    // slots 9/9/9, everything else the identity's 0s and 1s) so a mutation
    // that clobbers some element other than m[3]/m[7]/m[11] is still caught.
    const mm::Mat4 sentinel = mm::translation({9.0f, 9.0f, 9.0f});
    mm::Mat4 out = sentinel;
    const bool ok = mm::inverse(singular, out);
    if (ok) {
        FAIL("inverse() reported success on a matrix with zero determinant");
    }
    // Policy: false means "did not modify out". Verify all 16 elements are
    // intact, not just the translation slots.
    if (!mat4_nearly_equal(out, sentinel, 1e-6f)) {
        FAIL("inverse() modified `out` despite returning false");
    }

    const mm::Mat4 lenient = mm::inverse_or_identity(singular);
    if (!mat4_nearly_equal(lenient, mm::identity(), 1e-6f)) {
        FAIL("inverse_or_identity() did not return identity on singular input");
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Test: inverse() rejects non-finite input up front (singular-matrix policy
// point 3) and leaves `out` untouched. Previously untested -- the guard
// works today, but a regression (e.g. moving the isfinite check after the
// elimination loop) would have been invisible.
// ---------------------------------------------------------------------------
void test_inverse_rejects_non_finite_input() {
    const mm::Mat4 sentinel = mm::translation({7.0f, 8.0f, 9.0f});

    mm::Mat4 has_nan = mm::identity();
    has_nan.m[5] = NAN;
    mm::Mat4 out_nan = sentinel;
    if (mm::inverse(has_nan, out_nan)) {
        FAIL("inverse() reported success on input containing NaN");
    }
    if (!mat4_nearly_equal(out_nan, sentinel, 1e-6f)) {
        FAIL("inverse() modified `out` on NaN input despite returning false");
    }

    mm::Mat4 has_inf = mm::identity();
    has_inf.m[10] = INFINITY;
    mm::Mat4 out_inf = sentinel;
    if (mm::inverse(has_inf, out_inf)) {
        FAIL("inverse() reported success on input containing +inf");
    }
    if (!mat4_nearly_equal(out_inf, sentinel, 1e-6f)) {
        FAIL("inverse() modified `out` on +inf input despite returning false");
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
// Extra: rotation_x/y/z handedness pinned against the INDEPENDENT
// quaternion path (quat_from_axis_angle + from_trs), which never calls
// rotation_x/y/z/axis. test_rotation_axis_matches_cardinal_builders above
// only cross-checks rotation_axis against rotation_x/y/z -- if all four
// were transposed (equivalently: every angle negated) in the same
// direction, that test would still pass. This test would not: from_trs's
// quaternion-to-matrix formula is a separate derivation with its own
// independently-fixed sign convention, so it only agrees with
// rotation_x/y/z if their handedness is actually correct, not merely
// self-consistent.
// ---------------------------------------------------------------------------
void test_rotation_cardinal_matches_quaternion_path() {
    const float angle = 0.6f;
    const mm::Vec3 origin{0.0f, 0.0f, 0.0f};
    const mm::Vec3 unit_scale{1.0f, 1.0f, 1.0f};

    const mm::Mat4 rx_via_quat =
        mm::from_trs(origin, mm::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, angle), unit_scale);
    if (!mat4_nearly_equal(mm::rotation_x(angle), rx_via_quat, 1e-5f)) {
        FAIL("rotation_x(a) != from_trs(quat_from_axis_angle(X, a)) -- handedness mismatch");
    }

    const mm::Mat4 ry_via_quat =
        mm::from_trs(origin, mm::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, angle), unit_scale);
    if (!mat4_nearly_equal(mm::rotation_y(angle), ry_via_quat, 1e-5f)) {
        FAIL("rotation_y(a) != from_trs(quat_from_axis_angle(Y, a)) -- handedness mismatch");
    }

    const mm::Mat4 rz_via_quat =
        mm::from_trs(origin, mm::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, angle), unit_scale);
    if (!mat4_nearly_equal(mm::rotation_z(angle), rz_via_quat, 1e-5f)) {
        FAIL("rotation_z(a) != from_trs(quat_from_axis_angle(Z, a)) -- handedness mismatch");
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Extra: rotation_axis() on a degenerate (non-unit, or normalize()-collapsed
// zero-length) axis must degrade to identity(), not silently produce a
// uniform-scale matrix (see the comment above rotation_axis()'s definition).
// ---------------------------------------------------------------------------
void test_rotation_axis_degenerate_axis_returns_identity() {
    // normalize() collapses anything with |v| <= 1e-6 to {0,0,0}.
    const mm::Vec3 tiny{1e-8f, 0.0f, 0.0f};
    const mm::Mat4 via_normalize = mm::rotation_axis(mm::normalize(tiny), 1.2f);
    if (!mat4_nearly_equal(via_normalize, mm::identity(), 1e-6f)) {
        FAIL("rotation_axis(normalize(tiny), a) did not degrade to identity()");
    }

    // Directly non-unit (not just the normalize()-fallback path).
    const mm::Mat4 non_unit = mm::rotation_axis({2.0f, 0.0f, 0.0f}, 1.2f);
    if (!mat4_nearly_equal(non_unit, mm::identity(), 1e-6f)) {
        FAIL("rotation_axis() with a non-unit axis did not degrade to identity()");
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Extra: multiply() composition order. combined = multiply(parent, child)
// must transform a point the same as applying child first, then parent --
// matching MatterEngine3/src/world_tracer.cpp's mul16(world_xf, ci.transform,
// combined) usage.
//
// parent/child MUST NOT COMMUTE, or this test cannot fail: two translations
// (the previous parent/child here) commute under composition, so
// multiply(parent, child) and multiply(child, parent) produce the same
// result and transform every point identically -- a multiply() with its
// operand order reversed internally (result = b*a instead of a*b) would
// still pass. A translation composed with a rotation does not commute, so
// swapping the order changes the transformed point. Verified by mutation:
// changing multiply()'s inner loop to
// `sum += b.m[row*4+k] * a.m[k*4+col]` (computing b*a for a call written
// multiply(a, b)) turns this test RED; the previous translation-only
// version stayed 6/6 PASS under that same mutation.
// ---------------------------------------------------------------------------
void test_multiply_composition_order() {
    const mm::Mat4 parent = mm::translation({100.0f, 0.0f, 0.0f});
    const mm::Mat4 child = mm::rotation_z(1.4f); // arbitrary non-cardinal angle
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

// ---------------------------------------------------------------------------
// Extra (Phase 4 Step 2): matter_math_c.h layout compatibility.
//
// The static_asserts in matter_math.h already make a *compile-time*
// divergence between mm:: and Mt* impossible; these tests instead pin the
// *runtime* behaviour of to_c()/from_c() (values round-trip unchanged) and,
// for Mat4, prove the two types are genuinely interchangeable in memory --
// not just equal in size -- by memcpy'ing between them and checking the
// result still transforms points identically. A conversion helper that
// silently permuted fields (e.g. from_c swapping y/z) would pass the
// static_asserts (same size, same individual offsets exist on both sides)
// but fail this.
// ---------------------------------------------------------------------------
void test_c_pod_conversions_round_trip() {
    const MtVec2 c_v2{1.5f, -2.5f};
    const mm::Vec2 v2 = mm::from_c(c_v2);
    if (!nearly_equal(v2.x, 1.5f, 1e-6f) || !nearly_equal(v2.y, -2.5f, 1e-6f)) {
        FAIL("mm::from_c(MtVec2) did not preserve x/y");
    }
    const MtVec2 c_v2_back = mm::to_c(v2);
    if (!nearly_equal(c_v2_back.x, c_v2.x, 1e-6f) || !nearly_equal(c_v2_back.y, c_v2.y, 1e-6f)) {
        FAIL("mm::to_c(Vec2) did not round-trip");
    }

    const MtVec3 c_v3{1.0f, 2.0f, 3.0f};
    const mm::Vec3 v3 = mm::from_c(c_v3);
    if (!nearly_equal(v3.x, 1.0f, 1e-6f) || !nearly_equal(v3.y, 2.0f, 1e-6f) ||
        !nearly_equal(v3.z, 3.0f, 1e-6f)) {
        FAIL("mm::from_c(MtVec3) did not preserve x/y/z in order");
    }
    const MtVec3 c_v3_back = mm::to_c(v3);
    if (!nearly_equal(c_v3_back.x, c_v3.x, 1e-6f) || !nearly_equal(c_v3_back.y, c_v3.y, 1e-6f) ||
        !nearly_equal(c_v3_back.z, c_v3.z, 1e-6f)) {
        FAIL("mm::to_c(Vec3) did not round-trip");
    }

    const MtVec4 c_v4{1.0f, 2.0f, 3.0f, 4.0f};
    const mm::Vec4 v4 = mm::from_c(c_v4);
    if (!nearly_equal(v4.x, 1.0f, 1e-6f) || !nearly_equal(v4.y, 2.0f, 1e-6f) ||
        !nearly_equal(v4.z, 3.0f, 1e-6f) || !nearly_equal(v4.w, 4.0f, 1e-6f)) {
        FAIL("mm::from_c(MtVec4) did not preserve x/y/z/w in order");
    }
    const MtVec4 c_v4_back = mm::to_c(v4);
    if (!nearly_equal(c_v4_back.x, c_v4.x, 1e-6f) || !nearly_equal(c_v4_back.y, c_v4.y, 1e-6f) ||
        !nearly_equal(c_v4_back.z, c_v4.z, 1e-6f) || !nearly_equal(c_v4_back.w, c_v4.w, 1e-6f)) {
        FAIL("mm::to_c(Vec4) did not round-trip");
    }

    PASS();
}

void test_c_pod_mat4_memory_layout_interchangeable() {
    // A non-trivial TRS matrix, built entirely through mm:: builders.
    const mm::Mat4 built = mm::multiply(
        mm::multiply(mm::translation({3.0f, -2.0f, 5.0f}),
                     mm::rotation_axis(mm::normalize({1.0f, 2.0f, 3.0f}), 0.8f)),
        mm::scale({2.0f, 0.5f, 1.5f}));

    // Round-trip through the C POD via the sanctioned conversions.
    const MtMat4 c_mat = mm::to_c(built);
    const mm::Mat4 back = mm::from_c(c_mat);
    if (!mat4_nearly_equal(built, back, 1e-6f)) {
        FAIL("mm::to_c/from_c(Mat4) did not round-trip exactly");
    }

    // Prove the layouts are genuinely byte-identical, not just size-equal:
    // memcpy mm::Mat4 -> MtMat4 (bypassing to_c()) and confirm the raw bytes
    // still transform a point exactly like the original. If MtMat4 had a
    // different field order or padding, this would produce a garbled
    // transform even though sizeof matched.
    MtMat4 raw_copy{};
    static_assert(sizeof(raw_copy) == sizeof(built), "size checked again at the call site");
    std::memcpy(&raw_copy, &built, sizeof(built));
    const mm::Mat4 reinterpreted = mm::from_c(raw_copy);

    const mm::Vec3 p{7.0f, -3.0f, 11.0f};
    const mm::Vec3 expected = mm::transform_point(built, p);
    const mm::Vec3 actual = mm::transform_point(reinterpreted, p);
    if (!nearly_equal(actual.x, expected.x, 1e-4f) || !nearly_equal(actual.y, expected.y, 1e-4f) ||
        !nearly_equal(actual.z, expected.z, 1e-4f)) {
        FAIL("MtMat4 memcpy'd from mm::Mat4 did not transform points identically -- "
             "layouts are not actually byte-compatible");
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Extra (Phase 4 Step 4): quat_identity/quat_invert/quat_multiply, added to
// replace libs/MatterSurfaceLib/src/cluster.cpp's QuaternionIdentity/
// QuaternionInvert call sites.
// ---------------------------------------------------------------------------
void test_quat_identity_invert_multiply() {
    const mm::Quat id = mm::quat_identity();
    if (!nearly_equal(id.x, 0.0f, 1e-6f) || !nearly_equal(id.y, 0.0f, 1e-6f) ||
        !nearly_equal(id.z, 0.0f, 1e-6f) || !nearly_equal(id.w, 1.0f, 1e-6f)) {
        FAIL("quat_identity() != {0,0,0,1}");
    }

    const mm::Quat q = mm::quat_from_axis_angle(mm::normalize({1.0f, 2.0f, 3.0f}), 0.9f);
    const mm::Quat qi = mm::quat_invert(q);
    const mm::Quat should_be_identity = mm::quat_multiply(q, qi);
    if (!nearly_equal(should_be_identity.x, 0.0f, 1e-4f) ||
        !nearly_equal(should_be_identity.y, 0.0f, 1e-4f) ||
        !nearly_equal(should_be_identity.z, 0.0f, 1e-4f) ||
        !nearly_equal(should_be_identity.w, 1.0f, 1e-4f)) {
        FAIL("quat_multiply(q, quat_invert(q)) did not converge to the identity quaternion");
    }

    // quat_invert() on the zero quaternion must fall back to identity rather
    // than divide by zero (matches vulkan_only_compat.cpp's `n > 0.0f` guard).
    const mm::Quat zero_invert = mm::quat_invert(mm::Quat{0.0f, 0.0f, 0.0f, 0.0f});
    if (!nearly_equal(zero_invert.x, 0.0f, 1e-6f) || !nearly_equal(zero_invert.y, 0.0f, 1e-6f) ||
        !nearly_equal(zero_invert.z, 0.0f, 1e-6f) || !nearly_equal(zero_invert.w, 1.0f, 1e-6f)) {
        FAIL("quat_invert({0,0,0,0}) did not fall back to quat_identity()");
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Extra (Phase 4 Step 4): rotate() cross-checked against the INDEPENDENT
// from_trs() quaternion-to-matrix path -- rotate() is the closed-form vector
// rotation ported from vulkan_only_compat.cpp's Vector3RotateByQuaternion;
// from_trs() is a completely separate quaternion-to-matrix derivation. If
// rotate() had, say, a transposed cross product or a wrong sign on the q.w
// term, it would still "look plausible" (unit-length preserving) but this
// would catch it.
// ---------------------------------------------------------------------------
void test_rotate_matches_from_trs_quaternion_path() {
    const mm::Vec3 axis = mm::normalize({1.0f, -2.0f, 0.5f});
    const mm::Quat q = mm::quat_from_axis_angle(axis, 1.1f);
    const mm::Vec3 p{3.0f, -1.0f, 2.0f};

    const mm::Vec3 via_rotate = mm::rotate(p, q);

    const mm::Mat4 via_matrix = mm::from_trs({0.0f, 0.0f, 0.0f}, q, {1.0f, 1.0f, 1.0f});
    const mm::Vec3 via_matrix_point = mm::transform_point(via_matrix, p);

    if (!nearly_equal(via_rotate.x, via_matrix_point.x, 1e-4f) ||
        !nearly_equal(via_rotate.y, via_matrix_point.y, 1e-4f) ||
        !nearly_equal(via_rotate.z, via_matrix_point.z, 1e-4f)) {
        FAIL("rotate(v, q) diverged from from_trs(q)'s equivalent point transform");
    }

    // Rotating by q then by quat_invert(q) must return to the original point.
    const mm::Vec3 round_trip = mm::rotate(via_rotate, mm::quat_invert(q));
    if (!nearly_equal(round_trip.x, p.x, 1e-3f) || !nearly_equal(round_trip.y, p.y, 1e-3f) ||
        !nearly_equal(round_trip.z, p.z, 1e-3f)) {
        FAIL("rotate(rotate(p, q), quat_invert(q)) did not round-trip to p");
    }

    PASS();
}

} // namespace

int main() {
    std::printf("=== MathLib: mathlib_tests ===\n\n");

    RUN_TEST(test_inverse_round_trip);
    RUN_TEST(test_singular_matrix_policy);
    RUN_TEST(test_inverse_rejects_non_finite_input);
    RUN_TEST(test_trs_matches_transform_math_reference);
    RUN_TEST(test_row_major_translation_indices);
    RUN_TEST(test_rotation_axis_matches_cardinal_builders);
    RUN_TEST(test_rotation_cardinal_matches_quaternion_path);
    RUN_TEST(test_rotation_axis_degenerate_axis_returns_identity);
    RUN_TEST(test_multiply_composition_order);
    RUN_TEST(test_c_pod_conversions_round_trip);
    RUN_TEST(test_c_pod_mat4_memory_layout_interchangeable);
    RUN_TEST(test_quat_identity_invert_multiply);
    RUN_TEST(test_rotate_matches_from_trs_quaternion_path);

    std::printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
