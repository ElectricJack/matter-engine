// terrain_mesher.cpp — naive surface-nets sector mesher.
// Pure CPU; no JS, no GL.

#include "terrain_mesher.h"
#include <cmath>
#include <limits>
#include <unordered_map>

namespace terrain_mesher {

namespace {

struct V3 { float x, y, z; };
struct CellVert { V3 p; V3 n; };

MaterialBucket& bucket_for(SectorMesh& m, uint32_t mat) {
    for (auto& b : m.buckets) if (b.material == mat) return b;
    m.buckets.push_back(MaterialBucket{mat, {}, {}});
    return m.buckets.back();
}

void push_tri(MaterialBucket& b,
              const CellVert& a, const CellVert& c, const CellVert& d) {
    const CellVert* vs[3] = {&a, &c, &d};
    for (const CellVert* v : vs) {
        b.positions.push_back(v->p.x);
        b.positions.push_back(v->p.y);
        b.positions.push_back(v->p.z);
        b.normals.push_back(v->n.x);
        b.normals.push_back(v->n.y);
        b.normals.push_back(v->n.z);
    }
}

// 12 cell edges as corner-offset pairs (i0,j0,k0, i1,j1,k1).
const int kEdges[12][6] = {
    {0,0,0,1,0,0},{0,1,0,1,1,0},{0,0,1,1,0,1},{0,1,1,1,1,1},
    {0,0,0,0,1,0},{1,0,0,1,1,0},{0,0,1,0,1,1},{1,0,1,1,1,1},
    {0,0,0,0,0,1},{1,0,0,1,0,1},{0,1,0,0,1,1},{1,1,0,1,1,1},
};

} // namespace

bool mesh_sector(const terrain_field::FieldRuntime& field,
                 int64_t tx, int64_t tz, int rung,
                 float sector_size, float y_min, float y_max,
                 SectorMesh& out, std::string& err) {
    if (rung < 0 || rung > 3) {
        err = "terrain_mesher: rung out of 0..3";
        return false;
    }
    if (sector_size <= 0.0f || y_min >= y_max) {
        err = "terrain_mesher: bad slab config";
        return false;
    }

    const float voxel = 2.0f / float(1 << rung);
    const int   n     = int(std::lround(double(sector_size) / double(voxel)));
    const double ox   = double(tx) * double(sector_size);
    const double oz   = double(tz) * double(sector_size);

    // Evaluate height once per X/Z lattice point, then mesh only a narrow Y
    // slab snapped to the authored global lattice. Neighboring sectors can use
    // different depths without shifting their shared sample coordinates.
    const int sx = n + 3, szn = n + 3;
    std::vector<float> heights(size_t(sx) * size_t(szn));
    auto hat = [&](int i, int k) -> float& {
        return heights[size_t(k) * size_t(sx) + size_t(i)];
    };
    float h_min = std::numeric_limits<float>::infinity();
    float h_max = -std::numeric_limits<float>::infinity();
    for (int k = 0; k < szn; ++k) {
        for (int i = 0; i < sx; ++i) {
            const float h = field.height_at(
                float(ox + (i - 1) * double(voxel)),
                float(oz + (k - 1) * double(voxel)));
            if (!std::isfinite(h)) {
                err = "terrain_mesher: non-finite height";
                return false;
            }
            hat(i, k) = h;
            h_min = std::min(h_min, h);
            h_max = std::max(h_max, h);
        }
    }
    if (h_min < y_min || h_max > y_max) {
        err = "terrain_mesher: sampled height outside authored Y range";
        return false;
    }

    const int global_ny =
        std::max(1, int(std::ceil((y_max - y_min) / voxel)));
    const int j0_global = std::max(
        0, int(std::floor((h_min - y_min) / voxel)) - 2);
    const int j1_global = std::min(
        global_ny, int(std::ceil((h_max - y_min) / voxel)) + 2);
    const float y0 = y_min + float(j0_global) * voxel;
    const int sy = j1_global - j0_global + 1;

    // Narrow density lattice dimensions:
    //   x/z: (n+3) samples — one ring outside on each side (i=-1..n+1)
    //   y: globally aligned samples from j0_global through j1_global
    std::vector<float> d(size_t(sx) * size_t(sy) * size_t(szn));
    auto at = [&](int i, int j, int k) -> float& {
        return d[(size_t(k) * size_t(sy) + size_t(j)) * size_t(sx) + size_t(i)];
    };
    for (int k = 0; k < szn; ++k)
        for (int j = 0; j < sy; ++j)
            for (int i = 0; i < sx; ++i)
                at(i, j, k) = hat(i, k) - (y0 + j * voxel);

    // Surface-nets: one vertex per mixed-sign cell, placed at the centroid of
    // edge crossing positions. Normal from central-diff of the density field.
    std::unordered_map<int64_t, CellVert> verts;
    auto key = [&](int ci, int cj, int ck) -> int64_t {
        return (int64_t(ck) * sy + cj) * sx + ci;
    };
    auto get_vert = [&](int ci, int cj, int ck) -> const CellVert* {
        if (ci < 0 || cj < 0 || ck < 0 ||
            ci >= sx - 1 || cj >= sy - 1 || ck >= szn - 1) return nullptr;
        auto it = verts.find(key(ci, cj, ck));
        if (it != verts.end()) return &it->second;
        float px = 0, py = 0, pz = 0; int cnt = 0;
        for (const int* e : kEdges) {
            float a = at(ci + e[0], cj + e[1], ck + e[2]);
            float b = at(ci + e[3], cj + e[4], ck + e[5]);
            if ((a > 0) == (b > 0)) continue;
            float t = a / (a - b);
            px += (ci + e[0]) + t * float(e[3] - e[0]);
            py += (cj + e[1]) + t * float(e[4] - e[1]);
            pz += (ck + e[2]) + t * float(e[5] - e[2]);
            ++cnt;
        }
        if (!cnt) return nullptr;
        CellVert cv;
        // Local x/z: (lattice_index - 1) * voxel (undoes the ring offset)
        // World y: y0 + lattice_j * voxel
        cv.p = {
            (px / cnt - 1.0f) * voxel,
            y0 + (py / cnt) * voxel,
            (pz / cnt - 1.0f) * voxel
        };
        // Gradient normal from the WORLD position.
        const float e2 = voxel;
        float wx = float(ox) + cv.p.x, wy = cv.p.y, wz = float(oz) + cv.p.z;
        float gx = field.density_at(wx + e2, wy, wz) - field.density_at(wx - e2, wy, wz);
        float gy = field.density_at(wx, wy + e2, wz) - field.density_at(wx, wy - e2, wz);
        float gz = field.density_at(wx, wy, wz + e2) - field.density_at(wx, wy, wz - e2);
        float len = std::sqrt(gx * gx + gy * gy + gz * gz);
        // Density = height - y, so gradient points toward solid (downward for
        // above-ground terrain). Negate to get the outward surface normal.
        cv.n = len > 1e-12f ? V3{-gx / len, -gy / len, -gz / len} : V3{0, 1, 0};
        return &(verts[key(ci, cj, ck)] = cv);
    };

    // Face emission: for each lattice edge with a sign change, emit a quad
    // joining the 4 cells sharing that edge. Ownership: emit only when the
    // edge's base sample (i, k) maps to sector-local [0, sector_size).
    auto emit_quad = [&](const CellVert* v00, const CellVert* v10,
                         const CellVert* v11, const CellVert* v01,
                         bool flip, float wxc, float wzc) {
        if (!v00 || !v10 || !v11 || !v01) return;
        MaterialBucket& b = bucket_for(out,
            uint32_t(field.material_at(wxc, wzc)));
        if (flip) std::swap(v10, v01);
        push_tri(b, *v00, *v10, *v11);
        push_tri(b, *v00, *v11, *v01);
    };
    // Ownership predicate: lattice indices [1..n] map to sector-local [0, S).
    // Integer comparison avoids float precision gaps at sector boundaries.
    // Each sector's mesh ends EXACTLY at the border: the border cell rows are
    // shared with the neighbor (same world samples -> bitwise-identical verts)
    // and sit on the mesh's open boundary, so the LOD ladder's topological
    // boundary lock freezes them at every level. Watertight at any LOD pair
    // without skirts or overlap geometry — do not extend ownership past [1..n].
    auto owned = [&](int i, int k) -> bool {
        return i >= 1 && i <= n && k >= 1 && k <= n;
    };

    for (int k = 0; k < szn; ++k)
        for (int j = 0; j < sy; ++j)
            for (int i = 0; i < sx; ++i) {
                float a = at(i, j, k);
                // World coords of this sample (for material query midpoint).
                float wxs = float(ox) + float(i - 1) * voxel;
                float wzs = float(oz) + float(k - 1) * voxel;

                // +y edge — the typical terrain surface case (horizontal face).
                if (j + 1 < sy && owned(i, k)) {
                    float b = at(i, j + 1, k);
                    if ((a > 0) != (b > 0))
                        emit_quad(get_vert(i - 1, j, k - 1), get_vert(i, j, k - 1),
                                  get_vert(i, j, k),         get_vert(i - 1, j, k),
                                  /*flip=*/a > 0, wxs, wzs);
                }
                // +x edge (vertical face in x direction).
                if (i + 1 < sx && owned(i, k)) {
                    float b = at(i + 1, j, k);
                    if ((a > 0) != (b > 0))
                        emit_quad(get_vert(i, j - 1, k - 1), get_vert(i, j, k - 1),
                                  get_vert(i, j, k),         get_vert(i, j - 1, k),
                                  /*flip=*/a <= 0, wxs + 0.5f * voxel, wzs);
                }
                // +z edge (vertical face in z direction).
                if (k + 1 < szn && owned(i, k)) {
                    float b = at(i, j, k + 1);
                    if ((a > 0) != (b > 0))
                        emit_quad(get_vert(i - 1, j - 1, k), get_vert(i, j - 1, k),
                                  get_vert(i, j, k),         get_vert(i - 1, j, k),
                                  /*flip=*/a <= 0, wxs, wzs + 0.5f * voxel);
                }
            }

    // Border skirts REMOVED 2026-07-30. This path used to hang a vertical
    // curtain (>= 8 m, wound outward) under all four sector edges, inherited
    // from the old full-height slab's implicit border walls.
    //
    // They were cross-rung seam cover, and nothing needs covering any more:
    // the ownership rule above ([1..n], see the comment at the top of this
    // function) already makes any LOD pair watertight without skirts or
    // overlap geometry, and the heightfield path's edge masks do the same by
    // construction. What was left was a curtain that only ever showed when
    // something else was already wrong -- and with Ground POM on by default,
    // the parallax displaces the surface below the datum at the sector rim
    // and exposes the curtain edge-on, printing a dark band along every
    // seam. Nothing was hiding a hole; the cover itself was the artifact.
    //
    // The transient case they also covered -- a neighbour not yet resident --
    // now shows through as background rather than as a wall. That is the
    // intended trade: a streaming hole is momentary, a seam grid is not.
    return true;
}

// ---------------------------------------------------------------------------
// Heightfield LOD ladder (design doc 2026-07-28, LODs 0-4)
// ---------------------------------------------------------------------------

namespace {

struct HfVert { V3 p; V3 n; };

// Orientation-normalizing triangle push: every top-surface triangle must be
// counter-clockwise seen from above (outward-up under the engine's winding
// convention). The stitch patterns below are written corner-agnostic and rely
// on this helper instead of per-corner mirrored vertex orders.
void push_hf_tri(MaterialBucket& b, const HfVert& v0, const HfVert& v1,
                 const HfVert& v2) {
    // Signed area in the xz plane with x right / z up on paper: negative is
    // counter-clockwise from above (see the voxel emit_quad analysis).
    const float area2 = (v1.p.x - v0.p.x) * (v2.p.z - v0.p.z) -
                        (v1.p.z - v0.p.z) * (v2.p.x - v0.p.x);
    if (area2 == 0.0f) return;  // degenerate (collapsed stitch cell)
    const HfVert* a = &v1;
    const HfVert* c = &v2;
    if (area2 > 0.0f) std::swap(a, c);
    const HfVert* vs[3] = {&v0, a, c};
    for (const HfVert* v : vs) {
        b.positions.push_back(v->p.x);
        b.positions.push_back(v->p.y);
        b.positions.push_back(v->p.z);
        b.normals.push_back(v->n.x);
        b.normals.push_back(v->n.y);
        b.normals.push_back(v->n.z);
    }
}

} // namespace

bool mesh_sector_heightfield(const terrain_field::FieldRuntime& field,
                             int64_t tx, int64_t tz, int lod, int edge_mask,
                             float sector_size, float y_min, float y_max,
                             SectorMesh& out, std::string& err) {
    if (lod < 0 || lod > 4) {
        err = "terrain_mesher: heightfield lod out of 0..4";
        return false;
    }
    if (edge_mask < 0 || edge_mask > 15) {
        err = "terrain_mesher: edge mask out of 0..15";
        return false;
    }
    if (lod == 0 && edge_mask != 0) {
        err = "terrain_mesher: lod 0 is the coarsest level and cannot have a "
              "coarser neighbor (edge mask must be 0)";
        return false;
    }
    if (sector_size <= 0.0f || y_min >= y_max) {
        err = "terrain_mesher: bad slab config";
        return false;
    }

    const int N = 1 << lod;
    const double ox = double(tx) * double(sector_size);
    const double oz = double(tz) * double(sector_size);
    // Lattice coordinate: double(S) * i / N is exact for power-of-two N, so a
    // coarse neighbor (N/2 lattice) computes bitwise-identical coordinates for
    // the shared boundary points.
    auto lx = [&](int i) -> float {
        return float(double(sector_size) * double(i) / double(N));
    };

    const float cell = sector_size / float(N);

    // Area-filtered height sampling. Point-sampling the field at coarse
    // lattices aliases its ridged high-frequency layers into sawtooth spikes
    // (a 16 m lattice across a 200 m-wavelength ridged crease randomly clips
    // crests and troughs), so each vertex takes a 5-tap box filter at
    // R = cell / 2 — UNIFORM across the sector. A per-vertex radius that
    // matched the coarse neighbor on masked borders was tried first; it made
    // the 2:1 edge itself bitwise but broke the far more numerous same-LOD
    // corners wherever a band boundary steps (two equal-LOD sectors computed
    // a shared corner at different radii -> visible slits). With a uniform
    // radius every equal-LOD border is bitwise-identical in heights AND
    // normals; a 2:1 band border leaks only the (bounded, few-meter) delta
    // between the two filter scales, which the depth-scaled skirts on both
    // sides cover.
    const float filter_r = 0.5f * cell;
    auto filtered_height = [&](float wx, float wz) -> float {
        const float r = filter_r;
        return (field.height_at(wx, wz) +
                field.height_at(wx + r, wz) + field.height_at(wx - r, wz) +
                field.height_at(wx, wz + r) + field.height_at(wx, wz - r)) *
               0.2f;
    };

    // Heights once per lattice point.
    std::vector<float> heights(size_t(N + 1) * size_t(N + 1));
    auto hat = [&](int i, int k) -> float& {
        return heights[size_t(k) * size_t(N + 1) + size_t(i)];
    };
    for (int k = 0; k <= N; ++k) {
        for (int i = 0; i <= N; ++i) {
            const float h = filtered_height(float(ox + double(lx(i))),
                                            float(oz + double(lx(k))));
            if (!std::isfinite(h)) {
                err = "terrain_mesher: non-finite height";
                return false;
            }
            if (h < y_min || h > y_max) {
                err = "terrain_mesher: sampled height outside authored Y range";
                return false;
            }
            hat(i, k) = h;
        }
    }

    // Vertex table with gradient normals differentiated from the SAME
    // filtered height function as the positions (probe = half a cell,
    // clamped to the voxel path's 2 m at the finest levels). A fixed 2 m
    // point probe was tried first and lit distant sectors terribly — at a
    // 64 m quad it samples four essentially random micro-slopes of the
    // ±6 m surface noise, so far tiles shaded as noise instead of as their
    // filtered slope. Built lazily per vertex; odd vertices on masked
    // borders are never requested.
    const float probe = std::max(2.0f, 0.5f * cell);
    std::vector<HfVert> verts(size_t(N + 1) * size_t(N + 1));
    std::vector<uint8_t> vert_ready(size_t(N + 1) * size_t(N + 1), 0);
    auto vert = [&](int i, int k) -> const HfVert& {
        const size_t idx = size_t(k) * size_t(N + 1) + size_t(i);
        if (!vert_ready[idx]) {
            const float wx = float(ox + double(lx(i)));
            const float wz = float(oz + double(lx(k)));
            const float gx = filtered_height(wx + probe, wz) -
                             filtered_height(wx - probe, wz);
            const float gz = filtered_height(wx, wz + probe) -
                             filtered_height(wx, wz - probe);
            V3 n{-gx / (2.0f * probe), 1.0f, -gz / (2.0f * probe)};
            const float len =
                std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            n = {n.x / len, n.y / len, n.z / len};
            verts[idx] = HfVert{V3{lx(i), hat(i, k), lx(k)}, n};
            vert_ready[idx] = 1;
        }
        return verts[idx];
    };

    auto bucket_at = [&](float local_x, float local_z) -> MaterialBucket& {
        return bucket_for(out, uint32_t(field.material_at(
                                   float(ox + double(local_x)),
                                   float(oz + double(local_z)))));
    };
    auto emit = [&](const HfVert& a, const HfVert& b, const HfVert& c) {
        const float cx = (a.p.x + b.p.x + c.p.x) / 3.0f;
        const float cz = (a.p.z + b.p.z + c.p.z) / 3.0f;
        push_hf_tri(bucket_at(cx, cz), a, b, c);
    };

    const bool mask_px = (edge_mask & kEdgePosX) != 0;
    const bool mask_nx = (edge_mask & kEdgeNegX) != 0;
    const bool mask_pz = (edge_mask & kEdgePosZ) != 0;
    const bool mask_nz = (edge_mask & kEdgeNegZ) != 0;

    if (N == 1) {
        // LOD 0: a literal two-triangle quad (no masks possible).
        const HfVert& v00 = vert(0, 0);
        const HfVert& v10 = vert(1, 0);
        const HfVert& v01 = vert(0, 1);
        const HfVert& v11 = vert(1, 1);
        emit(v00, v01, v11);
        emit(v00, v11, v10);
    } else if (N == 2 && edge_mask != 0) {
        // LOD 1 with any coarser neighbor: the border cells are half the
        // sector, so the row patterns and corner patches overlap. Fan from
        // the center vertex around the boundary polyline instead (masked
        // edges contribute only their corner vertices; unmasked edges keep
        // their midpoint). This IS the collapsed result the patterns would
        // produce, expressed uniformly.
        const HfVert& center = vert(1, 1);
        // Boundary walk, counter-clockwise from above: -z edge left-to-right,
        // +x edge, +z edge right-to-left, -x edge.
        std::vector<const HfVert*> ring;
        auto add = [&](int i, int k) { ring.push_back(&vert(i, k)); };
        add(0, 0);
        if (!mask_nz) add(1, 0);
        add(2, 0);
        if (!mask_px) add(2, 1);
        add(2, 2);
        if (!mask_pz) add(1, 2);
        add(0, 2);
        if (!mask_nx) add(0, 1);
        for (size_t s = 0; s < ring.size(); ++s)
            emit(center, *ring[s], *ring[(s + 1) % ring.size()]);
    } else {
        // General case (N >= 4, and N == 2 unmasked which reduces to plain
        // quads below).
        //
        // Cell coverage plan:
        //  - cells in a masked border row/column are covered by segment
        //    patterns and corner patches;
        //  - everything else is an ordinary two-triangle quad.
        auto in_masked_row = [&](int ci, int ck) -> bool {
            return (mask_nz && ck == 0) || (mask_pz && ck == N - 1) ||
                   (mask_nx && ci == 0) || (mask_px && ci == N - 1);
        };
        for (int ck = 0; ck < N; ++ck)
            for (int ci = 0; ci < N; ++ci) {
                if (in_masked_row(ci, ck)) continue;
                emit(vert(ci, ck), vert(ci, ck + 1), vert(ci + 1, ck + 1));
                emit(vert(ci, ck), vert(ci + 1, ck + 1), vert(ci + 1, ck));
            }

        // Segment pattern along one masked edge. The edge is parameterized by
        // u in [0, N] along the border with a lambda mapping (u, v) lattice
        // coordinates to grid (i, k): v = 0 is the border row, v = 1 the
        // interior row. Each coarse segment m covers fine cells u = 2m and
        // 2m+1 with three triangles against the even border vertices.
        auto stitch_edge = [&](auto&& map, bool corner_lo, bool corner_hi) {
            const int segments = N / 2;
            for (int m = 0; m < segments; ++m) {
                if (m == 0 && corner_lo) continue;       // corner patch owns it
                if (m == segments - 1 && corner_hi) continue;
                const HfVert& A0 = map(2 * m, 0);
                const HfVert& A1 = map(2 * m + 2, 0);
                const HfVert& i0 = map(2 * m, 1);
                const HfVert& i1 = map(2 * m + 1, 1);
                const HfVert& i2 = map(2 * m + 2, 1);
                emit(A0, i0, i1);
                emit(A0, i1, A1);
                emit(A1, i1, i2);
            }
        };
        // Corner patch where two masked edges meet: covers the L-shaped
        // region of cells {(0,0),(1,0),(0,1)} in a corner-local frame where
        // (u, v) are the two lattice axes leaving the corner. Four
        // triangles: {C,A1,i11}, {A1,i21,i11}, {C,i11,B1}, {B1,i11,i12}
        // with C the corner vertex, A1/B1 the first even border vertices
        // along each edge, and i-- interior vertices.
        auto corner_patch = [&](auto&& map) {
            const HfVert& C = map(0, 0);
            const HfVert& A1 = map(2, 0);
            const HfVert& B1 = map(0, 2);
            const HfVert& i11 = map(1, 1);
            const HfVert& i21 = map(2, 1);
            const HfVert& i12 = map(1, 2);
            emit(C, A1, i11);
            emit(A1, i21, i11);
            emit(C, i11, B1);
            emit(B1, i11, i12);
        };

        // Edge frames. map(u, v): u along the border, v into the interior.
        auto map_nz = [&](int u, int v) -> const HfVert& { return vert(u, v); };
        auto map_pz = [&](int u, int v) -> const HfVert& { return vert(u, N - v); };
        auto map_nx = [&](int u, int v) -> const HfVert& { return vert(v, u); };
        auto map_px = [&](int u, int v) -> const HfVert& { return vert(N - v, u); };

        // Corner frames (u, v are the two axes leaving the corner).
        auto corner_nz_nx = [&](int u, int v) -> const HfVert& { return vert(u, v); };
        auto corner_nz_px = [&](int u, int v) -> const HfVert& { return vert(N - u, v); };
        auto corner_pz_nx = [&](int u, int v) -> const HfVert& { return vert(u, N - v); };
        auto corner_pz_px = [&](int u, int v) -> const HfVert& { return vert(N - u, N - v); };

        const bool c_nz_nx = mask_nz && mask_nx;
        const bool c_nz_px = mask_nz && mask_px;
        const bool c_pz_nx = mask_pz && mask_nx;
        const bool c_pz_px = mask_pz && mask_px;

        if (mask_nz) stitch_edge(map_nz, c_nz_nx, c_nz_px);
        if (mask_pz) stitch_edge(map_pz, c_pz_nx, c_pz_px);
        if (mask_nx) stitch_edge(map_nx, c_nz_nx, c_pz_nx);
        if (mask_px) stitch_edge(map_px, c_nz_px, c_pz_px);
        if (c_nz_nx) corner_patch(corner_nz_nx);
        if (c_nz_px) corner_patch(corner_nz_px);
        if (c_pz_nx) corner_patch(corner_pz_nx);
        if (c_pz_px) corner_patch(corner_pz_px);

        // An unmasked border row cell adjacent to a masked perpendicular
        // edge is NOT covered above when it sits in the masked column — that
        // case is already handled because in_masked_row() excluded it and
        // the perpendicular edge's own pattern/corner covers it. Nothing
        // further to emit.
    }

    // Border skirts REMOVED 2026-07-30, with the voxel path's (see there for
    // the full rationale). This path never needed them at all: the edge-mask
    // re-triangulation above already emits a border polyline bitwise-identical
    // to the coarse neighbour's own edge, so a masked seam is watertight with
    // no T-vertices and there is no crack for a curtain to hide.
    return true;
}

} // namespace terrain_mesher
