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

inline float3 face_normal_of(const MeshIndexed& m, size_t tri_i) {
    const float3& a = m.positions[m.indices[tri_i * 3 + 0]];
    const float3& b = m.positions[m.indices[tri_i * 3 + 1]];
    const float3& c = m.positions[m.indices[tri_i * 3 + 2]];
    float3 n = make_float3((b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y),
                           (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z),
                           (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
    float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-12f) return make_float3(n.x / len, n.y / len, n.z / len);
    return make_float3(0, 1, 0);
}

// SampleSource crease gate: a candidate source triangle may donate normals to
// a target corner only if its face plane roughly agrees with the target
// triangle's. cos(~69.5°) is deliberately permissive — a very coarse rung's
// face normal can sit 40-60° off the local source normal on curved geometry
// and must still find its donors — while a 90° box edge (dot 0) is always
// rejected, which is what keeps a corner on the crease from sampling the
// perpendicular face.
constexpr float kAlignDot = 0.35f;

// Barycentrics of the closest point on triangle (a,b,c) to p (Ericson,
// Real-Time Collision Detection §5.1.5). Donor ranking needs the true
// point-to-surface distance — a centroid is a terrible proxy for a large
// triangle (a 32-unit ground slab's top face is two triangles whose centroids
// sit ~10 units from its corners, farther than an unrelated sphere hovering
// over the slab) — and the same clamped barycentrics then sample the authored
// corner normals at the point where the corner actually sits.
inline void closest_point_bary(const float3& a, const float3& b, const float3& c,
                               const float3& p, float& u, float& v, float& w) {
    float3 ab = make_float3(b.x - a.x, b.y - a.y, b.z - a.z);
    float3 ac = make_float3(c.x - a.x, c.y - a.y, c.z - a.z);
    float3 ap = make_float3(p.x - a.x, p.y - a.y, p.z - a.z);
    float d1 = ab.x * ap.x + ab.y * ap.y + ab.z * ap.z;
    float d2 = ac.x * ap.x + ac.y * ap.y + ac.z * ap.z;
    if (d1 <= 0.0f && d2 <= 0.0f) { u = 1; v = 0; w = 0; return; }
    float3 bp = make_float3(p.x - b.x, p.y - b.y, p.z - b.z);
    float d3 = ab.x * bp.x + ab.y * bp.y + ab.z * bp.z;
    float d4 = ac.x * bp.x + ac.y * bp.y + ac.z * bp.z;
    if (d3 >= 0.0f && d4 <= d3) { u = 0; v = 1; w = 0; return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float t = (d1 - d3 != 0.0f) ? d1 / (d1 - d3) : 0.0f;
        u = 1 - t; v = t; w = 0; return;
    }
    float3 cp = make_float3(p.x - c.x, p.y - c.y, p.z - c.z);
    float d5 = ab.x * cp.x + ab.y * cp.y + ab.z * cp.z;
    float d6 = ac.x * cp.x + ac.y * cp.y + ac.z * cp.z;
    if (d6 >= 0.0f && d5 <= d6) { u = 0; v = 0; w = 1; return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float t = (d2 - d6 != 0.0f) ? d2 / (d2 - d6) : 0.0f;
        u = 1 - t; v = 0; w = t; return;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float denom = (d4 - d3) + (d5 - d6);
        float t = (denom != 0.0f) ? (d4 - d3) / denom : 0.0f;
        u = 0; v = 1 - t; w = t; return;
    }
    float denom = va + vb + vc;
    if (fabsf(denom) < 1e-30f) { u = v = w = 1.0f / 3.0f; return; }   // degenerate
    v = vb / denom; w = vc / denom; u = 1.0f - v - w;
}

// Sample a source triangle's authored corner normals at the closest point of
// the triangle to p (clamped barycentrics from above). Source triangles are
// often small next to a decimated rung's corners, so p routinely lands outside
// its donor; clamping samples the nearest edge/corner instead of extrapolating
// a tiny triangle's normal gradient across a large one.
inline float3 sample_source_normal(const MeshIndexed& src, size_t tri_i,
                                   const float3& p, const float3& fallback) {
    const float3& a = src.positions[src.indices[tri_i * 3 + 0]];
    const float3& b = src.positions[src.indices[tri_i * 3 + 1]];
    const float3& c = src.positions[src.indices[tri_i * 3 + 2]];
    float u, v, w;
    closest_point_bary(a, b, c, p, u, v, w);
    const TriEx& ex = src.triex[tri_i];
    float3 n = make_float3(u * ex.N0.x + v * ex.N1.x + w * ex.N2.x,
                           u * ex.N0.y + v * ex.N1.y + w * ex.N2.y,
                           u * ex.N0.z + v * ex.N1.z + w * ex.N2.z);
    float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-12f) return make_float3(n.x / len, n.y / len, n.z / len);
    return fallback;
}

inline float point_tri_dist2(const MeshIndexed& src, size_t tri_i, const float3& p) {
    const float3& a = src.positions[src.indices[tri_i * 3 + 0]];
    const float3& b = src.positions[src.indices[tri_i * 3 + 1]];
    const float3& c = src.positions[src.indices[tri_i * 3 + 2]];
    float u, v, w;
    closest_point_bary(a, b, c, p, u, v, w);
    float3 q = make_float3(u * a.x + v * b.x + w * c.x,
                           u * a.y + v * b.y + w * c.y,
                           u * a.z + v * b.z + w * c.z);
    float dx = q.x - p.x, dy = q.y - p.y, dz = q.z - p.z;
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

ReprojectSource::ReprojectSource(const MeshIndexed& source_in,
                                 ReprojectNormals normals_in)
    : source(source_in), normals(normals_in) {
    const size_t src_tri_count = source.indices.size() / 3;

    // Same precondition as the free function: an unusable source yields an
    // invalid index, and reprojecting through it clears the target's triex.
    if (src_tri_count == 0 || source.triex.size() != src_tri_count) {
        valid_ = false;
        return;
    }
    valid_ = true;

    // Precompute source centroids so the spatial-hash build and every
    // nearest-source query can share them (avoids recomputing per query).
    src_centroids.reserve(src_tri_count);
    for (size_t i = 0; i < src_tri_count; ++i) {
        src_centroids.push_back(centroid_of(source, i));
    }

    // Uniform spatial hash over source centroids. Cell size from the source
    // AABB so an average cell holds a handful of centroids.
    mn[0] = mn[1] = mn[2] = 1e30f;
    float mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const float3& c : src_centroids) {
        mn[0] = fminf(mn[0], c.x); mx[0] = fmaxf(mx[0], c.x);
        mn[1] = fminf(mn[1], c.y); mx[1] = fmaxf(mx[1], c.y);
        mn[2] = fminf(mn[2], c.z); mx[2] = fmaxf(mx[2], c.z);
    }
    float ext = fmaxf(fmaxf(mx[0] - mn[0], mx[1] - mn[1]),
                      fmaxf(mx[2] - mn[2], 1e-6f));
    cell = ext / fmaxf(1.0f, cbrtf((float)src_tri_count));

    auto key = [&](float x, float y, float z) {
        int ix = (int)floorf((x - mn[0]) / cell);
        int iy = (int)floorf((y - mn[1]) / cell);
        int iz = (int)floorf((z - mn[2]) / cell);
        return ((uint64_t)(uint32_t)ix * 73856093ull) ^
               ((uint64_t)(uint32_t)iy * 19349663ull) ^
               ((uint64_t)(uint32_t)iz * 83492791ull);
    };

    grid.reserve(src_tri_count);
    for (size_t i = 0; i < src_tri_count; ++i) {
        const float3& c = src_centroids[i];
        grid[key(c.x, c.y, c.z)].push_back((uint32_t)i);
    }

    cell_ov = cell;
    build_donor_machinery();
}

uint32_t ReprojectSource::nearest_src(const float3& c) const {
    {
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
    }
}

void ReprojectSource::build_donor_machinery() {
    const size_t src_tri_count = source.indices.size() / 3;
    // SampleSource donor machinery. Corner donor lookups need the true
    // point-to-SURFACE distance, so triangles are registered in every cell
    // their AABB overlaps (a slab-sized triangle is then found from any of its
    // corners at ring 0, where the centroid grid above would place it ~half a
    // slab away and let some unrelated-but-nearer surface win). The centroid
    // grid stays as-is for the attribute match — its semantics predate this
    // and material/tint/uv/AO are far less sensitive to the metric.
    //
    // This grid is sized from the VERTEX AABB, not the centroid AABB, and its
    // per-axis index range is hard-clamped to the grid span. Both matter: a
    // single-triangle source has a point-sized centroid AABB, whose 1e-6 cell
    // would spread that one triangle's insertion across ~1e12 cells (observed
    // as a 50 GB runaway in partstore_tests before the clamp).
    // (members: src_face_n, overlap_grid, vmn, cell_ov, ov_span — ov_span is
    // grid cells per axis; [0, ov_span] valid)
    if (normals == ReprojectNormals::SampleSource) {
        float vmx[3] = { -1e30f, -1e30f, -1e30f };
        vmn[0] = vmn[1] = vmn[2] = 1e30f;
        for (const float3& v : source.positions) {
            vmn[0] = fminf(vmn[0], v.x); vmx[0] = fmaxf(vmx[0], v.x);
            vmn[1] = fminf(vmn[1], v.y); vmx[1] = fmaxf(vmx[1], v.y);
            vmn[2] = fminf(vmn[2], v.z); vmx[2] = fmaxf(vmx[2], v.z);
        }
        float vext = fmaxf(fmaxf(vmx[0] - vmn[0], vmx[1] - vmn[1]),
                           fmaxf(vmx[2] - vmn[2], 1e-6f));
        ov_span = (int)fmaxf(1.0f, cbrtf((float)src_tri_count));
        cell_ov = vext / (float)ov_span;
        auto ov_clamp = [&](float t) {
            int i = (int)floorf(t / cell_ov);
            return i < 0 ? 0 : (i > ov_span ? ov_span : i);
        };
        src_face_n.reserve(src_tri_count);
        for (size_t i = 0; i < src_tri_count; ++i)
            src_face_n.push_back(face_normal_of(source, i));
        overlap_grid.reserve(src_tri_count);
        for (size_t i = 0; i < src_tri_count; ++i) {
            const float3* vs[3] = {
                &source.positions[source.indices[i * 3 + 0]],
                &source.positions[source.indices[i * 3 + 1]],
                &source.positions[source.indices[i * 3 + 2]],
            };
            float tmn[3] = { 1e30f, 1e30f, 1e30f }, tmx[3] = { -1e30f, -1e30f, -1e30f };
            for (const float3* v : vs) {
                tmn[0] = fminf(tmn[0], v->x); tmx[0] = fmaxf(tmx[0], v->x);
                tmn[1] = fminf(tmn[1], v->y); tmx[1] = fmaxf(tmx[1], v->y);
                tmn[2] = fminf(tmn[2], v->z); tmx[2] = fmaxf(tmx[2], v->z);
            }
            int x0 = ov_clamp(tmn[0] - vmn[0]), x1 = ov_clamp(tmx[0] - vmn[0]);
            int y0 = ov_clamp(tmn[1] - vmn[1]), y1 = ov_clamp(tmx[1] - vmn[1]);
            int z0 = ov_clamp(tmn[2] - vmn[2]), z1 = ov_clamp(tmx[2] - vmn[2]);
            for (int iz = z0; iz <= z1; ++iz)
            for (int iy = y0; iy <= y1; ++iy)
            for (int ix = x0; ix <= x1; ++ix) {
                uint64_t k = ((uint64_t)(uint32_t)ix * 73856093ull) ^
                             ((uint64_t)(uint32_t)iy * 19349663ull) ^
                             ((uint64_t)(uint32_t)iz * 83492791ull);
                overlap_grid[k].push_back((uint32_t)i);
            }
        }
    }
}

// Nearest crease-compatible donor triangle to corner p: candidates whose
// face plane disagrees with the target triangle's (`align`, the crease
// gate above) are rejected outright; the rest rank by point-to-triangle
// distance, with near-ties broken toward the better-aligned face — a
// corner ON a box edge touches both faces at distance zero, and the
// tie-break is what keeps it sampling its own. Returns -1 when no
// compatible donor exists within the ring cap.
int ReprojectSource::nearest_donor(const float3& p, const float3& align) const {
    {
        float best_d2 = 1e30f, best_dot = -2.0f;
        int best = -1;
        const float tie = 1e-6f * cell_ov * cell_ov;
        // Clamp the query cell into the grid so a corner QEM nudged outside
        // the source AABB still starts its ring walk at the nearest edge cell.
        auto qc = [&](float t) {
            int i = (int)floorf(t / cell_ov);
            return i < 0 ? 0 : (i > ov_span ? ov_span : i);
        };
        int cx = qc(p.x - vmn[0]);
        int cy = qc(p.y - vmn[1]);
        int cz = qc(p.z - vmn[2]);
        int hit_ring = -1;
        // Ring cap = ov_span, not a fixed 64. qc() clamps the query cell into
        // [0, ov_span] and every occupied cell was clamped into the same range
        // at build time, so the largest Chebyshev distance between the query
        // and any populated cell is exactly ov_span -- a larger ring can only
        // probe cells that cannot exist.
        //
        // The fixed 64 was pathological rather than merely wasteful. hit_ring
        // is set only when a candidate survives the kAlignDot crease gate, and
        // at a coarse LOD rung the decimated faces deviate far enough from the
        // source that EVERY candidate can be rejected. hit_ring then stays -1,
        // the early-out never fires, and the walk runs all 64 rings -- ring 63
        // alone is 127^3 ~ 2.05M probes. Measured on a streamed sector: a
        // 2224-triangle source reprojecting onto a 130-triangle rung took
        // 5483 ms, against 4 ms for the decimation that produced it.
        // Returning -1 early is exactly what the no-donor path already handles
        // (the caller falls back to the centroid match), so this changes cost,
        // not results.
        const int ring_cap = ov_span;
        for (int ring = 0; ring <= ring_cap; ++ring) {
            if (hit_ring >= 0 && ring > hit_ring + 1) break;
            bool any = false;
            for (int dz = -ring; dz <= ring; ++dz)
            for (int dy = -ring; dy <= ring; ++dy)
            for (int dx = -ring; dx <= ring; ++dx) {
                if (ring > 0 && abs(dx) != ring && abs(dy) != ring && abs(dz) != ring)
                    continue;
                uint64_t k = ((uint64_t)(uint32_t)(cx + dx) * 73856093ull) ^
                             ((uint64_t)(uint32_t)(cy + dy) * 19349663ull) ^
                             ((uint64_t)(uint32_t)(cz + dz) * 83492791ull);
                auto it = overlap_grid.find(k);
                if (it == overlap_grid.end()) continue;
                for (uint32_t i : it->second) {
                    const float3& fn = src_face_n[i];
                    float d = fn.x * align.x + fn.y * align.y + fn.z * align.z;
                    if (d < kAlignDot) continue;
                    any = true;
                    float d2 = point_tri_dist2(source, i, p);
                    if (d2 < best_d2 - tie ||
                        (d2 < best_d2 + tie && d > best_dot)) {
                        best_d2 = d2; best_dot = d; best = (int)i;
                    }
                }
            }
            if (any && hit_ring < 0) hit_ring = ring;
        }
        return best;
    }
}

void reproject_triex(const ReprojectSource& index, MeshIndexed& target) {
    const MeshIndexed& source = index.source;
    const ReprojectNormals normals = index.normals;
    const size_t tgt_tri_count = target.indices.size() / 3;

    if (!index.valid()) {
        target.triex.clear();
        return;
    }
    auto nearest_src = [&index](const float3& c) { return index.nearest_src(c); };
    auto nearest_donor = [&index](const float3& p, const float3& a) {
        return index.nearest_donor(p, a);
    };

    // SmoothTarget pre-pass: smooth per-vertex normals on TARGET. Accumulate
    // each triangle's twice-area-weighted face normal (unnormalized cross
    // product) into all three of its vertices, then normalize per vertex — the
    // same convention mesh_simplifier's buildMesh uses internally. This has no
    // notion of a crease, which is why LOD ladders use SampleSource instead.
    std::vector<float3> tgt_vert_normals;
    if (normals == ReprojectNormals::SmoothTarget) {
        tgt_vert_normals.assign(target.positions.size(), make_float3(0, 0, 0));
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
    }

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

        // Attribute match stays on the centroid grid, unfiltered: a crease
        // does not change which triangle a materialId/tint/uv/AO should come
        // from, and this keeps the original match semantics byte-stable.
        const uint32_t match = nearest_src(tc);
        TriEx ex = source.triex[match];   // materialId, tint, uv, AO carried
        if (normals == ReprojectNormals::SmoothTarget) {
            ex.N0 = tgt_vert_normals[i0];
            ex.N1 = tgt_vert_normals[i1];
            ex.N2 = tgt_vert_normals[i2];
        } else {
            // Each corner samples the authored normal field where it actually
            // sits, from a donor on its own side of any crease. Adjacent rung
            // triangles on a smooth surface resolve the same donor for a shared
            // corner (continuity); across a hard edge they resolve donors from
            // their own faces (the crease survives).
            const float3 ftgt = face_normal_of(target, ti);
            const float3 corners[3] = { a, b, cc };
            float3* out_n[3] = { &ex.N0, &ex.N1, &ex.N2 };
            for (int k = 0; k < 3; ++k) {
                int si = nearest_donor(corners[k], ftgt);
                if (si < 0) si = (int)match;   // no compatible donor: keep locality
                *out_n[k] = sample_source_normal(source, (size_t)si, corners[k], ftgt);
            }
        }
        target.triex.push_back(ex);
    }
}

void reproject_triex(const MeshIndexed& source, MeshIndexed& target,
                     ReprojectNormals normals) {
    // Single-target convenience: build the index and throw it away. Callers
    // that reproject the same source onto several targets (an LOD ladder)
    // should hoist a ReprojectSource instead — that is the whole point of it.
    const ReprojectSource index(source, normals);
    reproject_triex(index, target);
}
