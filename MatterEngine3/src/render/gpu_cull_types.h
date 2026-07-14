#ifndef VIEWER_GPU_CULL_TYPES_H
#define VIEWER_GPU_CULL_TYPES_H
#include "gpu_matrix_pack.h"
#include "part_store.h"
#include "vk_draw_command.h"
#include <cstdint>
#include <cstring>

namespace viewer {

constexpr int kMaxLod = 9;   // ratio-2 ladder max rung count (frame-time package Stage 2)

// std430 mirror of cull.comp's ClusterMeta. 128 B; field order must match the GLSL.
struct GpuClusterMeta {
    float aabb_min[3]; float radius;
    float aabb_max[3]; float pad0;
    float thresholds[kMaxLod];        // finest -> coarsest; unused tail = +inf
    uint32_t lod_mesh_idx[kMaxLod];   // index into the part's MeshRange table; tail = 0
    uint32_t lod_count;
    uint32_t part_slot;               // dense GpuCuller slot, NOT part_hash
    uint32_t cluster_index;           // debug
    uint32_t pad1[3];                 // pad struct to 128 (vec4 alignment)
};
static_assert(sizeof(GpuClusterMeta) == 128, "must match std430 layout in cull.comp");

// std430 mirror of cull.comp's GpuInstance. 80 B.
struct GpuInstanceRec {
    GpuMat4 object_to_world;  // explicit GLSL column-major packing
    uint32_t part_slot;
    uint32_t base_lod;        // debug/HUD only; cluster-level selection is authoritative
    uint32_t cluster_start;   // global ClusterMeta index of this part's first cluster
    uint32_t cluster_count;
};
static_assert(sizeof(GpuInstanceRec) == 80, "must match std430 layout in cull.comp");

// glMultiDrawArraysIndirect command layout (GL spec order).
struct DrawArraysCmd { uint32_t count, instance_count, first, base_instance; };
static_assert(sizeof(DrawArraysCmd) == 16, "GL DrawArraysIndirectCommand");

inline GpuInstanceRec pack_instance(const float source[16]) {
    matter::Mat4f matrix{};
    std::memcpy(matrix.m, source, sizeof matrix.m);
    GpuInstanceRec packed{};
    packed.object_to_world = pack_glsl_mat4(matrix);
    return packed;
}

inline GpuClusterMeta pack_cluster(const LoadedCluster& cl, uint32_t part_slot,
                                   uint32_t cluster_index) {
    GpuClusterMeta m{};
    for (int i = 0; i < 3; ++i) { m.aabb_min[i] = cl.aabb_min[i]; m.aabb_max[i] = cl.aabb_max[i]; }
    m.radius = cl.radius;
    uint32_t n = (uint32_t)cl.thresholds.size();
    if (n > (uint32_t)kMaxLod) n = kMaxLod;   // spec: MAX_LOD=9 covers the current ladder
    for (uint32_t i = 0; i < (uint32_t)kMaxLod; ++i) {
        m.thresholds[i]  = (i < n) ? cl.thresholds[i] : 3.402823e38f;  // +inf-ish: never selected
        m.lod_mesh_idx[i] = (i < n) ? (uint32_t)cl.lod_mesh[i] : 0;
    }
    m.lod_count = n;
    m.part_slot = part_slot;
    m.cluster_index = cluster_index;
    return m;
}

// Clusterless (whole-part) path: one synthetic cluster covering the part.
// Thresholds come from the part-level ladder, so the cull shader's selection
// formula (identical to lod_select.cpp) reproduces the resolver's pick.
inline GpuClusterMeta pack_whole_part(const LoadedPart& lp, uint32_t part_slot) {
    GpuClusterMeta m{};
    float r = lp.bound_radius;
    for (int i = 0; i < 3; ++i) { m.aabb_min[i] = -r; m.aabb_max[i] = r; }
    m.radius = r;
    uint32_t n = (uint32_t)lp.thresholds.size();
    if (n > (uint32_t)kMaxLod) n = kMaxLod;
    if (n == 0) { n = 1; m.thresholds[0] = 0.0f; m.lod_mesh_idx[0] = 0; }
    else for (uint32_t i = 0; i < n; ++i) {
        m.thresholds[i]   = lp.thresholds[i];
        m.lod_mesh_idx[i] = (i < lp.lod_mesh_data.size()) ? i : (n - 1);
    }
    // Fill the tail [n..kMaxLod) unconditionally to ensure unused slots are +inf.
    for (uint32_t i = n; i < (uint32_t)kMaxLod; ++i) {
        m.thresholds[i] = 3.402823e38f;
        m.lod_mesh_idx[i] = 0;
    }
    m.lod_count = n;
    m.part_slot = part_slot;
    m.cluster_index = 0xFFFFFFFFu;   // marks synthetic
    return m;
}

} // namespace viewer
#endif
