#pragma once

// MatterEditor/src/view_projection.h
//
// The editor's one CPU pinhole projection: world metres -> pixels, plus the
// row-major object-matrix helpers that feed it.
//
// It exists because two consumers must agree pixel-for-pixel and were about to
// grow two copies of the same math:
//   - selection_outline.cpp draws the frozen-cull frustum onto ImGui's
//     foreground draw list;
//   - capture_annotations.cpp projects the selection's oriented boxes into the
//     pixels of a captured PNG, so an agent can label the image it just read.
// A second copy would drift exactly the way the surface.c copies in this
// repo's history did (see CLAUDE.md), and the failure would be silent: two
// rectangles that disagree by a few pixels look plausible.
//
// Header-only and dependency-free on purpose -- no ImGui, no engine session,
// no Vulkan -- so the annotation projection is unit-testable with no renderer.
//
// CONVENTIONS, and there are two in play that are NOT the same:
//   - `Mat4` here is COLUMN-major, indexed `m[row + col*4]`. look_at,
//     perspective, multiply and project_to_pixels all share it.
//   - `transform_point` / `obb_corners` take a ROW-major 4x4 with translation
//     in elements 3, 7 and 11 -- the layout of
//     `SelectionBounds::world_matrix`. Nothing here converts between the two;
//     do not pass a Mat4 to those.
//
// This is a plain OpenGL-style pinhole model. It deliberately does not
// reproduce the renderer's reversed-Z projection or its temporal jitter: both
// move a projected point by well under a pixel at any distance worth
// inspecting, and reproducing them would couple this file to the renderer.

#include <cmath>

namespace viewer::projection {

// COLUMN-major 4x4, indexed `m[row + col*4]`.
struct Mat4 { float m[16]; };

inline Mat4 look_at(const float eye[3], const float target[3],
                    const float up[3]) {
    float f[3] = {target[0]-eye[0], target[1]-eye[1], target[2]-eye[2]};
    float fl = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    f[0]/=fl; f[1]/=fl; f[2]/=fl;
    float r[3] = {f[1]*up[2] - f[2]*up[1], f[2]*up[0] - f[0]*up[2],
                  f[0]*up[1] - f[1]*up[0]};
    float rl = std::sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    r[0]/=rl; r[1]/=rl; r[2]/=rl;
    float u[3] = {r[1]*f[2] - r[2]*f[1], r[2]*f[0] - r[0]*f[2],
                  r[0]*f[1] - r[1]*f[0]};
    Mat4 out{};
    out.m[0]=r[0]; out.m[4]=r[1]; out.m[8]=r[2];  out.m[12]=-(r[0]*eye[0]+r[1]*eye[1]+r[2]*eye[2]);
    out.m[1]=u[0]; out.m[5]=u[1]; out.m[9]=u[2];  out.m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    out.m[2]=-f[0]; out.m[6]=-f[1]; out.m[10]=-f[2]; out.m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    out.m[3]=0; out.m[7]=0; out.m[11]=0; out.m[15]=1;
    return out;
}

inline Mat4 perspective(float fov_y, float aspect, float near_p, float far_p) {
    float t = std::tan(fov_y * 0.5f);
    Mat4 out{};
    out.m[0] = 1.0f / (aspect * t);
    out.m[5] = 1.0f / t;
    out.m[10] = -(far_p + near_p) / (far_p - near_p);
    out.m[11] = -1.0f;
    out.m[14] = -(2.0f * far_p * near_p) / (far_p - near_p);
    return out;
}

inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += a.m[row + k*4] * b.m[k + col*4];
            out.m[row + col*4] = sum;
        }
    return out;
}

// World point -> pixel inside a `width` x `height` rectangle whose top-left
// corner is at (`off_x`, `off_y`). Returns false for a point at or behind the
// eye (clip w <= 0.001), leaving both outputs untouched -- callers drop the
// line or box rather than drawing a wild segment across the view. Y is flipped
// for the top-left pixel origin both ImGui and a decoded PNG use.
inline bool project_to_pixels(const Mat4& vp, float width, float height,
                              float off_x, float off_y, const float p[3],
                              float& out_x, float& out_y) {
    float x = vp.m[0]*p[0] + vp.m[4]*p[1] + vp.m[8]*p[2] + vp.m[12];
    float y = vp.m[1]*p[0] + vp.m[5]*p[1] + vp.m[9]*p[2] + vp.m[13];
    float w = vp.m[3]*p[0] + vp.m[7]*p[1] + vp.m[11]*p[2] + vp.m[15];
    if (w <= 0.001f) return false;
    x /= w; y /= w;
    out_x = (x * 0.5f + 0.5f) * width + off_x;
    out_y = (1.0f - (y * 0.5f + 0.5f)) * height + off_y;
    return true;
}

// Apply a ROW-major 4x4 (translation in elements 3, 7, 11) to a point,
// assuming the bottom row is [0 0 0 1]. Do NOT pass a `Mat4` here.
inline void transform_point(const float mat[16], const float in[3],
                            float out[3]) {
    out[0] = mat[0]*in[0] + mat[1]*in[1] + mat[2]*in[2]  + mat[3];
    out[1] = mat[4]*in[0] + mat[5]*in[1] + mat[6]*in[2]  + mat[7];
    out[2] = mat[8]*in[0] + mat[9]*in[1] + mat[10]*in[2] + mat[11];
}

// Expand a local-space AABB into its 8 world-space corners through a ROW-major
// matrix. Corner order is a contract: 0-3 walk the min-z face, 4-7 the
// matching max-z face, so the 12 box edges are {0,1},{1,2},{2,3},{3,0},
// {4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}.
inline void obb_corners(const float mn[3], const float mx[3],
                        const float mat[16], float out[8][3]) {
    const float local[8][3] = {
        {mn[0],mn[1],mn[2]}, {mx[0],mn[1],mn[2]},
        {mx[0],mx[1],mn[2]}, {mn[0],mx[1],mn[2]},
        {mn[0],mn[1],mx[2]}, {mx[0],mn[1],mx[2]},
        {mx[0],mx[1],mx[2]}, {mn[0],mx[1],mx[2]},
    };
    for (int i = 0; i < 8; ++i) transform_point(mat, local[i], out[i]);
}

}  // namespace viewer::projection
