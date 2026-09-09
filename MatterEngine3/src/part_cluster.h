#pragma once
// MatterEngine3/src/part_cluster.h
//
// Spatial cluster split for flattened meshes: recursive longest-axis centroid
// median split until every cluster is <= target_tris. Deterministic.
//
// WHERE IT SITS. The only consumer is the bake-time flattener
// (MatterEngine3/src/part_flatten.cpp). Once a root part's whole subtree has
// been merged into one world-space triangle soup, the soup is cut here into
// clusters, and each cluster then gets its own LOD ladder and its own
// part_asset::FlatCluster record in the .flat.part. Clusters are the unit the
// renderer culls and selects a LOD for, so `target_tris` is a granularity /
// draw-call trade, not a correctness knob. Depends only on tri.h
// (SpatialQueryLib's `Tri`/`TriEx`/`float3`) — no engine headers, no GL.
//
// WHICH ENTRY POINT. `split_centroids` is what the engine bakes with today;
// `split_clusters` is the in-place variant and now has only test callers. Fed
// the same centroid stream they emit identical cluster records.
//
// DETERMINISM IS THE POINT. A flat artifact is content-addressed by its root
// hash and served from a warm cache, so two bakes of the same subtree must
// produce the same bytes. Both entry points therefore tie-break equal centroids
// by the triangle's ORIGINAL index. Changing the split rule changes the
// contents of every flat on disk.
//
// COST. `std::nth_element` per split level: O(n log n) expected comparisons.
// `split_clusters` additionally materializes a permuted copy of the input, so
// its transient peak is ~2x; `split_centroids` allocates only the permutation.
#include "tri.h"        // Tri, TriEx (MSL include path)
#include <cstdint>
#include <vector>

namespace part_cluster {

// One contiguous run of the reordered triangle array plus the vertex AABB of
// that run. The flattener copies these straight into part_asset::FlatCluster.
//
// `first_tri`/`tri_count` always index the FINAL (post-permutation) layout,
// never the caller's original order — with `split_centroids` that layout only
// exists once the caller has applied `order_out` itself.
//
// The AABB is in whatever space the input triangles were in (world space on
// the flatten path) and covers VERTICES, not centroids. `split_centroids`
// leaves it zeroed because it never sees a vertex; see its note below.
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
