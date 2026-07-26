#ifndef MATTER_MATH_C_H
#define MATTER_MATH_C_H

// libs/MathLib/include/matter_math_c.h
//
// C-compatible POD math types: the C-boundary counterpart to matter_math.h's
// mm::Vec2/Vec3/Vec4/Mat4. Added in Phase 4 (Step 2) of
// docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md to unblock
// removing raylib from libs/MatterSurfaceLib/include/particle.h and
// fat_primitive.h.
//
// WHY THIS HEADER EXISTS SEPARATELY FROM matter_math.h:
// particle.h and fat_primitive.h are shared between real C (surface.c does
// field evaluation; fat_primitive.c does the SDF math) and C++
// (csg_lowering.cpp, cell.cpp, cluster.cpp). fat_primitive.h says so
// explicitly. mm::Vec3 in matter_math.h uses C++ default member
// initializers (`float x = 0.0f;`) and lives in a namespace -- neither is
// valid C, so that header cannot be included from a .c translation unit.
// This header can: plain C structs, no initializers, no namespace, wrapped
// in `extern "C"` when seen by a C++ compiler so the mangling matches what a
// C translation unit produces for any (today, there are none) free
// functions declared against these types.
//
// LAYOUT: byte-identical to matter_math.h's mm::Vec2/Vec3/Vec4/Mat4, and (by
// the transitive proof in that header's Mat4 comment) to raylib's
// Vector2/Vector3/Vector4/Matrix. matter_math.h static_asserts the
// mm:: <-> Mt* size and member-offset equivalence and provides to_c()/
// from_c() conversions; this header intentionally does NOT depend on
// matter_math.h (or anything else) so it stays includable from a plain .c
// file compiled with a C, not C++, compiler.
//
// MtMat4 uses the SAME row-major float[16] convention as mm::Mat4: element
// (row, col) lives at m[row*4 + col], translation at m[3]/m[7]/m[11]. See
// matter_math.h's Mat4 comment for the full derivation -- it applies
// unchanged here since this is the identical memory layout.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y;
} MtVec2;

typedef struct {
    float x, y, z;
} MtVec3;

typedef struct {
    float x, y, z, w;
} MtVec4;

// Row-major float[16]; see the file comment above for the layout convention.
typedef struct {
    float m[16];
} MtMat4;

#ifdef __cplusplus
}
#endif

#endif // MATTER_MATH_C_H
