// part_cluster.cpp — deterministic spatial cluster split for flattened meshes.
// Algorithm: build an index permutation via recursive longest-axis median split
// (nth_element on (centroid[axis], index) — index as tie-break → deterministic).
// Apply the permutation once at the end; compute per-cluster vertex AABBs.
#include "part_cluster.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <limits>

namespace part_cluster {

namespace {

// Centroid AABB over a sub-range of the order array. Templated on a functor
// that maps `order[i]` to its centroid so the same recursive walk can back
// either the Tri-based split_clusters or the centroid-only split_centroids.
template <typename GetCentroid>
static void centroid_aabb(const std::vector<uint32_t>& order,
                          uint32_t lo, uint32_t hi,
                          GetCentroid&& gc,
                          float mn[3], float mx[3]) {
    for (int k = 0; k < 3; ++k) {
        mn[k] =  std::numeric_limits<float>::max();
        mx[k] = -std::numeric_limits<float>::max();
    }
    for (uint32_t i = lo; i < hi; ++i) {
        const float3& c = gc(order[i]);
        float cv[3] = {c.x, c.y, c.z};
        for (int k = 0; k < 3; ++k) {
            if (cv[k] < mn[k]) mn[k] = cv[k];
            if (cv[k] > mx[k]) mx[k] = cv[k];
        }
    }
}

// Emit a "shape-only" cluster (first_tri/tri_count filled, AABB zeroed). Both
// entry points fill in the AABB later — split_clusters after the permutation is
// applied, split_centroids leaves it for the caller.
static Cluster make_shape_cluster(uint32_t lo, uint32_t hi, uint32_t output_first) {
    Cluster cl;
    cl.first_tri = output_first;
    cl.tri_count = hi - lo;
    for (int k = 0; k < 3; ++k) { cl.aabb_min[k] = 0.0f; cl.aabb_max[k] = 0.0f; }
    return cl;
}

// Recursive split, generic over the centroid source. `gc(i)` returns the world-
// space centroid of triangle i (an index into whatever per-triangle stream the
// caller owns). order[lo..hi) is the range to process; output_first tracks the
// cluster's first_tri in the final permuted layout.
//
// Determinism: nth_element with an (axis-value, original-index) comparator —
// the index tie-break makes cases of equal centroids reproducible across runs.
template <typename GetCentroid>
static void split_recursive_generic(std::vector<uint32_t>& order,
                                    uint32_t lo, uint32_t hi,
                                    uint32_t target_tris,
                                    uint32_t output_first,
                                    GetCentroid&& gc,
                                    std::vector<Cluster>& out) {
    uint32_t len = hi - lo;
    if (len == 0) return;

    // If small enough, emit one cluster.
    if (len <= target_tris) {
        out.push_back(make_shape_cluster(lo, hi, output_first));
        return;
    }

    // Find longest axis of centroid AABB.
    float mn[3], mx[3];
    centroid_aabb(order, lo, hi, gc, mn, mx);
    float extents[3] = {mx[0]-mn[0], mx[1]-mn[1], mx[2]-mn[2]};
    int axis = 0;
    if (extents[1] > extents[axis]) axis = 1;
    if (extents[2] > extents[axis]) axis = 2;

    // Degenerate guard: if all centroids are identical, emit as-is.
    if (extents[axis] == 0.0f) {
        out.push_back(make_shape_cluster(lo, hi, output_first));
        return;
    }

    // Median split using nth_element on (centroid[axis], original_index) — the
    // index tie-break makes the split deterministic for equal centroids.
    uint32_t mid = lo + len / 2;
    std::nth_element(order.begin() + lo, order.begin() + mid, order.begin() + hi,
        [&](uint32_t a, uint32_t b) {
            const float3& ca = gc(a);
            const float3& cb2 = gc(b);
            float va = (axis == 0) ? ca.x : (axis == 1) ? ca.y : ca.z;
            float vb = (axis == 0) ? cb2.x : (axis == 1) ? cb2.y : cb2.z;
            if (va != vb) return va < vb;
            return a < b; // deterministic tie-break
        });

    // Recurse left then right; left takes output positions [output_first .. output_first+(mid-lo)).
    split_recursive_generic(order, lo, mid, target_tris, output_first, gc, out);
    split_recursive_generic(order, mid, hi, target_tris, output_first + (mid - lo), gc, out);
}

} // anonymous namespace

std::vector<Cluster> split_clusters(std::vector<Tri>& tris,
                                    std::vector<TriEx>& triex,
                                    uint32_t target_tris) {
    const uint32_t n = (uint32_t)tris.size();
    if (n == 0) return {};

    // Validate parallel sizes if triex is non-empty.
    const bool has_triex = !triex.empty();
    assert(!has_triex || triex.size() == tris.size());

    // Build identity order vector.
    std::vector<uint32_t> order(n);
    for (uint32_t i = 0; i < n; ++i) order[i] = i;

    // Collect clusters (they carry first_tri/tri_count in the FINAL permuted layout).
    std::vector<Cluster> clusters;
    clusters.reserve((n + target_tris - 1) / target_tris + 1);
    split_recursive_generic(order, 0, n, target_tris, 0,
        [&](uint32_t i) -> const float3& { return tris[i].centroid; },
        clusters);

    // Apply the permutation ONCE to tris (and triex in parallel).
    // Build a permuted copy and move back.
    {
        std::vector<Tri> tmp(n);
        for (uint32_t i = 0; i < n; ++i) tmp[i] = tris[order[i]];
        tris = std::move(tmp);
    }
    if (has_triex) {
        std::vector<TriEx> tmp(n);
        for (uint32_t i = 0; i < n; ++i) tmp[i] = triex[order[i]];
        triex = std::move(tmp);
    }

    // Recompute each cluster's vertex AABB from the now-permuted tris.
    for (auto& cl : clusters) {
        for (int k = 0; k < 3; ++k) {
            cl.aabb_min[k] =  std::numeric_limits<float>::max();
            cl.aabb_max[k] = -std::numeric_limits<float>::max();
        }
        for (uint32_t j = cl.first_tri; j < cl.first_tri + cl.tri_count; ++j) {
            const Tri& t = tris[j];
            const float3* vs[3] = {&t.vertex0, &t.vertex1, &t.vertex2};
            for (const float3* v : vs) {
                float vv[3] = {v->x, v->y, v->z};
                for (int k = 0; k < 3; ++k) {
                    if (vv[k] < cl.aabb_min[k]) cl.aabb_min[k] = vv[k];
                    if (vv[k] > cl.aabb_max[k]) cl.aabb_max[k] = vv[k];
                }
            }
        }
    }

    return clusters;
}

std::vector<Cluster> split_centroids(const std::vector<float3>& centroids,
                                     std::vector<uint32_t>& order_out,
                                     uint32_t target_tris) {
    const uint32_t n = (uint32_t)centroids.size();
    order_out.clear();
    if (n == 0) return {};

    // Build identity order vector in the caller's out-param — the recursive
    // pass permutes it in-place. Caller uses this to apply the same permutation
    // to whichever per-triangle stream it owns.
    order_out.resize(n);
    for (uint32_t i = 0; i < n; ++i) order_out[i] = i;

    std::vector<Cluster> clusters;
    clusters.reserve((n + target_tris - 1) / target_tris + 1);
    split_recursive_generic(order_out, 0, n, target_tris, 0,
        [&](uint32_t i) -> const float3& { return centroids[i]; },
        clusters);

    // AABBs are left zeroed — vertices are not visible from a centroid array.
    // Caller (see part_flatten::flatten_part_impl) fills them from the
    // materialized per-cluster geometry in Pass 2.
    return clusters;
}

} // namespace part_cluster
