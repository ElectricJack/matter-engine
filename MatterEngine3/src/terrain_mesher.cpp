// terrain_mesher.cpp — naive surface-nets sector mesher.
// Pure CPU; no JS, no GL.

#include "terrain_mesher.h"
#include <cmath>
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

    // Use the full slab range so adjacent sectors share the same Y lattice alignment.
    // Per-sector Y clipping would shift y0 independently, breaking watertight seams.
    const float y0 = y_min;
    const float y1 = y_max;
    const int   ny = std::max(1, int(std::ceil((y1 - y0) / voxel)));

    // Density lattice dimensions:
    //   x/z: (n+3) samples — one ring outside on each side (i=-1..n+1)
    //   y:   (ny+1) samples — from y0 to y0 + ny*voxel
    const int sx = n + 3, sy = ny + 1, szn = n + 3;
    std::vector<float> d(size_t(sx) * size_t(sy) * size_t(szn));
    auto at = [&](int i, int j, int k) -> float& {
        return d[(size_t(k) * size_t(sy) + size_t(j)) * size_t(sx) + size_t(i)];
    };
    for (int k = 0; k < szn; ++k)
        for (int j = 0; j < sy; ++j)
            for (int i = 0; i < sx; ++i)
                at(i, j, k) = field.density_at(
                    float(ox + (i - 1) * double(voxel)),
                    y0 + j * voxel,
                    float(oz + (k - 1) * double(voxel)));

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

    return true;
}

} // namespace terrain_mesher
