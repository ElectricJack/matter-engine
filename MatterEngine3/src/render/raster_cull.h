// Frustum-cull + per-cluster LOD helpers shared by the raster path and tests.
// Camera construction lives in frame_matrices; persisted float[16] transforms
// enter here only at explicit CPU boundaries and keep their serialized layout.
//
// Header-only inline free functions, so the test binaries can link them
// without pulling in the renderer. Today's only callers are
// MatterEngine3/tests/partstore_tests.cpp — see the note at the bottom of this
// file for what was deleted. The SHIPPING frustum and LOD decisions are made
// on the GPU (shaders_vk/cull.comp) against the single LOD rule in
// MatterEngine3/src/render/lod_distance.h; nothing here participates in a
// production frame.
//
// Conventions: a `persisted_transform` is the serialized `float[16]` an
// artifact or instance record stores, and it is the same row-major /
// column-vector layout as matter::Mat4f (matter/math_types.h) — translation at
// [3], [7], [11]. `aabb_min`/`aabb_max` are in the object space that transform
// maps to world. `planes` is exactly what extract_frustum_planes_zo produces:
// six normalized, INWARD-facing planes, so `dot(plane.xyz, p) + plane.w >= 0`
// means inside. Lengths are world metres.
#pragma once

#include "matrix_math.h"
#include "part_store.h"

#include <cmath>
#include <cstring>

namespace viewer {

// Reinterpret a serialized float[16] as a matter::Mat4f. A straight memcpy —
// the two layouts are identical, so this is a type change, never a transpose.
inline matter::Mat4f persisted_mat4(const float source[16]) {
    matter::Mat4f matrix{};
    std::memcpy(matrix.m, source, sizeof matrix.m);
    return matrix;
}

// Returns true if the transformed AABB is entirely outside any frustum plane.
// Conservative in the safe direction: it culls only when all eight transformed
// corners are outside ONE plane, so a box that is outside the frustum while
// straddling several planes survives (a false negative — extra work, never a
// missing object). Rebuilds the Mat4f and transforms all eight corners on
// every call; cost is 8 transform_point plus up to 48 dot products.
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
// Returns the MEAN of the three basis lengths, so a non-uniform scale is
// averaged into a single number rather than detected or rejected, and a
// mirrored (negative-determinant) transform still reports a positive scale
// because each axis contributes a length. Shear is not accounted for.
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

// cluster_lod_select lived here and was the GL path's per-cluster LOD pick. It
// was deleted in M0 with the rest of that path (no caller outside its own
// tests). The shipping selector is the GPU cull shader; the Representation
// migration replaces both with one distance function (see
// docs/lod-vt-redesign-2026-08-04.md S5). The helpers above are still used by
// the test suites and stay.

} // namespace viewer
