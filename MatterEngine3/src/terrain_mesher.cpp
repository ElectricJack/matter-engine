// terrain_mesher.cpp — naive surface-nets sector mesher.
// Pure CPU; no JS, no GL.

#include "terrain_mesher.h"
#include "bake_mode.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

// --- overlap band (M0-WP7) -------------------------------------------------
// Same two helpers against seam::OverlapBand. Positions go out WORLD-absolute
// and in double (CellVert stores x/z tile-local, y world), because the band is
// consumed by the welder, which spans two tiles and rebases against an origin of
// its own. See the long note in seam_boundary.h.
seam::OverlapBucket& band_bucket_for(seam::OverlapBand& band, uint32_t mat) {
    for (auto& b : band.buckets) if (b.material == mat) return b;
    band.buckets.push_back(seam::OverlapBucket{});
    band.buckets.back().material = mat;
    return band.buckets.back();
}

// `tile_local_y` says whether CellVert::p.y is tile-local (the Y-TILED path, M2)
// or already world-absolute (the COLUMN path). It is a branch rather than an
// unconditional `oy + p.y` with oy = 0 on the column path because the column
// path's band bytes are pinned bitwise (terrain_mesher_tests "M0-WP7"), and
// `0.0 + -0.0` is `+0.0` -- a one-value difference that no plausible test would
// catch and that would be a real change to a value the cache keys on.
void push_band_tri(seam::OverlapBucket& b, double ox, double oy, double oz,
                   bool tile_local_y,
                   const CellVert& a, const CellVert& c, const CellVert& d) {
    const CellVert* vs[3] = {&a, &c, &d};
    for (const CellVert* v : vs) {
        b.positions.push_back(ox + double(v->p.x));
        b.positions.push_back(tile_local_y ? oy + double(v->p.y)
                                           : double(v->p.y));
        b.positions.push_back(oz + double(v->p.z));
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

// ===========================================================================
// THE CANONICAL SHARED CONTOUR  (docs/contour-seam-design-2026-08-13.md)
// ===========================================================================
//
// WHAT PROBLEM THIS SOLVES, in one paragraph. Surface nets put a tile's vertex
// at the centroid of its cell's edge crossings, which is a point strictly
// INSIDE the cell -- so where two tiles at different rungs meet, neither can
// name the other's vertices without knowing the other's lattice, and that is
// what forced every previous seam mechanism to be a stitch between two
// different curves. A stitch either gaps or overlaps, and coplanar overlap is
// ruled out (issue 736f92da: the texturing artefacts it causes). The way out is
// to give the SHARED PLANE a curve of its own that is a function of (plane,
// field) and nothing else, so both tiles compute it independently and get the
// same doubles. Each tile then terminates its own surface ON that curve, and
// the two meet there exactly -- no stitch, nothing drawn twice.
//
// WHY THE RESOLUTION IS THE FINEST RUNG AND NOT THE PAIR'S. A rule f that both
// sides of every legal pair can evaluate without knowing the other's level must
// satisfy f(L) = f(L+1) for every L, which forces f constant; and the constant
// has to serve the finest tile that can ever touch a plane, or that tile's own
// lattice would out-resolve the boundary it must land on. Hence
// `kCanonicalVoxel` -- rung 0, 2 m, the finest the octree's level-0 tile uses.
// It is a real cost on coarse tiles (their border geometry scales with contour
// LENGTH at 2 m, not with their own cell count) and the design doc's table
// measures it at ~+40% world triangles. That cost was accepted deliberately.
//
// WHY A CONTOUR VERTEX IS SHAREABLE WHEN A SURFACE-NETS VERTEX IS NOT. This is
// a PRIMAL construction: a vertex is the interpolation along a NAMED lattice
// edge -- from canonical lattice point (ia, ib) toward (ia+1, ib) or
// (ia, ib+1) -- and the lattice is anchored to the WORLD ORIGIN, never to a
// tile. Two tiles of any sizes asking about the same plane therefore walk the
// same lattice points, evaluate the field at bitwise-identical coordinates
// (the dyadic-double argument derived at `oy` in mesh_sector_impl), and get
// bitwise-identical crossings. The impossibility argument in
// volumetric-sectors-design-2026-08-10.md is about centroids inside cells and
// does not reach this; the same paragraph concedes a named-edge interpolation
// can be shared.
//
// The foundation gates are contour_seam_tests.cpp (`run-contourseam`): the
// agreement is bitwise, a tile-anchored lattice is tested and must FAIL, and
// tracing finds the same vertex set as exhaustive sampling.

// Rung 0. See "WHY THE RESOLUTION IS THE FINEST RUNG" above.
//
// LIMIT, stated rather than guarded: a tile whose own voxel is FINER than this
// (rung > 0) would out-resolve the canonical lattice, and its boundary would be
// coarser than its interior. No world does that -- the octree pairs level L
// with rung -L, so level 0 is rung 0 and nothing is finer -- and the fix if one
// ever does is to lower this constant for the whole world, not per tile: two
// tiles that disagree about it agree about nothing.
constexpr double kCanonicalVoxel = 2.0;

// Tangential axis order, fixed so both sides of a plane agree without
// negotiating: normal x -> (y, z), normal y -> (x, z), normal z -> (x, y).
// Identical to seam::face_tangent_axes; kept local so the contour construction
// depends on nothing but the field.
void contour_tangent_axes(int axis, int& a_axis, int& b_axis) {
    if (axis == 0)      { a_axis = 1; b_axis = 2; }
    else if (axis == 1) { a_axis = 0; b_axis = 2; }
    else                { a_axis = 0; b_axis = 1; }
}

struct ContourVert {
    int64_t ia = 0, ib = 0;   // the lattice point the crossed edge starts at
    int     dir = 0;          // 0 = a-edge, 1 = b-edge
    double  pa = 0, pb = 0;   // interpolated world position, tangential coords
};

struct Contour {
    std::vector<ContourVert> verts;
    std::vector<std::pair<int, int>> segs;   // indices into verts
};

// World coordinate of canonical lattice index i. A product, never an
// accumulation: the whole point is that two callers starting from different
// indices land on the same bits, and vc is a power of two so this is exact.
inline double contour_lat(int64_t i, double vc) { return double(i) * vc; }

// Build the contour on one face square, by TRACING from seeds rather than
// sampling the square.
//
// Sampling would be O(area) at the canonical voxel, which is ruinous exactly
// where tiles are cheapest today: a level-5 face is 1024 m across, so 512x512
// canonical points, a quarter of a million field evaluations per face and one
// and a half million per tile. Tracing is O(curve): follow each component cell
// by cell from a point known to be on it. `run-contourseam` gate [3] pins that
// the two find the SAME vertex set, which is what makes this an optimisation
// rather than a second, sloppier contour.
//
// SEEDS are this tile's own border cells that produced a dual vertex, in
// own-rung cell coordinates local to the square. That set is exactly right and
// the reasoning is worth keeping: the fan below needs contour segments only in
// the footprint of a border cell that HAS a vertex to fan from, and a cell with
// a sign change anywhere on its face square necessarily has one (the square is
// a face of the cell, so a sign change on it is a sign change in it). So
// seeding from dual-vertex cells is a superset of seeding from face-mixed
// cells, and it is a superset by the cases that matter -- a cell whose surface
// enters it without touching this face contributes a seed that finds nothing,
// which costs one sign test.
//
// A component NO seed reaches is skipped, and that is correct rather than a
// gap: if this tile's lattice sees no surface there, it owes no geometry there.
// The finer neighbour that does see it terminates on its own contour, and this
// tile's plane is simply empty at that spot -- which is what "the coarse LOD
// does not have this tunnel" looks like when it is stated honestly.
Contour trace_contour(const terrain_field::FieldRuntime& field, int axis,
                      double plane, double a0, double a1, double b0, double b1,
                      double vc, int64_t own_steps,
                      const std::vector<std::pair<int, int>>& seeds) {
    Contour c;
    if (seeds.empty()) return c;

    const int64_t ia0 = int64_t(std::llround(a0 / vc));
    const int64_t ia1 = int64_t(std::llround(a1 / vc));
    const int64_t ib0 = int64_t(std::llround(b0 / vc));
    const int64_t ib1 = int64_t(std::llround(b1 / vc));
    if (ia1 <= ia0 || ib1 <= ib0) return c;

    int aa = 0, bb = 0;
    contour_tangent_axes(axis, aa, bb);

    // One density per lattice point, memoised, so both edge directions read the
    // SAME value. Evaluating a point twice would be correct in exact arithmetic
    // and is a bit-level hazard in floating point.
    const int64_t stride = ib1 - ib0 + 2;
    std::unordered_map<int64_t, float> samples;
    auto pack = [&](int64_t i, int64_t j) -> int64_t {
        return (i - ia0) * stride + (j - ib0);
    };
    auto sample = [&](int64_t i, int64_t j) -> float {
        const int64_t k = pack(i, j);
        auto it = samples.find(k);
        if (it != samples.end()) return it->second;
        float p[3];
        p[axis] = float(plane);
        p[aa]   = float(contour_lat(i, vc));
        p[bb]   = float(contour_lat(j, vc));
        const float d = field.density_at(p[0], p[1], p[2]);
        samples.emplace(k, d);
        return d;
    };

    std::unordered_map<int64_t, int> vindex;
    auto vert = [&](int64_t i, int64_t j, int dir) -> int {
        const int64_t k = pack(i, j) * 2 + dir;
        auto it = vindex.find(k);
        if (it != vindex.end()) return it->second;
        const float da = sample(i, j);
        const float db = dir == 0 ? sample(i + 1, j) : sample(i, j + 1);
        // Fixed operand order, so both sides of the plane compute this from
        // identical inputs in identical order. Reversing it would not be
        // bitwise reproducible.
        const double t = double(da) / (double(da) - double(db));
        ContourVert v;
        v.ia = i; v.ib = j; v.dir = dir;
        v.pa = contour_lat(i, vc) + (dir == 0 ? t * vc : 0.0);
        v.pb = contour_lat(j, vc) + (dir == 1 ? t * vc : 0.0);
        const int id = int(c.verts.size());
        c.verts.push_back(v);
        vindex.emplace(k, id);
        return id;
    };

    std::vector<std::pair<int64_t, int64_t>> stack;
    for (const auto& s : seeds) {
        const int64_t i0 = ia0 + int64_t(s.first) * own_steps;
        const int64_t j0 = ib0 + int64_t(s.second) * own_steps;
        // The own-rung cell is mixed somewhere, so SOME canonical cell inside it
        // is; push them all and let the visited set collapse the duplicate work.
        for (int64_t jj = j0; jj < j0 + own_steps; ++jj)
            for (int64_t ii = i0; ii < i0 + own_steps; ++ii)
                stack.push_back({ii, jj});
    }

    std::unordered_set<int64_t> seen;
    while (!stack.empty()) {
        const auto cell = stack.back();
        stack.pop_back();
        const int64_t i = cell.first, j = cell.second;
        if (i < ia0 || j < ib0 || i + 1 > ia1 || j + 1 > ib1) continue;
        if (!seen.insert(pack(i, j)).second) continue;

        const float c00 = sample(i, j),     c10 = sample(i + 1, j);
        const float c01 = sample(i, j + 1), c11 = sample(i + 1, j + 1);
        const bool s00 = c00 > 0, s10 = c10 > 0;
        const bool s01 = c01 > 0, s11 = c11 > 0;
        const int code = (s00 ? 1 : 0) | (s10 ? 2 : 0) | (s01 ? 4 : 0) |
                         (s11 ? 8 : 0);
        if (code == 0 || code == 15) continue;

        // Marching squares. Edge ids: B = bottom a-edge (i, j, 0), T = top
        // a-edge (i, j+1, 0), L = left b-edge (i, j, 1), R = right b-edge
        // (i+1, j, 1).
        //
        // The SADDLE (two diagonal corners solid) is resolved by a FIXED rule
        // rather than by a centre sample. Both sides of a plane must resolve it
        // identically, and a fixed rule is identical by construction where a
        // sampled one is merely usually identical -- and "usually" is a hole
        // that appears once per world and cannot be reproduced.
        auto B = [&] { return vert(i, j, 0); };
        auto T = [&] { return vert(i, j + 1, 0); };
        auto L = [&] { return vert(i, j, 1); };
        auto R = [&] { return vert(i + 1, j, 1); };
        switch (code) {
            case 1: case 14: c.segs.push_back({B(), L()}); break;
            case 2: case 13: c.segs.push_back({B(), R()}); break;
            case 4: case 11: c.segs.push_back({T(), L()}); break;
            case 8: case 7:  c.segs.push_back({T(), R()}); break;
            case 3: case 12: c.segs.push_back({L(), R()}); break;
            case 5: case 10: c.segs.push_back({B(), T()}); break;
            case 6:
                c.segs.push_back({B(), L()});
                c.segs.push_back({T(), R()});
                break;
            case 9:
                c.segs.push_back({B(), R()});
                c.segs.push_back({T(), L()});
                break;
            default: break;
        }

        // Follow the curve: any neighbour sharing a crossed edge is on it.
        stack.push_back({i + 1, j});
        stack.push_back({i - 1, j});
        stack.push_back({i, j + 1});
        stack.push_back({i, j - 1});
    }
    return c;
}

} // namespace

// ---------------------------------------------------------------------------
// ONE implementation, TWO axis regimes (M2)
// ---------------------------------------------------------------------------
//
// `y_tiled == false` is the COLUMN path: the historical `mesh_sector`, meshing
// [y_min, surface] in one slab anchored to a global Y lattice at y_min. Every
// world in the tree still uses it, and it is pinned BITWISE -- mesh and boundary
// record -- over six configurations in terrain_mesher_tests.cpp. It must not
// move by an ulp.
//
// `y_tiled == true` is the Y-TILED path (M2, design §3.3): the tile is a cube
// `[ty*S, (ty+1)*S)` and Y becomes an ordinary tiled axis alongside X and Z.
//
// WHY THE TWO COEXIST RATHER THAN THE SECOND REPLACING THE FIRST. A Y-tiled tile
// covers 64 m of height, so a world whose streamer still asks for one tile per
// column would mesh a thin band and nothing else. Covering the world takes a
// vertical STACK of requests, which is the streamer's job (M3, gated on the
// world flag `volumetricSectors`). Until that lands the column path is what
// every live world draws, so it stays, unchanged, and the regime is selected
// explicitly by which public entry point the caller uses.
//
// WHY ONE FUNCTION RATHER THAN TWO COPIES. The regimes differ in about forty
// lines out of four hundred -- the Y lattice's origin and bounds, the ownership
// predicate's third clause, the band's third face, and where a global Y cell
// index comes from. Everything else (the density fill, surface-nets placement,
// quad emission, the record export machinery) is identical, and this repo's
// history is unambiguous about what happens to a copy: `surface.c` was copied
// twice and drifted for a year (CLAUDE.md, "Code Sharing Between Projects").
// The bitwise pins are what make sharing safe here -- they fail loudly if the
// column branch ever picks up a Y-tiled expression by accident.
static bool mesh_sector_impl(const terrain_field::FieldRuntime& field,
                             int64_t tx, int64_t ty, int64_t tz, int rung,
                             bool y_tiled,
                             float sector_size, float y_min, float y_max,
                             SectorMesh& out, seam::SectorBoundary* boundary_out,
                             std::string& err) {
    // Rung is a power-of-two voxel ladder around a 2 m base, extending in BOTH
    // directions:
    //   rung  3 -> 0.25 m      rung  0 -> 2 m (the base)
    //   rung  2 -> 0.5  m      rung -1 -> 4 m
    //   rung  1 -> 1    m      rung -2 -> 8 m ... rung -5 -> 64 m
    //
    // The negative half is new. It exists so the terrain ladder can stay VOXEL
    // all the way out instead of switching representation to a heightfield at
    // distance: that switch was the visible seam between near voxel terrain and
    // the far field, and no amount of band tuning could hide it because the two
    // sides were different surfaces, not different resolutions of one.
    //
    // This function meshes ONE tile from the field alone. It is told nothing
    // about its neighbours and there is nothing it could be told: the mask that
    // used to name them described the level the streamer WANTED next door, not
    // the tile actually on screen, so it was wrong for the whole duration of
    // every split, merge, park and pending bake -- and it forced a re-bake under
    // a new cache key whenever a distant neighbour changed level. See the note
    // on mesh_sector in terrain_mesher.h for the retraction in full. Cross-level
    // seams are welded at runtime from `boundary_out` instead.
    //
    // -5 is the floor because a 64 m sector at 64 m voxels is a single cell;
    // anything coarser has no lattice left to march.
    if (rung < -5 || rung > 3) {
        err = "terrain_mesher: rung out of -5..3";
        return false;
    }
    if (sector_size <= 0.0f || (!y_tiled && y_min >= y_max)) {
        err = "terrain_mesher: bad slab config";
        return false;
    }

    const float voxel = rung >= 0 ? 2.0f / float(1 << rung)
                                  : 2.0f * float(1 << -rung);
    const int   n     = int(std::lround(double(sector_size) / double(voxel)));
    const double ox   = double(tx) * double(sector_size);
    const double oz   = double(tz) * double(sector_size);

    // ---- The Y tile origin, and why a shared lattice point is BITWISE equal --
    //
    // (Y-tiled path only; the column path has no `oy` -- its Y lattice is
    // anchored globally at `y_min`, see the slab block below.)
    //
    //     oy = double(ty) * double(sector_size)
    //     world y of lattice index j = oy + (j - 1) * voxel
    //
    // -- letter for letter the construction the X and Z axes already use. That
    // is not stylistic symmetry; the whole seam contract rests on it, so here is
    // the derivation rather than the assertion.
    //
    // Write S for sector_size and v for the voxel. Both are powers of two times
    // a small integer and exactly representable as float, hence exactly as
    // double. S = n*v with n an integer (`n` above; the ladder guarantees it).
    //
    //   1. `double(ty) * double(S)` is EXACT. ty is an integer well inside 2^53
    //      (the §3.1 key allows +-2^19 tiles) and S is a power of two times a
    //      small integer, so the product needs at most ~26 significant bits.
    //      No rounding, so oy is exactly the real number ty*S.
    //   2. `(j - 1) * double(v)` is EXACT for the same reason: a small integer
    //      times a power-of-two-scaled value.
    //   3. Their SUM is exact. Both are integer multiples of v, and their sum's
    //      magnitude is bounded by (|ty|+1)*S, so it needs no more mantissa bits
    //      than the operands. IEEE addition of two exactly-representable values
    //      whose exact sum is representable rounds to that sum.
    //
    // So the double is the exact real number, and `float(...)` of it is the
    // correctly-rounded float of that real number -- both determined by the
    // VALUE alone, not by how it was reached.
    //
    // Now take two tiles sharing a horizontal plane at world height Y, at rungs
    // r and r-m (voxels v and 2^m v, tile sizes S and 2^m S). Y is an integer
    // multiple of the COARSER voxel by construction: the coarse tile's border is
    // ty'*2^m*S = ty'*n*2^m*v. Each tile computes Y as `oy + (j-1)*v` for its
    // own j, and by (1)-(3) each computation is exact, so each produces the
    // double whose value is exactly Y. Two doubles with the same value are the
    // same double. Both tiles therefore hand `field.density_at` / `height_at`
    // BITWISE IDENTICAL arguments, so they read bitwise identical densities, and
    // their sign bits and interpolated vertex positions on that plane agree
    // exactly. That -- not any stitching -- is why an equal-level pair is
    // watertight and why the welder's `corner_signs` can be compared across a
    // plane at all (seam_boundary.h).
    //
    // The trap this avoids is accumulating y by repeated addition, or basing it
    // on the slab's `y0` (a float sum involving y_min, which is NOT a lattice
    // multiple in general). Either would make the shared plane's coordinate
    // depend on which tile asked.
    const double oy = double(ty) * double(sector_size);

    // ---- The direction-asymmetric bridge, and what is NOT done about it -----
    //
    // Face ownership below is `i in [1..n]`, and a +y quad at sample i uses
    // cells ci in {i-1, i}. So a tile's surface reaches ONE OF ITS OWN VOXELS
    // back past its -x border and stops half a voxel short of its +x border:
    // every shared plane is bridged by the tile on its EAST/NORTH side, using
    // THAT tile's voxel size. That is intentional and stays.
    //
    // At equal size the bridge lands exactly on the neighbour's last vertex
    // (same world samples, bitwise-identical) and the pair is watertight -- the
    // property that let the border skirts go, and the only seam guarantee this
    // function makes. Across a 2:1 size step it is asymmetric:
    //
    //   fine west / coarse east -- the COARSE tile bridges, with a 2v-wide ring
    //     cell reaching back to ~X0-v, past the fine mesh's last vertex at
    //     ~X0-v/2. Overlaps. Closed.
    //   coarse west / fine east -- the FINE tile bridges, reaching back only
    //     ~v/2, while the coarse mesh's last vertex is at ~X0-v. The strip
    //     between them is emitted by NEITHER tile: a one-voxel gap running the
    //     length of the seam. Measured at 0.88-1.12 fine voxels, every level.
    //
    // There USED to be a `reach_x`/`reach_z` here that extended the lattice one
    // coarse voxel outward on a MASKED -x/-z face so the fine bridge overshot
    // the coarse tile's last vertex. The mechanism worked; the mask gate is what
    // made it unusable — it could only fire when the bake had guessed the
    // neighbour's level correctly, and the mask is a statement about the level
    // map the streamer WANTS, which disagrees with what is drawn throughout
    // every split, merge, park and pending bake. A seam closer that is correct
    // only in the steady state closes a seam that was not open, and a tile whose
    // lattice depends on a neighbour guess is a tile that cannot be cached by
    // its own identity.
    //
    // The strip is therefore closed at RUNTIME instead, in two layers, and this
    // function's only remaining job is to export what each needs:
    //
    //   1. The seam welder builds a crossing band out of the two tiles' own
    //      boundary vertices (`boundary_out`, exported at the bottom). That is
    //      the watertight layer, and it closes the strip wherever both sides
    //      have a vertex to land on.
    //   2. Where the COARSE side has no vertex at all -- at 2x the voxel a
    //      boundary cell can see no sign change, so there is no surface anywhere
    //      near the plane and nothing to land on -- no fan among the fine
    //      vertices can help. M0-WP6 measured exactly that: four heightfield
    //      scans, one gapped row each, 0.75-0.88 fine voxels. So the OVERLAP
    //      band below carries the old reach-back geometry, and the welder draws
    //      it or not from the pair actually on screen.
    //
    // ---- The overlap band (M0-WP7) ------------------------------------------
    //
    // `kBandCells` extra lattice columns are sampled on the -x and -z sides,
    // UNCONDITIONALLY: no mask, no neighbour, nothing about this tile's identity
    // changes. 2 is the old `reach` and is what one COARSE voxel costs.
    //
    // THE EMITTED MESH IS BITWISE UNCHANGED, and that is a gate, not a hope
    // (terrain_mesher_tests "M0-WP7: the overlap band does not move the mesh"
    // pins mesh AND record hashes taken from the pre-band build). Three things
    // make it hold, all of them about NOT letting the extension shift an index:
    //
    //   * The lattice index `i` keeps its meaning: world x = ox + (i-1)*voxel,
    //     exactly as before. The band is reached by letting `i` go NEGATIVE, not
    //     by re-basing it -- only the STORAGE offset moves (`kBandCells` is
    //     added inside the accessors and nowhere else). Every float expression
    //     that computes a vertex position sees the identical operands.
    //   * `h_min`/`h_max`, and therefore the Y slab (`j0_global`, `y0`, `sy`),
    //     are reduced over the tile's OWN sample box only. Feeding band columns
    //     into them would move `y0` and shift every vertex in the tile.
    //   * `owned` is untouched: [1..n] in world terms, as it always was. The
    //     band is emitted by a SEPARATE predicate into a separate soup.
    //
    // The band is precisely "the triangles the mesher would have emitted with
    // ownership extended into those columns" -- with `reach = 2` the old owned
    // box was `i in [1, n+2]` in a lattice shifted down by 2, i.e. `i in [-1, n]`
    // in this one. So the extra samples are the L-shaped set
    // `[-1,n]x[-1,n] \ [1,n]x[1,n]`, split between the two faces by
    //
    //     kFaceNegX band: the extra samples with i <= 0
    //     kFaceNegZ band: the extra samples with k <= 0
    //
    // which covers the L exactly and puts the 2x2 CORNER block (i<=0 and k<=0)
    // in BOTH. That duplication is deliberate. The corner block's coverage must
    // not depend on which of the two faces happens to be the cross-level one; if
    // both are, the two copies are bit-identical triangles drawn twice, which
    // for opaque geometry is nothing at all, whereas a corner hole is exactly
    // the defect this band exists to remove.
    //
    // Within the band the fine surface deviates from the coarse one by the
    // field's curvature over a coarse voxel. It is an OVERLAP, not a stitch --
    // the same thing the +x/+z direction has always done, and the reason the
    // welder's vertex fan stays: the two close different failures.
    //
    // This was never nested-specific. The uniform ladder has the same hole
    // wherever a coarse tile sits west or south of a fine one; nesting only
    // made those borders common and put them near the camera.
    //
    // ---- ...AND WHAT REPLACES ALL OF IT (the contour mesher) -----------------
    //
    // Everything above -- the direction-asymmetric bridge, the two runtime
    // layers, the overlap band -- exists because a tile's surface stops at a
    // position that depends on the tile's own lattice, so two tiles at
    // different rungs cannot meet exactly and something has to cover the
    // difference. `bake_mode::contour_seams()` removes the premise instead:
    // the tile terminates exactly ON its six face planes, against a curve that
    // every tile touching that plane computes identically (see THE CANONICAL
    // SHARED CONTOUR above). Then there is no strip to cover, no band, no
    // welder, and no asymmetry -- a tile reaches neither past its - faces nor
    // short of its + ones.
    //
    // Y-TILED ONLY, and that is not a stopgap. The column path's output is
    // pinned BITWISE over six configurations (terrain_mesher_tests), a column
    // has no ±y neighbour to share a plane with, and the uniform-grid worlds
    // that still run it have no level ladder and therefore no cross-rung seam
    // to close. Nothing there is asking for this.
    const bool contour_seams = y_tiled && bake_mode::contour_seams();

    // No band when the contour rule is on: the band IS the overlap the ruling
    // forbids, and with the seam closed at the plane there is nothing for it to
    // cover. Dropping it to zero also takes back the ~11% of sampling cost the
    // extra columns carry, which is most of what the border contour then spends.
    const int kBandCells = contour_seams ? 0 : 2;

    // VOLUMETRIC = the world's density is not `height(x, z) - y`: tunnels,
    // caverns, overhangs. Two things below still specialise on it, and each of
    // the heightfield versions is a PROVABLE consequence of that identity rather
    // than an approximation -- the narrow Y slab and the analytic density
    // gradient in y. A volumetric world takes the general form of both; a
    // heightfield world keeps both shortcuts and meshes bit-identically to
    // before the field became 3D.
    //
    // There was a THIRD: the cross-rung seam snap, in a heightfield form that
    // rewrote the boundary height polyline and a volumetric form one dimension
    // up that rewrote the boundary density PLANE. Both are deleted with the
    // edge mask that drove them (see the retraction above). Nothing here reads
    // a neighbour any more, so nothing here has to guess about one.
    const bool volumetric = !field.is_heightfield();

    // Evaluate height once per X/Z lattice point, then mesh only a narrow Y
    // slab snapped to the authored global lattice. Neighboring sectors can use
    // different depths without shifting their shared sample coordinates.
    //
    // LATTICE INDEXING. Lattice index i sits at world x = ox + (i-1)*voxel, so
    // the tile's own edges are i == 1 and i == n+1. Unconditional, and unchanged
    // by the band: the tile's own n+1 lattice points plus one ghost ring on each
    // side run i in [0, n+2], and the band simply lets i reach `kBandCells`
    // further NEGATIVE. Nothing is re-based; only `idx_i`/`idx_k` know about the
    // extension, so every world-coordinate expression below is untouched.
    const int i_lo = -kBandCells, i_hi = n + 2;   // inclusive lattice bounds
    const int k_lo = -kBandCells, k_hi = n + 2;
    const int sx = i_hi - i_lo + 1, szn = k_hi - k_lo + 1;   // n + 3 + kBandCells
    const auto idx_i = [&](int i) { return size_t(i - i_lo); };
    const auto idx_k = [&](int k) { return size_t(k - k_lo); };

    std::vector<float> heights(size_t(sx) * size_t(szn));
    auto hat = [&](int i, int k) -> float& {
        return heights[idx_k(k) * size_t(sx) + idx_i(i)];
    };
    // h_min/h_max are reduced over the tile's OWN sample box [0, n+2]^2 ONLY.
    // The band columns are sampled but excluded, because these two drive the Y
    // slab (j0_global -> y0) and every vertex position in the tile is measured
    // from y0. Letting a band column widen the slab would move the whole mesh --
    // the one place where "sample more" would not have been free.
    //
    // The band therefore lives inside a slab chosen without it. That is sound
    // and not merely convenient: the slab already carries a +/-2 voxel margin
    // past the sampled surface, and the band reaches 2 voxels tangentially, so
    // it is clipped only where the surface moves more than two voxels of height
    // across two voxels of ground -- terrain steeper than 45 degrees at the
    // sampling scale, where the mesh itself is already a stack of vertical
    // faces and an overlap band would add nothing.
    float h_min = std::numeric_limits<float>::infinity();
    float h_max = -std::numeric_limits<float>::infinity();
    for (int k = k_lo; k <= k_hi; ++k) {
        for (int i = i_lo; i <= i_hi; ++i) {
            const float h = field.height_at(
                float(ox + (i - 1) * double(voxel)),
                float(oz + (k - 1) * double(voxel)));
            if (!std::isfinite(h)) {
                err = "terrain_mesher: non-finite height";
                return false;
            }
            hat(i, k) = h;
            if (i >= 0 && k >= 0) {           // the tile's own box; see above
                h_min = std::min(h_min, h);
                h_max = std::max(h_max, h);
            }
        }
    }

    // THE COLUMN PATH ONLY. "Sampled height outside authored Y range" is a
    // statement about a slab that is supposed to contain the whole surface, and
    // a Y-TILED tile is not: a tile is one 64 m cube of a stack, and the surface
    // leaving it through the top or the bottom is the normal case, not an error
    // -- it is the neighbouring tile's geometry. Design §3.3 moves the check to
    // world-load-time validation of the octree's Y extent (`yMin`/`yMax` stop
    // being the mesher's cost dial and become the octree's vertical bounds,
    // rounded outward to coarsest-tile multiples), which is the only level at
    // which "the authored world does not contain its own terrain" is meaningful.
    // That validation belongs to M3's world-load path, not here; noted in the
    // report so it is sequenced and not silently dropped.
    if (!y_tiled && (h_min < y_min || h_max > y_max)) {
        err = "terrain_mesher: sampled height outside authored Y range";
        return false;
    }

    // ---- Y lattice ----------------------------------------------------------
    //
    // COLUMN PATH. A HEIGHTFIELD world meshes a few voxels either side of the
    // sampled surface, and that narrowing is sound only because `density = h - y`
    // says everything below the surface is solid and everything above it is air
    // -- there is provably nothing to mesh outside the band.
    //
    // A VOLUMETRIC world (tunnels, caverns, overhangs) has no such guarantee:
    // air a kilometre down is exactly the point. So the slab runs from the
    // authored floor up to just above the surface, and `yMin` becomes the
    // world's cost dial -- the slab is (h_max - yMin) / voxel samples deep, and
    // every one of them is a field evaluation.
    //
    // Y-TILED PATH. There is no slab and no cost dial: the extent is the tile,
    // and Y is indexed exactly as X and Z are. Lattice index j sits at
    // world y = oy + (j-1)*voxel, the tile's own lattice points are j in
    // [1, n+1], j = 0 and j = n+2 are the ghost ring, and the band reaches
    // `kBandCells` further NEGATIVE -- the same shape as i and k, for the same
    // reason (see the -y band note below). This is the cost-model flip of §3.3:
    // a StreamCaverns level-0 tile went from sampling 35 x ~590 x 35 to
    // 37 x 37 x 37, and tiles away from the surface/cavern shell fall out
    // essentially free.
    int j_lo, j_hi;
    float y0 = 0.0f;          // column path only; the tiled path uses `oy`
    int64_t gy_off = 0;       // global Y CELL index of local cell j == 0
    if (y_tiled) {
        j_lo = -kBandCells;
        j_hi = n + 2;
        // Global cell index of local cell cj is `ty*n + cj - 1`, exactly as
        // `tx*n + ci - 1` in x (derived in the record-export block below).
        gy_off = ty * int64_t(n) - 1;
    } else {
        const int global_ny =
            std::max(1, int(std::ceil((y_max - y_min) / voxel)));
        const int j0_global = volumetric
            ? 0
            : std::max(0, int(std::floor((h_min - y_min) / voxel)) - 2);
        const int j1_global = std::min(
            global_ny, int(std::ceil((h_max - y_min) / voxel)) + 2);
        y0 = y_min + float(j0_global) * voxel;
        j_lo = 0;
        j_hi = j1_global - j0_global;
        gy_off = int64_t(j0_global);
    }
    const int sy = j_hi - j_lo + 1;
    const auto idx_j = [&](int j) { return size_t(j - j_lo); };

    // World Y of lattice index j. The column branch is the historical
    // expression, operand for operand, so the pinned column bytes cannot move;
    // the tiled branch is the dyadic-double construction derived at the top.
    const auto y_at = [&](int j) -> float {
        return y_tiled ? float(oy + (j - 1) * double(voxel))
                       : y0 + j * voxel;
    };

    // Narrow density lattice dimensions:
    //   x/z: sx samples — the tile's n+1 lattice points, one ghost ring on each
    //        side, and kBandCells more on the -x/-z sides (i, k in [i_lo, i_hi])
    //   y:   column path — globally aligned samples from j0_global to j1_global;
    //        tiled path  — j in [-kBandCells, n+2], mirroring x and z
    std::vector<float> d(size_t(sx) * size_t(sy) * size_t(szn));
    auto at = [&](int i, int j, int k) -> float& {
        return d[(idx_k(k) * size_t(sy) + idx_j(j)) * size_t(sx) + idx_i(i)];
    };
    if (!volumetric) {
        for (int k = k_lo; k <= k_hi; ++k)
            for (int j = j_lo; j <= j_hi; ++j)
                for (int i = i_lo; i <= i_hi; ++i)
                    at(i, j, k) = hat(i, k) - y_at(j);
    } else {
        // One ColumnCache per (x, z), reused down the column. The y-independent
        // part of the program -- typically the whole multi-octave surface fbm --
        // is evaluated once per column instead of once per voxel of depth, which
        // at a 640-sample column is the difference between a usable world and an
        // unusable one.
        //
        // Note this loop does NOT read `hat`: the density comes from the field
        // directly, so the surface and the cave walls are the same lattice and
        // get the same treatment. `hat` still earns its keep -- h_min/h_max
        // bound the slab above.
        terrain_field::FieldRuntime::ColumnCache cc;
        for (int k = k_lo; k <= k_hi; ++k) {
            const float wz = float(oz + (k - 1) * double(voxel));
            for (int i = i_lo; i <= i_hi; ++i) {
                const float wx = float(ox + (i - 1) * double(voxel));
                field.eval_column(cc, wx, wz);
                for (int j = j_lo; j <= j_hi; ++j) {
                    // Not `d` -- that is the lattice vector this writes into.
                    const float dens = field.density_at(cc, y_at(j));
                    if (!std::isfinite(dens)) {
                        err = "terrain_mesher: non-finite density";
                        return false;
                    }
                    at(i, j, k) = dens;
                }
            }
        }

        // The volumetric cross-rung seam snap USED TO BE HERE, and its deletion
        // is worth recording because the mechanism it described is real and the
        // premise it rested on was not. It rewrote this tile's boundary density
        // PLANE -- each fine sample at an odd position replaced by the bilinear
        // interpolation of its even neighbours, along k, along j, or from the
        // four surrounding evens -- so the fine plane BECAME the coarse
        // neighbour's interpolant and a cave wall crossing the seam met exactly.
        // That derivation is sound.
        //
        // Its premise was "I know which of my faces has a coarser neighbour",
        // and the mask that answered was a statement about the level map the
        // streamer wants, not about the tile drawn next door. Every split,
        // merge, park and pending bake made the two disagree, and a plane
        // snapped for a neighbour that is not there is worse than one not
        // snapped at all: it bends the fine surface away from a coarse tile that
        // was never drawn. The lattice this tile writes is now the raw field,
        // full stop, and the boundary records exported below carry it to a
        // welder that can see both actual tiles.
    }

    // Surface-nets: one vertex per mixed-sign cell, placed at the centroid of
    // edge crossing positions. Normal from central-diff of the density field.
    // Cell ci spans lattice [ci, ci+1], so cells run [i_lo, i_hi-1]. Only the
    // LOWER bound moved (from 0 to -kBandCells) and only the storage offset in
    // `key` moved with it: no cell the owned mesh ever asks for -- ci in [0, n],
    // ck in [0, n] -- changes its bound test, its centroid arithmetic, or its
    // resulting position by so much as an ulp.
    std::unordered_map<int64_t, CellVert> verts;
    auto key = [&](int ci, int cj, int ck) -> int64_t {
        return (int64_t(idx_k(ck)) * sy + int64_t(idx_j(cj))) * sx +
               int64_t(idx_i(ci));
    };
    auto get_vert = [&](int ci, int cj, int ck) -> const CellVert* {
        if (ci < i_lo || cj < j_lo || ck < k_lo ||
            ci > i_hi - 1 || cj > j_hi - 1 || ck > k_hi - 1) return nullptr;
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
        // Local x/z: (lattice_index - 1) * voxel — undoes the ghost-ring offset,
        // so local (0,0) is the tile's own corner and a bridging vertex in the
        // ring is simply negative.
        //
        // Y: on the COLUMN path positions are world-absolute (y0 + lattice_j *
        // voxel), the historical contract every consumer of a column tile
        // assumes. On the Y-TILED path they are TILE-LOCAL, by the identical
        // (lattice - 1) * voxel expression as x and z -- design §3.3. The engine
        // side of that is `transform[7] = ty * sector_size` on the publish
        // transform, which lives in matter_engine.cpp and belongs to M3.
        cv.p = {
            (px / cnt - 1.0f) * voxel,
            y_tiled ? (py / cnt - 1.0f) * voxel
                    : y0 + (py / cnt) * voxel,
            (pz / cnt - 1.0f) * voxel
        };
        // Gradient normal from the WORLD position.
        const float e2 = voxel;
        float wx = float(ox) + cv.p.x;
        float wy = y_tiled ? float(oy) + cv.p.y : cv.p.y;
        float wz = float(oz) + cv.p.z;
        float gx = field.density_at(wx + e2, wy, wz) - field.density_at(wx - e2, wy, wz);
        // gy analytically FOR A HEIGHTFIELD: density = height(x,z) - y, so the
        // y-difference is (h - (wy+e2)) - (h - (wy-e2)) = -2*e2 -- the height
        // term cancels and the value is known without evaluating the field at
        // all. The float version differed from -2*e2 only by cancellation noise
        // (<= ~2 ulp of h, i.e. ~5e-4 against a magnitude of 2*voxel), and each
        // of the two density_at calls it replaced was a full field-program
        // evaluation.
        //
        // A volumetric density has no such cancellation -- the whole point is
        // that it varies in y independently of the surface -- so it pays for the
        // two probes. A cave roof's normal comes from nowhere else.
        float gy = volumetric
            ? field.density_at(wx, wy + e2, wz) - field.density_at(wx, wy - e2, wz)
            : -2.0f * e2;
        float gz = field.density_at(wx, wy, wz + e2) - field.density_at(wx, wy, wz - e2);
        float len = std::sqrt(gx * gx + gy * gy + gz * gz);
        // Density is positive in solid, so its gradient points INTO the solid
        // (downward for above-ground terrain, and upward off a cave floor).
        // Negate to get the outward surface normal. True of any density field,
        // which is why this line needs no volumetric case.
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
    // Ownership predicate: exactly [1..n], the lattice indices mapping to
    // sector-local [0, S). Integer comparison, no float precision gaps at
    // sector boundaries. Each sector's mesh then ends EXACTLY at its +x/+z
    // border, and its -x/-z bridge lands on an EQUAL-rung neighbour's last
    // vertex because the border cell rows are shared (same world samples ->
    // bitwise-identical verts). That is the whole seam guarantee, and it is
    // exact.
    //
    // Do not widen this in either direction. Past the tile's own +x/+z border
    // it would double-emit the border quads the neighbour already owns; below
    // 1 it would emit into the ghost ring the neighbour bridges. The upper
    // bound used to read `n + reach_x` to let a masked tile reach into an
    // extended lattice -- that machinery is gone, and it is NOT what the
    // overlap band re-introduces: the band goes to a separate soup that the
    // welder may or may not draw, and `owned` still says exactly what the tile
    // itself puts on screen.
    //
    // ---- Y (M2), and WHICH tile bridges a horizontal plane -------------------
    //
    // On the Y-TILED path j joins the predicate on identical terms, [1..n]. The
    // consequence is derived, not assumed, and it is the vertical half of the
    // seam contract, so here is the reading of `emit_quad`'s call sites that
    // produces it:
    //
    //   The +x edge at lattice (i, j, k) is spanned by the four cells with
    //   ci = i, cj in {j-1, j}, ck in {k-1, k}  (the q0..q3 above). Same for the
    //   +z edge. So an OWNED edge (j in [1..n]) uses cell layer cj = j-1 as well
    //   as cj = j, and at j = 1 that is cj = 0 -- the GHOST cell below the tile,
    //   spanning world y in [oy - v, oy].
    //
    //   Cell cj = 0 of tile ty has global index ty*n - 1. Cell cj = n of tile
    //   ty-1 has global index (ty-1)*n + n - 1 = ty*n - 1. THE SAME CELL, built
    //   from the same lattice samples (bitwise, by the derivation at the top),
    //   so the two tiles compute the same vertex in it.
    //
    //   At the other end, j = n uses cj in {n-1, n} and never n+1, so the tile's
    //   vertical faces stop half a voxel short of its +y border.
    //
    // Therefore: A HORIZONTAL SHARED PLANE IS BRIDGED BY THE TILE ABOVE IT --
    // the +y side, exactly as a vertical plane is bridged by its +x/+z side.
    // The tile above reaches down ~v/2 past the plane and the tile below stops
    // ~v/2 short of it, and at equal level the two meet on the same cell, which
    // is the same watertightness argument the east/north rule has always made.
    //
    // The +y edge case is consistent and needs no extra clause: the +y edge from
    // lattice j to j+1 lies ENTIRELY inside cell cj = j, so ownership j in [1..n]
    // selects exactly the tile's own cells. The +y edge at j = 0 -- inside the
    // shared cell -- is emitted by the tile BELOW (its j = n), which owns it.
    // Nothing is emitted twice and nothing is dropped.
    auto owned = [&](int i, int j, int k) -> bool {
        return i >= 1 && i <= n && k >= 1 && k <= n &&
               (!y_tiled || (j >= 1 && j <= n));
    };

    // ---- OWNERSHIP UNDER THE CONTOUR RULE -----------------------------------
    //
    // The bridge above is the thing the contour rule removes, so the predicate
    // has to change with it -- and the change is precisely "emit a quad only
    // when ALL FOUR cells sharing the edge are the tile's own", which is the
    // rule a mesh that terminates on its own boundary has. That is
    // direction-dependent, because which four cells share an edge depends on
    // the edge's direction, so `owned` (one predicate for all three) splits
    // into three. Read the call sites below for the cell sets:
    //
    //   +y edge at (i,j,k)  cells  (i-1..i,  j,      k-1..k)
    //   +x edge at (i,j,k)  cells  (i,       j-1..j, k-1..k)
    //   +z edge at (i,j,k)  cells  (i-1..i,  j-1..j, k)
    //
    // and owned cells are [1..n] on every axis, so each cell range's lower end
    // must be >= 1 and its upper end <= n.
    //
    // WHAT THIS ACTUALLY SUPPRESSES, which is less than it looks. An edge
    // DIRECTION's quad joins four cells that differ in the other two axes, so
    // on the -x face it is the y- and z-direction quads at lattice i = 1 that
    // go (they reached into ghost cell layer 0, which is the bridge); the
    // x-direction quads at i = 1 join four cells all in layer 1 and stay, so
    // the border layer is still stitched to itself and to layer 2. On the +x
    // face nothing is suppressed at all -- the old rule already stopped at
    // i = n. So the tile loses exactly its reach past the - faces, and the fan
    // below adds the reach up to the + ones. Together: the mesh ends on the
    // plane, from both sides.
    //
    // Off, all three are `owned` verbatim, so the emitted bytes cannot move.
    auto owned_y_edge = [&](int i, int j, int k) -> bool {
        return contour_seams ? (i >= 2 && i <= n && j >= 1 && j <= n &&
                                k >= 2 && k <= n)
                             : owned(i, j, k);
    };
    auto owned_x_edge = [&](int i, int j, int k) -> bool {
        return contour_seams ? (i >= 1 && i <= n && j >= 2 && j <= n &&
                                k >= 2 && k <= n)
                             : owned(i, j, k);
    };
    auto owned_z_edge = [&](int i, int j, int k) -> bool {
        return contour_seams ? (i >= 2 && i <= n && j >= 2 && j <= n &&
                                k >= 1 && k <= n)
                             : owned(i, j, k);
    };

    // ---- Band emission (M0-WP7) ---------------------------------------------
    //
    // `emit_band_quad` is `emit_quad` against an OverlapBand: same four cells,
    // same flip, same material query, positions rebased to world on the way out.
    //
    // The extra owned box the retired `reach = 2` produced is `[-1,n]^2`, so the
    // band samples are that box minus the tile's own `[1,n]^2`. `-x` takes the
    // ones with i <= 0 and `-z` the ones with k <= 0, which covers the L exactly
    // and puts the 2x2 corner block in both (see the note at the top of this
    // function for why the duplication is wanted).
    //
    // ---- WHICH Y FACE CARRIES A BAND (M2) -----------------------------------
    //
    // -y, and it follows from the bridging direction derived at `owned` above
    // rather than from symmetry with x/z.
    //
    // The band exists on the faces where THIS tile is the bridging side and the
    // bridge UNDER-REACHES a coarser neighbour. This tile reaches ~v/2 back past
    // its -x/-z/-y borders; a neighbour one level coarser (voxel V = 2v) stops
    // ~V/2 = v short of the same plane. The strip between v and v/2 is emitted
    // by neither. On the +side faces the neighbour is the bridging one, so a
    // band there is overlap on top of overlap.
    //
    // The tile bridges DOWNWARD (the tile above a horizontal plane is the one
    // that crosses it), so the under-reached Y face is -y and the band goes
    // there. On the Y-TILED path the sampled box is `[-1,n]^3` minus `[1,n]^3`,
    // split by  i <= 0 -> -x,  k <= 0 -> -z,  j <= 0 -> -y.  Those three cover
    // the shell exactly, with the shared edge/corner blocks duplicated into two
    // or three bands -- the same deliberate duplication as the 2D corner block,
    // for the same reason: a band's coverage must not depend on WHICH of the
    // faces meeting there happens to be the cross-level one, and coincident
    // opaque triangles cost nothing while a corner hole is the defect the band
    // exists to remove.
    //
    // The COLUMN path is untouched: two bands, `[-1,n]^2` in x/z, all j.
    //
    // Emitted only when there is a record to carry it. `boundary_out == nullptr`
    // means the caller wants a mesh and nothing else -- a tile that will never be
    // welded -- and the band's per-cell gradient normals are real field probes,
    // not free. The SAMPLING above is unconditional either way; what is skipped
    // here is only writing down a soup with nowhere to go.
    seam::OverlapBand band_neg_x, band_neg_z, band_neg_y;
    auto emit_band_quad = [&](seam::OverlapBand& band,
                              const CellVert* v00, const CellVert* v10,
                              const CellVert* v11, const CellVert* v01,
                              bool flip, float wxc, float wzc) {
        if (!v00 || !v10 || !v11 || !v01) return;
        seam::OverlapBucket& b = band_bucket_for(band,
            uint32_t(field.material_at(wxc, wzc)));
        if (flip) std::swap(v10, v01);
        push_band_tri(b, ox, oy, oz, y_tiled, *v00, *v10, *v11);
        push_band_tri(b, ox, oy, oz, y_tiled, *v00, *v11, *v01);
    };
    const bool want_band = boundary_out != nullptr;
    auto in_band_box = [&](int v) -> bool { return v >= -1 && v <= n; };
    auto band_extra = [&](int i, int j, int k) -> bool {
        return !owned(i, j, k) && in_band_box(i) && in_band_box(k) &&
               (!y_tiled || in_band_box(j));
    };

    for (int k = k_lo; k <= k_hi; ++k)
        for (int j = j_lo; j <= j_hi; ++j)
            for (int i = i_lo; i <= i_hi; ++i) {
                const bool own_y = owned_y_edge(i, j, k);
                const bool own_x = owned_x_edge(i, j, k);
                const bool own_z = owned_z_edge(i, j, k);
                const bool extra = want_band && band_extra(i, j, k);
                if (!own_x && !own_y && !own_z && !extra) continue;
                float a = at(i, j, k);
                // World coords of this sample (for material query midpoint).
                float wxs = float(ox) + float(i - 1) * voxel;
                float wzs = float(oz) + float(k - 1) * voxel;

                // +y edge — the typical terrain surface case (horizontal face).
                if (j + 1 <= j_hi) {
                    float b = at(i, j + 1, k);
                    if ((a > 0) != (b > 0)) {
                        const CellVert* q0 = get_vert(i - 1, j, k - 1);
                        const CellVert* q1 = get_vert(i,     j, k - 1);
                        const CellVert* q2 = get_vert(i,     j, k);
                        const CellVert* q3 = get_vert(i - 1, j, k);
                        if (own_y)
                            emit_quad(q0, q1, q2, q3, /*flip=*/a > 0, wxs, wzs);
                        if (extra && i <= 0)
                            emit_band_quad(band_neg_x, q0, q1, q2, q3, a > 0, wxs, wzs);
                        if (extra && k <= 0)
                            emit_band_quad(band_neg_z, q0, q1, q2, q3, a > 0, wxs, wzs);
                        if (extra && y_tiled && j <= 0)
                            emit_band_quad(band_neg_y, q0, q1, q2, q3, a > 0, wxs, wzs);
                    }
                }
                // +x edge (vertical face in x direction).
                if (i + 1 <= i_hi) {
                    float b = at(i + 1, j, k);
                    if ((a > 0) != (b > 0)) {
                        const CellVert* q0 = get_vert(i, j - 1, k - 1);
                        const CellVert* q1 = get_vert(i, j,     k - 1);
                        const CellVert* q2 = get_vert(i, j,     k);
                        const CellVert* q3 = get_vert(i, j - 1, k);
                        const float mx = wxs + 0.5f * voxel;
                        if (own_x)
                            emit_quad(q0, q1, q2, q3, /*flip=*/a <= 0, mx, wzs);
                        if (extra && i <= 0)
                            emit_band_quad(band_neg_x, q0, q1, q2, q3, a <= 0, mx, wzs);
                        if (extra && k <= 0)
                            emit_band_quad(band_neg_z, q0, q1, q2, q3, a <= 0, mx, wzs);
                        if (extra && y_tiled && j <= 0)
                            emit_band_quad(band_neg_y, q0, q1, q2, q3, a <= 0, mx, wzs);
                    }
                }
                // +z edge (vertical face in z direction).
                if (k + 1 <= k_hi) {
                    float b = at(i, j, k + 1);
                    if ((a > 0) != (b > 0)) {
                        const CellVert* q0 = get_vert(i - 1, j - 1, k);
                        const CellVert* q1 = get_vert(i,     j - 1, k);
                        const CellVert* q2 = get_vert(i,     j,     k);
                        const CellVert* q3 = get_vert(i - 1, j,     k);
                        const float mz = wzs + 0.5f * voxel;
                        if (own_z)
                            emit_quad(q0, q1, q2, q3, /*flip=*/a <= 0, wxs, mz);
                        if (extra && i <= 0)
                            emit_band_quad(band_neg_x, q0, q1, q2, q3, a <= 0, wxs, mz);
                        if (extra && k <= 0)
                            emit_band_quad(band_neg_z, q0, q1, q2, q3, a <= 0, wxs, mz);
                        if (extra && y_tiled && j <= 0)
                            emit_band_quad(band_neg_y, q0, q1, q2, q3, a <= 0, wxs, mz);
                    }
                }
            }

    // ---- THE CONSTRAINED BORDER, once per face ------------------------------
    //
    // With the bridge gone (see OWNERSHIP UNDER THE CONTOUR RULE), the tile's
    // surface stops at its border CELL layer and the six face planes are bare.
    // This closes each one onto the canonical contour, with two kinds of
    // triangle:
    //
    //   FAN     each contour segment gets one triangle joining it to the dual
    //           vertex of the border cell whose face footprint contains the
    //           segment's midpoint. This is what tents the tile's surface down
    //           onto the shared curve.
    //   BRIDGE  a contour vertex sitting exactly on the line between two
    //           adjacent border cells is shared by both of their fans, and
    //           without a triangle spanning the two dual vertices the surface
    //           has a slit there. Measured, not assumed: removing the bridge
    //           made the prototype's non-manifold count 3-6x WORSE.
    //
    // THIS IS A ZIPPER, AND THAT IS NOT A CONTRADICTION. The ruling that killed
    // the previous designs was about a zipper between two TILES' curves --
    // where the two curves cross you get coplanar bowties, which is the
    // texturing artefact. This zipper runs between ONE tile's own dual boundary
    // and the shared curve. It is invisible to the neighbour, which meets this
    // tile only ON the curve, where the two tent over it from opposite sides
    // and share every vertex exactly.
    //
    // A border cell with no dual vertex contributes nothing and its segments
    // are dropped. The design once proposed a flat in-plane cap for that case;
    // it was implemented and the triangle count did not move by one, because
    // the case cannot arise -- a sign change on a cell's face IS a sign change
    // in the cell, so a cell with contour in its footprint has a vertex. The
    // cap is not here because it was measured to be dead code, not because it
    // was forgotten.
    if (contour_seams) {
        const double S   = double(sector_size);
        const double v   = double(voxel);
        const double vc  = kCanonicalVoxel;
        const double org[3] = {ox, oy, oz};
        // Canonical cells spanning one of this tile's own cells: 2^L at level L.
        const int64_t own_steps =
            std::max<int64_t>(1, int64_t(std::llround(v / vc)));

        for (int face = 0; face < 6; ++face) {
            const int    ax       = face / 2;
            const bool   positive = (face & 1) != 0;
            const double plane    = positive ? org[ax] + S : org[ax];
            int aa = 0, bb = 0;
            contour_tangent_axes(ax, aa, bb);

            // The border CELL layer, in the tile's own [1..n] cell indices.
            const int layer = positive ? n : 1;
            auto dual_at = [&](int ca, int cb) -> const CellVert* {
                int c3[3];
                c3[ax] = layer; c3[aa] = ca; c3[bb] = cb;
                return get_vert(c3[0], c3[1], c3[2]);
            };

            // Seeds for the trace: every border cell that produced a vertex,
            // in own-rung cell coordinates local to the face square. See the
            // long note on trace_contour for why this set is exactly right.
            std::vector<std::pair<int, int>> seeds;
            for (int ca = 1; ca <= n; ++ca)
                for (int cb = 1; cb <= n; ++cb)
                    if (dual_at(ca, cb)) seeds.push_back({ca - 1, cb - 1});
            if (seeds.empty()) continue;

            const Contour c =
                trace_contour(field, ax, plane, org[aa], org[aa] + S,
                              org[bb], org[bb] + S, vc, own_steps, seeds);
            if (c.segs.empty()) continue;

            // Contour vertices as mesh vertices, once each.
            //
            // POSITION is tile-local, like every other vertex here, so the two
            // tiles sharing this curve store DIFFERENT floats for the same
            // world point (each is `world - its own origin`). That is the
            // existing contract, not a new compromise: the ownership rule has
            // always had two tiles reference one geometric vertex from two
            // frames, and the divergence is the rounding of one subtraction --
            // sub-millimetre, and it scales with distance exactly as the pixel
            // does, so it stays sub-pixel at every level.
            //
            // NORMAL uses the CANONICAL voxel as its central-difference
            // epsilon, not the tile's own. This is the one place that matters
            // visibly: with each side probing at its own scale, a shared vertex
            // would carry two different normals and the seam would print as a
            // shading crease -- which is what the seam hairlines of issue
            // ec2829d6 turned out to be, and the whole point of this design is
            // that the boundary is not visible. The cost is that a coarse
            // tile's border row carries finer-grained normals than its
            // interior; that is a gradient across one row of triangles, where
            // the alternative is a discontinuity along the whole seam.
            std::vector<CellVert> cw(c.verts.size());
            for (size_t vi = 0; vi < c.verts.size(); ++vi) {
                const ContourVert& cv = c.verts[vi];
                double w[3];
                w[ax] = plane; w[aa] = cv.pa; w[bb] = cv.pb;
                CellVert o;
                o.p = {float(w[0] - ox), float(w[1] - oy), float(w[2] - oz)};
                const float e  = float(vc);
                const float wx = float(w[0]), wy = float(w[1]), wz = float(w[2]);
                const float gx = field.density_at(wx + e, wy, wz) -
                                 field.density_at(wx - e, wy, wz);
                // Same analytic shortcut as get_vert: for a heightfield the
                // height term cancels out of the y-difference exactly.
                const float gy = volumetric
                    ? field.density_at(wx, wy + e, wz) -
                      field.density_at(wx, wy - e, wz)
                    : -2.0f * e;
                const float gz = field.density_at(wx, wy, wz + e) -
                                 field.density_at(wx, wy, wz - e);
                const float len = std::sqrt(gx * gx + gy * gy + gz * gz);
                o.n = len > 1e-12f ? V3{-gx / len, -gy / len, -gz / len}
                                   : V3{0, 1, 0};
                cw[vi] = o;
            }

            // Which owned cell a tangential world coordinate falls in. Cell ci
            // spans world [o + (ci-1)v, o + ci*v], so the +1 undoes the same
            // ghost-ring offset the lattice map uses.
            auto cell_of = [&](int tax, double w) -> int {
                int idx = int(std::floor((w - org[tax]) / v)) + 1;
                if (idx < 1) idx = 1;
                if (idx > n) idx = n;
                return idx;
            };

            // WINDING is derived, not guessed. Reading emit_quad's +y case: the
            // unflipped order runs (x-,z-) -> (x+,z-) -> (x+,z+), whose
            // right-hand normal is -y, and unflipped is the case with air below
            // and solid above, whose outward normal is also -y. So this mesh's
            // convention is "right-hand normal == outward surface normal", and
            // orienting against the vertex normals reproduces it for any
            // triangle without re-deriving a sign rule per case.
            auto push_oriented = [&](const CellVert& A, const CellVert& B,
                                     const CellVert& C, int ca, int cb) {
                const float ux = B.p.x - A.p.x, uy = B.p.y - A.p.y,
                            uz = B.p.z - A.p.z;
                const float vx = C.p.x - A.p.x, vy = C.p.y - A.p.y,
                            vz = C.p.z - A.p.z;
                const float cx = uy * vz - uz * vy;
                const float cy = uz * vx - ux * vz;
                const float cz = ux * vy - uy * vx;
                // Degenerate triangles are never emitted -- a collapsed sliver
                // on the seam is a crack that no coverage test can see.
                if (!(cx * cx + cy * cy + cz * cz > 0.0f)) return;
                int c3[3];
                c3[ax] = layer; c3[aa] = ca; c3[bb] = cb;
                // Material at the border cell's centre, the same convention the
                // boundary record uses -- so the border strip lands in the same
                // bucket as the interior it continues, rather than printing a
                // one-triangle material stripe along every seam.
                MaterialBucket& bkt = bucket_for(out, uint32_t(field.material_at(
                    float(ox + (double(c3[0]) - 0.5) * v),
                    float(oz + (double(c3[2]) - 0.5) * v))));
                const float nx = A.n.x + B.n.x + C.n.x;
                const float ny = A.n.y + B.n.y + C.n.y;
                const float nz = A.n.z + B.n.z + C.n.z;
                if (cx * nx + cy * ny + cz * nz >= 0.0f) push_tri(bkt, A, B, C);
                else                                     push_tri(bkt, A, C, B);
            };

            // THE ANCHOR, and why it is not simply `dual_at(ca, cb)`.
            //
            // A border cell can carry contour in its face footprint and have NO
            // vertex of its own: the canonical lattice is finer than this
            // tile's, so a feature between two of its own lattice points is
            // invisible to it while the contour sees it perfectly. Dropping the
            // segment there is what the prototype did, on the strength of a
            // measurement that said the case never arises -- and that
            // measurement was taken on a smooth rolling field with no structure
            // below a voxel. A CAVE field has it constantly.
            //
            // Dropping is not survivable, because the two sides of a plane are
            // DIFFERENT cells: one can see the feature and the other not, so one
            // side tents the segment and the other does not, and the result is a
            // one-triangle edge -- a hole you can see through. Measured on the
            // equal-level 2x2x2 block: 4 of them.
            //
            // So the fan reaches instead. The anchor is the nearest border cell
            // that does have a vertex, searched in a fixed ring order out to two
            // cells, which is where this tile's surface actually is. The
            // triangle is stretched rather than absent, and the two sides still
            // meet exactly on the contour -- which is the only place they are
            // required to meet.
            auto anchor_at = [&](int& ca, int& cb) -> const CellVert* {
                if (const CellVert* v = dual_at(ca, cb)) return v;
                for (int rad = 1; rad <= 2; ++rad)
                    for (int da = -rad; da <= rad; ++da)
                        for (int db = -rad; db <= rad; ++db) {
                            if (std::max(std::abs(da), std::abs(db)) != rad)
                                continue;
                            const int na = ca + da, nb = cb + db;
                            if (na < 1 || na > n || nb < 1 || nb > n) continue;
                            if (const CellVert* v = dual_at(na, nb)) {
                                ca = na; cb = nb;
                                return v;
                            }
                        }
                return nullptr;
            };

            // Each segment's anchor, resolved once. `get_vert` inserts into an
            // unordered_map, whose element pointers are stable across rehash,
            // so these stay valid as later cells are built.
            struct Anchor { const CellVert* v; int ca, cb; };
            std::vector<Anchor> seg_anchor(c.segs.size(), Anchor{nullptr, 0, 0});
            for (size_t si = 0; si < c.segs.size(); ++si) {
                const ContourVert& p = c.verts[c.segs[si].first];
                const ContourVert& q = c.verts[c.segs[si].second];
                int ca = cell_of(aa, 0.5 * (p.pa + q.pa));
                int cb = cell_of(bb, 0.5 * (p.pb + q.pb));
                seg_anchor[si] = Anchor{anchor_at(ca, cb), ca, cb};
            }

            // FAN: one triangle per segment, tenting the tile's surface down
            // onto the shared curve.
            for (size_t si = 0; si < c.segs.size(); ++si) {
                const Anchor& a = seg_anchor[si];
                if (!a.v) continue;
                push_oriented(*a.v, cw[c.segs[si].first], cw[c.segs[si].second],
                              a.ca, a.cb);
            }

            // BRIDGE: where two segments MEET AT A CONTOUR VERTEX with different
            // anchors, the two fans share that vertex and leave a slit between
            // their anchor vertices. One triangle closes it.
            //
            // THE CONDITION IS "DIFFERENT ANCHORS", NOT "ON A CELL LINE", and
            // the difference is the whole bug. The first version tested whether
            // the contour vertex sat exactly on the line between two adjacent
            // border cells -- which is the case that arises when every segment
            // anchors to the cell containing it, and stops being the case the
            // moment `anchor_at` has to reach. It then left one-triangle edges
            // wherever the reach changed anchor mid-curve: closing the 4
            // measured holes by reaching MOVED them rather than removing them,
            // which is what said the test was on the anchor and not the
            // geometry. Stated as "join consecutive fans that disagree", the
            // cell-line case is just the instance where they disagree because
            // the curve crossed a cell boundary, and every other instance is
            // covered by the same triangle.
            std::vector<std::vector<int>> at_vert(c.verts.size());
            for (size_t si = 0; si < c.segs.size(); ++si) {
                at_vert[c.segs[si].first].push_back(int(si));
                at_vert[c.segs[si].second].push_back(int(si));
            }
            for (size_t vi = 0; vi < at_vert.size(); ++vi) {
                const std::vector<int>& inc = at_vert[vi];
                for (size_t p = 0; p < inc.size(); ++p)
                    for (size_t q = p + 1; q < inc.size(); ++q) {
                        const Anchor& a0 = seg_anchor[inc[p]];
                        const Anchor& a1 = seg_anchor[inc[q]];
                        if (!a0.v || !a1.v || a0.v == a1.v) continue;
                        push_oriented(*a0.v, *a1.v, cw[vi], a0.ca, a0.cb);
                    }
            }
        }
    }

    // ---- Boundary record export (M0-WP2) ------------------------------------
    //
    // What the seam welder gets instead of a baked-in stitch: for each of the
    // four XZ faces, the boundary cells of THIS tile that produced a vertex,
    // named by GLOBAL cell index at this tile's rung. Nothing here reads a
    // neighbour, so nothing here belongs in the tile's bake identity.
    //
    // INDEXING, derived. With `reach` deleted the lattice map is unconditional:
    // lattice index i sits at world x = ox + (i-1)*voxel, and cell ci spans
    // lattice [ci, ci+1], i.e. world x in [ox + (ci-1)*v, ox + ci*v]. A global
    // cell is named by its minimum lattice corner divided by the voxel, so
    //
    //     gx = (ox + (ci-1)*v) / v = tx*S/v + ci - 1 = tx*n + ci - 1
    //
    // using S = n*v. Interior (owned) cells are ci in [1..n], which is exactly
    // gx in [tx*n, tx*n + n - 1] -- n cells, no overlap with either neighbour,
    // and the fine cells 2g/2g+1 of a rung-(r) tile sit inside coarse cell g of
    // rung (r-1) by construction. Same in z.
    //
    // Y depends on the regime, and `gy_off` (set with the Y lattice above) is
    // the one place that difference lives -- global y cell = gy_off + cj:
    //   COLUMN   gy_off = j0_global. The slab is anchored to the global lattice
    //            at y_min, and cell cj is global cell j0_global + cj.
    //   Y-TILED  gy_off = ty*n - 1, i.e. global = ty*n + cj - 1, letter for
    //            letter the x/z map above with ty for tx. That is what makes
    //            `coarse = floor_div2(fine)` work on the vertical axis too, and
    //            hence what makes a ±y face weldable by exactly the same fan.
    //
    // Per face, then:
    //   kFaceNegX  cell layer ci = 1      cell_layer = tx*n            plane = tx*n
    //   kFacePosX  cell layer ci = n      cell_layer = tx*n + n - 1    plane = (tx+1)*n
    //   kFaceNegZ / kFacePosZ  the same in z.
    // (`plane` is the shared LATTICE index -- for a +side face it is
    // cell_layer + 1, for a -side face it equals cell_layer. seam_boundary.h
    // states that relation; this is where it comes from.)
    //
    // The tangential index order is NOT ours to choose: seam::face_tangent_axes
    // fixes it so both sides of a plane agree without negotiating. Normal x ->
    // (a, b) = (global y cell, global z cell); normal z -> (a, b) = (global x
    // cell, global y cell). The welder joins the two sides by (a, b) alone.
    //
    // WHY FOUR SIGN BITS PER ENTRY REPLACE A DENSE (n+1)^2 SIGN IMAGE. The
    // welder needs to know which edges of the shared plane the surface crosses.
    // An edge crosses iff its two endpoint signs differ; and if it crosses, then
    // BOTH plane cells adjacent to that edge have a sign change among their four
    // corners, so both are in this list with the bits to prove it. So the sparse
    // list plus its corner bits determines every crossing edge on the plane --
    // an edge that no listed cell reports cannot be a crossing edge. Cells with
    // no vertex are cells the surface misses entirely, and they carry no
    // crossing to lose.
    //
    // On the COLUMN path +y/-y stay empty: the grid is XZ-columnar, one tile
    // spans the whole authored slab, and there is no neighbour above or below to
    // weld against. The Y-TILED path builds all six, by the same code -- see the
    // axis-generic rewrite below.
    if (boundary_out && contour_seams) {
        // THE WELDER HAS NOTHING LEFT TO DO, so it is handed nothing to do it
        // with: an EMPTY record, with only the header identifying the tile.
        //
        // This is the point of the whole design. The seam is closed at BAKE, by
        // both tiles independently, for every pair including equal-level ones
        // -- so there is no runtime pair to rebuild, no fan to enumerate, no
        // band to draw, and no `missing_coarse_pair` residue to account for.
        // `rebuild_weld_pair` reads a record with no verts and no band, emits
        // nothing, and drops the pair; the welder is already dead code here in
        // everything but name.
        //
        // Deleting it outright (rebuild_weld_pair, the pair pool, seam_weld.*,
        // the FaceRecord export, the weld parts and their TLAS/tracer
        // exclusions) is the follow-up, and it is deliberately NOT bundled with
        // this change: while the mode flag exists both paths have to work, and
        // the welder is the rollback.
        seam::SectorBoundary& sb = *boundary_out;
        sb = seam::SectorBoundary{};
        sb.rung    = rung;
        sb.cells   = n;
        sb.tx      = tx;
        sb.tz      = tz;
        sb.ty      = ty;
        sb.y_tiled = y_tiled;
        for (int f = 0; f < 6; ++f) sb.faces[f].face = f;
    } else if (boundary_out) {
        seam::SectorBoundary& sb = *boundary_out;
        sb = seam::SectorBoundary{};
        sb.rung    = rung;
        sb.cells   = n;
        sb.tx      = tx;
        sb.tz      = tz;
        sb.ty      = y_tiled ? ty : 0;
        sb.y_tiled = y_tiled;

        // Global CELL index of a LOCAL cell index c, per axis: global = goff + c.
        //   x: global = tx*n + ci - 1        (derived above)
        //   z: global = tz*n + ck - 1
        //   y: global = gy_off + cj          (regime-dependent; see the Y block)
        // Index 0/1/2 = x/y/z, matching seam::face_axis.
        const int64_t goff[3] = {
            tx * int64_t(n) - 1, gy_off, tz * int64_t(n) - 1
        };
        // The LOCAL cell range each axis contributes to a record. x and z always
        // export their owned cells [1..n]. Y exports [1..n] when tiled, and the
        // whole slab [0, sy-2] when not (there is no Y ownership on the column
        // path, so every cell of the slab is on the tile's ±x/±z boundary layer).
        const int cell_lo[3] = { 1, y_tiled ? 1 : 0,      1 };
        const int cell_hi[3] = { n, y_tiled ? n : sy - 2, n };

        // AXIS-GENERIC, and that is the M2 change here: the four XZ faces used
        // to be built by a body that hard-coded "normal is x or z, the other
        // tangential axis is y, and y is special". With Y tiled that distinction
        // is gone -- all three axes are tiled, indexed and owned identically --
        // so the body is written once over (normal axis, a axis, b axis) and the
        // ±y faces fall out of it rather than needing a second implementation.
        // Reproduces the previous XZ output exactly (the pinned record hashes in
        // terrain_mesher_tests.cpp are the gate on that claim).
        const auto build_face = [&](int face) {
            seam::FaceRecord& fr = sb.faces[face];
            fr.face = face;
            const int  ax       = seam::face_axis(face);     // 0 = x, 1 = y, 2 = z
            const bool positive = seam::face_is_positive(face);
            const int  layer    = positive ? cell_hi[ax] : cell_lo[ax];  // local CELL
            const int  plane    = positive ? layer + 1 : layer;          // local LATTICE
            int a_axis = 0, b_axis = 0;
            seam::face_tangent_axes(face, a_axis, b_axis);
            fr.cell_layer = goff[ax] + int64_t(layer);
            fr.plane      = goff[ax] + int64_t(plane);

            // (ca, cb) are LOCAL cell indices along the face's two tangential
            // axes; this scatters them plus `layer` into an (x, y, z) triple.
            const auto spread = [&](int ca, int cb, int nrm, int c[3]) {
                c[ax] = nrm; c[a_axis] = ca; c[b_axis] = cb;
            };
            const auto emit_vert = [&](int ca, int cb) {
                int c[3];
                spread(ca, cb, layer, c);
                const CellVert* v = get_vert(c[0], c[1], c[2]);
                if (!v) return;
                seam::BoundaryVert bv;
                bv.a = goff[a_axis] + int64_t(ca);
                bv.b = goff[b_axis] + int64_t(cb);
                // WORLD position, in double. The mesh stores x/z tile-local (and
                // y tile-local too once Y is tiled), so the origin goes back on
                // here rather than in the welder, which must never have to know
                // how this tile was rebased.
                bv.px = ox + double(v->p.x);
                bv.py = y_tiled ? oy + double(v->p.y) : double(v->p.y);
                bv.pz = oz + double(v->p.z);
                bv.nx = v->n.x; bv.ny = v->n.y; bv.nz = v->n.z;
                // RAW field material, the same 0..3 space MaterialBucket uses --
                // callers that remap buckets through a palette must remap these
                // identically (j_terrainVolume does). Sampled at the cell's x/z
                // centre; material_at is a 2D query, so a ±y face's cells are
                // named by their own (x, z) exactly as any other face's are.
                bv.material = uint32_t(field.material_at(
                    float(ox + (double(c[0]) - 0.5) * double(voxel)),
                    float(oz + (double(c[2]) - 0.5) * double(voxel))));
                // Density at the four corners of this cell's face ON THE PLANE:
                // bit0 (a,b)  bit1 (a+1,b)  bit2 (a,b+1)  bit3 (a+1,b+1), with
                // `> 0` = solid, matching the mesher's own sign test. Cell index
                // and its minimum LATTICE index are the same integer, so a
                // corner offset of (da, db) cells is (da, db) lattice steps.
                const auto corner = [&](int da, int db) -> float {
                    int L[3];
                    spread(ca + da, cb + db, plane, L);
                    return at(L[0], L[1], L[2]);
                };
                uint8_t bits = 0;
                if (corner(0, 0) > 0) bits |= 1u << 0;
                if (corner(1, 0) > 0) bits |= 1u << 1;
                if (corner(0, 1) > 0) bits |= 1u << 2;
                if (corner(1, 1) > 0) bits |= 1u << 3;
                bv.corner_signs = bits;
                fr.verts.push_back(bv);
            };

            // Loop `a` outer so (a, b) comes out ascending without a sort: the
            // global index is monotonic in the local one on every axis. (On a ±x
            // face `a` is the y cell, so this is the historical cj-outer/t-inner
            // nesting; on a ±z face it is t-outer/cj-inner. Both preserved.) The
            // std::sort below is therefore a no-op, and is kept because
            // FaceRecord::find binary-searches and a byte-identical record across
            // two bakes is a determinism gate -- the invariant should not rest on
            // reading the loop nesting right.
            for (int ca = cell_lo[a_axis]; ca <= cell_hi[a_axis]; ++ca)
                for (int cb = cell_lo[b_axis]; cb <= cell_hi[b_axis]; ++cb)
                    emit_vert(ca, cb);
            std::sort(fr.verts.begin(), fr.verts.end(),
                      [](const seam::BoundaryVert& p, const seam::BoundaryVert& q) {
                          return p.a != q.a ? p.a < q.a : p.b < q.b;
                      });
        };
        build_face(seam::kFacePosX);
        build_face(seam::kFaceNegX);
        build_face(seam::kFacePosZ);
        build_face(seam::kFaceNegZ);
        if (y_tiled) {
            build_face(seam::kFacePosY);
            build_face(seam::kFaceNegY);
        } else {
            sb.faces[seam::kFacePosY].face = seam::kFacePosY;
            sb.faces[seam::kFaceNegY].face = seam::kFaceNegY;
        }

        // The overlap bands (M0-WP7, and -y from M2), on the -side faces only.
        // Like the vertex records they depend on nothing but this tile and the
        // field, so they are not part of its bake identity either; unlike them
        // they are finished geometry, emitted verbatim by the welder rather than
        // joined.
        sb.faces[seam::kFaceNegX].band = std::move(band_neg_x);
        sb.faces[seam::kFaceNegZ].band = std::move(band_neg_z);
        if (y_tiled) sb.faces[seam::kFaceNegY].band = std::move(band_neg_y);
    }

    // Border skirts REMOVED 2026-07-30. This path used to hang a vertical
    // curtain (>= 8 m, wound outward) under all four sector edges, inherited
    // from the old full-height slab's implicit border walls.
    //
    // They were cross-rung seam cover, and nothing needs covering any more:
    // the ownership rule above ([1..n], see the comment at the top of this
    // function) already makes any LOD pair watertight without skirts or
    // overlap geometry. What was left was a curtain that only ever showed when
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

// The COLUMN entry point. Unchanged signature, unchanged bytes.
bool mesh_sector(const terrain_field::FieldRuntime& field,
                 int64_t tx, int64_t tz, int rung,
                 float sector_size, float y_min, float y_max,
                 SectorMesh& out, seam::SectorBoundary* boundary_out,
                 std::string& err) {
    return mesh_sector_impl(field, tx, /*ty=*/0, tz, rung, /*y_tiled=*/false,
                            sector_size, y_min, y_max, out, boundary_out, err);
}

// The Y-TILED entry point (M2). No y_min/y_max: the tile IS the extent.
bool mesh_sector_tiled(const terrain_field::FieldRuntime& field,
                       int64_t tx, int64_t ty, int64_t tz, int rung,
                       float sector_size,
                       SectorMesh& out, seam::SectorBoundary* boundary_out,
                       std::string& err) {
    return mesh_sector_impl(field, tx, ty, tz, rung, /*y_tiled=*/true,
                            sector_size, /*y_min=*/0.0f, /*y_max=*/0.0f,
                            out, boundary_out, err);
}

} // namespace terrain_mesher
