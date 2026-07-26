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
//
// Phase 4 (Step 2) adds matter_math_c.h: plain C structs (MtVec2/MtVec3/
// MtVec4/MtMat4) that are layout-compatible with Vec2/Vec3/Vec4/Mat4 below,
// for headers shared between C and C++ (see that file's comment). It has
// the same "no raylib/GL/Vulkan" constraint, so including it here does not
// weaken this header's own promise.

#include <cmath>
#include <cstddef>

#include "matter_math_c.h"

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
// This IS byte-compatible with raylib's `Matrix`: raylib's struct declares
// its members as `m0,m4,m8,m12` / `m1,m5,m9,m13` / `m2,m6,m10,m14` /
// `m3,m7,m11,m15` (four groups of four, in that order), so its in-memory
// layout is row-major, identical to this header's `m[16]`. Verified by a
// compiled probe: `MatrixTranslate(10,20,30)` memcpy'd to `float[16]` gives
// raw[3]=10, raw[7]=20, raw[11]=30 — exactly where this header's
// translation() puts them. Phase 3, which retires raylib's POD types, must
// NOT transpose when crossing that boundary: a plain memcpy is valid, or
// relabel field `m12` -> `m[3]`, `m13` -> `m[7]`, `m14` -> `m[11]`.
//
// The trap is raylib's field *naming*, not its layout: raylib names its 16
// floats as if the struct were a flat OpenGL column-major array, i.e. field
// `mN` denotes row `N%4`, col `N/4` (so `m12` reads as "row 0, col 3" even
// though it physically sits at memory offset 3). A comment that takes those
// names at face value — e.g. MatterEngine3/src/csg_lowering.cpp's "raylib
// Matrix is column-major: m0..m3 col0, m4..m7 col1, etc" — describes the
// naming, not the memory, and transposing on that basis would silently
// rotate the matrix instead of translating it. Two in-repo call sites
// already rely on the memory being row-major:
//   - MatterEngine3/src/render/raster_mesh.cpp:158 `row_major_to_matrix` is
//     a bare `memcpy(&m, t, sizeof(Matrix))` — no transpose.
//   - MatterEngine3/tests/gpu_cull_tests.cpp:41-46's comment states it
//     outright: "raylib Matrix memory is ROW-major (declaration order
//     m0,m4,m8,m12 = first row) ... row_major_to_matrix is a straight copy."
//
// One real function DOES need care: `MatrixToFloatV` (raymath.h:2010) walks
// the struct in *field-name* order (`v[12]=m.m12` etc.), so its output array
// is genuinely column-major by this header's convention — that one is not a
// memcpy and not byte-compatible with `mm::Mat4`.
//
// multiply() and transform() below use column-vector algebra: a point is
// transformed as v' = M * v, i.e. result[row] = sum_col M(row,col)*v[col].
//
// Mat4{} default-constructs to IDENTITY (unlike matter::Mat4f, whose
// `float m[16] = {}` defaults to all-zero, AND unlike raylib's `Matrix{}`,
// which is a plain C struct with no member initializers and so is also
// all-zero) — a math library's default value should be inert under
// multiply()/transform_point(), and the builders below (translation(),
// scale(), rotation_*()) rely on starting from identity and overwriting
// only the entries they change.
//
// MIGRATION HAZARD: this silently breaks the "zero-fill, then assign a few
// elements" idiom used by code written against a zero-default matrix type.
// Two concrete call sites in MatterEngine3/src/render/matrix_math.cpp rely
// on `matter::Mat4f{}` being zero:
//   - `perspective_rh_zo` (:88-98) and `perspective_rh_zo_reversed`
//     (:108-118) each do `matter::Mat4f projection{};` and then assign only
//     5 of the 16 elements (m[0], m[5], m[10], m[11], m[14]), relying on
//     every other element — critically m[15] — staying 0. Ported naively to
//     `Mat4 projection{};`, m[15] starts at 1 instead of 0, so
//     `clip.w = -z + 1` instead of `-z`: every projected vertex and every
//     extracted frustum plane comes out subtly wrong, with no compile-time
//     or obvious runtime signal.
//   - `mat4_mul` (:64-75) does `matter::Mat4f result{};` then accumulates
//     into it with `result.m[...] += ...`. Ported to `Mat4 result{};`, the
//     accumulation starts from identity instead of zero, so the product
//     comes out as `a·b + I` (extra +1 on each diagonal term) instead of
//     `a·b`.
// Use zero() (below), not Mat4{}, when porting either pattern.
struct Mat4 {
    float m[16] = {1, 0, 0, 0,
                    0, 1, 0, 0,
                    0, 0, 1, 0,
                    0, 0, 0, 1};

    float operator()(int row, int col) const { return m[row * 4 + col]; }
    float& operator()(int row, int col) { return m[row * 4 + col]; }
};

inline Mat4 identity() { return Mat4{}; }

// All-zero matrix. Mat4{} is deliberately identity (see the struct comment
// above), so code migrating a zero-fill-then-assign-a-few-elements idiom
// (perspective_rh_zo, perspective_rh_zo_reversed, mat4_mul in
// MatterEngine3/src/render/matrix_math.cpp — see the migration-hazard note
// above) must call zero() explicitly instead of relying on Mat4{}.
inline Mat4 zero() {
    Mat4 out{};
    for (float& value : out.m) {
        value = 0.0f;
    }
    return out;
}

// ---------------------------------------------------------------------------
// C-POD bridge — layout equivalence + conversions to/from matter_math_c.h.
//
// These static_asserts are the actual mechanism, not the comment: if a field
// is ever added to one side and not the other, or the two struct definitions
// drift out of member order, this fails to compile. sizeof alone would not
// catch a reordering that happens to keep the same size (e.g. swapping x/y),
// so each member's offsetof is checked individually too.
//
// to_c()/from_c() are the ONLY sanctioned way to cross between mm:: and Mt*
// — do not memcpy/reinterpret_cast at call sites; a future divergence should
// break here, at one place, not silently at every call site that rolled its
// own copy.
// ---------------------------------------------------------------------------

static_assert(sizeof(Vec2) == sizeof(MtVec2), "mm::Vec2 / MtVec2 size mismatch");
static_assert(offsetof(Vec2, x) == offsetof(MtVec2, x), "mm::Vec2 / MtVec2 member offset mismatch (x)");
static_assert(offsetof(Vec2, y) == offsetof(MtVec2, y), "mm::Vec2 / MtVec2 member offset mismatch (y)");

static_assert(sizeof(Vec3) == sizeof(MtVec3), "mm::Vec3 / MtVec3 size mismatch");
static_assert(offsetof(Vec3, x) == offsetof(MtVec3, x), "mm::Vec3 / MtVec3 member offset mismatch (x)");
static_assert(offsetof(Vec3, y) == offsetof(MtVec3, y), "mm::Vec3 / MtVec3 member offset mismatch (y)");
static_assert(offsetof(Vec3, z) == offsetof(MtVec3, z), "mm::Vec3 / MtVec3 member offset mismatch (z)");

static_assert(sizeof(Vec4) == sizeof(MtVec4), "mm::Vec4 / MtVec4 size mismatch");
static_assert(offsetof(Vec4, x) == offsetof(MtVec4, x), "mm::Vec4 / MtVec4 member offset mismatch (x)");
static_assert(offsetof(Vec4, y) == offsetof(MtVec4, y), "mm::Vec4 / MtVec4 member offset mismatch (y)");
static_assert(offsetof(Vec4, z) == offsetof(MtVec4, z), "mm::Vec4 / MtVec4 member offset mismatch (z)");
static_assert(offsetof(Vec4, w) == offsetof(MtVec4, w), "mm::Vec4 / MtVec4 member offset mismatch (w)");

static_assert(sizeof(Mat4) == sizeof(MtMat4), "mm::Mat4 / MtMat4 size mismatch");
static_assert(offsetof(Mat4, m) == offsetof(MtMat4, m), "mm::Mat4 / MtMat4 member offset mismatch (m)");

inline Vec2 from_c(const MtVec2& v) { return {v.x, v.y}; }
inline MtVec2 to_c(const Vec2& v) { return {v.x, v.y}; }

inline Vec3 from_c(const MtVec3& v) { return {v.x, v.y, v.z}; }
inline MtVec3 to_c(const Vec3& v) { return {v.x, v.y, v.z}; }

inline Vec4 from_c(const MtVec4& v) { return {v.x, v.y, v.z, v.w}; }
inline MtVec4 to_c(const Vec4& v) { return {v.x, v.y, v.z, v.w}; }

inline Mat4 from_c(const MtMat4& in) {
    Mat4 out{};
    for (int i = 0; i < 16; ++i) {
        out.m[i] = in.m[i];
    }
    return out;
}

inline MtMat4 to_c(const Mat4& in) {
    MtMat4 out{};
    for (int i = 0; i < 16; ++i) {
        out.m[i] = in.m[i];
    }
    return out;
}

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
//      identity-on-singular-input behaviour, in the spirit of
//      mat4::Inverted. It is NOT an exact behavioural match, though — see
//      the divergence note above inverse_or_identity()'s definition below
//      before treating this as a drop-in replacement. Do not add a second
//      convenience wrapper that returns zero, or one that echoes the input
//      back — if a call site needs different singular-input behaviour than
//      these two options, that is a sign it should be handling inverse()'s
//      bool itself, not asking for a third wrapper.
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
// value, if elimination hits a pivot that is exactly zero (i.e. singular —
// `pivot == 0.0`, no epsilon: this is a stricter test than an
// "approximately singular" check, not a looser one), or if any resulting
// element would be non-finite.
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
//
// NOT an exact match for libs/SpatialQueryLib/include/tri.h's
// `mat4::Inverted`, despite matching its spirit (lenient, identity-on-
// failure). The two disagree near, but not at, exact singularity:
// `mat4::Inverted` (tri.h:94) rejects when `fabs(pivot) < 1e-8f`; this
// header's `inverse()` (above) rejects only `pivot == 0.0` exactly. Feed
// both a matrix scaled by 1e-9 and `mat4::Inverted` returns identity while
// `inverse_or_identity` succeeds, producing an inverse with ~1e9-magnitude
// entries — stricter-by-zero (rejects less near zero) but more
// permissive-near-zero (accepts input `mat4::Inverted` would treat as
// singular) than `mat4::Inverted`. The effective conditioning guard here is
// not an epsilon on the pivot; it's policy point 3 above — the finite-
// output re-check after elimination, which catches the cases where a
// near-zero pivot actually blew up the result, but not the cases where it
// merely produced a large-but-finite one.
//
// Consequence: migrating a `mat4::Inverted` call site to
// `inverse_or_identity` is a real behaviour change on near-singular input,
// not a like-for-like swap. libs/SpatialQueryLib/src/bvh.cpp:433
// `BVHInstance::SetTransform` (`invTransform = transform.Inverted();`) is
// the call site that will need to account for this when it migrates.
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
//
// raymath's `MatrixMultiply(a, b)` is REVERSED from this: it applies `a`
// first, then `b` (verified by compiled probe: composing
// MatrixMultiply(Translate, Scale) then transforming a point matches
// "translate then scale", not "scale then translate"). In this header's
// terms:
//
//     MatrixMultiply(a, b) == mm::multiply(b, a)
//
// The operands swap sides when a call site migrates. The concrete example:
// MatterEngine3/src/dsl_state.cpp:30-36 and :76 build a transform stack as
// `stack_.back() = MatrixMultiply(<new op>, stack_.back())` (apply the new
// op first, composed onto the existing stack). Ported mechanically that is
// `mm::multiply(stack_.back(), <new op>)` — note `<new op>` and
// `stack_.back()` trade places, they do not stay in the same argument
// position. Every one of dsl_state.cpp's MatrixMultiply(new_op, stack_top)
// call sites needs this same operand swap; a symbol-for-symbol
// find/replace of `MatrixMultiply` -> `mm::multiply` without swapping
// arguments silently reverses the entire DSL transform stack.
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

// Rodrigues' rotation formula about an arbitrary axis. `axis` must be a
// unit vector; returns identity() when it is not (checked via
// `axis.x*axis.x + axis.y*axis.y + axis.z*axis.z` staying within 1e-6 of
// 1, rather than calling dot()/normalize() below, both of which are
// declared later in this header). Without this guard, a degenerate axis --
// e.g. rotation_axis(normalize(tiny), theta) where normalize() (below)
// already collapsed `tiny` to {0,0,0} because |tiny| <= 1e-6 -- makes every
// off-diagonal term of the Rodrigues formula vanish while every diagonal
// term becomes cos(theta), silently producing a uniform-SCALE matrix
// instead of a rotation. An axis of (1,0,0)/(0,1,0)/(0,0,1) reduces exactly
// to rotation_x/y/z(radians) respectively (verified in
// tests/mathlib_tests.cpp).
inline Mat4 rotation_axis(const Vec3& axis, float radians) {
    const float axis_length_squared = axis.x * axis.x + axis.y * axis.y + axis.z * axis.z;
    if (!std::isfinite(axis_length_squared) ||
        std::fabs(axis_length_squared - 1.0f) > 1e-6f) {
        return Mat4{};
    }

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

// ---------------------------------------------------------------------------
// Quaternion algebra -- added in Phase 4 (Step 4) of
// docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md to replace
// libs/MatterSurfaceLib's QuaternionIdentity/QuaternionInvert/
// Vector3RotateByQuaternion call sites (cluster.cpp).
//
// These are ported ELEMENT-FOR-ELEMENT from
// MatterEngine3/src/render/vulkan_only_compat.cpp's QuaternionInvert and
// Vector3RotateByQuaternion -- NOT re-derived, and NOT copied from raymath.h.
// Under MATTER_VULKAN_ONLY (the build this engine actually ships), raymath.h
// is not linked at all; vulkan_only_compat.cpp's hand-written CPU-only
// replacements are the functions that actually run today. They are
// mathematically equivalent to raymath's canonical formulas but not
// guaranteed bit-identical to them (e.g. QuaternionInvert here divides by
// the squared length directly rather than multiplying by its reciprocal), so
// porting the ACTUALLY-EXECUTING implementation, not the "textbook" one, is
// what keeps a migrated call site byte-identical rather than merely
// equally-correct.
// ---------------------------------------------------------------------------

inline Quat quat_identity() { return Quat{}; } // {0,0,0,1}, matches Quat's own default

// General inverse (divides by squared length, so this also handles non-unit
// input, not just the conjugate). Falls back to quat_identity() on a
// zero-length quaternion, matching vulkan_only_compat.cpp's QuaternionInvert
// guard (`n > 0.0f`, not `!= 0.0f`).
inline Quat quat_invert(const Quat& q) {
    const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (n > 0.0f) {
        return Quat{-q.x / n, -q.y / n, -q.z / n, q.w / n};
    }
    return quat_identity();
}

// Hamilton product a*b (raymath's QuaternionMultiply convention). No call
// site in this migration composes two quaternions directly today, but
// quat_invert()/rotate() below are meaningless without a multiply alongside
// them, and the next caller that needs one should not have to hand-roll it.
inline Quat quat_multiply(const Quat& a, const Quat& b) {
    return Quat{
        a.x * b.w + a.w * b.x + a.y * b.z - a.z * b.y,
        a.y * b.w + a.w * b.y + a.z * b.x - a.x * b.z,
        a.z * b.w + a.w * b.z + a.x * b.y - a.y * b.x,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// Rotate v by q. Ported element-for-element from vulkan_only_compat.cpp's
// Vector3RotateByQuaternion (see the section comment above) -- deliberately
// NOT implemented as quat_multiply(quat_multiply(q, {v,0}), quat_invert(q)),
// which would be a different (if mathematically equivalent) sequence of
// float operations and so not guaranteed to round the same way.
inline Vec3 rotate(const Vec3& v, const Quat& q) {
    const Vec3 u{q.x, q.y, q.z};
    const float uv = dot(u, v);
    const float uu = dot(u, u);
    const Vec3 c = cross(u, v);
    return Vec3{
        2.0f * uv * u.x + (q.w * q.w - uu) * v.x + 2.0f * q.w * c.x,
        2.0f * uv * u.y + (q.w * q.w - uu) * v.y + 2.0f * q.w * c.y,
        2.0f * uv * u.z + (q.w * q.w - uu) * v.z + 2.0f * q.w * c.z,
    };
}

} // namespace mm
