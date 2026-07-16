#include "mesh_transform.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>       // abs
#include <unordered_map>
#include <vector>

// Ported verbatim (adapted to MeshIndexed) from
// MatterEngine3/src/lod_bake.cpp::lod_bake::reproject_triex — uniform spatial
// hash over source centroids, cell size derived from the source AABB extent
// divided by cbrt(source triangle count) so an average cell holds a handful of
// centroids. The nearest-source search grows outward one shell at a time and,
// once an occupied shell is hit, scans one additional shell before stopping
// (a neighbor cell can hold a closer centroid than the first cell that
// happened to be occupied).

namespace {

inline float3 centroid_of(const MeshIndexed& m, size_t tri_i) {
    uint32_t i0 = m.indices[tri_i * 3 + 0];
    uint32_t i1 = m.indices[tri_i * 3 + 1];
    uint32_t i2 = m.indices[tri_i * 3 + 2];
    const float3& a = m.positions[i0];
    const float3& b = m.positions[i1];
    const float3& c = m.positions[i2];
    return make_float3((a.x + b.x + c.x) / 3.0f,
                       (a.y + b.y + c.y) / 3.0f,
                       (a.z + b.z + c.z) / 3.0f);
}

} // namespace

void reproject_triex(const MeshIndexed& source, MeshIndexed& target) {
    const size_t src_tri_count = source.indices.size() / 3;
    const size_t tgt_tri_count = target.indices.size() / 3;

    if (src_tri_count == 0 || source.triex.size() != src_tri_count) {
        target.triex.clear();
        return;
    }

    // Precompute source centroids so the spatial-hash build and every
    // nearest-source query can share them (avoids recomputing per query).
    std::vector<float3> src_centroids;
    src_centroids.reserve(src_tri_count);
    for (size_t i = 0; i < src_tri_count; ++i) {
        src_centroids.push_back(centroid_of(source, i));
    }

    // Uniform spatial hash over source centroids. Cell size from the source
    // AABB so an average cell holds a handful of centroids.
    float mn[3] = { 1e30f,  1e30f,  1e30f};
    float mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const float3& c : src_centroids) {
        mn[0] = fminf(mn[0], c.x); mx[0] = fmaxf(mx[0], c.x);
        mn[1] = fminf(mn[1], c.y); mx[1] = fmaxf(mx[1], c.y);
        mn[2] = fminf(mn[2], c.z); mx[2] = fmaxf(mx[2], c.z);
    }
    float ext = fmaxf(fmaxf(mx[0] - mn[0], mx[1] - mn[1]),
                      fmaxf(mx[2] - mn[2], 1e-6f));
    float cell = ext / fmaxf(1.0f, cbrtf((float)src_tri_count));

    auto key = [&](float x, float y, float z) {
        int ix = (int)floorf((x - mn[0]) / cell);
        int iy = (int)floorf((y - mn[1]) / cell);
        int iz = (int)floorf((z - mn[2]) / cell);
        return ((uint64_t)(uint32_t)ix * 73856093ull) ^
               ((uint64_t)(uint32_t)iy * 19349663ull) ^
               ((uint64_t)(uint32_t)iz * 83492791ull);
    };

    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    grid.reserve(src_tri_count);
    for (size_t i = 0; i < src_tri_count; ++i) {
        const float3& c = src_centroids[i];
        grid[key(c.x, c.y, c.z)].push_back((uint32_t)i);
    }

    auto nearest_src = [&](const float3& c) -> uint32_t {
        float best_d2 = 1e30f;
        uint32_t best = 0;
        int cx = (int)floorf((c.x - mn[0]) / cell);
        int cy = (int)floorf((c.y - mn[1]) / cell);
        int cz = (int)floorf((c.z - mn[2]) / cell);
        // Scan outward shell by shell; after the first occupied shell, scan one
        // more (a neighbor cell can hold a closer centroid than the first cell
        // that happened to be occupied), then stop.
        int hit_ring = -1;
        for (int ring = 0; ring < 64; ++ring) {
            if (hit_ring >= 0 && ring > hit_ring + 1) break;
            bool any = false;
            for (int dz = -ring; dz <= ring; ++dz)
            for (int dy = -ring; dy <= ring; ++dy)
            for (int dx = -ring; dx <= ring; ++dx) {
                // Only the shell of this ring (inner rings already scanned).
                if (ring > 0 && abs(dx) != ring && abs(dy) != ring && abs(dz) != ring)
                    continue;
                uint64_t k = ((uint64_t)(uint32_t)(cx + dx) * 73856093ull) ^
                             ((uint64_t)(uint32_t)(cy + dy) * 19349663ull) ^
                             ((uint64_t)(uint32_t)(cz + dz) * 83492791ull);
                auto it = grid.find(k);
                if (it == grid.end()) continue;
                any = true;
                for (uint32_t i : it->second) {
                    const float3& s = src_centroids[i];
                    float ddx = s.x - c.x, ddy = s.y - c.y, ddz = s.z - c.z;
                    float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
                    if (d2 < best_d2) { best_d2 = d2; best = i; }
                }
            }
            if (any && hit_ring < 0) hit_ring = ring;
        }
        return best;
    };

    // Pre-pass: smooth per-vertex normals on TARGET. Accumulate each triangle's
    // twice-area-weighted face normal (unnormalized cross product) into all
    // three of its vertices, then normalize per vertex. This gives smooth
    // shading across shared vertices — the same convention mesh_simplifier's
    // buildMesh uses internally, so a simplify()→reproject_triex chain now
    // preserves smooth shading end-to-end instead of collapsing to per-face flat
    // normals as the pre-2026-07-07 behavior did.
    std::vector<float3> tgt_vert_normals(target.positions.size(), make_float3(0, 0, 0));
    for (size_t ti = 0; ti < tgt_tri_count; ++ti) {
        uint32_t i0 = target.indices[ti * 3 + 0];
        uint32_t i1 = target.indices[ti * 3 + 1];
        uint32_t i2 = target.indices[ti * 3 + 2];
        const float3& a = target.positions[i0];
        const float3& b = target.positions[i1];
        const float3& c = target.positions[i2];
        float3 e1 = make_float3(b.x - a.x, b.y - a.y, b.z - a.z);
        float3 e2 = make_float3(c.x - a.x, c.y - a.y, c.z - a.z);
        float3 fn = make_float3(e1.y * e2.z - e1.z * e2.y,
                                e1.z * e2.x - e1.x * e2.z,
                                e1.x * e2.y - e1.y * e2.x);
        tgt_vert_normals[i0].x += fn.x; tgt_vert_normals[i0].y += fn.y; tgt_vert_normals[i0].z += fn.z;
        tgt_vert_normals[i1].x += fn.x; tgt_vert_normals[i1].y += fn.y; tgt_vert_normals[i1].z += fn.z;
        tgt_vert_normals[i2].x += fn.x; tgt_vert_normals[i2].y += fn.y; tgt_vert_normals[i2].z += fn.z;
    }
    for (size_t v = 0; v < tgt_vert_normals.size(); ++v) {
        float3& n = tgt_vert_normals[v];
        float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len > 1e-12f) { n.x /= len; n.y /= len; n.z /= len; }
        else { n.x = 0; n.y = 1; n.z = 0; }
    }

    // Per-corner AO sampling: baked AO varies smoothly across the surface, so
    // copying one source triangle's corner AO onto a (much larger) decimated
    // triangle produces flat blocky patches and visible seams where adjacent
    // clusters decimate differently. Sample AO at each target corner instead:
    // nearest source triangle to the corner, clamped barycentric interpolation
    // of its ao0/ao1/ao2 at the corner position. materialId/tint/uv stay
    // nearest-triangle copies (piecewise-constant in practice).
    auto ao_at_point = [&](const float3& p) -> float {
        uint32_t si = nearest_src(p);
        uint32_t s0 = source.indices[si * 3 + 0];
        uint32_t s1 = source.indices[si * 3 + 1];
        uint32_t s2 = source.indices[si * 3 + 2];
        const float3& a = source.positions[s0];
        const float3& b = source.positions[s1];
        const float3& c = source.positions[s2];
        const TriEx& ex = source.triex[si];
        float3 v0 = make_float3(b.x - a.x, b.y - a.y, b.z - a.z);
        float3 v1 = make_float3(c.x - a.x, c.y - a.y, c.z - a.z);
        float3 v2 = make_float3(p.x - a.x, p.y - a.y, p.z - a.z);
        float d00 = v0.x*v0.x + v0.y*v0.y + v0.z*v0.z;
        float d01 = v0.x*v1.x + v0.y*v1.y + v0.z*v1.z;
        float d11 = v1.x*v1.x + v1.y*v1.y + v1.z*v1.z;
        float d20 = v2.x*v0.x + v2.y*v0.y + v2.z*v0.z;
        float d21 = v2.x*v1.x + v2.y*v1.y + v2.z*v1.z;
        float denom = d00 * d11 - d01 * d01;
        if (fabsf(denom) < 1e-20f)   // degenerate source tri: average its AO
            return (ex.ao0 + ex.ao1 + ex.ao2) / 3.0f;
        float v = (d11 * d20 - d01 * d21) / denom;
        float w = (d00 * d21 - d01 * d20) / denom;
        float u = 1.0f - v - w;
        // Clamp outside-the-triangle barycentrics (corner lies off the source
        // tri's footprint), then renormalize so the weights still sum to 1.
        u = fmaxf(u, 0.0f); v = fmaxf(v, 0.0f); w = fmaxf(w, 0.0f);
        float sum = u + v + w;
        if (sum < 1e-12f) return (ex.ao0 + ex.ao1 + ex.ao2) / 3.0f;
        return (u * ex.ao0 + v * ex.ao1 + w * ex.ao2) / sum;
    };

    // Per-vertex AO cache: corner AO depends only on position, so shared target
    // vertices sample once and stay continuous across their triangles.
    std::vector<float> tgt_vert_ao(target.positions.size(), -1.0f);
    auto vert_ao = [&](uint32_t vi) -> float {
        if (tgt_vert_ao[vi] < 0.0f)
            tgt_vert_ao[vi] = ao_at_point(target.positions[vi]);
        return tgt_vert_ao[vi];
    };

    target.triex.clear();
    target.triex.reserve(tgt_tri_count);
    for (size_t ti = 0; ti < tgt_tri_count; ++ti) {
        uint32_t i0 = target.indices[ti * 3 + 0];
        uint32_t i1 = target.indices[ti * 3 + 1];
        uint32_t i2 = target.indices[ti * 3 + 2];
        const float3& a = target.positions[i0];
        const float3& b = target.positions[i1];
        const float3& cc = target.positions[i2];
        float3 tc = make_float3((a.x + b.x + cc.x) / 3.0f,
                                (a.y + b.y + cc.y) / 3.0f,
                                (a.z + b.z + cc.z) / 3.0f);

        const TriEx& src = source.triex[nearest_src(tc)];
        TriEx ex = src;   // materialId, tint, uv carried from source
        ex.N0 = tgt_vert_normals[i0];
        ex.N1 = tgt_vert_normals[i1];
        ex.N2 = tgt_vert_normals[i2];
        ex.ao0 = vert_ao(i0);
        ex.ao1 = vert_ao(i1);
        ex.ao2 = vert_ao(i2);
        target.triex.push_back(ex);
    }
}
