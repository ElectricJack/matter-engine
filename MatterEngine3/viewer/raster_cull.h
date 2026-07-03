// Frustum-cull + per-cluster LOD helpers shared by raster_composer.cpp and
// the viewer logic tests. All matrices are the engine's float[16] instance
// transforms (see part_graph manifest placements).
#pragma once

#include "part_store.h"
#include <cmath>

namespace viewer {

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
