#pragma once
// Spatial cluster split for flattened meshes: recursive longest-axis centroid
// median split until every cluster is <= target_tris. Deterministic.
#include "bvh.h"        // Tri, TriEx (MSL include path)
#include <cstdint>
#include <vector>

namespace part_cluster {

struct Cluster {
    uint32_t first_tri = 0;      // range into the REORDERED tri array
    uint32_t tri_count = 0;
    float aabb_min[3] = {0,0,0};
    float aabb_max[3] = {0,0,0}; // vertex AABB (not centroid AABB) of the range
};

// Reorders tris (and triex in parallel, when non-empty — sizes must match) so
// each cluster is one contiguous range. Returns clusters in emission order.
// count <= target_tris => exactly one cluster covering everything.
std::vector<Cluster> split_clusters(std::vector<Tri>& tris,
                                    std::vector<TriEx>& triex,
                                    uint32_t target_tris = 16000);

} // namespace part_cluster
