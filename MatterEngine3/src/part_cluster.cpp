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

// Centroid AABB over a sub-range of the order array.
struct CentBox {
    float mn[3], mx[3];
};

static CentBox centroid_aabb(const std::vector<Tri>& tris,
                             const std::vector<uint32_t>& order,
                             uint32_t lo, uint32_t hi) {
    CentBox b;
    for (int k = 0; k < 3; ++k) {
        b.mn[k] =  std::numeric_limits<float>::max();
        b.mx[k] = -std::numeric_limits<float>::max();
    }
    for (uint32_t i = lo; i < hi; ++i) {
        const float3& c = tris[order[i]].centroid;
        float cv[3] = {c.x, c.y, c.z};
        for (int k = 0; k < 3; ++k) {
            if (cv[k] < b.mn[k]) b.mn[k] = cv[k];
            if (cv[k] > b.mx[k]) b.mx[k] = cv[k];
        }
    }
    return b;
}

static Cluster make_cluster(const std::vector<Tri>& tris,
                             const std::vector<uint32_t>& order,
                             uint32_t lo, uint32_t hi,
                             uint32_t output_first) {
    Cluster cl;
    cl.first_tri = output_first;
    cl.tri_count = hi - lo;
    for (int k = 0; k < 3; ++k) {
        cl.aabb_min[k] =  std::numeric_limits<float>::max();
        cl.aabb_max[k] = -std::numeric_limits<float>::max();
    }
    for (uint32_t i = lo; i < hi; ++i) {
        const Tri& t = tris[order[i]];
        const float3* vs[3] = {&t.vertex0, &t.vertex1, &t.vertex2};
        for (const float3* v : vs) {
            float vv[3] = {v->x, v->y, v->z};
            for (int k = 0; k < 3; ++k) {
                if (vv[k] < cl.aabb_min[k]) cl.aabb_min[k] = vv[k];
                if (vv[k] > cl.aabb_max[k]) cl.aabb_max[k] = vv[k];
            }
        }
    }
    return cl;
}

// Recursive split. order[lo..hi) is the range to process.
// output_first tracks the cluster's first_tri in the final permuted array.
static void split_recursive(const std::vector<Tri>& tris,
                             std::vector<uint32_t>& order,
                             uint32_t lo, uint32_t hi,
                             uint32_t target_tris,
                             uint32_t output_first,
                             std::vector<Cluster>& out) {
    uint32_t len = hi - lo;
    if (len == 0) return;

    // If small enough, emit one cluster.
    if (len <= target_tris) {
        out.push_back(make_cluster(tris, order, lo, hi, output_first));
        return;
    }

    // Find longest axis of centroid AABB.
    CentBox cb = centroid_aabb(tris, order, lo, hi);
    float extents[3] = {cb.mx[0]-cb.mn[0], cb.mx[1]-cb.mn[1], cb.mx[2]-cb.mn[2]};
    int axis = 0;
    if (extents[1] > extents[axis]) axis = 1;
    if (extents[2] > extents[axis]) axis = 2;

    // Degenerate guard: if all centroids are identical, emit as-is.
    if (extents[axis] == 0.0f) {
        out.push_back(make_cluster(tris, order, lo, hi, output_first));
        return;
    }

    // Median split using nth_element on (centroid[axis], original_index) — the
    // index tie-break makes the split deterministic for equal centroids.
    uint32_t mid = lo + len / 2;
    std::nth_element(order.begin() + lo, order.begin() + mid, order.begin() + hi,
        [&](uint32_t a, uint32_t b) {
            const float3& ca = tris[a].centroid;
            const float3& cb2 = tris[b].centroid;
            float va = (axis == 0) ? ca.x : (axis == 1) ? ca.y : ca.z;
            float vb = (axis == 0) ? cb2.x : (axis == 1) ? cb2.y : cb2.z;
            if (va != vb) return va < vb;
            return a < b; // deterministic tie-break
        });

    // Recurse left then right; left takes output positions [output_first .. output_first+(mid-lo)).
    split_recursive(tris, order, lo, mid, target_tris, output_first, out);
    split_recursive(tris, order, mid, hi, target_tris, output_first + (mid - lo), out);
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
    split_recursive(tris, order, 0, n, target_tris, 0, clusters);

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

} // namespace part_cluster
