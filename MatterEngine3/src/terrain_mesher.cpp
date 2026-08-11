// terrain_mesher.cpp — naive surface-nets sector mesher.
// Pure CPU; no JS, no GL.

#include "terrain_mesher.h"
#include <algorithm>
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
    const int kBandCells = 2;

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
                const bool own   = owned(i, j, k);
                const bool extra = want_band && band_extra(i, j, k);
                if (!own && !extra) continue;
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
                        if (own)
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
                        if (own)
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
                        if (own)
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
    if (boundary_out) {
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
