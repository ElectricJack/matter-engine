#pragma once
// seam_boundary.h — the sparse per-face boundary record a tile bake exports, and
// the global cell/lattice indexing the seam welder joins tiles by.
//
// Design: docs/volumetric-sectors-design-2026-08-10.md §4.1.
//
// WHY THIS EXISTS. Cross-level seams used to be closed at BAKE time, by telling
// the mesher which neighbours were one level coarser (`edge_mask`). That made a
// tile's geometry depend on its neighbours' levels, which (a) put a guess about
// the DESIRED map into the tile's bake identity, and (b) opened a one-voxel strip
// every time the drawn neighbour disagreed with that guess — mid-split, mid-merge,
// parked, or simply still baking. The fix is to bake nothing about the neighbour:
// each tile exports the sparse set of vertices sitting on each of its six faces,
// and an engine-side welder builds the crossing band at runtime from the two
// ACTUAL drawn tiles. Equal-level pairs export records too, but need no weld —
// they already meet bitwise (shared samples, `[1..n]` ownership).
//
// No dependencies beyond the standard library: this header is shared by the
// mesher (producer), the welder (consumer), and the bake/publish plumbing that
// carries records between them.

#include <cstdint>
#include <vector>

namespace seam {

// ---------------------------------------------------------------------------
// Faces
// ---------------------------------------------------------------------------
//
// Six faces, ordered so the xz four keep the historical edge-mask order
// (+x, -x, +z, -z) that WorldSector.js and the streamer used. +y/-y are unused
// in M0 (the grid is still XZ-columnar) and become live in M2 when Y is tiled.
enum Face : int {
    kFacePosX = 0,
    kFaceNegX = 1,
    kFacePosZ = 2,
    kFaceNegZ = 3,
    kFacePosY = 4,
    kFaceNegY = 5,
    kFaceCount = 6,
};

// The axis a face's normal runs along: 0 = x, 1 = y, 2 = z.
inline int face_axis(int face) {
    switch (face) {
        case kFacePosX: case kFaceNegX: return 0;
        case kFacePosY: case kFaceNegY: return 1;
        default:                        return 2;
    }
}
// True for the +side faces (the ones whose plane is at the tile's upper bound).
inline bool face_is_positive(int face) {
    return face == kFacePosX || face == kFacePosZ || face == kFacePosY;
}
inline int face_opposite(int face) {
    return face_is_positive(face) ? face + 1 : face - 1;
}

// The two tangential axes of a face, in a FIXED order per normal axis, so both
// sides of a shared plane agree on which index is `a` and which is `b` without
// negotiating. Normal x -> (y, z); normal y -> (x, z); normal z -> (x, y).
inline void face_tangent_axes(int face, int& a_axis, int& b_axis) {
    const int ax = face_axis(face);
    if (ax == 0)      { a_axis = 1; b_axis = 2; }
    else if (ax == 1) { a_axis = 0; b_axis = 2; }
    else              { a_axis = 0; b_axis = 1; }
}

// ---------------------------------------------------------------------------
// Global cell indexing
// ---------------------------------------------------------------------------
//
// Every tile at rung r shares one infinite lattice of spacing `voxel(r)`. A cell
// is named by the GLOBAL index of its minimum lattice corner, so the same cell
// has the same name whichever tile meshed it, and a coarse cell at rung r-1
// covers exactly the fine cells `2g` and `2g+1` at rung r. That is the whole
// basis of the welder's 2:1 fan: `coarse = floor_div2(fine)`.
//
// For a tile with `n` cells per axis, local cell index ci in [1..n] (ci 0 and
// n+1 are the ghost ring the mesher samples but does not own) maps to
//   global = tile_index * n + (ci - 1).
//
// Arithmetic right shift is floor division for negative values on every
// compiler this project builds with, but say it explicitly rather than rely on
// it — tile indices are routinely negative.
inline int64_t floor_div2(int64_t v) { return v >= 0 ? (v >> 1) : -((-v + 1) >> 1); }

// Voxel size for a rung, matching terrain_mesher's ladder about a 2 m base:
// rung 3 -> 0.25 m, rung 0 -> 2 m, rung -5 -> 64 m. Larger rung = finer.
inline double rung_voxel(int rung) {
    return rung >= 0 ? 2.0 / double(int64_t(1) << rung)
                     : 2.0 * double(int64_t(1) << -rung);
}

// ---------------------------------------------------------------------------
// The record
// ---------------------------------------------------------------------------

// One boundary cell that produced a surface-nets vertex, on one face.
//
// `a`/`b` are GLOBAL cell indices along the face's two tangential axes, at this
// tile's own rung. The third (normal-axis) index is implied by the face and is
// stored once on the FaceRecord.
//
// `corner_signs` carries the density sign (bit set = solid, density > 0) at the
// four corners of THIS CELL'S FACE ON THE PLANE:
//   bit 0 = (a,   b  )   bit 1 = (a+1, b  )
//   bit 2 = (a,   b+1)   bit 3 = (a+1, b+1)
// That is what lets the welder enumerate crossing edges on the shared plane
// without a dense sign image: an edge on the plane crosses iff its two endpoint
// signs differ, and if it crosses then BOTH cells adjacent to it necessarily
// have a sign change and therefore appear in this list. Four bits per sparse
// entry replaces an (n+1)^2 bitset per face.
//
// Position is WORLD-absolute and `double`. Records are tens of entries per face,
// so the width costs nothing, and it keeps the welder free of the tile-local
// rebasing (and its float cancellation at 10 km out) that the mesh itself uses.
struct BoundaryVert {
    int64_t  a = 0;
    int64_t  b = 0;
    double   px = 0, py = 0, pz = 0;   // world position of the cell's vertex
    float    nx = 0, ny = 1, nz = 0;   // outward normal (already normalized)
    uint32_t material = 0;             // post-mapping material id
    uint8_t  corner_signs = 0;         // see above
};

// ---------------------------------------------------------------------------
// The overlap band (M0-WP7)
// ---------------------------------------------------------------------------
//
// WHY A SECOND, DIFFERENT THING RIDES ON A FACE RECORD. The vertex list above
// lets the welder BUILD a crossing band out of the two tiles' own vertices. That
// works wherever both sides have a vertex to land on. It cannot work where the
// coarse side has none — at 2x the voxel a boundary cell can see no sign change
// at all, so the coarse tile has no surface anywhere near the plane there, and
// §4.1's "collapsed cap" has nothing to collapse onto either. Measured (M0-WP6):
// four heightfield scans, one gapped row each, 0.75-0.88 fine voxels wide. No
// fan among the FINE vertices can close those — it would close the fine mesh's
// opening and leave the same void on the coarse side.
//
// What closed them before this design existed was the retired `reach_x/reach_z`:
// the fine tile's lattice was extended one COARSE voxel past its -x/-z border so
// the two meshes OVERLAPPED. That mechanism was sound; what made it unusable was
// its mask gate — it fired only when the bake had guessed the neighbour's level
// correctly. The gate existed for one reason: at an EQUAL-level seam the extra
// geometry is duplicate coplanar surface, so it must not be drawn.
//
// M0-WP7 therefore applies this design's own philosophy one level deeper: the
// mesher SAMPLES the extra columns unconditionally, keeps them out of the mesh
// (world-space ownership is exactly what it was — the emitted mesh is bitwise
// unchanged), and exports the triangles it WOULD have emitted as this band. The
// welder, which can see both ACTUAL drawn tiles, decides at runtime whether to
// draw them: cross-level yes, equal-level no. The neighbour guess is gone; only
// the geometry survived.
//
// TRIANGLES, NOT VERTICES. This band is emitted verbatim — the welder copies it
// and rebases it, it does not reconstruct anything from it. So it is a triangle
// soup and not a sparse cell record: there is no join to perform, hence nothing
// to index by. Positions are WORLD-absolute `double` for the same reason
// BoundaryVert's are (see above): the welder rebases once against its own
// origin, and a band that spans two tiles cannot inherit either one's.
//
// Layout mirrors seam::WeldBucket and terrain_mesher::MaterialBucket (9 scalars
// of position and 9 of normal per triangle, bucketed by material) so the copy
// into a WeldMesh is a loop and not a translation.
struct OverlapBucket {
    uint32_t material = 0;
    std::vector<double> positions;  // 9 doubles per triangle, WORLD-absolute
    std::vector<float>  normals;    // 9 floats per triangle
};

struct OverlapBand {
    std::vector<OverlapBucket> buckets;

    size_t triangle_count() const {
        size_t n = 0;
        for (const OverlapBucket& b : buckets) n += b.positions.size() / 9;
        return n;
    }
    bool empty() const { return triangle_count() == 0; }
};

// The vertices a tile put on one of its six faces.
//
// `plane` is the global LATTICE index of the shared plane along the normal axis,
// at this tile's rung — the coordinate the neighbour must agree on. `cell_layer`
// is the global CELL index of the single cell layer this record describes: the
// tile's own outermost owned layer on that side. For a +side face the cell layer
// is `plane - 1`; for a -side face it is `plane`.
//
// Entries are sorted by (a, b) so lookup can binary-search, and so two bakes of
// the same tile produce byte-identical records (determinism gate).
struct FaceRecord {
    int32_t face = kFacePosX;
    int64_t plane = 0;
    int64_t cell_layer = 0;
    std::vector<BoundaryVert> verts;

    // The overlap band for this face (M0-WP7). Populated by the mesher on
    // kFaceNegX and kFaceNegZ ONLY, and empty on the other four: the [1..n]
    // ownership rule is direction-asymmetric, so a tile already overshoots its
    // -x/-z border and already stops short of its +x/+z one. It is the +side
    // faces that are bridged by the neighbour, and a band there would be
    // overlap on top of overlap.
    OverlapBand band;

    const BoundaryVert* find(int64_t a, int64_t b) const {
        size_t lo = 0, hi = verts.size();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            const BoundaryVert& v = verts[mid];
            if (v.a < a || (v.a == a && v.b < b)) lo = mid + 1;
            else                                  hi = mid;
        }
        if (lo < verts.size() && verts[lo].a == a && verts[lo].b == b)
            return &verts[lo];
        return nullptr;
    }
};

// Everything one tile bake exports for seam purposes. Rides on the staged bake
// artifact and lands on the engine's SectorEntry; it is NOT part of the tile's
// bake identity — no field of it depends on any neighbour.
struct SectorBoundary {
    int      rung = 0;       // this tile's rung (finer = larger)
    int      cells = 0;      // cells per axis (n); with the tile size this fixes the lattice
    int64_t  tx = 0, ty = 0, tz = 0;
    FaceRecord faces[kFaceCount];

    // NOTE: `empty` and `vert_count` speak about the VERTEX records only, not
    // the overlap bands — they are the "is there anything to weld by" question
    // and predate the band. `band_triangle_count` is the band's own.
    bool empty() const {
        for (const FaceRecord& f : faces)
            if (!f.verts.empty()) return false;
        return true;
    }
    size_t vert_count() const {
        size_t n = 0;
        for (const FaceRecord& f : faces) n += f.verts.size();
        return n;
    }
    size_t band_triangle_count() const {
        size_t n = 0;
        for (const FaceRecord& f : faces) n += f.band.triangle_count();
        return n;
    }
};

} // namespace seam
