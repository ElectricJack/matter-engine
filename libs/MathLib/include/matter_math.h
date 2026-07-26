#pragma once

// libs/MathLib/include/matter_math.h
//
// One Vec2/Vec3/Vec4/Mat4/Quat for MatterEngine2, with ONE documented
// matrix-inverse policy. Resolves tech-debt.md sections 2 and 3 (six
// disagreeing matrix inverses; three matrix multiplies that exist only
// because there were three matrix types) as Phase 1 of
// docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md.
//
// Phase 1 is deliberately additive-only: nothing in the engine calls into
// this header yet. Later phases move MatterSurfaceLib's Matrix4x4 (Phase 2),
// then MatterEngine3's raylib Vector3/Matrix (Phase 3), onto these types.
//
// No raylib, no GL, no Vulkan includes here — this header must stay
// includable from any layer, including ones that never touch a renderer.
// Everything is inline/header-only to avoid ODR hazards once multiple TUs
// pull it in during later phases.

#include <cmath>

namespace mm {

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

// x, y, z, w with w defaulting to 1 (identity rotation) — same field order
// and default as matter::Quaternion (MatterEngine3/include/matter/math_types.h)
// and raylib's Quaternion.
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

// Row-major float[16]: element (row, col) lives at m[row*4 + col].
// Translation lives at m[3], m[7], m[11] — the last column of rows 0-2.
//
// This matches:
//   - libs/SpatialQueryLib/include/tri.h's `mat4::cell` (Translate() writes
//     cell[3], cell[7], cell[11])
//   - libs/MatterSurfaceLib/src/tlas_manager.cpp's `Matrix4x4::m` (Phase 2
//     collapses that type onto this one; its matrix_translation() writes
//     the same three indices with the same comment)
//   - MatterEngine3/include/matter/math_types.h's `Mat4f::m`, and
//     MatterEngine3/src/mat_math.h's `mul16`, whose comment states exactly
//     this convention ("Translation lives in m[3], m[7], m[11]")
//
// This is NOT raylib's `Matrix` layout: raylib names its 16 floats m0..m15
// in COLUMN-major order (see MatterEngine3/src/csg_lowering.cpp's comment:
// "m0..m3 col0, m4..m7 col1, etc", translation at m12/m13/m14). Phase 3,
// which retires raylib's POD types, must transpose when crossing that
// boundary — relabeling m12/13/14 as m3/7/11 without transposing would
// silently rotate the matrix instead of translating it.
//
// multiply() and transform() below use column-vector algebra: a point is
// transformed as v' = M * v, i.e. result[row] = sum_col M(row,col)*v[col].
//
// Mat4{} default-constructs to IDENTITY (unlike matter::Mat4f, whose
// `float m[16] = {}` defaults to all-zero) — a math library's default value
// should be inert under multiply()/transform_point(), and the builders below
// (translation(), scale(), rotation_*()) rely on starting from identity and
// overwriting only the entries they change.
struct Mat4 {
    float m[16] = {1, 0, 0, 0,
                    0, 1, 0, 0,
                    0, 0, 1, 0,
                    0, 0, 0, 1};

    float operator()(int row, int col) const { return m[row * 4 + col]; }
    float& operator()(int row, int col) { return m[row * 4 + col]; }
};

inline Mat4 identity() { return Mat4{}; }

// ---------------------------------------------------------------------------
// Singular-matrix policy — READ THIS BEFORE ADDING A SEVENTH INVERSE.
//
// tech-debt.md #2 catalogued six existing matrix-inverse implementations in
// this tree that disagree about what happens on a singular (non-invertible)
// input:
//   - MatterEngine3/src/render/matrix_math.cpp:120 `mat4_inverse`     -> returns false, caller decides
//   - libs/SpatialQueryLib/include/tri.h:57 `mat4::Inverted`         -> returns identity
//   - MatterEngine3/src/csg_lowering.cpp:24 `mat_invert`              -> returns the zero matrix (guarded)
//   - libs/MatterSurfaceLib/src/tlas_manager.cpp:49 `matrix_inverse`  -> echoes the input back unmodified
//   - MatterEngine3/src/world_tracer.cpp:34 `invert4x4`               -> returns false, caller decides
//   - MatterEngine3/src/matter_engine.cpp:197 `invert4x4`             -> unguarded adjugate form (dead code — deleted in this phase)
//   - third_party/raylib/src/raymath.h:1538 `MatrixInvert`            -> unguarded, computes 1/det -> inf/NaN
//
// A degenerate transform (zero scale on an axis, a collapsed basis, a bad
// bake input) therefore behaves differently depending on which of six code
// paths happens to reach it: silently becomes the identity on one path, a
// zero matrix on another, quietly echoes back an un-inverted matrix on a
// third, and produces NaN that propagates outward on a fourth. That is a
// correctness hazard, not a style complaint — a caller cannot reason about
// what "singular" does without knowing which of six functions it called,
// and the same bug (an uninverted matrix silently used as-if inverted) can
// hide behind any of them.
//
// THE POLICY, decided once, here:
//   1. inverse(const Mat4&, Mat4&) returns bool and leaves `out` unmodified
//      on failure, matching matrix_math.cpp's existing mat4_inverse — the
//      one implementation of the six whose caller already gets to *choose*
//      what happens on failure. This is the shape every call site should
//      migrate to. Check the return value, not the output.
//   2. inverse_or_identity(const Mat4&) is the ONLY sanctioned convenience
//      wrapper for call sites that genuinely want today's lenient
//      identity-on-singular behaviour (matching mat4::Inverted). Do not add
//      a second convenience wrapper that returns zero, or one that echoes
//      the input back — if a call site needs different singular-input
//      behaviour than these two options, that is a sign it should be
//      handling inverse()'s bool itself, not asking for a third wrapper.
//   3. Neither entry point ever hands back NaN/inf silently: both reject
//      non-finite input up front and re-check every output element before
//      returning success.
//
// The next person tempted to write a seventh inverse: don't. Extend the
// caller around inverse(), or use inverse_or_identity().
// ---------------------------------------------------------------------------

// Gauss-Jordan elimination with partial pivoting, accumulated in double
// precision (same approach as matrix_math.cpp:120's mat4_inverse). Returns
// false — and leaves `out` unmodified — if `in` contains a non-finite
// value, if elimination hits a pivot that is exactly zero or non-finite
// (singular, or too close to it for this method to trust), or if any
// resulting element would be non-finite.
inline bool inverse(const Mat4& in, Mat4& out) {
    double augmented[4][8] = {};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            const float value = in.m[row * 4 + col];
            if (!std::isfinite(value)) {
                return false;
            }
            augmented[row][col] = value;
        }
        augmented[row][row + 4] = 1.0;
    }

    for (int col = 0; col < 4; ++col) {
        int pivot_row = col;
        for (int row = col + 1; row < 4; ++row) {
            if (std::fabs(augmented[row][col]) > std::fabs(augmented[pivot_row][col])) {
                pivot_row = row;
            }
        }
        const double pivot = augmented[pivot_row][col];
        if (!std::isfinite(pivot) || pivot == 0.0) {
            return false;
        }
        if (pivot_row != col) {
            for (int element = 0; element < 8; ++element) {
                const double tmp = augmented[col][element];
                augmented[col][element] = augmented[pivot_row][element];
                augmented[pivot_row][element] = tmp;
            }
        }

        const double divisor = augmented[col][col];
        for (int element = 0; element < 8; ++element) {
            augmented[col][element] /= divisor;
        }
        for (int row = 0; row < 4; ++row) {
            if (row == col) {
                continue;
            }
            const double factor = augmented[row][col];
            for (int element = 0; element < 8; ++element) {
                augmented[row][element] -= factor * augmented[col][element];
            }
        }
    }

    Mat4 candidate{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            const double value = augmented[row][col + 4];
            if (!std::isfinite(value)) {
                return false;
            }
            const float candidate_value = static_cast<float>(value);
            if (!std::isfinite(candidate_value)) {
                return false;
            }
            candidate.m[row * 4 + col] = candidate_value;
        }
    }
    out = candidate;
    return true;
}

// The one sanctioned lenient wrapper — see the policy block above. Returns
// identity() when `m` is singular or contains non-finite values.
inline Mat4 inverse_or_identity(const Mat4& m) {
    Mat4 out{};
    if (inverse(m, out)) {
        return out;
    }
    return identity();
}

// ---------------------------------------------------------------------------
// Multiply / transform
// ---------------------------------------------------------------------------

// Standard row-major matrix product: result = a * b. Under the
// column-vector convention this header uses, applying `result` to a point
// is equivalent to applying `b` first and then `a`: (a*b)*v == a*(b*v).
// Same convention as MatterEngine3/src/render/matrix_math.cpp:64 `mat4_mul`
// and MatterEngine3/src/mat_math.h:27 `mul16`.
inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[row * 4 + k] * b.m[k * 4 + col];
            }
            result.m[row * 4 + col] = sum;
        }
    }
    return result;
}

inline Vec4 transform(const Mat4& m, const Vec4& v) {
    return {
        m.m[0] * v.x + m.m[1] * v.y + m.m[2] * v.z + m.m[3] * v.w,
        m.m[4] * v.x + m.m[5] * v.y + m.m[6] * v.z + m.m[7] * v.w,
        m.m[8] * v.x + m.m[9] * v.y + m.m[10] * v.z + m.m[11] * v.w,
        m.m[12] * v.x + m.m[13] * v.y + m.m[14] * v.z + m.m[15] * v.w,
    };
}

// w = 1: applies rotation, scale AND translation.
inline Vec3 transform_point(const Mat4& m, const Vec3& p) {
    const Vec4 r = transform(m, Vec4{p.x, p.y, p.z, 1.0f});
    return {r.x, r.y, r.z};
}

// w = 0: applies rotation and scale, ignores translation.
inline Vec3 transform_vector(const Mat4& m, const Vec3& v) {
    const Vec4 r = transform(m, Vec4{v.x, v.y, v.z, 0.0f});
    return {r.x, r.y, r.z};
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

inline Mat4 translation(const Vec3& t) {
    Mat4 result{};
    result.m[3] = t.x;
    result.m[7] = t.y;
    result.m[11] = t.z;
    return result;
}

inline Mat4 scale(const Vec3& s) {
    Mat4 result{};
    result.m[0] = s.x;
    result.m[5] = s.y;
    result.m[10] = s.z;
    return result;
}

inline Mat4 scale(float uniform) {
    return scale(Vec3{uniform, uniform, uniform});
}

inline Mat4 rotation_x(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 result{};
    result.m[5] = c;
    result.m[6] = -s;
    result.m[9] = s;
    result.m[10] = c;
    return result;
}

inline Mat4 rotation_y(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 result{};
    result.m[0] = c;
    result.m[2] = s;
    result.m[8] = -s;
    result.m[10] = c;
    return result;
}

inline Mat4 rotation_z(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 result{};
    result.m[0] = c;
    result.m[1] = -s;
    result.m[4] = s;
    result.m[5] = c;
    return result;
}

// Rodrigues' rotation formula about an arbitrary axis. `axis` must already
// be normalized by the caller. An axis of (1,0,0)/(0,1,0)/(0,0,1) reduces
// exactly to rotation_x/y/z(radians) respectively (verified in
// tests/mathlib_tests.cpp).
inline Mat4 rotation_axis(const Vec3& axis, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float t = 1.0f - c;
    const float x = axis.x, y = axis.y, z = axis.z;

    Mat4 result{};
    result.m[0] = t * x * x + c;
    result.m[1] = t * x * y - s * z;
    result.m[2] = t * x * z + s * y;
    result.m[4] = t * x * y + s * z;
    result.m[5] = t * y * y + c;
    result.m[6] = t * y * z - s * x;
    result.m[8] = t * x * z - s * y;
    result.m[9] = t * y * z + s * x;
    result.m[10] = t * z * z + c;
    return result;
}

// Translation * Rotation * Scale (column-vector convention: a point is
// scaled, then rotated, then translated). Element layout matches
// MatterEngine3/src/ecs/transform_math.h `trs_matrix`'s formula exactly —
// cross-checked in tests/mathlib_tests.cpp against a literal copy of that
// formula (see the comment there for why it's a copy rather than a direct
// call: transform_math.h pulls in matter/ecs.h -> flecs.h, a ~39k-line
// dependency this header-only, engine-independent library does not take).
//
// A non-finite or zero-length `rotation` normalizes to the identity
// rotation, matching trs_matrix's fallback.
inline Mat4 from_trs(const Vec3& t, const Quat& rotation, const Vec3& s) {
    double x = rotation.x;
    double y = rotation.y;
    double z = rotation.z;
    double w = rotation.w;
    const double length_squared = x * x + y * y + z * z + w * w;
    if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
        std::isfinite(w) && std::isfinite(length_squared) && length_squared > 0.0) {
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

    Mat4 result{};
    result.m[0] = static_cast<float>((1.0 - 2.0 * (yy + zz)) * s.x);
    result.m[1] = static_cast<float>((2.0 * (xy - zw)) * s.y);
    result.m[2] = static_cast<float>((2.0 * (xz + yw)) * s.z);
    result.m[3] = t.x;
    result.m[4] = static_cast<float>((2.0 * (xy + zw)) * s.x);
    result.m[5] = static_cast<float>((1.0 - 2.0 * (xx + zz)) * s.y);
    result.m[6] = static_cast<float>((2.0 * (yz - xw)) * s.z);
    result.m[7] = t.y;
    result.m[8] = static_cast<float>((2.0 * (xz - yw)) * s.x);
    result.m[9] = static_cast<float>((2.0 * (yz + xw)) * s.y);
    result.m[10] = static_cast<float>((1.0 - 2.0 * (xx + yy)) * s.z);
    result.m[11] = t.z;
    result.m[12] = 0.0f;
    result.m[13] = 0.0f;
    result.m[14] = 0.0f;
    result.m[15] = 1.0f;
    return result;
}

// ---------------------------------------------------------------------------
// Minimal Vec3 algebra — enough for callers migrating off raylib's Vector3
// without pulling in a second header. Deliberately not exhaustive: this is
// not meant to become a general-purpose vector-math grab bag (see
// tech-debt.md #4 on why the three type families stay separate).
// ---------------------------------------------------------------------------

inline Vec3 add(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 scale_vec(const Vec3& a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}

inline float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(const Vec3& a) {
    return std::sqrt(dot(a, a));
}

inline Vec3 normalize(const Vec3& a) {
    const float length_squared = dot(a, a);
    if (!(length_squared > 1e-12f) || !std::isfinite(length_squared)) {
        return {};
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    return {a.x * inverse_length, a.y * inverse_length, a.z * inverse_length};
}

// Axis-angle -> quaternion. `axis` must already be normalized by the caller.
inline Quat quat_from_axis_angle(const Vec3& axis, float radians) {
    const float half = radians * 0.5f;
    const float s = std::sin(half);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

} // namespace mm
