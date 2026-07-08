// mat_math.h — internal row-major matrix helpers shared across MatterEngine3 TUs.
//
// ALL functions are inline (header-only).  Include in an anonymous namespace
// or a named namespace in each .cpp that needs them to avoid ODR issues with
// the static-by-default convention used elsewhere in this codebase.
//
// Covered helpers:
//   mul16       — row-major 4×4 float matrix multiply (a*b -> out)
//   NormalMat   — inverse-transpose upper-3×3 for normal transformation
//
// NOT covered here (operate on different types / layouts — kept local):
//   csg_lowering.cpp  mat_invert / mat_mul  — raylib column-major Matrix
//   world_flatten.cpp mat4_mul              — mat4 struct (row-major but separate type)
//   tileset_bake.cpp  mat_to_pose (Shepperd) — full TRS extract + orthonorm validation
//   tileset_settle.cpp axes_to_quat (Shepperd) — column-axes convention, different input

#pragma once

#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// mul16 — row-major 4×4 multiply.
// Element [r][c] = m[r*4 + c].  Translation lives in m[3], m[7], m[11].
// Same convention as ChildInstance transforms and WorldComposer.
// ---------------------------------------------------------------------------
inline void mul16(const float* a, const float* b, float* out) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i*4+k] * b[k*4+j];
            out[i*4+j] = s;
        }
}

// ---------------------------------------------------------------------------
// NormalMat — inverse-transpose of the upper 3×3 for shading normals under
// non-uniform scale.  Falls back to the raw 3×3 when the matrix is singular.
//
// After construction, apply() maps a local-space normal to world space and
// normalises the result.  Both in and out are raw float[3] arrays.
//
// NOTE: part_flatten.cpp uses a float3 version of apply(); call sites there
// convert via local wrappers in that TU rather than diverging the math here.
// ---------------------------------------------------------------------------
struct NormalMat {
    float n[9];

    explicit NormalMat(const float* m) {
        const float a=m[0], b=m[1], c=m[2],
                    d=m[4], e=m[5], f=m[6],
                    g=m[8], h=m[9], i=m[10];
        const float A =  (e*i - f*h), B = -(d*i - f*g), C =  (d*h - e*g);
        const float det = a*A + b*B + c*C;
        if (std::fabs(det) < 1e-12f) {
            // Singular fallback: use the raw 3×3
            n[0]=a; n[1]=b; n[2]=c;
            n[3]=d; n[4]=e; n[5]=f;
            n[6]=g; n[7]=h; n[8]=i;
            return;
        }
        // Cofactor / det gives M^-1; store its transpose in n[].
        const float id = 1.0f / det;
        float tmp[9];
        tmp[0] = A*id;             tmp[3] = -(b*i - c*h)*id;  tmp[6] =  (b*f - c*e)*id;
        tmp[1] = B*id;             tmp[4] =  (a*i - c*g)*id;  tmp[7] = -(a*f - c*d)*id;
        tmp[2] = C*id;             tmp[5] = -(a*h - b*g)*id;  tmp[8] =  (a*e - b*d)*id;
        std::swap(tmp[1], tmp[3]); std::swap(tmp[2], tmp[6]); std::swap(tmp[5], tmp[7]);
        std::memcpy(n, tmp, 36);
    }

    // Transform local normal v[3] to world space and normalise into out[3].
    void apply(const float v[3], float out[3]) const {
        out[0] = n[0]*v[0] + n[1]*v[1] + n[2]*v[2];
        out[1] = n[3]*v[0] + n[4]*v[1] + n[5]*v[2];
        out[2] = n[6]*v[0] + n[7]*v[1] + n[8]*v[2];
        float len = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
        if (len > 1e-12f) { out[0]/=len; out[1]/=len; out[2]/=len; }
    }
};
