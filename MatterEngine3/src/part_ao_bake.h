#pragma once
#include <cstdint>
#include <vector>
#include "precomp.h"   // Tri, TriEx, float3 (match includes used by lod_bake.h)
#include "bvh.h"       // BVH, BvhMesh, BVHRay, Tri, TriEx

namespace part_ao {

struct AoBakeParams {
    float quality = 1.0f;          // 0 disables; rays/vertex = clamp(quality*32, 4, 128)
    float radius = 2.0f;           // max occlusion reach (world units)
    uint64_t max_total_rays = 8000000;  // adaptive cap: scales rays/vertex down
};

struct AoBakeStats {
    uint32_t rays_per_vertex = 0;  // after adaptive scaling
    uint64_t unique_positions = 0;
};

// Bakes ao0/ao1/ao2 in place across all groups of one part. group_tris and
// group_triex are parallel; each triex[i] is parallel to tris[i]. Geometry is
// combined into one BVH; occlusion is part-local. Deterministic: identical
// inputs yield identical ao bytes. quality <= 0 leaves triex untouched.
void bake_part_ao(const std::vector<const std::vector<Tri>*>& group_tris,
                  const std::vector<std::vector<TriEx>*>& group_triex,
                  const AoBakeParams& params, AoBakeStats* stats = nullptr);

}  // namespace part_ao
