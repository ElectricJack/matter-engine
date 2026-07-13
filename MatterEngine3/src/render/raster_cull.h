// Frustum-cull + per-cluster LOD helpers shared by the raster path and tests.
// Camera construction lives in frame_matrices; persisted float[16] transforms
// enter here only at explicit CPU boundaries and keep their serialized layout.
#pragma once

#include "matrix_math.h"
#include "part_store.h"

#include <cmath>
#include <cstring>

namespace viewer {

inline matter::Mat4f persisted_mat4(const float source[16]) {
    matter::Mat4f matrix{};
    std::memcpy(matrix.m, source, sizeof matrix.m);
    return matrix;
}

// Returns true if the transformed AABB is entirely outside any frustum plane.
inline bool aabb_culled(const float aabb_min[3], const float aabb_max[3],
                        const float persisted_transform[16],
                        const float planes[6][4]) {
    const matter::Mat4f object_to_world = persisted_mat4(persisted_transform);
    float cx[2] = {aabb_min[0], aabb_max[0]};
    float cy[2] = {aabb_min[1], aabb_max[1]};
    float cz[2] = {aabb_min[2], aabb_max[2]};

    matter::Float3 world_corners[8]{};
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k) {
                const int index = i * 4 + j * 2 + k;
                world_corners[index] =
                    transform_point(object_to_world, {cx[i], cy[j], cz[k]});
            }

    for (int plane = 0; plane < 6; ++plane) {
        bool all_outside = true;
        for (const matter::Float3& corner : world_corners) {
            if (planes[plane][0] * corner.x + planes[plane][1] * corner.y +
                    planes[plane][2] * corner.z + planes[plane][3] >= 0.0f) {
                all_outside = false;
                break;
            }
        }
        if (all_outside) return true;
    }
    return false;
}

// Extract uniform scale from the canonical CPU transform. Basis vectors are
// columns, and serialized translation remains at [3], [7], [11].
inline float inst_scale(const matter::Mat4f& matrix) {
    const float sx = std::sqrt(matrix.m[0] * matrix.m[0] +
                               matrix.m[4] * matrix.m[4] +
                               matrix.m[8] * matrix.m[8]);
    const float sy = std::sqrt(matrix.m[1] * matrix.m[1] +
                               matrix.m[5] * matrix.m[5] +
                               matrix.m[9] * matrix.m[9]);
    const float sz = std::sqrt(matrix.m[2] * matrix.m[2] +
                               matrix.m[6] * matrix.m[6] +
                               matrix.m[10] * matrix.m[10]);
    return (sx + sy + sz) * (1.0f / 3.0f);
}

inline int cluster_lod_select(const LoadedCluster& cluster,
                              const float persisted_transform[16],
                              const float camera_eye[3],
                              float pixel_budget = 1.0f) {
    const matter::Mat4f object_to_world = persisted_mat4(persisted_transform);
    const float scale = inst_scale(object_to_world);
    const matter::Float3 local_center{
        (cluster.aabb_min[0] + cluster.aabb_max[0]) * 0.5f,
        (cluster.aabb_min[1] + cluster.aabb_max[1]) * 0.5f,
        (cluster.aabb_min[2] + cluster.aabb_max[2]) * 0.5f};
    const matter::Float3 world_center = transform_point(object_to_world, local_center);
    const float dx = world_center.x - camera_eye[0];
    const float dy = world_center.y - camera_eye[1];
    const float dz = world_center.z - camera_eye[2];
    float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance < 0.01f) distance = 0.01f;
    const float projected_size =
        cluster.radius * scale / distance * pixel_budget;

    const auto& thresholds = cluster.thresholds;
    if (thresholds.empty()) return 0;
    for (size_t index = 0; index < thresholds.size(); ++index)
        if (projected_size >= thresholds[index]) return static_cast<int>(index);
    return static_cast<int>(thresholds.size()) - 1;
}

} // namespace viewer
