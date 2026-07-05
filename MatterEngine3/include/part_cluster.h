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

// Bake-hardening #3: centroid-only variant. Same algorithm as split_clusters
// (recursive longest-axis centroid-median, deterministic tie-break by original
// index), but consumes only per-triangle centroids and produces an out-of-
// place permutation `order[]` — the caller applies the permutation to whichever
// per-triangle stream it manages (tris, tickets into a source pool, etc.).
// Returned Cluster records carry first_tri/tri_count in the FINAL permuted
// layout; aabb_min/max are LEFT ZEROED because per-triangle vertices are not
// available here — the caller must fill them from the materialized geometry
// (see part_flatten.cpp Pass 2).
//
// Byte-identical to split_clusters when fed the same centroid stream: same
// nth_element order (index tie-break), same emission order, same first_tri /
// tri_count. Motivation: streaming flatten builds only the centroids in Pass 1
// (28 bytes/tri) and permutes just the ticket array in Pass 2, instead of
// materializing the whole ~160-bytes/tri Tri+TriEx buffer to feed
// split_clusters.
std::vector<Cluster> split_centroids(const std::vector<float3>& centroids,
                                     std::vector<uint32_t>& order_out,
                                     uint32_t target_tris = 16000);

} // namespace part_cluster
