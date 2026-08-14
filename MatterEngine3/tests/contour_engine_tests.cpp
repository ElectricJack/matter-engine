// contour_engine_tests.cpp — the contour mesher IN THE ENGINE, gated.
// Design: docs/contour-seam-design-2026-08-13.md.
//
// WHAT THIS SUITE DECIDES, and why it is not the prototype suites.
//
// `run-contourseam` proved the canonical contour agrees bitwise across levels.
// `run-contourmesh` proved a tile can terminate on it with no overlap. Both run
// against an analytic field and their own surface nets, deliberately, so they
// could not be made to pass by the code they justify replacing. Both also share
// one blind spot, and it is the last structural claim in the design:
//
//   THE TILE-EDGE HANDOVER. A face contour terminates on its face square's
//   boundary, and where two faces of the same cube meet -- a cube EDGE -- the
//   two contours have to hand over to each other. `run-contourmesh` reports 4
//   non-manifold edges on cube-edge lines in every case and EXCLUDES them from
//   its gate, because its fixture is one pair of tiles: the other tiles meeting
//   along those lines are simply not there, so their absence is what the count
//   measures. That is the same fixture gap that once reported 21 interior
//   failures at 2:1 (a 2x1 face covered by 2x2 fine tiles, one of them meshed),
//   and the lesson recorded there applies to itself: A SEAM GATE WHOSE FIXTURE
//   IS MISSING A TILE MEASURES THE MISSING TILE.
//
// So this suite meshes CLOSED BLOCKS through the real `mesh_sector_tiled` and
// asks about their INTERIOR only -- the region far enough from the block's own
// outer shell that every tile a surface there could need is present. Nothing is
// excluded from the gate; if the handover is broken, it fails here.
//
//   [1] EQUAL LEVEL, 2x2x2. Eight tiles around one interior corner. The centre
//       point is where eight tiles meet, and every cube edge through it is
//       fully surrounded.
//   [2] CROSS LEVEL, 2x2x2 with one tile SPLIT into its eight children. A legal
//       octree configuration (neighbours differ by one level across faces), and
//       the split tile's three interior faces are 2:1 with their neighbours --
//       so the fixture carries 2:1 faces, 2:1 cube edges and a 2:1 corner at
//       once, which is the configuration `restrict_levels` actually produces.
//   [3] NO COPLANAR OVERLAP on the interior planes, the owner's ruling
//       (issue 736f92da) checked exactly rather than sampled.
//
// The field is kCave0 from seam_integration_tests: ridge tunnels under a noise
// surface straddling y = 0, so a block centred there carries surface, cave
// shell and topology a coarse lattice can miss, rather than one smooth sheet.

#include "check.h"
#include "../src/terrain_field.h"
#include "../src/terrain_mesher.h"
#include "../src/bake_mode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace terrain_field;
using namespace terrain_mesher;

namespace {

static FieldRuntime make(const char* text) {
    FieldProgram p;
    std::string err;
    if (!FieldProgram::parse(text, p, err)) printf("parse err: %s\n", err.c_str());
    return FieldRuntime(std::move(p));
}

// Ridge tunnels under a noise surface, the top surface straddling y = 0 so a
// block centred there is crossed by it. (seam_integration_tests.cpp `kCave0`.)
static const char* kCave0 =
    "noise2 1234 0.02 4 0.5 2.0\nconst 20\nmul r0 r1\n"
    "const 0\nadd r2 r3\n"
    "input wy\n"
    "sub r4 r5\n"
    "ridge3 4242 0.02 2 0.5 2\n"
    "const 0.45\nsub r7 r8\n"
    "const -1\nmul r9 r10\n"
    "min r6 r11\n"
    "const -100\nsub r13 r5\n"
    "max r12 r14\n"
    "const 0.5\n"
    "height r4\ndensity r15\nmoisture r16\nrelief r16\n"
    "seaLevel -1000\nbiome 0.65 0.35\n";

// ---------------------------------------------------------------------------
// The soup: every tile's triangles in one world-space mesh, vertices merged.
//
// MERGED BY TOLERANCE, NOT BY BITS, and the reason is a property of the mesh
// format rather than of this design. Positions are stored TILE-LOCAL, so two
// tiles sharing a world point store `float(P - o1)` and `float(P - o2)` -- the
// same real number rounded from two different subtractions, which differ in the
// last bits of a float. That is the existing contract: the [1..n] ownership
// rule has always had two tiles reference one geometric vertex from two frames,
// and the divergence scales with distance exactly as the pixel does, so it is
// sub-pixel at every level.
//
// kTol is chosen against the fixture: local coordinates here are at most 128 m,
// where a float ulp is ~7.6e-6 m, so 1e-4 m is more than an order of magnitude
// clear of the rounding and five orders below the smallest real feature (a
// canonical voxel is 2 m). The defects this suite hunts -- a missing triangle,
// a doubled surface -- are metres wide, not microns.
constexpr double kTol = 1e-4;

// NORMAL DISAGREEMENT AT A SHARED VERTEX. When two tiles both reference one
// world point, they each store a normal for it, and if those differ the surface
// creases along the seam however watertight the geometry is. `nx/ny/nz` holds
// the FIRST normal seen at a merged vertex and `worst_cos`/`from_tile` let the
// second one be compared against it, so a shared vertex reports the angle
// between the two tiles' answers rather than one of them.
struct Soup {
    std::vector<double> px, py, pz;
    std::vector<float> nx, ny, nz;
    std::vector<int> owner;          // which tile first wrote this vertex
    std::vector<float> worst_dot;    // min dot against a later tile's normal
    std::vector<int> tri;
    std::unordered_map<int64_t, std::vector<int>> grid;

    static int64_t cellkey(int64_t a, int64_t b, int64_t c) {
        return (a * 73856093LL) ^ (b * 19349663LL) ^ (c * 83492791LL);
    }
    int add(double x, double y, double z,
            float vnx = 0.0f, float vny = 0.0f, float vnz = 0.0f,
            int tile = -1) {
        const double inv = 1.0 / (2.0 * kTol);
        const int64_t ci = int64_t(std::floor(x * inv));
        const int64_t cj = int64_t(std::floor(y * inv));
        const int64_t ck = int64_t(std::floor(z * inv));
        // A hash collision between two distinct cells only lengthens a
        // candidate list; the distance test below is what decides.
        for (int di = -1; di <= 1; ++di)
            for (int dj = -1; dj <= 1; ++dj)
                for (int dk = -1; dk <= 1; ++dk) {
                    auto it = grid.find(cellkey(ci + di, cj + dj, ck + dk));
                    if (it == grid.end()) continue;
                    for (int id : it->second)
                        if (std::fabs(px[id] - x) <= kTol &&
                            std::fabs(py[id] - y) <= kTol &&
                            std::fabs(pz[id] - z) <= kTol) {
                            // Only a DIFFERENT tile's answer is a disagreement.
                            // Two triangles of one tile sharing a vertex store
                            // the same normal by construction.
                            if (tile >= 0 && owner[id] >= 0 && owner[id] != tile) {
                                const float d = nx[id] * vnx + ny[id] * vny +
                                                nz[id] * vnz;
                                if (d < worst_dot[id]) worst_dot[id] = d;
                            }
                            return id;
                        }
                }
        const int id = int(px.size());
        px.push_back(x); py.push_back(y); pz.push_back(z);
        nx.push_back(vnx); ny.push_back(vny); nz.push_back(vnz);
        owner.push_back(tile);
        worst_dot.push_back(1.0f);
        grid[cellkey(ci, cj, ck)].push_back(id);
        return id;
    }
    void add_tri(int a, int b, int c) {
        if (a == b || b == c || a == c) return;   // collapsed after merging
        tri.push_back(a); tri.push_back(b); tri.push_back(c);
    }
};

// Mesh one tile and fold it into the soup. Y-tiled positions are tile-local on
// all three axes, so the origin goes back on here.
size_t add_tile(Soup& s, const FieldRuntime& f, int64_t tx, int64_t ty,
                int64_t tz, int rung, float sector_size, int tile_id = -1) {
    SectorMesh m;
    seam::SectorBoundary sb;
    std::string err;
    if (!mesh_sector_tiled(f, tx, ty, tz, rung, sector_size, m, &sb, err)) {
        printf("  mesh_sector_tiled(%lld,%lld,%lld) failed: %s\n",
               (long long)tx, (long long)ty, (long long)tz, err.c_str());
        return 0;
    }
    const double ox = double(tx) * double(sector_size);
    const double oy = double(ty) * double(sector_size);
    const double oz = double(tz) * double(sector_size);
    size_t tris = 0;
    for (const auto& b : m.buckets)
        for (size_t t = 0; t * 9 + 8 < b.positions.size(); ++t) {
            int id[3];
            for (int v = 0; v < 3; ++v)
                id[v] = s.add(ox + double(b.positions[t * 9 + v * 3 + 0]),
                              oy + double(b.positions[t * 9 + v * 3 + 1]),
                              oz + double(b.positions[t * 9 + v * 3 + 2]),
                              b.normals[t * 9 + v * 3 + 0],
                              b.normals[t * 9 + v * 3 + 1],
                              b.normals[t * 9 + v * 3 + 2], tile_id);
            s.add_tri(id[0], id[1], id[2]);
            ++tris;
        }
    return tris;
}

struct Region {
    double lo[3], hi[3];
    bool holds(const Soup& s, int v) const {
        const double p[3] = {s.px[v], s.py[v], s.pz[v]};
        for (int c = 0; c < 3; ++c)
            if (p[c] < lo[c] || p[c] > hi[c]) return false;
        return true;
    }
};

// Every edge with BOTH endpoints strictly inside the region must be incident to
// exactly two triangles. Inside the region every tile a surface could need is
// present, so there is no legitimate boundary there -- an edge with one
// triangle is a hole and an edge with three or more is a fold.
int non_manifold_in(const Soup& s, const Region& r, int& holes, int& folds,
                    std::vector<std::array<double, 3>>* where) {
    std::map<std::pair<int, int>, int> count;
    for (size_t t = 0; t * 3 + 2 < s.tri.size(); ++t)
        for (int e = 0; e < 3; ++e) {
            int a = s.tri[t * 3 + e], b = s.tri[t * 3 + (e + 1) % 3];
            if (a > b) std::swap(a, b);
            ++count[{a, b}];
        }
    holes = folds = 0;
    for (const auto& kv : count) {
        if (kv.second == 2) continue;
        if (!r.holds(s, kv.first.first) || !r.holds(s, kv.first.second)) continue;
        if (kv.second < 2) ++holes; else ++folds;
        if (where && where->size() < 12)
            where->push_back({0.5 * (s.px[kv.first.first] + s.px[kv.first.second]),
                              0.5 * (s.py[kv.first.first] + s.py[kv.first.second]),
                              0.5 * (s.pz[kv.first.first] + s.pz[kv.first.second])});
    }
    return holes + folds;
}

// Two triangles both lying IN a plane whose projections share area. This is the
// exact failure mode of the overlap band and of any zipper between two curves,
// so it is checked exactly rather than sampled.
int coplanar_overlaps(const Soup& s, int axis, double plane, const Region& r) {
    const int aa = axis == 0 ? 1 : 0;
    const int bb = axis == 2 ? 1 : 2;
    const std::vector<double>* co[3] = {&s.px, &s.py, &s.pz};
    struct T { double a[3], b[3]; };
    std::vector<T> flat;
    for (size_t t = 0; t * 3 + 2 < s.tri.size(); ++t) {
        bool in = true;
        T q{};
        for (int c = 0; c < 3 && in; ++c) {
            const int v = s.tri[t * 3 + c];
            if (std::fabs((*co[axis])[v] - plane) > kTol || !r.holds(s, v))
                in = false;
            else { q.a[c] = (*co[aa])[v]; q.b[c] = (*co[bb])[v]; }
        }
        if (in) flat.push_back(q);
    }
    auto area2 = [](const T& t) {
        return (t.a[1] - t.a[0]) * (t.b[2] - t.b[0]) -
               (t.a[2] - t.a[0]) * (t.b[1] - t.b[0]);
    };
    int overlaps = 0;
    for (size_t i = 0; i < flat.size(); ++i) {
        if (std::fabs(area2(flat[i])) < 1e-12) continue;
        for (size_t j = 0; j < flat.size(); ++j) {
            if (i == j || std::fabs(area2(flat[j])) < 1e-12) continue;
            const double ca = (flat[j].a[0] + flat[j].a[1] + flat[j].a[2]) / 3.0;
            const double cb = (flat[j].b[0] + flat[j].b[1] + flat[j].b[2]) / 3.0;
            const T& t = flat[i];
            const double d0 = (t.a[1] - t.a[0]) * (cb - t.b[0]) -
                              (t.b[1] - t.b[0]) * (ca - t.a[0]);
            const double d1 = (t.a[2] - t.a[1]) * (cb - t.b[1]) -
                              (t.b[2] - t.b[1]) * (ca - t.a[1]);
            const double d2 = (t.a[0] - t.a[2]) * (cb - t.b[2]) -
                              (t.b[0] - t.b[2]) * (ca - t.a[2]);
            if (!((d0 < 0 || d1 < 0 || d2 < 0) && (d0 > 0 || d1 > 0 || d2 > 0)))
                ++overlaps;
        }
    }
    return overlaps;
}

// Is a coordinate inside the border BAND of a tile plane? `grid` is the
// fixture's smallest tile pitch (every tile plane is a multiple of it) and
// `band` is the deepest the border rule can reach in from a plane -- one
// COARSEST voxel, because the fan runs from a border cell's dual vertex, which
// sits anywhere inside a cell that deep.
//
// Getting this wrong is not cosmetic: with the band set to one CANONICAL voxel
// (2 m) instead of one coarse voxel (4 m), half of the border layer was
// classified as interior, and the "interior" counts then differed between the
// two modes -- which is impossible, since the rule cannot reach there. The
// disagreement was the classifier, not the mesher.
bool near_plane(double v, double grid, double band) {
    const double r = v / grid;
    return std::fabs(r - std::round(r)) * grid <= band;
}

// SEAM defects are the ones this rule is responsible for: within one canonical
// voxel of a tile plane, which is the whole reach of the border fan and bridge.
// INTERIOR defects are further in than the rule ever touches.
//
// The split is what makes the gate meaningful. Naive surface nets is not a
// manifold mesher -- one vertex per mixed cell pinches wherever two sheets of
// surface pass through one cell, which a cave field does constantly -- so a raw
// non-manifold count over a cave block measures the mesher's oldest property,
// not this change. Only the seam class is the claim under test, and the
// interior class is checked against the SAME BLOCK meshed the old way.
struct Counts {
    int seam = 0, interior = 0, holes = 0, folds = 0;
    std::vector<std::array<double, 4>> seam_where;   // x, y, z, incident tris
};

Counts classify(const Soup& s, const Region& r, double grid, double band) {
    Counts c;
    std::map<std::pair<int, int>, int> count;
    for (size_t t = 0; t * 3 + 2 < s.tri.size(); ++t)
        for (int e = 0; e < 3; ++e) {
            int a = s.tri[t * 3 + e], b = s.tri[t * 3 + (e + 1) % 3];
            if (a > b) std::swap(a, b);
            ++count[{a, b}];
        }
    for (const auto& kv : count) {
        if (kv.second == 2) continue;
        const int va = kv.first.first, vb = kv.first.second;
        if (!r.holds(s, va) || !r.holds(s, vb)) continue;
        const double m[3] = {0.5 * (s.px[va] + s.px[vb]),
                             0.5 * (s.py[va] + s.py[vb]),
                             0.5 * (s.pz[va] + s.pz[vb])};
        const bool seam = near_plane(m[0], grid, band) ||
                          near_plane(m[1], grid, band) ||
                          near_plane(m[2], grid, band);
        if (seam) {
            ++c.seam;
            if (kv.second < 2) ++c.holes; else ++c.folds;
            if (c.seam_where.size() < 10)
                c.seam_where.push_back({m[0], m[1], m[2], double(kv.second)});
        } else {
            ++c.interior;
        }
    }
    return c;
}

}  // namespace

int main() {
    printf("=== contour mesher in the engine "
           "(docs/contour-seam-design-2026-08-13.md) ===\n");

    FieldRuntime f = make(kCave0);

    // The block: 64 m tiles spanning [0,128] x [-64,64] x [0,128], so its
    // centre (64, 0, 64) -- where all eight meet at a point -- sits in the
    // surface band and the interior corner carries real geometry.
    const float S = 64.0f;
    const int   rung = -1;                 // 4 m voxels, 16 cells per tile
    const Region interior{{32.0, -32.0, 32.0}, {96.0, 32.0, 96.0}};

    // `split` replaces the tile at (1, 0, 1) -- the one whose -x/-y/-z faces
    // ARE the three interior planes -- with its eight 32 m children. Face
    // neighbours then differ by exactly one level, which is what
    // `restrict_levels` guarantees; the fixture carries three 2:1 faces, the
    // cube EDGES where two of them meet and the corner where three do, all in
    // the interior at once.
    auto build = [&](bool contour, bool split, size_t& tris) {
        bake_mode::forced_contour_seams() = contour ? 1 : 0;
        Soup s;
        tris = 0;
        int tile_id = 0;
        for (int64_t tx = 0; tx <= 1; ++tx)
            for (int64_t ty = -1; ty <= 0; ++ty)
                for (int64_t tz = 0; tz <= 1; ++tz) {
                    if (split && tx == 1 && ty == 0 && tz == 1) continue;
                    tris += add_tile(s, f, tx, ty, tz, rung, S, tile_id++);
                }
        if (split)
            for (int64_t tx = 2; tx <= 3; ++tx)
                for (int64_t ty = 0; ty <= 1; ++ty)
                    for (int64_t tz = 2; tz <= 3; ++tz)
                        tris += add_tile(s, f, tx, ty, tz, 0, 32.0f, tile_id++);
        return s;
    };

    // SHADING CONTINUITY ACROSS THE SEAM. Watertight geometry with two normals
    // at a shared vertex still prints a hard edge, so this is measured
    // separately from the manifold gates: over every vertex two different tiles
    // both wrote, the angle between their two answers.
    auto normal_split = [](const Soup& s, int& shared, double& worst_deg,
                           double& mean_deg) {
        shared = 0;
        worst_deg = 0.0;
        double sum = 0.0;
        for (size_t i = 0; i < s.worst_dot.size(); ++i) {
            if (s.worst_dot[i] >= 1.0f) continue;   // never seen twice
            ++shared;
            const double d = std::max(-1.0, std::min(1.0, double(s.worst_dot[i])));
            const double deg = std::acos(d) * 180.0 / 3.14159265358979323846;
            worst_deg = std::max(worst_deg, deg);
            sum += deg;
        }
        mean_deg = shared ? sum / double(shared) : 0.0;
    };

    // `band` is one COARSEST voxel: 4 m at rung -1, which is exactly how far in
    // from a plane the border rule can reach (the fan runs from a border cell's
    // dual vertex, and that cell is one voxel deep).
    struct Case { const char* name; bool split; double grid; double band; };
    const Case cases[] = {
        {"[1] equal level 2x2x2",     false, 64.0, 4.0},
        {"[2] cross level 2:1 split", true,  32.0, 4.0},
    };

    for (const Case& cs : cases) {
        size_t tris_c = 0, tris_w = 0;
        const Soup sc = build(/*contour=*/true,  cs.split, tris_c);
        const Soup sw = build(/*contour=*/false, cs.split, tris_w);
        const Counts c = classify(sc, interior, cs.grid, cs.band);
        const Counts w = classify(sw, interior, cs.grid, cs.band);

        printf("  %s\n", cs.name);
        printf("      contour %zu tris / %zu verts | welder %zu tris / %zu verts"
               "  (%+.1f%%)\n", tris_c, sc.px.size(), tris_w, sw.px.size(),
               100.0 * (double(tris_c) / double(tris_w) - 1.0));
        printf("      interior non-manifold: contour %d, welder %d "
               "(surface nets pinches, not the seam)\n", c.interior, w.interior);
        printf("      SEAM non-manifold:     contour %d (%d holes, %d folds), "
               "welder %d\n", c.seam, c.holes, c.folds, w.seam);
        {
            int nsh = 0, wsh = 0;
            double nworst = 0, nmean = 0, wworst = 0, wmean = 0;
            normal_split(sc, nsh, nworst, nmean);
            normal_split(sw, wsh, wworst, wmean);
            printf("      SHARED-VERTEX NORMAL SPLIT: contour %d verts, worst "
                   "%.1f deg, mean %.1f | welder %d verts, worst %.1f, mean %.1f\n",
                   nsh, nworst, nmean, wsh, wworst, wmean);
            // WATERTIGHT IS NOT ENOUGH. Two tiles can share a vertex exactly
            // and still store two different normals for it, and the seam then
            // prints as a hard shading edge however perfect the geometry is --
            // which is what the hairlines of issue ec2829d6 turned out to be.
            //
            // This is the gate on the one decision that buys it: a contour
            // vertex takes its normal from a CANONICAL central difference, not
            // the tile's own voxel. Nothing guarded that until the owner asked
            // why sector boundaries had hard edges, and the alternative is not
            // marginal -- probing at each tile's own scale measures, on this
            // fixture, a WORST SPLIT OF 85.3 DEGREES and a mean of 8.2 across
            // every 2:1 seam, against 0.0 here. Equal-level seams are 0.0
            // either way, because both sides have the same voxel, so a test
            // that only ever meshed one level would have missed it entirely.
            char nmsg[224];
            snprintf(nmsg, sizeof nmsg,
                     "%s: shared vertices carry normals %.1f deg apart (mean "
                     "%.1f over %d) -- the seam is watertight but will shade "
                     "as a hard edge", cs.name, nworst, nmean, nsh);
            CHECK(nworst < 0.05, nmsg);
        }
        for (const auto& p : c.seam_where)
            printf("          contour at (%.3f, %.3f, %.3f)  %d incident tris\n",
                   p[0], p[1], p[2], int(p[3]));
        for (const auto& p : w.seam_where)
            printf("          welder  at (%.3f, %.3f, %.3f)  %d incident tris\n",
                   p[0], p[1], p[2], int(p[3]));

        char msg[256];
        // THE HARD GATE: no HOLES on the seam. An edge with one triangle is a
        // one-sided surface -- you see through the world at it -- and closing
        // that is the entire claim of the design. Every neighbouring tile is
        // present in this fixture, so there is nothing to excuse.
        snprintf(msg, sizeof msg,
                 "%s: %d SEE-THROUGH holes on the seam (one-triangle edges "
                 "within a voxel of a tile plane), with every neighbouring tile "
                 "present", cs.name, c.holes);
        CHECK(c.holes == 0, msg);

        // THE RESIDUE, named rather than excused. A FOLD is an edge with four
        // incident triangles: two sheets of surface meeting along it. It is not
        // see-through and it is not new -- naive surface nets puts one vertex in
        // a cell that two sheets pass through, so it pinches, and the welder
        // path meshing THIS SAME BLOCK produces folds of its own (printed
        // above; two of them are at coordinates the contour path reports too).
        //
        // What is not yet explained is the handful the contour path adds, all
        // of them clustered on one cave shell grazing the y = 0 plane. The
        // mechanism is understood -- a bridge triangle and an interior quad can
        // both span the same pair of anchor vertices, which doubles that edge --
        // and the condition that would suppress the redundant one needs the
        // tile's own quad boundary, which this rule does not currently track.
        //
        // So this is a REGRESSION gate on a known number, not a claim of zero.
        // It fails if the residue grows, which is what it is for.
        const int kFoldAllowance = 6;
        snprintf(msg, sizeof msg,
                 "%s: %d seam folds (4-incident edges), allowance %d -- the "
                 "known bridge/quad redundancy, tracked not fixed",
                 cs.name, c.folds, kFoldAllowance);
        CHECK(c.folds <= kFoldAllowance, msg);

        // And the whole point of the exercise: at 2:1 the welder path is the
        // thing being replaced, so the contour path must be decisively better
        // than it, not merely comparable.
        if (cs.split) {
            snprintf(msg, sizeof msg,
                     "%s: contour %d seam defects vs the welder path's %d on "
                     "the same block -- the replacement must WIN at 2:1",
                     cs.name, c.seam, w.seam);
            CHECK(c.seam * 4 < w.seam, msg);
        }
        // The interior belongs to surface nets, not to this rule, so the gate
        // is that the rule did not make it worse -- not that it is zero.
        // Away from every plane the two rules emit the SAME triangles, so this
        // is an equality rather than a bound: a difference would mean the
        // border rule reached somewhere it structurally cannot.
        snprintf(msg, sizeof msg,
                 "%s: interior non-manifold count differs between the rules "
                 "(%d contour vs %d welder) -- the border rule cannot reach "
                 "there, so this is an overreach or a mis-classification",
                 cs.name, c.interior, w.interior);
        CHECK(c.interior == w.interior, msg);

        for (int axis = 0; axis < 3; ++axis) {
            const double plane = axis == 1 ? 0.0 : 64.0;
            const int ov = coplanar_overlaps(sc, axis, plane, interior);
            snprintf(msg, sizeof msg,
                     "%s: %d coplanar triangle overlaps on the interior plane "
                     "axis %d -- the ruling of issue 736f92da", cs.name, ov, axis);
            CHECK(ov == 0, msg);
        }
    }

    printf("=== contour mesher in the engine ");
    return check_summary();
}
