// Frustum-cull + per-cluster LOD helpers shared by raster_composer.cpp and
// the viewer logic tests. All matrices are the engine's float[16] instance
// transforms (see part_graph manifest placements).
//
// Also provides the frustum/matrix math (mul16, make_lookat, make_perspective,
// extract_frustum_planes, camera_frustum_planes_raw) that was formerly in
// raster_composer.cpp. These are GL-free so they can be used by headless tests
// and the upcoming GpuCuller compute path.
#pragma once

#include "part_store.h"
#include <cmath>

namespace viewer {

// ---------------------------------------------------------------------------
// Row-major 4x4 multiply (same convention as world_composer.cpp:9).
// ---------------------------------------------------------------------------
inline void mul16(const float* a, const float* b, float* out) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i*4+k] * b[k*4+j];
            out[i*4+j] = s;
        }
}

// Build a view matrix stored row-major in a float[16].
// The C++ side composes matrices with row-major multiplies; row_major_to_matrix
// memcpys these 16 floats into raylib's Matrix, whose field layout the GLSL shader
// reads as column-major — the memcpy acts as an implicit transpose, so the shader
// receives the standard column-vector look-at (model * vec4(...) convention).
inline void make_lookat(const float eye[3], const float target[3],
                        const float up[3], float out[16]) {
    // Forward = normalize(target - eye)
    float fx = target[0]-eye[0], fy = target[1]-eye[1], fz = target[2]-eye[2];
    float flen = std::sqrt(fx*fx + fy*fy + fz*fz);
    if (flen < 1e-9f) flen = 1.0f;
    fx /= flen; fy /= flen; fz /= flen;

    // Right = normalize(forward x up)
    float rx = fy*up[2] - fz*up[1];
    float ry = fz*up[0] - fx*up[2];
    float rz = fx*up[1] - fy*up[0];
    float rlen = std::sqrt(rx*rx + ry*ry + rz*rz);
    if (rlen < 1e-9f) rlen = 1.0f;
    rx /= rlen; ry /= rlen; rz /= rlen;

    // True up = right x forward
    float ux = ry*fz - rz*fy;
    float uy = rz*fx - rx*fz;
    float uz = rx*fy - ry*fx;

    // Row-major storage: out[i*4+j] = M[row=i, col=j].
    // After memcpy-transpose via row_major_to_matrix, the shader sees column-major layout:
    // col0=(right), col1=(up), col2=(-forward), col3=(translation) — standard GL column-vector look-at.
    float dot_re = rx*eye[0]+ry*eye[1]+rz*eye[2];
    float dot_ue = ux*eye[0]+uy*eye[1]+uz*eye[2];
    float dot_fe = fx*eye[0]+fy*eye[1]+fz*eye[2];
    out[0]=rx;   out[1]=ux;   out[2]=-fx;  out[3]=0;
    out[4]=ry;   out[5]=uy;   out[6]=-fy;  out[7]=0;
    out[8]=rz;   out[9]=uz;   out[10]=-fz; out[11]=0;
    out[12]=-dot_re; out[13]=-dot_ue; out[14]=dot_fe; out[15]=1;
}

// Build a perspective projection matrix stored row-major in a float[16].
// row_major_to_matrix memcpy-transposes it into raylib's Matrix so the shader
// receives the standard column-vector OpenGL projection (gl_Position = mvp * world).
// After the implicit transpose: w_clip = -v_view.z, z_clip = v_view.z*(-(f+n)/(f-n)) - 2fn/(f-n).
inline void make_perspective(float fovy_deg, float aspect,
                              float near_z, float far_z, float out[16]) {
    const float pi = 3.14159265358979323846f;
    float fovy_rad = fovy_deg * pi / 180.0f;
    float f = 1.0f / std::tan(fovy_rad * 0.5f);
    float d = far_z - near_z;
    // Row-major storage; after memcpy-transpose via row_major_to_matrix the shader receives the standard GL column-vector projection:
    out[ 0]=f/aspect; out[ 1]=0; out[ 2]=0;                   out[ 3]=0;
    out[ 4]=0;        out[ 5]=f; out[ 6]=0;                   out[ 7]=0;
    out[ 8]=0;        out[ 9]=0; out[10]=-(far_z+near_z)/d;   out[11]=-1.0f;
    out[12]=0;        out[13]=0; out[14]=-(2.0f*far_z*near_z)/d; out[15]=0;
}

// Extract 6 frustum planes from the row-major C++ VP matrix (Gribb-Hartmann).
// planes[0..5][4] = {a,b,c,d}; point p is inside plane when dot(p,{a,b,c})+d >= 0.
//
// The C++ VP is stored row-major: vp[i*4+j] = VP[row=i, col=j].
// For a column-vector clip transform (v_clip = VP_shader * v_world), the shader-side
// matrix is the transpose of the C++ VP — so VP_shader's rows are the C++ VP's columns.
// Gribb-Hartmann plane extraction reads VP_shader's rows, i.e. C++ VP's columns:
//   col0 = {vp[0],vp[4],vp[8],vp[12]}, col3 = {vp[3],vp[7],vp[11],vp[15]}, etc.
// Left (x+w>=0): col0+col3,  Right (w-x>=0): col3-col0,
// Bottom (y+w>=0): col1+col3, Top (w-y>=0): col3-col1,
// Near (z+w>=0): col2+col3,  Far (w-z>=0): col3-col2.
inline void extract_frustum_planes(const float vp[16], float planes[6][4]) {
    // col0 = vp[0],vp[4],vp[8],vp[12]
    // col1 = vp[1],vp[5],vp[9],vp[13]
    // col2 = vp[2],vp[6],vp[10],vp[14]
    // col3 = vp[3],vp[7],vp[11],vp[15]
    // left  = col0 + col3
    planes[0][0]=vp[0]+vp[3];  planes[0][1]=vp[4]+vp[7];
    planes[0][2]=vp[8]+vp[11]; planes[0][3]=vp[12]+vp[15];
    // right = col3 - col0
    planes[1][0]=vp[3]-vp[0];  planes[1][1]=vp[7]-vp[4];
    planes[1][2]=vp[11]-vp[8]; planes[1][3]=vp[15]-vp[12];
    // bottom = col1 + col3
    planes[2][0]=vp[1]+vp[3];  planes[2][1]=vp[5]+vp[7];
    planes[2][2]=vp[9]+vp[11]; planes[2][3]=vp[13]+vp[15];
    // top = col3 - col1
    planes[3][0]=vp[3]-vp[1];  planes[3][1]=vp[7]-vp[5];
    planes[3][2]=vp[11]-vp[9]; planes[3][3]=vp[15]-vp[13];
    // near = col2 + col3
    planes[4][0]=vp[2]+vp[3];  planes[4][1]=vp[6]+vp[7];
    planes[4][2]=vp[10]+vp[11];planes[4][3]=vp[14]+vp[15];
    // far  = col3 - col2
    planes[5][0]=vp[3]-vp[2];  planes[5][1]=vp[7]-vp[6];
    planes[5][2]=vp[11]-vp[10];planes[5][3]=vp[15]-vp[14];
}

// Build the 6 frustum planes from raw float eye/target/up + fovy.
// GL-free: no Camera3D. near/far are the engine defaults (0.05, 4000).
// raster_composer.cpp wraps this with a Camera3D-unpacking shim.
inline void camera_frustum_planes_raw(const float eye[3], const float target[3],
                                      const float up[3], float fovy_deg, float aspect,
                                      float planes[6][4]) {
    const float near_z = 0.05f, far_z = 4000.0f;
    float view[16], proj[16], vp[16];
    make_lookat(eye, target, up, view);
    make_perspective(fovy_deg, aspect, near_z, far_z, proj);
    mul16(view, proj, vp);
    extract_frustum_planes(vp, planes);
}

// Transform a point by an engine instance transform. These are affine
// column-vector matrices stored row-major: v_out = M * [x,y,z,1], with
// translation at m[3], m[7], m[11] (what mk_entry writes and
// sector_grid::instance_position reads). No projective row, so no w divide.
inline void transform_point(const float m[16], float x, float y, float z,
                            float& ox, float& oy, float& oz) {
    ox = x*m[0] + y*m[1] + z*m[2]  + m[3];
    oy = x*m[4] + y*m[5] + z*m[6]  + m[7];
    oz = x*m[8] + y*m[9] + z*m[10] + m[11];
}

// Returns true if the AABB (in local space), transformed by inst_transform,
// is entirely outside any of the 6 frustum planes (culled).
// We transform all 8 corners to world space and test.
inline bool aabb_culled(const float aabb_min[3], const float aabb_max[3],
                        const float inst[16], const float planes[6][4]) {
    // Build 8 corners in local space
    float cx[2] = { aabb_min[0], aabb_max[0] };
    float cy[2] = { aabb_min[1], aabb_max[1] };
    float cz[2] = { aabb_min[2], aabb_max[2] };

    // Transform 8 corners to world space
    float wx[8], wy[8], wz[8];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k) {
                int idx = i*4 + j*2 + k;
                transform_point(inst, cx[i], cy[j], cz[k], wx[idx], wy[idx], wz[idx]);
            }

    // For each plane: if ALL corners are outside (negative side), the AABB is culled.
    for (int p = 0; p < 6; ++p) {
        float a = planes[p][0], b = planes[p][1], c = planes[p][2], d = planes[p][3];
        bool all_outside = true;
        for (int ci = 0; ci < 8; ++ci) {
            if (a*wx[ci] + b*wy[ci] + c*wz[ci] + d >= 0.0f) {
                all_outside = false;
                break;
            }
        }
        if (all_outside) return true;   // culled
    }
    return false;   // not culled (visible)
}

// Extract the uniform scale from an instance transform (for projected-size
// LOD). Basis vectors are the columns in this convention.
inline float inst_scale(const float m[16]) {
    float sx = std::sqrt(m[0]*m[0] + m[4]*m[4] + m[8]*m[8]);
    float sy = std::sqrt(m[1]*m[1] + m[5]*m[5] + m[9]*m[9]);
    float sz = std::sqrt(m[2]*m[2] + m[6]*m[6] + m[10]*m[10]);
    return (sx + sy + sz) * (1.0f / 3.0f);   // average; clusters use same metric
}

// Select per-cluster LOD level: same formula as lod_select.cpp.
// projected_size = cluster.radius * scale / max(dist, 0.01) * pixel_budget
// pixel_budget is the runtime quality/speed dial (Stage 2); default 1.0 is
// bit-identical to the pre-budget behaviour.
// Pick coarsest level whose threshold <= projected_size (thresholds fine->coarse).
inline int cluster_lod_select(const LoadedCluster& cl,
                              const float* inst,
                              const float* cam_eye,
                              float pixel_budget = 1.0f) {
    float scale = inst_scale(inst);
    // Cluster center = midpoint of AABB, transformed to world space.
    float lcx = (cl.aabb_min[0] + cl.aabb_max[0]) * 0.5f;
    float lcy = (cl.aabb_min[1] + cl.aabb_max[1]) * 0.5f;
    float lcz = (cl.aabb_min[2] + cl.aabb_max[2]) * 0.5f;
    float wcx, wcy, wcz;
    transform_point(inst, lcx, lcy, lcz, wcx, wcy, wcz);
    float dx = wcx - cam_eye[0], dy = wcy - cam_eye[1], dz = wcz - cam_eye[2];
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (dist < 0.01f) dist = 0.01f;
    float psize = cl.radius * scale / dist * pixel_budget;

    // Same as lod_select::select_level: iterate fine->coarse, pick first >= threshold.
    const auto& thr = cl.thresholds;
    if (thr.empty()) return 0;
    for (size_t i = 0; i < thr.size(); ++i)
        if (psize >= thr[i]) return (int)i;
    return (int)thr.size() - 1;   // coarsest
}

} // namespace viewer
