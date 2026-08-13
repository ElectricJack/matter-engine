// contour_seam_tests.cpp — the SHARED CONTOUR foundation, gated.
// Design: docs/contour-seam-design-2026-08-13.md.
//
// WHAT THIS SUITE IS FOR. The contour design replaces the runtime welder with a
// single canonical curve per shared plane that BOTH tiles compute independently
// and agree on bitwise. Everything else in that design — the constrained border
// cells, the tenting fans, the deletion of seam_weld.* — rests on that agreement
// actually holding, in floating point, between tiles of different sizes and
// different rungs asking about the same plane. If it does not hold exactly, the
// whole design collapses back into a zipper between two nearly-identical curves,
// which is the thing the owner's "no overlapping polygons, at any scale" ruling
// rules out.
//
// So this first increment gates the foundation and nothing else:
//
//   [1] AGREEMENT. A coarse tile and the four fine tiles across its face
//       compute BITWISE-IDENTICAL contour vertices on the region they share.
//   [2] ANCHORING. The agreement comes from the lattice being anchored to the
//       WORLD, not to the tile. Re-anchoring to the tile is tested and must
//       break agreement — a negative control, so [1] cannot pass by accident.
//   [3] TRACING == SAMPLING. The curve found by tracing from own-rung seeds is
//       the same vertex set as the curve found by sampling the whole face.
//       Tracing is what makes a level-5 tile affordable (O(curve) against
//       O(area) — 1M samples per face at the coarsest level otherwise), so the
//       equivalence is load-bearing, not an optimisation detail.
//   [4] COST. Contour vertices per face per level, which is the +40% triangle
//       estimate in the design doc measured rather than assumed.
//
// The meshing gates (no-overlap, watertight, the grazing dome, the topology
// mismatch) are the NEXT increment and are listed in the design doc's
// acceptance section. They are not here yet, and this suite passing does not
// mean the design works — it means the foundation the design rests on is sound
// enough to build the mesher against.
//
// NO ENGINE DEPENDENCIES, deliberately: an analytic field, a marching-squares
// contour, and the check harness. Nothing here includes terrain_mesher.h, so
// the suite cannot be made to pass by accident by the code it is meant to
// justify replacing.

#include "check.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

// ---------------------------------------------------------------------------
// The field. Analytic, and deliberately awkward in the ways that matter:
// several octaves so the surface has structure at every rung in the ladder, a
// tunnel so a plane can carry more than one contour component, and no
// randomness so a failure is reproducible from the seed alone.
// ---------------------------------------------------------------------------
namespace {

double smooth_noise(double x, double z, double freq, double phase) {
    // A deterministic band-limited wiggle. Not value noise — trigonometric, so
    // it is exactly reproducible across compilers at the bit level given the
    // same libm, which is what the agreement gates need.
    return std::sin(x * freq + phase) * std::cos(z * freq * 1.37 + phase * 0.5);
}

struct Field {
    // Density: positive in solid, negative in air, matching the engine's sign
    // convention (terrain_mesher.cpp's `> 0` tests).
    double density(double x, double y, double z) const {
        double h = 72.0;
        h += 26.0 * smooth_noise(x, z, 1.0 / 420.0, 0.3);
        h += 14.0 * smooth_noise(x, z, 1.0 / 110.0, 1.7);
        h +=  5.0 * smooth_noise(x, z, 1.0 /  26.0, 2.9);
        double d = h - y;
        if (tunnel) {
            // A horizontal tube through the rock, small enough that a coarse
            // lattice can miss it entirely — which is the topology-mismatch
            // case the design closes with a flat in-plane cap.
            const double cy = 40.0, cz = 30.0, r = 7.0;
            const double dy = y - cy, dz = z - cz;
            const double air = r - std::sqrt(dy * dy + dz * dz);
            d = std::min(d, -air);
        }
        return d;
    }
    bool tunnel = false;
};

// ---------------------------------------------------------------------------
// The canonical contour.
//
// A plane perpendicular to `axis` at world coordinate `plane`. The two
// tangential axes follow seam_boundary.h's fixed order so both sides of a plane
// agree on which index is `a` and which is `b` without negotiating:
// normal x -> (y, z), normal y -> (x, z), normal z -> (x, y).
//
// THE LATTICE IS ANCHORED TO THE WORLD, at integer multiples of `vc` from the
// origin — never to the tile. That is the entire mechanism: a coarse tile and a
// fine tile asking about the same plane walk the same lattice points and
// evaluate the same field at them, so their crossings are bitwise identical
// without either knowing the other exists. Anchor it to the tile and the two
// lattices are offset, the crossings differ in the last bits, and the design's
// central claim is false — which is what test [2] proves by doing exactly that.
//
// A vertex is the linear interpolation along a NAMED lattice edge: the edge
// from lattice point (ia, ib) to (ia+1, ib) ("a-edge") or to (ia, ib+1)
// ("b-edge"). That naming is what makes the vertex a primal, shareable quantity
// rather than a surface-nets centroid, and it is why the impossibility argument
// in volumetric-sectors-design-2026-08-10.md:226-233 does not reach this
// construction (see the design doc).
// ---------------------------------------------------------------------------

struct ContourVert {
    int64_t ia = 0, ib = 0;   // the lattice point the edge starts at
    int     dir = 0;          // 0 = a-edge, 1 = b-edge
    double  pa = 0, pb = 0;   // interpolated position, world units

    bool operator<(const ContourVert& o) const {
        if (ia != o.ia) return ia < o.ia;
        if (ib != o.ib) return ib < o.ib;
        return dir < o.dir;
    }
};

void tangent_axes(int axis, int& a_axis, int& b_axis) {
    if (axis == 0)      { a_axis = 1; b_axis = 2; }
    else if (axis == 1) { a_axis = 0; b_axis = 2; }
    else                { a_axis = 0; b_axis = 1; }
}

// Density at a point ON the plane, named by its two tangential coordinates.
double plane_density(const Field& f, int axis, double plane, double a, double b) {
    double p[3];
    int a_axis, b_axis;
    tangent_axes(axis, a_axis, b_axis);
    p[axis] = plane;
    p[a_axis] = a;
    p[b_axis] = b;
    return f.density(p[0], p[1], p[2]);
}

// The world coordinate of canonical lattice index i.
//
// `double(i) * vc` and not an accumulated sum: an accumulation would drift, and
// the whole point is that two callers starting from different indices land on
// the same bits. vc is a power of two in every configuration the ladder uses,
// so this product is exact.
inline double lattice_coord(int64_t i, double vc, double origin = 0.0) {
    return origin + double(i) * vc;
}

// Sample the field over a face square and emit every crossing on a canonical
// lattice edge inside it. `a0/a1/b0/b1` are WORLD bounds of the square; the
// lattice indices they map to are derived, never assumed.
// `origin` is the world coordinate the lattice is anchored at, and it exists
// ONLY so the negative control can move it. Production passes 0 -- the world
// origin -- and that is the entire mechanism the design rests on. Note that
// without this parameter the function is world-anchored BY CONSTRUCTION (the
// llrounds below snap any bounds back onto the world grid), which is exactly
// why the first version of the control silently tested nothing and passed.
std::vector<ContourVert> contour_by_sampling(const Field& f, int axis,
                                             double plane, double a0, double a1,
                                             double b0, double b1, double vc,
                                             double origin = 0.0) {
    const int64_t ia0 = (int64_t)std::llround((a0 - origin) / vc);
    const int64_t ia1 = (int64_t)std::llround((a1 - origin) / vc);
    const int64_t ib0 = (int64_t)std::llround((b0 - origin) / vc);
    const int64_t ib1 = (int64_t)std::llround((b1 - origin) / vc);

    // One density value per lattice point, so each point is evaluated once and
    // both edge directions read the SAME value — evaluating twice would be
    // correct in exact arithmetic and is a bit-level hazard in floating point.
    const int64_t na = ia1 - ia0 + 1, nb = ib1 - ib0 + 1;
    std::vector<double> d((size_t)(na * nb));
    for (int64_t j = 0; j < nb; ++j)
        for (int64_t i = 0; i < na; ++i)
            d[(size_t)(j * na + i)] = plane_density(
                f, axis, plane, lattice_coord(ia0 + i, vc, origin),
                lattice_coord(ib0 + j, vc, origin));

    std::vector<ContourVert> out;
    auto at = [&](int64_t i, int64_t j) { return d[(size_t)(j * na + i)]; };
    auto emit = [&](int64_t i, int64_t j, int dir) {
        const double da = at(i, j);
        const double db = dir == 0 ? at(i + 1, j) : at(i, j + 1);
        if ((da > 0) == (db > 0)) return;
        // t from the two samples in a fixed operand order. Both sides compute
        // this from identical inputs in identical order, so it is bitwise
        // reproducible; reversing the operands would not be.
        const double t = da / (da - db);
        ContourVert v;
        v.ia = ia0 + i;
        v.ib = ib0 + j;
        v.dir = dir;
        v.pa = lattice_coord(v.ia, vc, origin) + (dir == 0 ? t * vc : 0.0);
        v.pb = lattice_coord(v.ib, vc, origin) + (dir == 1 ? t * vc : 0.0);
        out.push_back(v);
    };
    for (int64_t j = 0; j < nb; ++j)
        for (int64_t i = 0; i < na; ++i) {
            if (i + 1 < na) emit(i, j, 0);
            if (j + 1 < nb) emit(i, j, 1);
        }
    return out;
}

// The same curve, found by TRACING instead of sampling the whole square.
//
// Seeds come from the asking tile's OWN rung: it samples its face at its own
// (much coarser) lattice, and every own-rung cell with a sign change is a place
// the curve provably passes through. From each seed the walk refines to the
// canonical lattice and follows the curve cell by cell until it closes or
// leaves the square.
//
// A component the tile's own lattice does not see is MISSED here, and that is
// correct rather than a defect: if this tile's lattice sees no crossing there,
// its surface does not reach the plane there, so it owes no geometry. The finer
// neighbour that does see it traces it and caps it flat. Test [3] therefore
// compares tracing against sampling only over the components the seeds reach.
std::vector<ContourVert> contour_by_tracing(const Field& f, int axis,
                                            double plane, double a0, double a1,
                                            double b0, double b1, double vc,
                                            double own_v) {
    const int64_t ia0 = (int64_t)std::llround(a0 / vc);
    const int64_t ia1 = (int64_t)std::llround(a1 / vc);
    const int64_t ib0 = (int64_t)std::llround(b0 / vc);
    const int64_t ib1 = (int64_t)std::llround(b1 / vc);

    std::map<std::pair<int64_t, int64_t>, double> cache;
    auto sample = [&](int64_t i, int64_t j) -> double {
        auto key = std::make_pair(i, j);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        const double v = plane_density(f, axis, plane, lattice_coord(i, vc),
                                       lattice_coord(j, vc));
        cache.emplace(key, v);
        return v;
    };

    // Canonical cells the walk has already visited, so a component is traced
    // once however many seeds land on it.
    std::set<std::pair<int64_t, int64_t>> seen;
    std::vector<std::pair<int64_t, int64_t>> stack;

    // Seeds: own-rung cells with a sign change among their four corners.
    const int64_t own_steps = (int64_t)std::llround(own_v / vc);
    for (int64_t j = ib0; j + own_steps <= ib1; j += own_steps)
        for (int64_t i = ia0; i + own_steps <= ia1; i += own_steps) {
            const double c[4] = {sample(i, j), sample(i + own_steps, j),
                                 sample(i, j + own_steps),
                                 sample(i + own_steps, j + own_steps)};
            bool pos = c[0] > 0, mixed = false;
            for (int k = 1; k < 4; ++k) mixed |= ((c[k] > 0) != pos);
            if (!mixed) continue;
            // The own-rung cell is mixed, so SOME canonical cell inside it is
            // mixed; push them all and let the visited set collapse the work.
            for (int64_t jj = j; jj < j + own_steps; ++jj)
                for (int64_t ii = i; ii < i + own_steps; ++ii)
                    stack.push_back({ii, jj});
        }

    std::vector<ContourVert> out;
    while (!stack.empty()) {
        const auto cell = stack.back();
        stack.pop_back();
        const int64_t i = cell.first, j = cell.second;
        if (i < ia0 || j < ib0 || i + 1 > ia1 || j + 1 > ib1) continue;
        if (!seen.insert(cell).second) continue;

        const double c00 = sample(i, j), c10 = sample(i + 1, j);
        const double c01 = sample(i, j + 1), c11 = sample(i + 1, j + 1);
        bool pos = c00 > 0, mixed = false;
        const double cs[3] = {c10, c01, c11};
        for (double v : cs) mixed |= ((v > 0) != pos);
        if (!mixed) continue;

        // Emit this cell's own two owned edges (the -a and -b ones), matching
        // the sampling walk's ownership so the two vertex sets are comparable
        // rather than merely similar.
        auto emit = [&](int64_t ei, int64_t ej, int dir, double da, double db) {
            if ((da > 0) == (db > 0)) return;
            const double t = da / (da - db);
            ContourVert v;
            v.ia = ei; v.ib = ej; v.dir = dir;
            v.pa = lattice_coord(ei, vc) + (dir == 0 ? t * vc : 0.0);
            v.pb = lattice_coord(ej, vc) + (dir == 1 ? t * vc : 0.0);
            out.push_back(v);
        };
        // All four of this cell's edges, unguarded. The guards that used to
        // sit on the last two were wrong: an edge on the square's FAR row or
        // column is owned by a cell outside the cell range, so nothing else
        // would ever emit it, and tracing came back exactly one vertex short of
        // sampling at every level. The cell is inside the square by the bounds
        // test above, so all four of its edges are too.
        emit(i, j, 0, c00, c10);
        emit(i, j, 1, c00, c01);
        emit(i + 1, j, 1, c10, c11);
        emit(i, j + 1, 0, c01, c11);

        // Follow the curve: any neighbour sharing a crossed edge is on it.
        stack.push_back({i + 1, j});
        stack.push_back({i - 1, j});
        stack.push_back({i, j + 1});
        stack.push_back({i, j - 1});
    }

    // The walk can reach one edge from either of its two cells, so dedupe by
    // the edge's NAME. That the name is enough is the point of naming them.
    std::set<ContourVert> uniq(out.begin(), out.end());
    return std::vector<ContourVert>(uniq.begin(), uniq.end());
}

// Restrict a vertex list to a sub-square, so a coarse tile's face contour can be
// compared against a fine tile's on the quadrant they actually share.
std::vector<ContourVert> clip(const std::vector<ContourVert>& in, double a0,
                              double a1, double b0, double b1) {
    std::vector<ContourVert> out;
    for (const ContourVert& v : in)
        if (v.pa >= a0 && v.pa <= a1 && v.pb >= b0 && v.pb <= b1)
            out.push_back(v);
    return out;
}

// Bitwise, not approximate. `==` on doubles is the whole assertion: "the two
// sides agree to within an epsilon" is exactly the property that forces a
// zipper, and a zipper is what the design exists to avoid.
bool same_bitwise(const std::vector<ContourVert>& x,
                  const std::vector<ContourVert>& y, int& first_bad) {
    std::map<std::pair<std::pair<int64_t, int64_t>, int>,
             std::pair<double, double>> m;
    for (const ContourVert& v : x)
        m[{{v.ia, v.ib}, v.dir}] = {v.pa, v.pb};
    for (const ContourVert& v : y) {
        auto it = m.find({{v.ia, v.ib}, v.dir});
        if (it == m.end()) { first_bad = 1; return false; }
        if (it->second.first != v.pa || it->second.second != v.pb) {
            first_bad = 2;
            return false;
        }
        m.erase(it);
    }
    if (!m.empty()) { first_bad = 3; return false; }
    first_bad = 0;
    return true;
}

}  // namespace

int main() {
    printf("=== contour seam foundation (docs/contour-seam-design-2026-08-13.md) ===\n");

    Field field;
    const double vc = 2.0;          // canonical rung: the finest voxel
    const double S0 = 32.0;         // level-0 tile size (SeamLab's)

    // A vertical plane (normal x) at a coordinate that is a border for several
    // levels at once, which is exactly where a cross-level pair can occur.
    const int axis = 0;
    const double plane = 128.0;

    // ---------------------------------------------------------------------
    // [1] AGREEMENT: a coarse tile and a fine tile across the same plane
    //     compute the SAME contour vertices, bitwise, on the region they share.
    // ---------------------------------------------------------------------
    for (int L = 1; L <= 3; ++L) {
        const double Sc = S0 * double(1 << L);        // coarse tile size
        const double Sf = Sc * 0.5;                   // fine tile size
        // The squares must STRADDLE THE SURFACE or the gate is vacuous: the
        // first run of this suite reported "BITWISE IDENTICAL" over ZERO shared
        // vertices at two of three levels, which asserts nothing at all. Each
        // tile sits at a real tile index of its own size containing the surface
        // height, so the fine square is a genuine quadrant of the coarse one
        // and both carry contour.
        const double kSurface = 72.0;
        const double ca0 = std::floor(kSurface / Sc) * Sc, ca1 = ca0 + Sc;
        const double cb0 = 0.0, cb1 = Sc;
        const double fa0 = std::floor(kSurface / Sf) * Sf, fa1 = fa0 + Sf;
        const double fb0 = 0.0, fb1 = Sf;

        const auto coarse = contour_by_sampling(field, axis, plane, ca0, ca1,
                                                cb0, cb1, vc);
        const auto fine = contour_by_sampling(field, axis, plane, fa0, fa1,
                                              fb0, fb1, vc);
        const auto coarse_on_quadrant = clip(coarse, fa0, fa1, fb0, fb1);
        const auto fine_on_quadrant = clip(fine, fa0, fa1, fb0, fb1);

        int bad = 0;
        const bool ok = same_bitwise(coarse_on_quadrant, fine_on_quadrant, bad);
        char msg[192];
        snprintf(msg, sizeof msg,
                 "[1] L%d/L%d shared quadrant: contours differ (code %d, %zu vs %zu verts)",
                 L, L - 1, bad, coarse_on_quadrant.size(), fine_on_quadrant.size());
        CHECK(ok, msg);
        printf("  [1] L%d (%.0f m) vs L%d (%.0f m): %zu shared vertices, %s\n",
               L, Sc, L - 1, Sf, fine_on_quadrant.size(),
               ok ? "BITWISE IDENTICAL" : "DIFFER");
    }

    // ---------------------------------------------------------------------
    // [2] NEGATIVE CONTROL: the agreement above comes from anchoring the
    //     lattice to the WORLD. Anchor it to the tile instead and it must
    //     break — otherwise [1] is passing for some reason other than the one
    //     the design claims, and the claim is untested.
    // ---------------------------------------------------------------------
    {
        const double Sc = S0 * 4.0;
        const double a0 = std::floor(72.0 / Sc) * Sc;
        // Half a canonical voxel of anchor offset -- what a tile-anchored
        // lattice gives for any tile whose origin is not a multiple of vc.
        const double shift = vc * 0.5;
        const auto world_anchored = contour_by_sampling(
            field, axis, plane, a0, a0 + Sc, 0.0, Sc, vc, 0.0);
        const auto tile_anchored = contour_by_sampling(
            field, axis, plane, a0, a0 + Sc, 0.0, Sc, vc, shift);
        int bad = 0;
        const bool differ = !same_bitwise(world_anchored, tile_anchored, bad);
        CHECK(differ,
              "[2] a tile-anchored lattice agreed with a world-anchored one -- "
              "the control is not controlling anything");
        printf("  [2] negative control: tile-anchored lattice %s (as it must)\n",
               differ ? "DISAGREES" : "agrees -- BROKEN");
    }

    // ---------------------------------------------------------------------
    // [3] TRACING == SAMPLING. Tracing is what makes a coarse tile affordable;
    //     if it finds a different curve the saving is worthless.
    // ---------------------------------------------------------------------
    for (int L = 1; L <= 3; ++L) {
        const double S = S0 * double(1 << L);
        const double own_v = vc * double(1 << L);   // this tile's own voxel
        const double a0 = std::floor(72.0 / S) * S;
        const auto sampled =
            contour_by_sampling(field, axis, plane, a0, a0 + S, 0.0, S, vc);
        const auto traced = contour_by_tracing(field, axis, plane, a0, a0 + S,
                                               0.0, S, vc, own_v);
        int bad = 0;
        const bool ok = same_bitwise(sampled, traced, bad);
        char msg[192];
        snprintf(msg, sizeof msg,
                 "[3] L%d: traced contour differs from sampled (code %d, %zu vs %zu)",
                 L, bad, sampled.size(), traced.size());
        CHECK(ok, msg);
        printf("  [3] L%d (%.0f m, own voxel %.0f m): sampled %zu, traced %zu -- %s\n",
               L, S, own_v, sampled.size(), traced.size(),
               ok ? "IDENTICAL" : "DIFFER");
    }

    // ---------------------------------------------------------------------
    // [4] COST. Contour vertices per face against the tile's own interior cell
    //     count, which is the design doc's +40% estimate measured on one face
    //     of one field rather than assumed from a table.
    // ---------------------------------------------------------------------
    printf("  [4] cost, one vertical face, this field:\n");
    printf("      %-6s %8s %10s %12s %10s\n", "level", "tile m", "contour v",
           "interior c", "ratio");
    for (int L = 0; L <= 5; ++L) {
        const double S = S0 * double(1 << L);
        const double a0 = std::floor(72.0 / S) * S;
        const auto c =
            contour_by_sampling(field, axis, plane, a0, a0 + S, 0.0, S, vc);
        // A face's worth of interior: the tile has n^2 cells on the face layer,
        // n = 16 for SeamLab's sectorSize 32 at any level.
        const double interior_cells = 16.0 * 16.0;
        printf("      %-6d %8.0f %10zu %12.0f %10.2f\n", L, S, c.size(),
               interior_cells, double(c.size()) / interior_cells);
    }

    // ---------------------------------------------------------------------
    // [5] TOPOLOGY: a plane carrying more than one component, and one a coarse
    //     lattice misses entirely. Not a gate yet -- the flat-cap response
    //     belongs to the meshing increment -- but the counts are recorded here
    //     so the next increment starts from a measured fixture rather than a
    //     hypothetical one.
    // ---------------------------------------------------------------------
    {
        Field caved;
        caved.tunnel = true;
        const double S = S0 * 4.0;
        const double a0 = std::floor(40.0 / S) * S;   // the tunnel's altitude
        const auto fine =
            contour_by_sampling(caved, axis, plane, a0, a0 + S, 0.0, S, vc);
        const auto coarse_seeded = contour_by_tracing(caved, axis, plane, a0,
                                                      a0 + S, 0.0, S, vc, vc * 8.0);
        printf("  [5] tunnel fixture: canonical %zu verts, traced from an 8x "
               "own-rung seed set %zu -- %zu fine-only\n",
               fine.size(), coarse_seeded.size(),
               fine.size() - std::min(fine.size(), coarse_seeded.size()));
    }

    printf("=== contour seam foundation ");
    return check_summary();
}
