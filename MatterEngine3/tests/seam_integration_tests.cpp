// MatterEngine3/tests/seam_integration_tests.cpp — M0-WP6: mesh AND weld, run
// together, measured with the plumb-line probe.
// Design: docs/volumetric-sectors-design-2026-08-10.md §4.1 and §6 (M0).
//
// WHY THIS FILE EXISTS, AND WHAT IT IS THE DECIDING TEST OF.
//
// Cross-level seams used to be closed at BAKE time. `mesh_sector` took an
// `edge_mask` naming the cardinal neighbours the streamer INTENDED to draw one
// level coarser, and on a masked -x/-z face it extended the tile's lattice one
// coarse voxel outward so the fine bridge overshot the coarse tile's last
// vertex. That closed the seam whenever the mask was right, put a neighbour
// guess into the tile's bake identity, and was wrong for the whole duration of
// every split, merge, park and pending bake -- which is when the holes were
// actually reported.
//
// M0-WP1 deleted it. `mesh_sector` is now pure per-tile surface nets and knows
// nothing about anyone; WP2 added the sparse per-face boundary export
// (seam_boundary.h) and WP3 the runtime welder (seam_weld.h) that builds the
// crossing band from the two tiles that are ACTUALLY drawn.
//
// Deleting the mask reopened the one-voxel strip BY CONSTRUCTION, and
// terrain_mesher_tests.cpp was downgraded to say so: its -x/-z blocks are now a
// bounded characterization (measured 0.88-1.12 fine voxels at every level), not
// a gate. The zero-gap gate lives HERE, because here is the only place the
// question "is the seam closed" has a meaningful answer: the mesher does not
// close it and was never going to, and the welder alone has no surfaces to be
// measured against.
//
// THE MEASUREMENT. Vertex lists cannot be compared across a seam -- neither
// side puts a vertex on the border and surface nets places each side's vertices
// from its own cells -- but where a plumb line lands can be. So the probe is the
// proven one from terrain_mesher_tests.cpp: a vertical ray, the highest triangle
// covering the column in XZ projection, barycentric, near-vertical faces
// skipped. The union-coverage row scan is the same one, with exactly ONE change,
// which is the entire point of the stage:
//
//     a column is covered if the coarse mesh, the fine mesh, OR THE WELD BAND
//     covers it.
//
// Since M0-WP7 the weld band has two parts, and every scan reports them
// separately (the SPLIT line): the VERTEX FAN, built from the two tiles' own
// boundary vertices, and the OVERLAP BAND, the fine tile's surface extended one
// coarse voxel back past its -x/-z border, sampled unconditionally at bake time
// and drawn or suppressed at weld time. mesh / +fan / +band / +both are all
// scanned, so "which layer closed what" is measured and not argued.
//
// WHY THE HELPERS ARE DUPLICATED RATHER THAN SHARED. `surface_y_at` and the row
// scan are copied in from terrain_mesher_tests.cpp (~:177-197 and ~:296-411)
// instead of being hoisted into a shared header. Deliberately: over there they
// measure a KNOWN-OPEN strip and are pinned to a characterization bound that is
// expected to keep reading ~1 voxel; over here they gate it at zero. The two
// suites must be free to disagree about the same numbers -- that disagreement is
// the finding -- and a shared helper would invite someone to "fix" one bound by
// editing the other's probe. The probe is also small, total, and has no state.
// (The copy here additionally band-filters the triangle soup into world space
// before probing, because a volumetric cave tile is ~10^5 triangles and the
// original's per-column full-mesh loop would make this suite minutes long.)
//
// CASES, in the order they appear:
//   1. the -x pairing (coarse west / fine east -- the orientation ownership
//      leaves open), levels 0-3, heightfield and volumetric
//   2. the -z mirror, levels 0-2
//   3. the three-tile corner: two fine siblings against one coarse tile
//   4. mid-transition configurations that were UNTESTABLE before the mask went
//   5. no double-emission: the anchor-region partition, derived and proven
//   6. WeldStats for every case, with missing_landing as a fraction of crossings
//  6b. replace-or-supplement: how much double surface the fan adds over the band
//   7. the verdict: how many scans the weld closed, and where the rest are
//
// RESULT AS OF THIS COMMIT, so a reader is not left to run it to find out.
// 16 of 16 welded scans are clean, against a mesh-only baseline gapped in all 16
// (26-31 rows of 31 on the heightfield). It took both layers to get there and
// the SPLIT lines say so:
//
//   The VERTEX FAN alone closes 12 of 16. The four it does not are heightfield
//   single-pair scans, each ONE row of 31, each 0.75-0.88 fine voxels, and each
//   sitting exactly on a row carrying a `missing_landing`. There the coarse
//   tile's boundary cell has no vertex at all -- at 2x the voxel it sees no sign
//   change -- so the fan has nowhere to land and the welder, correctly, invents
//   nothing. That is design §4.1's "honest residue", with one correction the
//   design should absorb: §4.1 predicts the residual defect is SUB-FINE-VOXEL
//   and it is not. It is the same ~1-voxel width as the original strip, confined
//   to one column instead of running the seam's length. A cave field hides it
//   (5x the crossings per plane, so a lost quad is covered by its neighbours); a
//   smooth heightfield at a coarse rung does not.
//
//   The OVERLAP BAND (M0-WP7) closes all 16 on its own, including those four,
//   because it does not need the coarse side to have anything: it is the fine
//   tile's own surface drawn one coarse voxel back over the coarse tile's.
//
// Both are emitted. Coverage alone would say the band makes the fan redundant,
// and that is the wrong conclusion from the right number -- the plumb-line probe
// takes the highest triangle and cannot see how many sheets are stacked under
// it. Case 6b measures the thing coverage is blind to (interpenetration) and
// finds the fan costs nothing the band did not already cost: it interpolates
// between the two tiles' own vertices, so it lies BETWEEN the two sheets the
// band and the coarse mesh are drawing anyway (0.18-0.29 fine voxels of
// fan-vs-band separation against 0.31-0.47 of band-vs-coarse). And the fan is
// the only mechanism on the +x/+z faces and, from M2, on +-y, where the
// asymmetry runs the other way and no band exists.
//
// `missing_landing` is therefore still reported and still non-zero. It is no
// longer the thing that decides whether the seam is closed.

#include "check.h"
#include "../src/seam_weld.h"
#include "../src/terrain_field.h"
#include "../src/terrain_mesher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace terrain_field;
using namespace terrain_mesher;

// ---------------------------------------------------------------------------
// Fixtures. Both copied verbatim from terrain_mesher_tests.cpp so the numbers
// printed here are directly comparable with the characterization printed there.
// ---------------------------------------------------------------------------

static FieldRuntime make(const char* text) {
    FieldProgram p; std::string err;
    if (!FieldProgram::parse(text, p, err)) printf("parse err: %s\n", err.c_str());
    return FieldRuntime(std::move(p));
}

// kNoise (terrain_mesher_tests.cpp:21-23): a heightfield. density = h(x,z) - y.
static const char* kNoise =
    "noise2 1234 0.02 4 0.5 2.0\nconst 20\nmul r0 r1\nconst 0.5\nconst 0.5\n"
    "height r2\nmoisture r3\nrelief r4\nseaLevel -100\nbiome 0.65 0.35\n";

// kNoiseCave (terrain_mesher_tests.cpp:750-763): ridge tunnels under a noise
// surface, floored at -100 so a downward ray always ends in solid. This is the
// one StreamCaverns exercises hardest, and the one where the retired snap did
// real work: terrain_mesher_tests measures only 481/1600 plane samples where the
// raw fine density equals the coarse interpolant, worst delta 1.02. So the weld
// has genuine reconciliation to do here, not a formality.
static const char* kNoiseCave =
    "noise2 1234 0.02 4 0.5 2.0\nconst 20\nmul r0 r1\n"
    "const 30\nadd r2 r3\n"
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

// kNoiseCave with its `const 30` height offset replaced by `const 0`, so the top
// surface runs h = 20*noise ~ [-20, 20] and STRADDLES y = 0 instead of sitting
// at [10, 50]. Same tunnels, same -100 m floor. The vertical cases (8, 9) need a
// tile BOUNDARY that the geometry actually crosses, and every 64 m boundary is a
// multiple of 64; kNoiseCave's surface fits inside one tile and would make a
// vertical seam test pass by having nothing to test.
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
// The probe. Copied from terrain_mesher_tests.cpp:177-197 (`surface_y_at`) and
// :296-411 (the union-coverage row scan). See the header note above for why it
// is duplicated rather than shared.
//
// One addition: triangles are rebased into WORLD space and band-filtered once
// per scan. Tile meshes store x/z tile-local and y world-absolute; weld meshes
// store all three world-absolute (seam_weld.h rebases once, precisely so a band
// spanning two tiles with two origins has an origin of its own). Probing them
// together requires one common space, and the band filter is what keeps a
// 10^5-triangle cave tile from turning the scan into minutes.
// ---------------------------------------------------------------------------

struct Probe {
    std::vector<float> pos;   // world space, 9 floats per triangle

    // `oy` is 0 for every COLUMN-path mesh (their y is already world-absolute)
    // and `ty * S` for a Y-TILED one (M2), whose y is tile-local like its x/z.
    // It is a parameter rather than an assumption because this suite now probes
    // both regimes in the same file.
    void add_soup(const std::vector<float>& P, float ox, float oy, float oz,
                  int axis, float lo, float hi) {
        for (size_t t = 0; t * 9 < P.size(); ++t) {
            const float* p = &P[t * 9];
            float w[3];
            for (int i = 0; i < 3; ++i)
                w[i] = (axis == 0) ? p[i * 3 + 0] + ox : p[i * 3 + 2] + oz;
            const float mn = std::min(w[0], std::min(w[1], w[2]));
            const float mx = std::max(w[0], std::max(w[1], w[2]));
            if (mx < lo || mn > hi) continue;      // cannot cover the scan band
            for (int i = 0; i < 3; ++i) {
                pos.push_back(p[i * 3 + 0] + ox);
                pos.push_back(p[i * 3 + 1] + oy);
                pos.push_back(p[i * 3 + 2] + oz);
            }
        }
    }
    void add(const SectorMesh& m, float ox, float oz, int axis, float lo, float hi) {
        for (const MaterialBucket& b : m.buckets)
            add_soup(b.positions, ox, 0.0f, oz, axis, lo, hi);
    }
    void add_tiled(const SectorMesh& m, float ox, float oy, float oz,
                   int axis, float lo, float hi) {
        for (const MaterialBucket& b : m.buckets)
            add_soup(b.positions, ox, oy, oz, axis, lo, hi);
    }
    void add(const seam::WeldMesh& m, int axis, float lo, float hi) {
        for (const seam::WeldBucket& b : m.buckets)
            add_soup(b.positions, float(m.origin_x), float(m.origin_y),
                     float(m.origin_z), axis, lo, hi);
    }
    // An overlap band on its own (M0-WP7), so the suite can scan mesh+band
    // WITHOUT the vertex fan and answer "does the band replace the fan or
    // supplement it" from measurement instead of from argument. Band positions
    // are world-absolute doubles, so there is no origin to add back.
    void add_band(const seam::OverlapBand& band, int axis, float lo, float hi) {
        for (const seam::OverlapBucket& b : band.buckets) {
            std::vector<float> f(b.positions.size());
            for (size_t i = 0; i < b.positions.size(); ++i) f[i] = float(b.positions[i]);
            add_soup(f, 0.0f, 0.0f, 0.0f, axis, lo, hi);
        }
    }

    // Vertical-ray surface height at WORLD (x, z): the highest triangle covering
    // that column in XZ projection, or -inf if none does. Verbatim from
    // terrain_mesher_tests.cpp:177-197 apart from the flattened storage.
    float y_at(float x, float z) const {
        float best = -1e30f;
        for (size_t t = 0; t * 9 < pos.size(); ++t) {
            const float* p = &pos[t * 9];
            const float x0 = p[0], z0 = p[2];
            const float x1 = p[3], z1 = p[5];
            const float x2 = p[6], z2 = p[8];
            const float det = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2);
            if (std::fabs(det) < 1e-9f) continue;   // vertical: no XZ area
            const float l0 = ((z1 - z2) * (x - x2) + (x2 - x1) * (z - z2)) / det;
            const float l1 = ((z2 - z0) * (x - x2) + (x0 - x2) * (z - z2)) / det;
            const float l2 = 1.0f - l0 - l1;
            const float eps = -1e-4f;
            if (l0 < eps || l1 < eps || l2 < eps) continue;
            best = std::max(best, l0 * p[1] + l1 * p[4] + l2 * p[7]);
        }
        return best;
    }
    size_t tris() const { return pos.size() / 9; }
};

// The union-coverage row scan, from terrain_mesher_tests.cpp:361-378. `axis` 0
// scans wx across the plane with rows over wz; axis 2 is the mirror. Step and
// span are unchanged: v/8 samples over [P0 - 2.5v, P0 + 2v], rows at v spacing.
// A row is "gapped" if it contains an uncovered run longer than a quarter voxel.
// `worst_t` / `worst_u` locate the worst run so a failure is a coordinate, not
// just a number -- "which level, which face, which rows" is what a negative
// result here has to answer.
struct GapStat {
    int rows = 0, gapped = 0;
    float worst_run = 0.0f;
    float worst_t = 0.0f, worst_u = 0.0f;   // row, and the run's END position
};

static GapStat scan(const Probe& pr, int axis, float P0, float v,
                    float t_lo, float t_hi) {
    GapStat g;
    const float step = v / 8.0f;
    for (float t = t_lo; t <= t_hi; t += v) {
        ++g.rows;
        float run = 0.0f, run_max = 0.0f, run_end = 0.0f;
        for (float u = P0 - 2.5f * v; u <= P0 + 2.0f * v; u += step) {
            const float y = (axis == 0) ? pr.y_at(u, t) : pr.y_at(t, u);
            if (y > -1e29f) run = 0.0f;
            else {
                run += step;
                if (run > run_max) { run_max = run; run_end = u; }
            }
        }
        if (run_max > 0.25f * v) ++g.gapped;
        if (run_max > g.worst_run) {
            g.worst_run = run_max; g.worst_t = t; g.worst_u = run_end;
        }
    }
    return g;
}

// THE VERTICAL SCAN (M2). Same probe, same run-length gap rule, same GapStat --
// only the u span changes, and the reason it has to is the whole geometric
// difference between a vertical seam and a horizontal one.
//
// On a VERTICAL plane (normal x or z) the seam is a straight line in the ground
// plane, so `scan` above walks rows parallel to it and samples a narrow +-2.5v
// band across it. On a HORIZONTAL plane (normal y) the seam is wherever the
// SURFACE crosses height Y0 -- a CONTOUR in (x, z), whose position is a property
// of the field, not of the tiling. There is no band to narrow to, so the scan
// walks the fine tile's whole (x, z) footprint and asks the union question over
// all of it:
//
//     every column must be covered by the tile below, the tile above, or the
//     weld -- and away from the contour that is trivially true, so the gapped
//     columns this finds ARE the seam.
//
// Rows are z at v spacing and samples are x at v/8, exactly as `scan` uses; the
// caller keeps the cost down by band-filtering the soup in z (Probe::add's
// existing filter), which matters because a volumetric tile is ~10^5 triangles
// and the probe is O(triangles) per column.
static GapStat scan_columns(const Probe& pr, float x_lo, float x_hi,
                            float z_lo, float z_hi, float v) {
    GapStat g;
    const float step = v / 8.0f;
    for (float t = z_lo; t <= z_hi; t += v) {
        ++g.rows;
        float run = 0.0f, run_max = 0.0f, run_end = 0.0f;
        for (float u = x_lo; u <= x_hi; u += step) {
            const float y = pr.y_at(u, t);
            if (y > -1e29f) run = 0.0f;
            else {
                run += step;
                if (run > run_max) { run_max = run; run_end = u; }
            }
        }
        if (run_max > 0.25f * v) ++g.gapped;
        if (run_max > g.worst_run) {
            g.worst_run = run_max; g.worst_t = t; g.worst_u = run_end;
        }
    }
    return g;
}

// DOUBLE COVERAGE, measured (M0-WP7). Coverage alone cannot decide whether the
// overlap band should REPLACE the vertex fan or supplement it: a vertical ray
// takes the highest triangle and is blind to how many surfaces are stacked under
// it. The cost of drawing both is interpenetration -- two surfaces at the same
// column, at different heights -- so that is what this measures directly.
//
// Two separations, over the same columns the row scan walks:
//   fan_band    : |y(fan) - y(band)| where both cover the column.
//   coarse_band : |y(coarse mesh) - y(band)| where both cover the column.
// The second is the overlap the band creates ON ITS OWN -- it draws the fine
// surface back across ground the coarse tile already covers, which is what the
// retired `reach` did and what "an overlap is all the seam needs" has always
// meant here. The first is the EXTRA interpenetration that keeping the fan adds.
// If it is no larger than the second, keeping the fan introduces no new double
// surface: the fan interpolates between the coarse vertex and the fine one, so
// it lies between the two sheets that are being drawn anyway.
//
// SINGLE-SHEET FIELDS ONLY, and this is a limit of the probe rather than a
// choice about which answer to keep. `y_at` returns the HIGHEST triangle in a
// soup, which is a surface height only where the column carries one sheet.
// kNoise does. kNoiseCave carries the terrain top, a cave roof, a cave floor and
// the -100 m floor, and each of the three partial soups compared here covers a
// different x extent, so "highest triangle of soup A minus highest triangle of
// soup B" is a difference between two DIFFERENT sheets and reads 15-65 voxels
// with no interpenetration behind it. Measured and discarded, not unmeasured:
// the coverage SPLIT above still runs on every scan, volumetric included, and it
// is the coverage claim that the M0 gate rests on.
struct Divergence {
    double fan_band = 0, coarse_band = 0;
    int fan_cols = 0, coarse_cols = 0;
};

static Divergence divergence(const Probe& fan, const Probe& band, const Probe& coarse,
                             int axis, float P0, float v, float t_lo, float t_hi) {
    Divergence d;
    const float step = v / 8.0f;
    for (float t = t_lo; t <= t_hi; t += v)
        for (float u = P0 - 2.5f * v; u <= P0 + 2.0f * v; u += step) {
            const float x = (axis == 0) ? u : t, z = (axis == 0) ? t : u;
            const float yb = band.y_at(x, z);
            if (yb < -1e29f) continue;
            const float yf = fan.y_at(x, z);
            const float yc = coarse.y_at(x, z);
            if (yf > -1e29f) {
                ++d.fan_cols;
                d.fan_band = std::max(d.fan_band, double(std::fabs(yf - yb)));
            }
            if (yc > -1e29f) {
                ++d.coarse_cols;
                d.coarse_band = std::max(d.coarse_band, double(std::fabs(yc - yb)));
            }
        }
    return d;
}

// Running totals of the above, for the WP7 verdict block at the end.
struct DivTotals { double worst_fan = 0, worst_coarse = 0; int scans = 0, worse = 0; };
static DivTotals g_div;

static void report_divergence(const Divergence& d, float v) {
    ++g_div.scans;
    if (d.fan_band > d.coarse_band + 1e-4) ++g_div.worse;
    g_div.worst_fan = std::max(g_div.worst_fan, d.fan_band / double(v));
    g_div.worst_coarse = std::max(g_div.worst_coarse, d.coarse_band / double(v));
    printf("      DOUBLE COVER: band vs fan %.3f m (%.2f v) over %d columns |"
           " band vs coarse mesh %.3f m (%.2f v) over %d columns\n",
           d.fan_band, d.fan_band / v, d.fan_cols,
           d.coarse_band, d.coarse_band / v, d.coarse_cols);
}

// Every WELDED scan is logged so the suite can end with a verdict rather than a
// pile of individual failures: "restored in N of M configurations" is the answer
// M0-WP6 was commissioned to produce, and it has to survive being read months
// later by someone who does not want to reconstruct it from FAIL lines.
struct ScanLog {
    int scans = 0, clean = 0;
    std::vector<std::string> residual;
};
static ScanLog g_scan;

static void log_scan(const char* what, const GapStat& g, float v) {
    ++g_scan.scans;
    if (!g.gapped) { ++g_scan.clean; return; }
    char buf[256];
    snprintf(buf, sizeof buf,
             "%s: %d/%d rows, worst %.3f m = %.2f fine voxels at %.0f",
             what, g.gapped, g.rows, g.worst_run, g.worst_run / v, g.worst_t);
    g_scan.residual.push_back(buf);
}

// One line naming where the worst uncovered run is, in world coordinates and in
// fine voxels off the plane. Printed only when a scan is not clean, so a green
// run stays quiet and a red one is already localized.
static void report_worst(const char* tag, const GapStat& g, int axis, float P0,
                         float v) {
    if (!g.gapped) return;
    printf("      %s WORST ROW: %s = %.3f, uncovered %.3f m (%.2f v) ending at"
           " %s = %.3f, i.e. plane %+.2f v\n",
           tag, axis == 0 ? "z" : "x", g.worst_t, g.worst_run, g.worst_run / v,
           axis == 0 ? "x" : "z", g.worst_u, (g.worst_u - P0) / v);
}

// ---------------------------------------------------------------------------
// Weld plumbing: sides, regions, accounting
// ---------------------------------------------------------------------------

// A WeldSide backed by ANY number of FaceRecords, which is what the engine's
// real lookup is (it spans the whole sector map). `find` returns &verts[i] into
// the record's own storage, so the pointers are stable and canonical -- the
// contract seam_weld.h spells out, and the reason the 2:1 collapse is detectable
// by pointer equality at all. The vector is captured BY VALUE into the closure;
// the FaceRecords it points at must outlive the weld call.
//
// `band` (M0-WP7) is the OVERLAP band `weld_face` emits verbatim when this side
// turns out to be the fine one of a cross-level pair. It is passed separately
// from `recs` on purpose: `recs` is a LOOKUP and may span the whole map, while a
// band is one tile's finished border geometry. A side spanning two siblings has
// two bands and no way to choose between them from inside the welder, so the
// choice is made out here, per pass, by the same rule that partitions the
// anchors (see weld_pair). Defaults to null, which is exactly the pre-band
// welder.
static seam::WeldSide side_of(int rung, std::vector<const seam::FaceRecord*> recs,
                              const seam::OverlapBand* band = nullptr) {
    seam::WeldSide s;
    s.rung = rung;
    s.band = band;
    s.at = [recs](int64_t a, int64_t b) -> const seam::BoundaryVert* {
        for (const seam::FaceRecord* r : recs)
            if (const seam::BoundaryVert* v = r->find(a, b)) return v;
        return nullptr;
    };
    return s;
}

// The box of GLOBAL fine cell indices a tile owns on one face.
//
//   A TILED axis's extent comes from the tile identity, not from the record: the
//   mesher's export maps local cell c in [1..n] to global t*n + c - 1, so the
//   tile owns exactly n cells and the box is known whether or not every cell
//   produced a vertex.
//
//   The Y axis is UNTILED on the COLUMN path -- one tile spans the whole
//   authored slab -- so there is no identity to read it from and its extent
//   comes from the record. M2's Y-TILED path (SectorBoundary::y_tiled) folds Y
//   into the first case, and then a +-y face's BOTH tangential axes are x and z,
//   i.e. both from identity. That collapse is the point of the stage, so it is
//   written as one rule over the axis rather than as an extra branch.
static bool face_cell_box(const seam::SectorBoundary& sb, int face,
                          int64_t& a_lo, int64_t& a_hi,
                          int64_t& b_lo, int64_t& b_hi) {
    const seam::FaceRecord& fr = sb.faces[face];
    if (fr.verts.empty()) return false;
    const int64_t n = sb.cells;
    int a_axis = 0, b_axis = 0;
    seam::face_tangent_axes(face, a_axis, b_axis);
    const auto axis_box = [&](int axis, bool is_a, int64_t& lo, int64_t& hi) {
        if (axis == 1 && !sb.y_tiled) {           // untiled Y: read the record
            bool first = true;
            for (const seam::BoundaryVert& v : fr.verts) {
                const int64_t y = is_a ? v.a : v.b;
                if (first) { lo = hi = y; first = false; }
                lo = std::min(lo, y);
                hi = std::max(hi, y);
            }
        } else {                                   // tiled: read the identity
            const int64_t t = axis == 0 ? sb.tx : (axis == 1 ? sb.ty : sb.tz);
            lo = t * n;
            hi = lo + n - 1;
        }
    };
    axis_box(a_axis, true,  a_lo, a_hi);
    axis_box(b_axis, false, b_lo, b_hi);
    return true;
}

// ---------------------------------------------------------------------------
// Where the missing landings ARE
// ---------------------------------------------------------------------------
//
// `WeldStats::missing_landing` is one number for two different situations, and a
// zero-gap failure cannot be judged without telling them apart -- R4 of
// docs/volumetric-sectors-m0-resolutions.md makes exactly this point about
// records-missing ("the absence of a weld is not by itself evidence of a welder
// bug"), and the same trap is one level down inside the counter:
//
//   FIXTURE EDGE. The region reaches one fine cell lower on each axis (see
//     weld_pair), into a tile this fixture simply does not contain. In the
//     engine that cell belongs to a resident sibling and the multi-tile lookup
//     answers it; here it is null. Not a defect, and not the design's residue
//     either -- an artifact of welding a lone pair. The corner fixture, which
//     has the sibling, is where this term goes to zero.
//
//   COARSE-SIDE TOPOLOGY. The genuine §4.1 residue: a fine-only sign change
//     whose covering coarse cell produced no vertex. The two resolutions
//     disagree about whether there is a surface there at all, so there is no
//     landing site and nothing honest to emit.
//
// This walks the same region with the same sign rules as seam_weld.cpp and
// attributes each missing landing. It duplicates ~20 lines of the welder on
// purpose: it is a diagnostic ABOUT the welder, and one that re-derived nothing
// would only be able to repeat the welder's own opinion of itself.
//   COARSE-SIDE EXTENT. Split out of the above because it would otherwise be
//     mistaken for it: the coarse cell is outside the populated (a,b) box of the
//     coarse record altogether -- the coarse boundary layer has no surface
//     anywhere near that height, rather than merely missing this one cell. On an x-plane `a` is the global Y cell, and the two
//     tiles choose their Y slabs independently from their own sampled h_min/
//     h_max (terrain_mesher.cpp:193-201) over DIFFERENT footprints -- so their
//     slabs can end a voxel apart. That is a slab-alignment edge, not a topology
//     disagreement, and conflating the two would misattribute the residue.
//
// This walks the same region with the same sign rules as seam_weld.cpp and
// attributes each missing landing. It duplicates ~20 lines of the welder on
// purpose: it is a diagnostic ABOUT the welder, and one that re-derived nothing
// would only be able to repeat the welder's own opinion of itself.
struct MissDiag {
    int total = 0, fixture_edge = 0, coarse_topology = 0, coarse_extent = 0,
        fine_topology = 0;
    std::vector<double> rows;   // world tangential coord of each missing landing
};

static MissDiag diagnose_missing(int face_axis,
                                 const seam::WeldSide& neg, const seam::WeldSide& pos,
                                 int64_t a0, int64_t a1, int64_t b0, int64_t b1,
                                 int64_t cell_a_lo, int64_t cell_a_hi,
                                 int64_t cell_b_lo, int64_t cell_b_hi,
                                 const seam::FaceRecord* coarse_rec) {
    MissDiag d;
    const bool neg_is_fine = neg.rung > pos.rung;
    const seam::WeldSide& fine = neg_is_fine ? neg : pos;
    const double vf = seam::rung_voxel(fine.rung);
    // The coarse record's own (a, b) box, for the extent-vs-topology split.
    int64_t ca_lo = 0, ca_hi = -1, cb_lo = 0, cb_hi = -1;
    if (coarse_rec && !coarse_rec->verts.empty()) {
        ca_lo = ca_hi = coarse_rec->verts[0].a;
        cb_lo = cb_hi = coarse_rec->verts[0].b;
        for (const seam::BoundaryVert& v : coarse_rec->verts) {
            ca_lo = std::min(ca_lo, v.a); ca_hi = std::max(ca_hi, v.a);
            cb_lo = std::min(cb_lo, v.b); cb_hi = std::max(cb_hi, v.b);
        }
    }
    const auto resolve = [](const seam::WeldSide& s, bool is_fine,
                            int64_t a, int64_t b) -> const seam::BoundaryVert* {
        return is_fine ? s.at(a, b) : s.at(seam::floor_div2(a), seam::floor_div2(b));
    };
    for (int64_t A = a0; A < a1; ++A)
        for (int64_t B = b0; B < b1; ++B)
            for (int dir = 0; dir < 2; ++dir) {
                const bool along_a = (dir == 0);
                int64_t al, bl, ah, bh;
                if (along_a) { al = A;     bl = B - 1; ah = A; bh = B; }
                else         { al = A - 1; bl = B;     ah = A; bh = B; }
                const int bit_hi_hi = along_a ? 1 : 2;
                const int bit_lo_lo = along_a ? 2 : 1;
                const seam::BoundaryVert* f_hi = fine.at(ah, bh);
                const seam::BoundaryVert* f_lo = fine.at(al, bl);
                int s0 = -1, s1 = -1;
                if (f_hi) {
                    s0 = (f_hi->corner_signs >> 0) & 1;
                    s1 = (f_hi->corner_signs >> bit_hi_hi) & 1;
                }
                if (f_lo && s0 < 0) {
                    s0 = (f_lo->corner_signs >> bit_lo_lo) & 1;
                    s1 = (f_lo->corner_signs >> 3) & 1;
                }
                if (s0 < 0 || s0 == s1) continue;
                const seam::BoundaryVert* q[4] = {
                    resolve(neg, neg_is_fine,  al, bl),
                    resolve(neg, neg_is_fine,  ah, bh),
                    resolve(pos, !neg_is_fine, ah, bh),
                    resolve(pos, !neg_is_fine, al, bl),
                };
                if (q[0] && q[1] && q[2] && q[3]) continue;
                // A crossing with both FINE corners present and exactly ONE
                // coarse corner absent is not a missing landing: the welder
                // caps it, duplicating the present coarse corner into the
                // absent slot (§4.1's "collapsed cap"). That is the same
                // degeneration the 2:1 fan already performs when floor_div2
                // aliases two fine cells onto one coarse cell, reached by a
                // different route, so it lands in `tris` and never in
                // `missing_landing`.
                //
                // This walk must mirror the welder's precedence exactly or the
                // attribution stops summing to `missing`. It predates the cap
                // and counted every null-cornered crossing, which over-counted
                // by the 24 the cap recovers.
                {
                    const int fi0 = neg_is_fine ? 0 : 2, fi1 = neg_is_fine ? 1 : 3;
                    const int ci0 = neg_is_fine ? 2 : 0, ci1 = neg_is_fine ? 3 : 1;
                    if (q[fi0] && q[fi1] && (!q[ci0] != !q[ci1])) continue;
                }
                ++d.total;
                // The scan row this landing sits on, in world units: on an
                // x-plane the rows are z (tangential index b), on a z-plane x.
                d.rows.push_back(double((face_axis == 0) ? bh : ah) * vf);
                // Attribute ONE cause per missing landing, so the buckets sum to
                // `total`. FINE-side nulls are looked at first: if the fine cell
                // is absent the crossing has no owner at all in this fixture,
                // and the coarse side's opinion of the same cell is a
                // consequence, not an independent cause. (Checking in q order
                // instead reports every low-boundary edge as "coarse extent",
                // because on a coarse-neg pairing q[0] is the coarse corner.)
                bool done = false;
                for (int pass = 0; pass < 2 && !done; ++pass)
                for (int i = 0; i < 4 && !done; ++i) {
                    if (q[i]) continue;
                    const bool on_neg  = (i == 0 || i == 1);
                    const bool on_fine = (on_neg == neg_is_fine);
                    if (on_fine != (pass == 0)) continue;
                    done = true;
                    const int64_t ca = (i == 0 || i == 3) ? al : ah;
                    const int64_t cb = (i == 0 || i == 3) ? bl : bh;
                    if (!on_fine) {
                        const int64_t ka = seam::floor_div2(ca), kb = seam::floor_div2(cb);
                        if (ca_hi >= ca_lo &&
                            (ka < ca_lo || ka > ca_hi || kb < cb_lo || kb > cb_hi))
                            ++d.coarse_extent;
                        else
                            ++d.coarse_topology;
                    }
                    else if (ca < cell_a_lo || ca > cell_a_hi ||
                             cb < cell_b_lo || cb > cell_b_hi) ++d.fixture_edge;
                    else                                      ++d.fine_topology;
                    break;
                }
            }
    return d;
}

// Running totals across every weld this suite performs, for the §4.1 residue
// report at the end.
struct Totals {
    long long crossings = 0, quads = 0, tris = 0, missing = 0, degen = 0,
              conflicts = 0, welds = 0, emitted = 0;
    long long miss_fixture = 0, miss_coarse = 0, miss_extent = 0, miss_fine = 0;
    // [8] shading: geometric normal vs the corners' averaged stored normals.
    long long wind_checked = 0, wind_wrong = 0;
    double    wind_worst = 1.0;
    // The same three over the band-less welds the suite already builds, which
    // is what splits a fan defect from a band defect.
    long long fan_checked = 0, fan_wrong = 0;
    double    fan_worst = 1.0;
    // Severity split. A fan triangle legitimately bridges two surfaces half a
    // coarse voxel apart, so one that is nearly perpendicular to both endpoint
    // normals has a dot whose SIGN is noise -- counting it as "backwards" would
    // make this gate an opinion about steepness. `severe` is dot < -0.5, which
    // no bridging triangle reaches and only an actual reversal does.
    long long wind_severe = 0, fan_severe = 0;
};
static Totals g_tot;

// Every triangle's geometric normal (from its winding) must agree with the mean
// of its three corners' stored normals -- the same check seam_weld_tests.cpp [4]
// applies to the fan.
//
// WHY IT IS REPEATED HERE. That one runs on `make_pairing`, a synthetic pairing
// built from an analytic density, and a synthetic pairing has no `band`: it
// tests the FAN's `reverse_frame` table and nothing else. The OVERLAP BAND
// (M0-WP7) reaches the welder already triangulated by the mesher and is copied
// verbatim -- "the welder does not reconstruct, filter or re-wind a band" -- so
// its winding has never been checked against anything, and it is the bulk of
// the emitted area. This suite welds REAL mesher records with real bands, which
// makes it the only place the band's own orientation is testable.
//
// Slivers with no defined plane, and corners whose normals cancel, are skipped
// rather than guessed at.
static void account_winding(const seam::WeldMesh& m, bool fan_only = false) {
    long long* checked = fan_only ? &g_tot.fan_checked : &g_tot.wind_checked;
    long long* wrong   = fan_only ? &g_tot.fan_wrong   : &g_tot.wind_wrong;
    double*    worst   = fan_only ? &g_tot.fan_worst   : &g_tot.wind_worst;
    for (const seam::WeldBucket& b : m.buckets)
        for (size_t t = 0; t * 9 < b.positions.size(); ++t) {
            const float* P = &b.positions[t * 9];
            const float* N = &b.normals[t * 9];
            const double e1[3] = {P[3]-P[0], P[4]-P[1], P[5]-P[2]};
            const double e2[3] = {P[6]-P[0], P[7]-P[1], P[8]-P[2]};
            const double g[3] = {e1[1]*e2[2] - e1[2]*e2[1],
                                 e1[2]*e2[0] - e1[0]*e2[2],
                                 e1[0]*e2[1] - e1[1]*e2[0]};
            const double gl = std::sqrt(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
            if (gl < 1e-12) continue;
            double mn[3] = {0, 0, 0};
            for (int v = 0; v < 3; ++v)
                for (int c = 0; c < 3; ++c) mn[c] += N[v*3 + c];
            const double ml = std::sqrt(mn[0]*mn[0] + mn[1]*mn[1] + mn[2]*mn[2]);
            if (ml < 1e-6) continue;
            const double dot = (g[0]*mn[0] + g[1]*mn[1] + g[2]*mn[2]) / (gl * ml);
            ++*checked;
            if (dot <= 0) ++*wrong;
            if (dot < -0.5) ++(fan_only ? g_tot.fan_severe : g_tot.wind_severe);
            *worst = std::min(*worst, dot);
        }
}

static void account(const seam::WeldStats& s, const seam::WeldMesh& m) {
    g_tot.crossings += s.crossings; g_tot.quads += s.quads; g_tot.tris += s.tris;
    g_tot.missing += s.missing_landing; g_tot.degen += s.degenerate;
    g_tot.conflicts += s.sign_conflicts;
    g_tot.emitted += (long long)m.triangle_count();
    ++g_tot.welds;
    account_winding(m);
}

static void print_stats(const char* tag, const seam::WeldStats& s, size_t tris) {
    printf("      %-14s crossings %5d = quads %5d + tris %5d + missing %4d + degen %d"
           " | conflicts %d | %5zu emitted tris (%d band) | missing %.2f%% of"
           " crossings\n",
           tag, s.crossings, s.quads, s.tris, s.missing_landing, s.degenerate,
           s.sign_conflicts, tris, s.band_tris,
           s.crossings ? 100.0 * double(s.missing_landing) / double(s.crossings) : 0.0);
}

// Print the attribution and fold it into the running totals. Silent when there
// is nothing to attribute, so a clean weld stays a one-line report.
static void print_missing(const char* tag, const MissDiag& d) {
    g_tot.miss_fixture += d.fixture_edge;
    g_tot.miss_coarse  += d.coarse_topology;
    g_tot.miss_extent  += d.coarse_extent;
    g_tot.miss_fine    += d.fine_topology;
    CHECK(d.fine_topology == 0,
          "a crossing edge always has BOTH its fine cells in the record -- the "
          "sparse-signs argument in seam_boundary.h -- so a fine-side null is "
          "never a topology case");
    if (!d.total) return;
    printf("      %-14s missing %d = %d fixture edge + %d COARSE TOPOLOGY (the "
           "4.1 residue) + %d coarse slab extent + %d fine-side; rows at",
           tag, d.total, d.fixture_edge, d.coarse_topology, d.coarse_extent,
           d.fine_topology);
    std::vector<double> r = d.rows;
    std::sort(r.begin(), r.end());
    r.erase(std::unique(r.begin(), r.end()), r.end());
    for (size_t i = 0; i < r.size() && i < 8; ++i) printf(" %.0f", r[i]);
    if (r.size() > 8) printf(" ...(%zu rows)", r.size());
    printf("\n");
}

// THE ANCHOR REGION FOR ONE TILE. One rule, used by every case in this file --
// including the two-sibling partition of case 5, which is this rule applied
// twice.
//
//     region = [a_lo, a_hi + 1) x [b_lo, b_hi + 1)
//
// over the tile's own face cells [a_lo,a_hi] x [b_lo,b_hi]. Equivalently:
// ANCHOR (A,B) BELONGS TO THE TILE THAT OWNS FINE CELL (A,B).
//
// This was got wrong once here, and the wrong version is instructive because it
// looks more careful. `weld_face`'s doc says the cells touched run one lower,
// a in [a0-1, a1), so "cover exactly my own cells and no one else's" reads as
// [a_lo+1, a_hi+1) x [b_lo+1, b_hi+1). That is a real region and it silently
// loses geometry, because THE TWO EDGE DIRECTIONS DO NOT REACH THE SAME WAY:
//
//   b-edge at (A,B) joins cells (A-1,B) and (A,B) -- reaches one lower in a.
//   a-edge at (A,B) joins cells (A,B-1) and (A,B) -- reaches one lower in b,
//                                                    and lives ENTIRELY inside
//                                                    cell row A.
//
// So "cells touched = [a_lo, a_hi]" is true of the b-edges and false of the
// a-edges: dropping anchor row A = a_lo drops every a-edge in cell row a_lo,
// which on an x-plane is a whole row of the vertical (y-direction) crossings
// that carry the terrain surface. The suite's first run showed it as 3-5 gapped
// rows per level with missing_landing == 0 -- geometry that was never asked for
// rather than geometry that had nowhere to land, which is exactly why the
// counter could not see it. The corner fixture, which had the rule right from
// the start, was 0/63 on the same pairing; that disagreement is what located it.
//
// The rule above has neither problem: every edge of either direction has (A,B)
// among its two cells, so naming the anchor by that cell covers each edge once
// and only once. It reaches one cell lower on each axis into the NEIGHBOURING
// tile, which is why the fine side must be a multi-tile lookup (side_of takes a
// list) rather than one record -- the engine's real lookup spans the sector map
// for precisely this reason.
static bool weld_pair(int face_axis, const seam::WeldSide& neg,
                      const seam::WeldSide& pos,
                      const seam::SectorBoundary& fine_sb, int fine_face,
                      seam::WeldMesh& wm, seam::WeldStats& ws, std::string& err) {
    int64_t a_lo, a_hi, b_lo, b_hi;
    if (!face_cell_box(fine_sb, fine_face, a_lo, a_hi, b_lo, b_hi)) {
        err = "fine face record is empty";
        return false;
    }
    return seam::weld_face(face_axis, neg, pos, a_lo, a_hi + 1,
                           b_lo, b_hi + 1, wm, ws, err);
}

// The same region weld_pair uses, walked for attribution (see MissDiag). The
// owned-cell box is the single fine tile's, so anything the region reaches
// outside it is the absent-neighbour artifact.
static MissDiag diag_for(int face_axis, const seam::WeldSide& neg,
                         const seam::WeldSide& pos,
                         const seam::SectorBoundary& fine_sb, int fine_face,
                         const seam::FaceRecord* coarse_rec) {
    int64_t a_lo, a_hi, b_lo, b_hi;
    if (!face_cell_box(fine_sb, fine_face, a_lo, a_hi, b_lo, b_hi)) return MissDiag{};
    return diagnose_missing(face_axis, neg, pos, a_lo, a_hi + 1, b_lo, b_hi + 1,
                            a_lo, a_hi, b_lo, b_hi, coarse_rec);
}

// ---------------------------------------------------------------------------
// Case 1 / 2: the two orientations ownership leaves open
// ---------------------------------------------------------------------------
//
// Mirrors terrain_mesher_tests.cpp:341-429 exactly -- same tiles, same sizes,
// same probe span, same rows -- and adds the weld to the union. The "mesh only"
// row it prints alongside is the characterization that suite gates at <= 1.5
// fine voxels; the "mesh + weld" row is the gate this suite exists for.

static void run_neg_x(const FieldRuntime& f, const char* field_name,
                      float y_min, float y_max, int max_level,
                      bool one_sheet) {
    printf("  [1] -x pairing (coarse WEST of fine -- the orientation the [1..n]"
           " ownership rule leaves open), field %s\n", field_name);
    for (int L = 0; L <= max_level; ++L) {
        const float SL = 64.0f * float(1 << L);
        const float v  = SL / 32.0f;          // the FINE voxel
        const float X0 = 2.0f * SL;           // the shared plane
        SectorMesh cm, fm;
        seam::SectorBoundary cb, fb;
        std::string err;
        // coarse tile 0 at 2*SL, rung -(L+1): x,z in [0, 2 SL)
        // fine   tile 2 at SL,     rung -L:   x in [2 SL, 3 SL), z in [0, SL)
        CHECK(mesh_sector(f, 0, 0, -(L + 1), 2.0f * SL, y_min, y_max, cm, &cb, err),
              err.c_str());
        CHECK(mesh_sector(f, 2, 0, -L, SL, y_min, y_max, fm, &fb, err), err.c_str());

        // neg = the side BELOW the plane (the coarse tile's +x face);
        // pos = the side above it (the fine tile's -x face), and the side that
        // carries the overlap band -- bands live on -x/-z faces only, and the
        // fine tile is always the one on the plane's +side here.
        const seam::OverlapBand& band = fb.faces[seam::kFaceNegX].band;
        seam::WeldSide neg = side_of(-(L + 1), {&cb.faces[seam::kFacePosX]});
        seam::WeldSide pos = side_of(-L,       {&fb.faces[seam::kFaceNegX]}, &band);
        seam::WeldMesh wm; seam::WeldStats ws;
        CHECK(weld_pair(0, neg, pos, fb, seam::kFaceNegX, wm, ws, err), err.c_str());
        CHECK(ws.crossings > 0, "-x weld: the shared plane carries crossings");
        CHECK(ws.sign_conflicts == 0,
              "-x weld: the two tiles agree about every shared plane sample");
        CHECK(ws.crossings == ws.accounted(), "-x weld: every crossing accounted");
        CHECK(ws.band_tris == int(band.triangle_count()),
              "-x weld: the band is emitted VERBATIM -- every triangle, none "
              "added, none filtered");
        account(ws, wm);

        // The same weld with the band withheld. Not a second configuration --
        // the same two tiles -- purely so the scan below can separate what the
        // vertex fan closes from what the band closes. See the four-way print.
        seam::WeldSide pos_nb = side_of(-L, {&fb.faces[seam::kFaceNegX]});
        seam::WeldMesh wf; seam::WeldStats wsf;
        CHECK(weld_pair(0, neg, pos_nb, fb, seam::kFaceNegX, wf, wsf, err),
              err.c_str());
        account_winding(wf, /*fan_only=*/true);
        CHECK(wsf.band_tris == 0 && wsf.crossings == ws.crossings &&
              wsf.quads == ws.quads && wsf.tris == ws.tris,
              "-x weld: the band changes NOTHING about the fan -- same crossings, "
              "same quads, same tris, with and without it");

        const float lo = X0 - 3.0f * v, hi = X0 + 3.0f * v;
        Probe mesh_only, fan_only, band_only, welded;
        mesh_only.add(cm, 0.0f, 0.0f, 0, lo, hi);
        mesh_only.add(fm, X0,   0.0f, 0, lo, hi);
        fan_only = mesh_only;  fan_only.add(wf, 0, lo, hi);
        band_only = mesh_only; band_only.add_band(band, 0, lo, hi);
        welded = mesh_only;    welded.add(wm, 0, lo, hi);

        const GapStat gm = scan(mesh_only, 0, X0, v, v, SL - v);
        const GapStat gf = scan(fan_only,  0, X0, v, v, SL - v);
        const GapStat gb = scan(band_only, 0, X0, v, v, SL - v);
        const GapStat gw = scan(welded,    0, X0, v, v, SL - v);
        printf("    L%d/%d (voxel %.2f/%.2f m): mesh only %d/%d rows gapped, worst"
               " %.3f m (%.2f v) | mesh+weld %d/%d rows gapped, worst %.3f m (%.2f v)\n",
               L, L + 1, v, 2 * v, gm.gapped, gm.rows, gm.worst_run, gm.worst_run / v,
               gw.gapped, gw.rows, gw.worst_run, gw.worst_run / v);
        printf("      SPLIT: mesh %d gapped | +fan only %d | +band only %d |"
               " +both %d  (of %d rows; band %zu tris)\n",
               gm.gapped, gf.gapped, gb.gapped, gw.gapped, gm.rows,
               band.triangle_count());
        if (one_sheet) {
            Probe p_fan, p_band, p_coarse;
            p_fan.add(wf, 0, lo, hi);
            p_band.add_band(band, 0, lo, hi);
            p_coarse.add(cm, 0.0f, 0.0f, 0, lo, hi);
            report_divergence(divergence(p_fan, p_band, p_coarse, 0, X0, v,
                                         v, SL - v), v);
        }
        print_stats("-x", ws, wm.triangle_count());
        print_missing("-x", diag_for(0, neg, pos, fb, seam::kFaceNegX,
                                     &cb.faces[seam::kFacePosX]));
        report_worst("-x", gw, 0, X0, v);
        { char t[64]; snprintf(t, sizeof t, "%s -x L%d/%d", field_name, L, L + 1);
          log_scan(t, gw, v); }
        // The gate. Not a threshold to tune: if this fails the welder did not
        // close what deleting the mask opened, and that is the finding.
        CHECK(gw.gapped == 0,
              "-x cross-level seam: every border row is covered by the coarse "
              "mesh, the fine mesh, or the weld band");
    }
}

static void run_neg_z(const FieldRuntime& f, const char* field_name,
                      float y_min, float y_max, int max_level,
                      bool one_sheet) {
    printf("  [2] -z mirror (coarse SOUTH of fine), field %s\n", field_name);
    for (int L = 0; L <= max_level; ++L) {
        const float SL = 64.0f * float(1 << L);
        const float v  = SL / 32.0f;
        const float Z0 = 2.0f * SL;
        SectorMesh cm, fm;
        seam::SectorBoundary cb, fb;
        std::string err;
        CHECK(mesh_sector(f, 0, 0, -(L + 1), 2.0f * SL, y_min, y_max, cm, &cb, err),
              err.c_str());
        CHECK(mesh_sector(f, 0, 2, -L, SL, y_min, y_max, fm, &fb, err), err.c_str());

        const seam::OverlapBand& band = fb.faces[seam::kFaceNegZ].band;
        seam::WeldSide neg = side_of(-(L + 1), {&cb.faces[seam::kFacePosZ]});
        seam::WeldSide pos = side_of(-L,       {&fb.faces[seam::kFaceNegZ]}, &band);
        seam::WeldMesh wm; seam::WeldStats ws;
        CHECK(weld_pair(2, neg, pos, fb, seam::kFaceNegZ, wm, ws, err), err.c_str());
        CHECK(ws.crossings > 0, "-z weld: the shared plane carries crossings");
        CHECK(ws.sign_conflicts == 0, "-z weld: no sign conflicts");
        CHECK(ws.crossings == ws.accounted(), "-z weld: every crossing accounted");
        CHECK(ws.band_tris == int(band.triangle_count()),
              "-z weld: the band is emitted verbatim");
        account(ws, wm);

        seam::WeldSide pos_nb = side_of(-L, {&fb.faces[seam::kFaceNegZ]});
        seam::WeldMesh wf; seam::WeldStats wsf;
        CHECK(weld_pair(2, neg, pos_nb, fb, seam::kFaceNegZ, wf, wsf, err),
              err.c_str());
        account_winding(wf, /*fan_only=*/true);
        CHECK(wsf.band_tris == 0 && wsf.crossings == ws.crossings &&
              wsf.quads == ws.quads && wsf.tris == ws.tris,
              "-z weld: the band changes nothing about the fan");

        const float lo = Z0 - 3.0f * v, hi = Z0 + 3.0f * v;
        Probe mesh_only, fan_only, band_only, welded;
        mesh_only.add(cm, 0.0f, 0.0f, 2, lo, hi);
        mesh_only.add(fm, 0.0f, Z0,   2, lo, hi);
        fan_only = mesh_only;  fan_only.add(wf, 2, lo, hi);
        band_only = mesh_only; band_only.add_band(band, 2, lo, hi);
        welded = mesh_only;    welded.add(wm, 2, lo, hi);

        const GapStat gm = scan(mesh_only, 2, Z0, v, v, SL - v);
        const GapStat gf = scan(fan_only,  2, Z0, v, v, SL - v);
        const GapStat gb = scan(band_only, 2, Z0, v, v, SL - v);
        const GapStat gw = scan(welded,    2, Z0, v, v, SL - v);
        printf("    L%d/%d (voxel %.2f/%.2f m): mesh only %d/%d rows gapped, worst"
               " %.3f m (%.2f v) | mesh+weld %d/%d rows gapped, worst %.3f m (%.2f v)\n",
               L, L + 1, v, 2 * v, gm.gapped, gm.rows, gm.worst_run, gm.worst_run / v,
               gw.gapped, gw.rows, gw.worst_run, gw.worst_run / v);
        printf("      SPLIT: mesh %d gapped | +fan only %d | +band only %d |"
               " +both %d  (of %d rows; band %zu tris)\n",
               gm.gapped, gf.gapped, gb.gapped, gw.gapped, gm.rows,
               band.triangle_count());
        if (one_sheet) {
            Probe p_fan, p_band, p_coarse;
            p_fan.add(wf, 2, lo, hi);
            p_band.add_band(band, 2, lo, hi);
            p_coarse.add(cm, 0.0f, 0.0f, 2, lo, hi);
            report_divergence(divergence(p_fan, p_band, p_coarse, 2, Z0, v,
                                         v, SL - v), v);
        }
        print_stats("-z", ws, wm.triangle_count());
        print_missing("-z", diag_for(2, neg, pos, fb, seam::kFaceNegZ,
                                     &cb.faces[seam::kFacePosZ]));
        report_worst("-z", gw, 2, Z0, v);
        { char t[64]; snprintf(t, sizeof t, "%s -z L%d/%d", field_name, L, L + 1);
          log_scan(t, gw, v); }
        CHECK(gw.gapped == 0,
              "-z cross-level seam: every border row is covered by the coarse "
              "mesh, the fine mesh, or the weld band");
    }
}

// ---------------------------------------------------------------------------
// Cases 3 + 5: the three-tile corner, and the anchor-region partition
// ---------------------------------------------------------------------------
//
// THE REGION PARTITION, DERIVED. `weld_face`'s region_a0..b1 bound the ANCHOR
// lattice points, and the cells reach one lower: an a-edge anchored at (A,B)
// joins cells (A,B-1) and (A,B); a b-edge anchored at (A,B) joins (A-1,B) and
// (A,B). So every edge, of either direction, has (A,B) among its two cells --
// call it the anchor's PRIMARY cell -- and the other cell is one lower on
// exactly one axis.
//
//   RULE: anchor (A,B) belongs to the pass for the fine tile that owns fine
//   cell (A,B). For a tile owning cells a in [a_lo,a_hi], b in [b_lo,b_hi] that
//   is region [a_lo, a_hi+1) x [b_lo, b_hi+1).
//
// DISJOINT because fine cell ownership is a partition and each anchor names one
// cell. COMPLETE because every anchor whose primary cell is owned by some drawn
// tile runs in exactly that tile's pass. And it is why the fine side's lookup
// MUST span tiles: each pass reaches one cell lower on each axis, into the
// neighbouring sibling, and a single-record side would answer null there and
// bank the edge as missing_landing -- a hole exactly on the sibling-sibling
// border, which is the corner leak this case is looking for.
//
// (The alternative partition -- give the tile the anchors whose LOWER cell it
// owns, [a_lo+1, a_hi+2) -- is equally disjoint and equally complete. The one
// above is chosen because "the anchor is named by a cell, and the cell has an
// owner" needs no further argument, and because it matches the mesher's own
// convention that the +side tile bridges a shared plane.)
//
// Case 3 rides the same fixture: world (X0, SL) is the corner where both fine
// siblings and the coarse tile meet. It is the case that killed the previous
// BAKED design -- a band quad near a tile corner needs the other axis's ghost
// vertices, so per-face baked variants are not independent -- and the welder is
// supposed to have no notion of it at all.

static void run_corner_and_partition(const FieldRuntime& f, const char* field_name,
                                     int L, float y_min, float y_max) {
    const float SL = 64.0f * float(1 << L);
    const float v  = SL / 32.0f;
    const float X0 = 2.0f * SL;
    printf("  [3/5] corner + partition, field %s: coarse (0,0) @ %.0f m rung %d,"
           " fine siblings (2,0) and (2,1) @ %.0f m rung %d, plane x = %.0f,"
           " corner at (%.0f, %.0f)\n",
           field_name, 2.0f * SL, -(L + 1), SL, -L, X0, X0, SL);

    SectorMesh cm, f0m, f1m;
    seam::SectorBoundary cb, f0b, f1b;
    std::string err;
    CHECK(mesh_sector(f, 0, 0, -(L + 1), 2.0f * SL, y_min, y_max, cm, &cb, err),
          err.c_str());
    CHECK(mesh_sector(f, 2, 0, -L, SL, y_min, y_max, f0m, &f0b, err), err.c_str());
    CHECK(mesh_sector(f, 2, 1, -L, SL, y_min, y_max, f1m, &f1b, err), err.c_str());

    // ONE fine side spanning BOTH siblings -- the engine's real lookup shape.
    const seam::FaceRecord& r0 = f0b.faces[seam::kFaceNegX];
    const seam::FaceRecord& r1 = f1b.faces[seam::kFaceNegX];
    seam::WeldSide neg  = side_of(-(L + 1), {&cb.faces[seam::kFacePosX]});
    seam::WeldSide both = side_of(-L, {&r0, &r1});

    int64_t a0_lo, a0_hi, b0_lo, b0_hi, a1_lo, a1_hi, b1_lo, b1_hi;
    CHECK(face_cell_box(f0b, seam::kFaceNegX, a0_lo, a0_hi, b0_lo, b0_hi),
          "sibling 0 exported a -x face");
    CHECK(face_cell_box(f1b, seam::kFaceNegX, a1_lo, a1_hi, b1_lo, b1_hi),
          "sibling 1 exported a -x face");
    CHECK(b0_hi + 1 == b1_lo,
          "the two siblings own adjacent, non-overlapping z cell runs");
    // Y is untiled, so both passes take the SAME a region: the union of the two
    // records' extents, so the two passes plus the combined pass are comparable.
    const int64_t a_lo = std::min(a0_lo, a1_lo), a_hi = std::max(a0_hi, a1_hi);

    seam::WeldMesh w0, w1, wall;
    seam::WeldStats s0, s1, sall;
    CHECK(seam::weld_face(0, neg, both, a_lo, a_hi + 1, b0_lo, b0_hi + 1,
                          w0, s0, err), err.c_str());
    CHECK(seam::weld_face(0, neg, both, a_lo, a_hi + 1, b1_lo, b1_hi + 1,
                          w1, s1, err), err.c_str());
    CHECK(seam::weld_face(0, neg, both, a_lo, a_hi + 1, b0_lo, b1_hi + 1,
                          wall, sall, err), err.c_str());
    account(s0, w0);
    account(s1, w1);

    // --- 5. no double emission ---------------------------------------------
    auto tri_set = [](const seam::WeldMesh& m, std::vector<std::array<float, 9>>& out) {
        for (const seam::WeldBucket& b : m.buckets)
            for (size_t t = 0; t * 9 < b.positions.size(); ++t) {
                std::array<float, 9> a{};
                for (int i = 0; i < 9; ++i) a[i] = b.positions[t * 9 + i];
                out.push_back(a);
            }
    };
    std::vector<std::array<float, 9>> t0, t1, tall;
    tri_set(w0, t0); tri_set(w1, t1); tri_set(wall, tall);
    std::set<std::array<float, 9>> set0(t0.begin(), t0.end());
    size_t shared = 0;
    for (const std::array<float, 9>& t : t1) if (set0.count(t)) ++shared;
    CHECK(shared == 0,
          "no double emission: two fine siblings against one coarse neighbour "
          "emit DISJOINT seam triangle sets across the shared plane");
    // ...and together they are exactly the single full-plane pass: a partition,
    // not merely an overlap-free pair of subsets.
    std::vector<std::array<float, 9>> tsum = t0;
    tsum.insert(tsum.end(), t1.begin(), t1.end());
    std::sort(tsum.begin(), tsum.end());
    std::sort(tall.begin(), tall.end());
    CHECK(tsum == tall,
          "the per-sibling regions PARTITION the plane: the two passes together "
          "are the single full-plane weld, triangle for triangle");
    CHECK(s0.crossings + s1.crossings == sall.crossings &&
          s0.quads + s1.quads == sall.quads && s0.tris + s1.tris == sall.tris,
          "partition: the two passes' crossing counts sum to the whole plane's");
    CHECK(s0.sign_conflicts == 0 && s1.sign_conflicts == 0,
          "partition: no sign conflicts across the sibling-sibling border");
    printf("      partition: sibling0 anchors b in [%lld,%lld) -> %zu tris;"
           " sibling1 b in [%lld,%lld) -> %zu tris; %zu shared (must be 0);"
           " combined pass %zu tris\n",
           (long long)b0_lo, (long long)(b0_hi + 1), t0.size(),
           (long long)b1_lo, (long long)(b1_hi + 1), t1.size(), shared, tall.size());
    print_stats("corner sib0", s0, w0.triangle_count());
    print_missing("corner sib0",
                  diagnose_missing(0, neg, both, a_lo, a_hi + 1, b0_lo, b0_hi + 1,
                                   a_lo, a_hi, b0_lo, b1_hi,
                                   &cb.faces[seam::kFacePosX]));
    print_stats("corner sib1", s1, w1.triangle_count());
    print_missing("corner sib1",
                  diagnose_missing(0, neg, both, a_lo, a_hi + 1, b1_lo, b1_hi + 1,
                                   a_lo, a_hi, b0_lo, b1_hi,
                                   &cb.faces[seam::kFacePosX]));

    // --- 3. coverage across the whole plane INCLUDING the corner column ------
    //
    // The partition welds above are deliberately BAND-FREE: a band is one tile's
    // finished geometry rather than a function of the anchor region, so it has
    // nothing to say about whether the two anchor passes partition the plane,
    // and putting it in the combined pass would make "t0 + t1 == tall" a
    // statement about which sibling's band got attached instead of about the
    // rule. Coverage is a different question, so the two per-sibling welds are
    // repeated here WITH each sibling's own band -- which is exactly the shape
    // the engine's per-tile passes have.
    seam::WeldMesh w0b, w1b;
    seam::WeldStats s0b, s1b;
    {
        seam::WeldSide b0 = side_of(-L, {&r0, &r1}, &r0.band);
        seam::WeldSide b1 = side_of(-L, {&r0, &r1}, &r1.band);
        CHECK(seam::weld_face(0, neg, b0, a_lo, a_hi + 1, b0_lo, b0_hi + 1,
                              w0b, s0b, err), err.c_str());
        CHECK(seam::weld_face(0, neg, b1, a_lo, a_hi + 1, b1_lo, b1_hi + 1,
                              w1b, s1b, err), err.c_str());
        CHECK(s0b.band_tris == int(r0.band.triangle_count()) &&
              s1b.band_tris == int(r1.band.triangle_count()),
              "corner: each sibling's pass emits that sibling's band verbatim");
        CHECK(s0b.crossings == s0.crossings && s1b.crossings == s1.crossings,
              "corner: the band leaves the fan's crossing accounting alone");
        printf("      corner bands: sibling0 %zu tris, sibling1 %zu tris\n",
               r0.band.triangle_count(), r1.band.triangle_count());
    }

    const float lo = X0 - 3.0f * v, hi = X0 + 3.0f * v;
    Probe mesh_only, welded;
    mesh_only.add(cm,  0.0f, 0.0f, 0, lo, hi);
    mesh_only.add(f0m, X0,   0.0f, 0, lo, hi);
    mesh_only.add(f1m, X0,   SL,   0, lo, hi);
    welded = mesh_only;
    welded.add(w0b, 0, lo, hi);
    welded.add(w1b, 0, lo, hi);

    const GapStat gm = scan(mesh_only, 0, X0, v, v, 2.0f * SL - v);
    const GapStat gw = scan(welded,    0, X0, v, v, 2.0f * SL - v);
    printf("      corner scan (rows z in [%.0f, %.0f]): mesh only %d/%d gapped,"
           " worst %.3f m (%.2f v) | mesh+weld %d/%d gapped, worst %.3f m (%.2f v)\n",
           v, 2.0f * SL - v, gm.gapped, gm.rows, gm.worst_run, gm.worst_run / v,
           gw.gapped, gw.rows, gw.worst_run, gw.worst_run / v);
    report_worst("corner", gw, 0, X0, v);
    { char t[64]; snprintf(t, sizeof t, "%s corner L%d/%d", field_name, L, L + 1);
      log_scan(t, gw, v); }
    CHECK(gw.gapped == 0,
          "three-tile corner: both fine siblings' halves of the coarse tile's "
          "edge are covered, all the way through the corner column where they "
          "meet -- the case that killed the baked-variant design");

    // The corner column itself, called out separately so a pass here cannot be
    // an averaging artifact of 60-odd rows.
    const GapStat gc = scan(welded, 0, X0, v, SL - v, SL + v);
    printf("      corner column (z = %.0f +- %.0f): %d/%d rows gapped, worst"
           " %.3f m\n", SL, v, gc.gapped, gc.rows, gc.worst_run);
    CHECK(gc.gapped == 0, "the three rows straddling the corner are covered");
}

// ---------------------------------------------------------------------------
// Case 8: the VERTICAL seam (M2) — coarse BELOW fine
// ---------------------------------------------------------------------------
//
// The direct mirror of case 1, one axis over. `mesh_sector_tiled` gives Y the
// same [1..n] ownership x and z have, and the consequence (derived in
// terrain_mesher.cpp, not assumed) is that the tile ABOVE a horizontal plane is
// the one that bridges it. So the orientation ownership leaves open is COARSE
// BELOW / FINE ABOVE, exactly as it is coarse-west/fine-east horizontally, and
// the -y face is the one that carries an overlap band.
//
// WHAT IS DIFFERENT ABOUT MEASURING IT, and it is geometry rather than
// bookkeeping: a vertical seam plane meets the ground plane in a LINE, so case 1
// scans a narrow band across it. A horizontal seam plane meets the SURFACE in a
// CONTOUR whose position is a property of the field. There is no band; the scan
// covers the fine tile's whole (x, z) footprint and asserts the union rule over
// all of it (`scan_columns`). Away from the contour every column is trivially
// covered by one tile or the other, so the gapped columns it finds are the seam.
//
// HEIGHTFIELD ONLY for the coverage gate, and the reason is the probe's known
// limit rather than a preference: `y_at` returns the HIGHEST triangle, so a
// column is "covered" the moment anything is above it. In a cave world a roof
// thirty metres down would answer for a missing terrain top and the scan would
// pass vacuously. The volumetric case therefore runs the STRUCTURAL gates only
// (crossings accounted, no sign conflicts, band verbatim) -- which is where a
// vertical indexing error would show anyway, since a sign conflict IS the two
// sides disagreeing about the shared plane's samples.
static void run_neg_y(const FieldRuntime& f, const char* field_name,
                      int max_level, bool one_sheet) {
    printf("  [8] -y pairing (coarse BELOW fine -- the vertical orientation the"
           " [1..n] ownership rule leaves open), field %s\n", field_name);
    for (int L = 0; L <= max_level; ++L) {
        const float SL = 64.0f * float(1 << L);
        const float v  = SL / 32.0f;          // the FINE voxel
        const float Y0 = 0.0f;                // the shared plane, y = 0
        SectorMesh cm, fm;
        seam::SectorBoundary cb, fb;
        std::string err;
        // coarse tile (0,-1,0) at 2*SL, rung -(L+1): x,z in [0, 2SL), y in [-2SL, 0)
        // fine   tile (0, 0,0) at   SL, rung -L:     x,z in [0,  SL), y in [0,  SL)
        CHECK(mesh_sector_tiled(f, 0, -1, 0, -(L + 1), 2.0f * SL, cm, &cb, err),
              err.c_str());
        CHECK(mesh_sector_tiled(f, 0, 0, 0, -L, SL, fm, &fb, err), err.c_str());

        const seam::OverlapBand& band = fb.faces[seam::kFaceNegY].band;
        seam::WeldSide neg = side_of(-(L + 1), {&cb.faces[seam::kFacePosY]});
        seam::WeldSide pos = side_of(-L,       {&fb.faces[seam::kFaceNegY]}, &band);
        seam::WeldMesh wm; seam::WeldStats ws;
        CHECK(weld_pair(1, neg, pos, fb, seam::kFaceNegY, wm, ws, err), err.c_str());
        CHECK(ws.crossings > 0, "-y weld: the shared plane carries crossings");
        CHECK(ws.sign_conflicts == 0,
              "-y weld: the two tiles agree about every shared plane sample -- "
              "the vertical half of the dyadic-double property, measured");
        CHECK(ws.crossings == ws.accounted(), "-y weld: every crossing accounted");
        CHECK(ws.band_tris == int(band.triangle_count()),
              "-y weld: the band is emitted VERBATIM");
        account(ws, wm);

        // The same weld with the band withheld, so the scan can separate what
        // the vertex fan closes from what the band closes.
        seam::WeldSide pos_nb = side_of(-L, {&fb.faces[seam::kFaceNegY]});
        seam::WeldMesh wf; seam::WeldStats wsf;
        CHECK(weld_pair(1, neg, pos_nb, fb, seam::kFaceNegY, wf, wsf, err),
              err.c_str());
        account_winding(wf, /*fan_only=*/true);
        CHECK(wsf.band_tris == 0 && wsf.crossings == ws.crossings &&
              wsf.quads == ws.quads && wsf.tris == ws.tris,
              "-y weld: the band changes NOTHING about the fan");
        print_stats("-y", ws, wm.triangle_count());
        print_missing("-y", diag_for(1, neg, pos, fb, seam::kFaceNegY,
                                     &cb.faces[seam::kFacePosY]));

        if (!one_sheet) {
            printf("      (volumetric: structural gates only -- the plumb probe "
                   "takes the highest triangle and a cave roof would answer for "
                   "a missing terrain top)\n");
            continue;
        }

        // Coverage. Both tile meshes are Y-TILED, so their y is tile-local and
        // the probe is told each tile's origin (`add_tiled`) -- getting that
        // wrong would stack the two tiles on top of each other and read as a
        // spectacular, obvious failure rather than a subtle one.
        const float m = 2.0f * v;                      // stay off the x/z borders
        Probe mesh_only, fan_only, band_only, welded;
        mesh_only.add_tiled(cm, 0.0f, -2.0f * SL, 0.0f, 2, -1e30f, 1e30f);
        mesh_only.add_tiled(fm, 0.0f, 0.0f,       0.0f, 2, -1e30f, 1e30f);
        fan_only  = mesh_only; fan_only.add(wf, 2, -1e30f, 1e30f);
        band_only = mesh_only; band_only.add_band(band, 2, -1e30f, 1e30f);
        welded    = mesh_only; welded.add(wm, 2, -1e30f, 1e30f);

        const GapStat gm = scan_columns(mesh_only, m, SL - m, m, SL - m, v);
        const GapStat gf = scan_columns(fan_only,  m, SL - m, m, SL - m, v);
        const GapStat gb = scan_columns(band_only, m, SL - m, m, SL - m, v);
        const GapStat gw = scan_columns(welded,    m, SL - m, m, SL - m, v);
        printf("    L%d/%d (voxel %.2f/%.2f m, plane y = %.0f): mesh only %d/%d"
               " rows gapped, worst %.3f m (%.2f v) | mesh+weld %d/%d rows"
               " gapped, worst %.3f m (%.2f v)\n",
               L, L + 1, v, 2 * v, Y0, gm.gapped, gm.rows, gm.worst_run,
               gm.worst_run / v, gw.gapped, gw.rows, gw.worst_run,
               gw.worst_run / v);
        printf("      SPLIT: mesh %d gapped | +fan only %d | +band only %d |"
               " +both %d  (of %d rows; band %zu tris)\n",
               gm.gapped, gf.gapped, gb.gapped, gw.gapped, gm.rows,
               band.triangle_count());
        report_worst("-y", gw, 0, Y0, v);
        { char t[64]; snprintf(t, sizeof t, "%s -y L%d/%d", field_name, L, L + 1);
          log_scan(t, gw, v); }
        CHECK(gm.gapped > 0,
              "-y: the mesh-only baseline IS gapped -- if it were not, the weld "
              "would be closing nothing and this scan would prove nothing");
        // The gate.
        CHECK(gw.gapped == 0,
              "-y cross-level seam: every column of the fine tile's footprint is "
              "covered by the tile below, the tile above, or the weld band");
    }
}

// ---------------------------------------------------------------------------
// Case 9: a HORIZONTAL weld whose fine side is SPLIT IN Y (M2)
// ---------------------------------------------------------------------------
//
// Case 3/5 partitions a vertical plane between two fine siblings adjacent in z.
// Once Y is tiled the same plane can be split in Y instead -- and that is the
// configuration that did not exist before this stage, because on the column path
// a tile spanned the whole slab and one record answered for the entire vertical
// extent of a face.
//
// So: coarse tile (0,-1,0) at 2S against fine tiles (2,-2,0) and (2,-1,0) at S,
// sharing the plane x = 2S. On an x-plane the `a` axis IS the global Y cell, so
// the anchor partition now runs along a, the fine side's lookup must span the
// two records to answer the cells one lower in a, and the internal ty border is
// a place a Y indexing error would show as a sign conflict or a dropped row.
//
// Volumetric fixture, necessarily: a heightfield's surface is a single sheet and
// therefore lives in exactly ONE tile of any vertical stack, so two stacked fine
// tiles cannot both have a face record. The zero-gap coverage number here is
// taken on the -100 m floor sheet alone (Probe::y_lo/y_hi), which is inside the
// LOWER fine tile -- one sheet, so the plumb probe means something, and it is the
// tile the split introduced.
static void run_x_plane_split_in_y(const FieldRuntime& f, const char* field_name,
                                   int L) {
    const float SL = 64.0f * float(1 << L);
    const float v  = SL / 32.0f;
    const float X0 = 2.0f * SL;
    printf("  [9] x-plane whose FINE side is split in Y, field %s: coarse"
           " (0,-1,0) @ %.0f m rung %d, fine (2,-2,0) and (2,-1,0) @ %.0f m"
           " rung %d, plane x = %.0f\n",
           field_name, 2.0f * SL, -(L + 1), SL, -L, X0);

    SectorMesh cm, f0m, f1m;
    seam::SectorBoundary cb, f0b, f1b;
    std::string err;
    CHECK(mesh_sector_tiled(f, 0, -1, 0, -(L + 1), 2.0f * SL, cm, &cb, err),
          err.c_str());
    CHECK(mesh_sector_tiled(f, 2, -2, 0, -L, SL, f0m, &f0b, err), err.c_str());
    CHECK(mesh_sector_tiled(f, 2, -1, 0, -L, SL, f1m, &f1b, err), err.c_str());

    const seam::FaceRecord& r0 = f0b.faces[seam::kFaceNegX];   // lower fine tile
    const seam::FaceRecord& r1 = f1b.faces[seam::kFaceNegX];   // upper fine tile
    CHECK(!r0.verts.empty() && !r1.verts.empty(),
          "both vertically stacked fine tiles exported a -x face -- otherwise "
          "there is no y split to test");
    seam::WeldSide neg  = side_of(-(L + 1), {&cb.faces[seam::kFacePosX]});
    seam::WeldSide both = side_of(-L, {&r0, &r1});

    int64_t a0_lo, a0_hi, b0_lo, b0_hi, a1_lo, a1_hi, b1_lo, b1_hi;
    CHECK(face_cell_box(f0b, seam::kFaceNegX, a0_lo, a0_hi, b0_lo, b0_hi),
          "lower fine tile exported a -x face");
    CHECK(face_cell_box(f1b, seam::kFaceNegX, a1_lo, a1_hi, b1_lo, b1_hi),
          "upper fine tile exported a -x face");
    CHECK(a0_hi + 1 == a1_lo,
          "the two stacked tiles own ADJACENT, non-overlapping y cell runs -- "
          "read from tile identity, which is only possible now Y is tiled");
    CHECK(b0_lo == b1_lo && b0_hi == b1_hi,
          "and the same z cell run, since they differ only in ty");
    const int64_t b_lo = b0_lo, b_hi = b0_hi;

    seam::WeldMesh w0, w1, wall;
    seam::WeldStats s0, s1, sall;
    CHECK(seam::weld_face(0, neg, both, a0_lo, a0_hi + 1, b_lo, b_hi + 1,
                          w0, s0, err), err.c_str());
    CHECK(seam::weld_face(0, neg, both, a1_lo, a1_hi + 1, b_lo, b_hi + 1,
                          w1, s1, err), err.c_str());
    CHECK(seam::weld_face(0, neg, both, a0_lo, a1_hi + 1, b_lo, b_hi + 1,
                          wall, sall, err), err.c_str());
    account(s0, w0);
    account(s1, w1);

    auto tri_set = [](const seam::WeldMesh& m, std::vector<std::array<float, 9>>& o) {
        for (const seam::WeldBucket& b : m.buckets)
            for (size_t t = 0; t * 9 < b.positions.size(); ++t) {
                std::array<float, 9> a{};
                for (int i = 0; i < 9; ++i) a[i] = b.positions[t * 9 + i];
                o.push_back(a);
            }
    };
    std::vector<std::array<float, 9>> t0, t1, tall;
    tri_set(w0, t0); tri_set(w1, t1); tri_set(wall, tall);
    std::set<std::array<float, 9>> set0(t0.begin(), t0.end());
    size_t shared = 0;
    for (const std::array<float, 9>& t : t1) if (set0.count(t)) ++shared;
    std::vector<std::array<float, 9>> tsum = t0;
    tsum.insert(tsum.end(), t1.begin(), t1.end());
    std::sort(tsum.begin(), tsum.end());
    std::sort(tall.begin(), tall.end());
    printf("      partition in Y: lower anchors a in [%lld,%lld) -> %zu tris;"
           " upper a in [%lld,%lld) -> %zu tris; %zu shared (must be 0);"
           " combined %zu tris\n",
           (long long)a0_lo, (long long)(a0_hi + 1), t0.size(),
           (long long)a1_lo, (long long)(a1_hi + 1), t1.size(), shared,
           tall.size());
    CHECK(shared == 0,
          "no double emission across the ty boundary: the two stacked fine "
          "tiles emit DISJOINT seam triangle sets on the shared x plane");
    CHECK(tsum == tall,
          "the per-tile y regions PARTITION the plane: the two passes together "
          "are the single full-plane weld, triangle for triangle");
    CHECK(s0.crossings + s1.crossings == sall.crossings &&
          s0.quads + s1.quads == sall.quads && s0.tris + s1.tris == sall.tris,
          "partition in Y: the two passes' crossing counts sum to the plane's");
    CHECK(s0.sign_conflicts == 0 && s1.sign_conflicts == 0 &&
          sall.sign_conflicts == 0,
          "no sign conflicts across the ty boundary: the upper tile's lowest "
          "boundary cells and the lower tile's highest read the same samples");
    CHECK(s0.crossings == s0.accounted() && s1.crossings == s1.accounted(),
          "every crossing accounted on both sides of the ty boundary");
    print_stats("split-y lower", s0, w0.triangle_count());
    print_missing("split-y lower",
                  diagnose_missing(0, neg, both, a0_lo, a0_hi + 1, b_lo, b_hi + 1,
                                   a0_lo, a1_hi, b_lo, b_hi,
                                   &cb.faces[seam::kFacePosX]));
    print_stats("split-y upper", s1, w1.triangle_count());
    print_missing("split-y upper",
                  diagnose_missing(0, neg, both, a1_lo, a1_hi + 1, b_lo, b_hi + 1,
                                   a0_lo, a1_hi, b_lo, b_hi,
                                   &cb.faces[seam::kFacePosX]));

    // The upper tile's pass reaches one cell LOWER in a -- into the lower
    // tile -- which is exactly why `both` is a two-record lookup. If it were
    // one record those cells would answer null and be banked as
    // missing_landing, opening a hole running the length of the ty border.
    {
        seam::WeldSide only1 = side_of(-L, {&r1});
        seam::WeldMesh w1s; seam::WeldStats s1s;
        CHECK(seam::weld_face(0, neg, only1, a1_lo, a1_hi + 1, b_lo, b_hi + 1,
                              w1s, s1s, err), err.c_str());
        printf("      single-record control: upper pass with a lookup that does"
               " NOT span the ty border -> missing_fine %d (vs %d spanning)\n",
               s1s.missing_fine, s1.missing_fine);
        CHECK(s1s.missing_fine > s1.missing_fine,
              "...and the control proves the multi-tile lookup is load-bearing: "
              "narrowing it to one record strands the cells on the ty border");
    }

    // NO PLUMB-LINE COVERAGE NUMBER HERE, and the reason is worth recording
    // because the obvious scan looks reasonable and is not a measurement.
    //
    // This case needs a volumetric field (see the note above), and a volumetric
    // column has no single surface for `y_at` to report. Restricting the soup to
    // a y band around the -100 m floor was tried and is WRONG for a sharper
    // reason than multi-sheet ambiguity: in this field the -100 m plane is a
    // floor only where a tunnel is open above it, and everywhere else that band
    // is solid rock with no geometry in it at all. The probe cannot tell "no
    // triangle because the seam is open" from "no triangle because it is inside
    // the mountain", so it reported 8 of 31 rows gapped by up to 4.6 voxels on a
    // weld that is provably complete. A coverage gate that fires on solid rock is
    // not a gate.
    //
    // What IS gated here is what the Y split can actually break, and all of it
    // is checked above: the partition, disjointness, the crossing sums, zero
    // sign conflicts across the ty border, and the single-record control showing
    // the cross-tile lookup is load-bearing. The zero-gap coverage claim for a
    // horizontal plane lives in case 8, on a single-sheet field where the probe
    // means something.
    (void)f0m; (void)f1m; (void)v; (void)X0;
}

int main() {
    printf("=== seam integration: mesh + weld (M0-WP6) ===\n");

    // =======================================================================
    // HEIGHTFIELD (kNoise). The cheap case, and the one whose numbers line up
    // one-for-one with terrain_mesher_tests.cpp's characterization block.
    // =======================================================================
    {
        FieldRuntime f = make(kNoise);
        run_neg_x(f, "kNoise", -300.0f, 300.0f, 3, /*one_sheet=*/true);
        run_neg_z(f, "kNoise", -300.0f, 300.0f, 2, /*one_sheet=*/true);
        run_corner_and_partition(f, "kNoise", 1, -300.0f, 300.0f);
    }

    // =======================================================================
    // VOLUMETRIC (kNoiseCave). The case StreamCaverns exercises hardest, and
    // the one where the retired snap did real work: terrain_mesher_tests
    // measures only 481/1600 plane samples where the raw fine density equals
    // the coarse interpolant, worst delta 1.02. If the weld were a formality it
    // would show here first.
    // =======================================================================
    {
        FieldRuntime f = make(kNoiseCave);
        CHECK(!f.is_heightfield(), "kNoiseCave is volumetric");
        run_neg_x(f, "kNoiseCave", -128.0f, 192.0f, 2, /*one_sheet=*/false);
        run_neg_z(f, "kNoiseCave", -128.0f, 192.0f, 1, /*one_sheet=*/false);
        run_corner_and_partition(f, "kNoiseCave", 0, -128.0f, 192.0f);
    }

    // =======================================================================
    // [4] MID-TRANSITION CONFIGURATIONS -- untestable before the mask went.
    //
    // Under the old scheme a tile's geometry was baked AGAINST a neighbour
    // level: `edge_mask` said "my -x neighbour is exactly one level coarser"
    // and the lattice was extended accordingly. So there was no such thing as
    // welding "whatever pair is drawn" -- the tile itself only existed in one
    // of the two shapes, and the configurations below could not be constructed
    // at all, let alone asserted on.
    //
    // Now the tile is baked ONCE, from the field alone, and the same bytes
    // serve every neighbour it ever has. That is what these two cases show.
    // =======================================================================
    {
        FieldRuntime f = make(kNoise);
        const int L = 1;
        const float SL = 64.0f * float(1 << L);      // 128 m
        const float v  = SL / 32.0f;                 // 4 m
        const float X0 = 2.0f * SL;                  // 256 m
        printf("  [4] mid-transition configurations (field kNoise, L%d, %.0f m"
               " tiles, %.0f m fine voxel)\n", L, SL, v);

        std::string err;
        // ONE bake of the fine tile. Everything below welds these same bytes.
        SectorMesh fm; seam::SectorBoundary fb;
        CHECK(mesh_sector(f, 2, 0, -L, SL, -300, 300, fm, &fb, err), err.c_str());

        // (a) THE COARSE SIDE IS ONE LEVEL COARSER THAN THE FINE SIDE EXPECTS.
        //     The drawn -x neighbour is first an EQUAL-level tile (1,0) at SL,
        //     then a COARSE tile (0,0) at 2 SL. Under the mask these needed two
        //     different bakes of the fine tile, and any frame in which the
        //     drawn neighbour disagreed with the baked guess printed a strip.
        //     Here the fine tile is one object and both welds read it.
        SectorMesh em, cm;
        seam::SectorBoundary eb, cb;
        CHECK(mesh_sector(f, 1, 0, -L, SL, -300, 300, em, &eb, err), err.c_str());
        CHECK(mesh_sector(f, 0, 0, -(L + 1), 2.0f * SL, -300, 300, cm, &cb, err),
              err.c_str());

        // BOTH welds are handed the SAME fine tile with the SAME band attached.
        // That is the point: the equal-level pairing must suppress it and the
        // cross-level one must draw it, decided from the drawn pair alone. It is
        // the direct gate on M0-WP7's suppression rule, and the reason the band
        // could not simply be baked into the mesh -- baked, this geometry would
        // be duplicate coplanar surface in the equal-level frame.
        const seam::OverlapBand& fband = fb.faces[seam::kFaceNegX].band;
        CHECK(fband.triangle_count() > 0,
              "(4a) the fine tile exported a -x overlap band at all");
        seam::WeldMesh w_eq, w_co;
        seam::WeldStats s_eq, s_co;
        {
            seam::WeldSide neg = side_of(-L, {&eb.faces[seam::kFacePosX]});
            seam::WeldSide pos = side_of(-L, {&fb.faces[seam::kFaceNegX]}, &fband);
            CHECK(weld_pair(0, neg, pos, fb, seam::kFaceNegX, w_eq, s_eq, err),
                  err.c_str());
        }
        {
            seam::WeldSide neg = side_of(-(L + 1), {&cb.faces[seam::kFacePosX]});
            seam::WeldSide pos = side_of(-L, {&fb.faces[seam::kFaceNegX]}, &fband);
            CHECK(weld_pair(0, neg, pos, fb, seam::kFaceNegX, w_co, s_co, err),
                  err.c_str());
        }
        account(s_co, w_co);
        CHECK(w_eq.triangle_count() == 0 && s_eq.crossings == 0 &&
              s_eq.band_tris == 0,
              "(4a) the equal-level pairing welds NOTHING -- not the fan and NOT "
              "the overlap band, though the band was handed to it: those tiles "
              "already meet bitwise through shared samples");
        CHECK(s_co.band_tris == int(fband.triangle_count()),
              "(4a) the SAME band, the SAME bytes, is drawn in full against the "
              "coarse neighbour -- the drawn pair decides, not the bake");
        CHECK(s_co.crossings > 0 && w_co.triangle_count() > 0,
              "(4a) the same fine tile, unchanged, welds a real band against a "
              "coarse neighbour it was never told about");
        CHECK(s_co.sign_conflicts == 0, "(4a) no sign conflicts");
        printf("      (4a) one bake of fine tile (2,0), two drawn neighbours:"
               " equal-level -> %zu tris (crossings %d); one level coarser ->"
               " %zu tris (crossings %d)\n",
               w_eq.triangle_count(), s_eq.crossings,
               w_co.triangle_count(), s_co.crossings);
        print_stats("(4a) coarse", s_co, w_co.triangle_count());
        {
            seam::WeldSide neg = side_of(-(L + 1), {&cb.faces[seam::kFacePosX]});
            seam::WeldSide pos = side_of(-L,       {&fb.faces[seam::kFaceNegX]});
            print_missing("(4a) coarse", diag_for(0, neg, pos, fb, seam::kFaceNegX,
                                                  &cb.faces[seam::kFacePosX]));
        }

        // Coverage for both drawn maps, from the same fine mesh.
        {
            const float lo = X0 - 3.0f * v, hi = X0 + 3.0f * v;
            Probe p_eq, p_co;
            p_eq.add(em, SL, 0.0f, 0, lo, hi);
            p_eq.add(fm, X0, 0.0f, 0, lo, hi);
            p_co.add(cm, 0.0f, 0.0f, 0, lo, hi);
            p_co.add(fm, X0,   0.0f, 0, lo, hi);
            p_co.add(w_co, 0, lo, hi);
            const GapStat g_eq = scan(p_eq, 0, X0, v, v, SL - v);
            const GapStat g_co = scan(p_co, 0, X0, v, v, SL - v);
            printf("      (4a) coverage: equal-level pair %d/%d rows gapped"
                   " (no weld involved); coarse pair %d/%d rows gapped\n",
                   g_eq.gapped, g_eq.rows, g_co.gapped, g_co.rows);
            report_worst("(4a) coarse pair", g_co, 0, X0, v);
            log_scan("kNoise (4a) coarse pair L1/2", g_co, v);
            CHECK(g_eq.gapped == 0,
                  "(4a) the equal-level pair is watertight on baked geometry "
                  "alone -- the untouched [1..n] ownership guarantee");
            CHECK(g_co.gapped == 0,
                  "(4a) the SAME fine tile welded against a coarse neighbour is "
                  "watertight too: the drawn pair decides the seam, not the bake");
        }

        // (b) TWO FACES, TWO DIFFERENT NEIGHBOUR LEVELS AT ONCE. Fine tile
        //     (2,1): a coarse neighbour across -x, an equal-level sibling
        //     across -z. `restrict_levels` permits it and the mask could only
        //     ever describe it as a static promise; the welder handles the two
        //     faces independently because it is handed a pair, not a map.
        SectorMesh gm; seam::SectorBoundary gb;
        CHECK(mesh_sector(f, 2, 1, -L, SL, -300, 300, gm, &gb, err), err.c_str());
        SectorMesh sm; seam::SectorBoundary sb;   // the -z equal-level sibling
        CHECK(mesh_sector(f, 2, 0, -L, SL, -300, 300, sm, &sb, err), err.c_str());

        seam::WeldMesh wx, wz;
        seam::WeldStats sx, sz;
        {
            seam::WeldSide neg = side_of(-(L + 1), {&cb.faces[seam::kFacePosX]});
            seam::WeldSide pos = side_of(-L, {&gb.faces[seam::kFaceNegX]},
                                         &gb.faces[seam::kFaceNegX].band);
            CHECK(weld_pair(0, neg, pos, gb, seam::kFaceNegX, wx, sx, err),
                  err.c_str());
        }
        {
            seam::WeldSide neg = side_of(-L, {&sb.faces[seam::kFacePosZ]});
            seam::WeldSide pos = side_of(-L, {&gb.faces[seam::kFaceNegZ]},
                                         &gb.faces[seam::kFaceNegZ].band);
            CHECK(weld_pair(2, neg, pos, gb, seam::kFaceNegZ, wz, sz, err),
                  err.c_str());
        }
        account(sx, wx);
        CHECK(sx.crossings > 0 && sx.sign_conflicts == 0,
              "(4b) the cross-level -x face welds, with no sign conflicts");
        CHECK(sx.band_tris == int(gb.faces[seam::kFaceNegX].band.triangle_count()),
              "(4b) the cross-level -x face draws its band");
        CHECK(wz.triangle_count() == 0 && sz.crossings == 0 && sz.band_tris == 0,
              "(4b) the equal-level -z face welds nothing -- fan or band -- in "
              "the SAME frame in which the -x face welds both, from ONE bake of "
              "the tile that owns both faces");
        CHECK(gb.faces[seam::kFaceNegZ].band.triangle_count() > 0,
              "(4b) ...and the -z band it suppressed was not empty to begin with");
        printf("      (4b) fine tile (2,1): -x coarse -> %zu tris (crossings %d,"
               " missing %d); -z equal -> %zu tris (crossings %d)\n",
               wx.triangle_count(), sx.crossings, sx.missing_landing,
               wz.triangle_count(), sz.crossings);
        print_stats("(4b) -x", sx, wx.triangle_count());
        {
            seam::WeldSide neg = side_of(-(L + 1), {&cb.faces[seam::kFacePosX]});
            seam::WeldSide pos = side_of(-L,       {&gb.faces[seam::kFaceNegX]});
            print_missing("(4b) -x", diag_for(0, neg, pos, gb, seam::kFaceNegX,
                                              &cb.faces[seam::kFacePosX]));
        }
        {
            const float lox = X0 - 3.0f * v, hix = X0 + 3.0f * v;
            Probe px;
            px.add(cm, 0.0f, 0.0f, 0, lox, hix);
            px.add(gm, X0,   SL,   0, lox, hix);
            px.add(wx, 0, lox, hix);
            const GapStat gx = scan(px, 0, X0, v, SL + v, 2.0f * SL - v);

            const float loz = SL - 3.0f * v, hiz = SL + 3.0f * v;
            Probe pz;
            pz.add(sm, X0, 0.0f, 2, loz, hiz);
            pz.add(gm, X0, SL,   2, loz, hiz);
            const GapStat gz = scan(pz, 2, SL, v, X0 + v, X0 + SL - v);
            printf("      (4b) coverage: -x (welded) %d/%d rows gapped, worst"
                   " %.3f m; -z (equal, no weld) %d/%d rows gapped, worst %.3f m\n",
                   gx.gapped, gx.rows, gx.worst_run,
                   gz.gapped, gz.rows, gz.worst_run);
            report_worst("(4b) -x", gx, 0, X0, v);
            log_scan("kNoise (4b) -x coarse L1/2", gx, v);
            CHECK(gx.gapped == 0,
                  "(4b) the coarse face is closed by its weld while the equal "
                  "face has none -- the two are independent");
            CHECK(gz.gapped == 0,
                  "(4b) the equal-level face stays watertight on mesh alone");
        }
    }

    // =======================================================================
    // [8/9] THE VERTICAL AXIS (M2). Y stops being a slab and becomes a tiled
    // axis, so the two configurations below did not exist before this stage:
    // a shared HORIZONTAL plane between two levels, and a shared VERTICAL plane
    // whose fine side is two tiles stacked in Y.
    //
    // Both run through the same `seam::weld_face` with face_axis = 1 and 0
    // respectively -- no new welder code, which is the claim being tested as
    // much as anything. seam_weld.h's winding derivation already covers all
    // three normal axes and seam_weld_tests.cpp already cross-checks them
    // against averaged vertex normals; what was never exercised was a real
    // mesher record on a y plane.
    // =======================================================================
    {
        FieldRuntime f = make(kNoise);
        run_neg_y(f, "kNoise", 2, /*one_sheet=*/true);
    }
    {
        FieldRuntime f = make(kCave0);
        CHECK(!f.is_heightfield(), "kCave0 is volumetric");
        run_neg_y(f, "kCave0", 1, /*one_sheet=*/false);
        run_x_plane_split_in_y(f, "kCave0", 0);
    }

    // =======================================================================
    // [6] The §4.1 residue report. `missing_landing` is ALLOWED to be non-zero
    // -- it is a fine-only sign change whose covering coarse cell produced no
    // vertex, so there is nowhere honest to land. It must be REPORTED, never
    // hidden, and never closed by inventing a vertex.
    //
    // No percentage bound is asserted on it, deliberately. A ratio gate here
    // would be a number chosen to match today's measurement, and the first time
    // it failed the cheapest response would be to widen it -- which is exactly
    // the move this whole work exists to stop (the mask "closed" the seam by
    // being right about a guess). What IS gated is structural: every crossing
    // accounted exactly once, no sign conflicts, and no fine-side null (which
    // would contradict the sparse-signs argument in seam_boundary.h rather than
    // being a residue at all). The ratio is printed and read.
    // =======================================================================
    printf("  [6] weld totals over %lld welds: crossings %lld = quads %lld +"
           " tris %lld + missing_landing %lld + degenerate %lld\n",
           g_tot.welds, g_tot.crossings, g_tot.quads, g_tot.tris,
           g_tot.missing, g_tot.degen);
    printf("      emitted %lld triangles; sign_conflicts %lld;"
           " missing_landing = %.3f%% of crossings (the honest residue, 4.1)\n",
           g_tot.emitted, g_tot.conflicts,
           g_tot.crossings ? 100.0 * double(g_tot.missing) / double(g_tot.crossings)
                           : 0.0);
    printf("      residue attribution: %lld coarse-side topology + %lld coarse"
           " slab extent + %lld fixture edge + %lld fine-side\n",
           g_tot.miss_coarse, g_tot.miss_extent, g_tot.miss_fixture,
           g_tot.miss_fine);
    CHECK(g_tot.crossings ==
          g_tot.quads + g_tot.tris + g_tot.missing + g_tot.degen,
          "every crossing is accounted for exactly once");
    CHECK(g_tot.conflicts == 0,
          "no sign conflicts anywhere: both sides of every plane read the same "
          "world samples");
    CHECK(g_tot.miss_coarse + g_tot.miss_extent + g_tot.miss_fixture +
          g_tot.miss_fine == g_tot.missing,
          "every missing landing is attributed to exactly one cause");

    // =======================================================================
    // [7] THE VERDICT. The question M0-WP6 was commissioned to answer, in one
    // block, whichever way it comes out.
    // =======================================================================
    // =======================================================================
    // [6b] REPLACE OR SUPPLEMENT (M0-WP7). The per-scan SPLIT lines above
    // answer the coverage half: the band alone closes every scan and the fan
    // alone does not. Coverage alone would therefore say "replace" -- and it is
    // the wrong question to stop on, because a vertical ray takes the highest
    // triangle and cannot see how many sheets are stacked beneath it.
    //
    // The cost of drawing both is interpenetration, and the DOUBLE COVER lines
    // measure it against the interpenetration the band creates by itself. The
    // band draws fine surface back across ground the coarse tile already covers
    // (that IS the overlap mechanism), so `band vs coarse mesh` is the price of
    // having a band at all. `band vs fan` is what keeping the fan adds on top --
    // and the fan interpolates between the coarse tile's own vertex and the fine
    // tile's own vertex, so it lies BETWEEN the two sheets already being drawn.
    // If that number does not exceed the first, keeping the fan introduces no
    // new double surface anywhere, and it is kept for what the ray cannot see:
    // it is an exact join through both tiles' own vertices, it is the only
    // mechanism on the +x/+z and (from M2) +-y faces where no band exists, and
    // it degrades to `missing_coarse_pair` rather than to overlap.
    printf("  [6b] REPLACE-OR-SUPPLEMENT over %d single-sheet scans: worst"
           " band-vs-fan separation %.2f fine voxels, worst band-vs-coarse-mesh"
           " %.2f; scans where keeping the fan cost MORE than the band already"
           " did: %d\n",
           g_div.scans, g_div.worst_fan, g_div.worst_coarse, g_div.worse);
    CHECK(g_div.worse == 0,
          "the vertex fan SUPPLEMENTS the band at no cost in double surface: on "
          "every scan it lies within the overlap the band creates on its own, "
          "because it interpolates between the two tiles' own boundary vertices");

    // =======================================================================
    // [8] SHADING ORIENTATION. Coverage says a seam is closed; it says nothing
    // about whether the geometry that closes it faces the right way. Issue
    // ec2829d6 was filed as "gaps" and turned out to be weld strips rendering
    // dark, which every coverage gate in this file passes without noticing --
    // so the orientation of what the welder emits is now asserted here, over
    // REAL mesher records with their bands attached (see account_winding).
    // =======================================================================
    printf("  [8] SHADING: %lld emitted triangles testable, %lld with the "
           "geometric normal opposed to their corners' averaged normals, worst "
           "cos = %.4f\n",
           g_tot.wind_checked, g_tot.wind_wrong, g_tot.wind_worst);
    printf("      SEVERE (cos < -0.5, a real reversal rather than a steep "
           "bridge): %lld\n", g_tot.wind_severe);
    printf("      fan only (same pairs, band withheld): %lld testable, %lld "
           "opposed (%lld severe), worst cos = %.4f\n",
           g_tot.fan_checked, g_tot.fan_wrong, g_tot.fan_severe,
           g_tot.fan_worst);
    CHECK(g_tot.wind_severe == 0,
          "every emitted weld triangle -- fan AND band -- is wound to agree "
          "with the stored normals it carries, so it shades as the surface it "
          "joins rather than as a back face");

    printf("  [7] VERDICT: zero-gap restored in %d of %d welded scans"
           " (mesh-only was gapped in every one of them).\n",
           g_scan.clean, g_scan.scans);
    for (const std::string& r : g_scan.residual)
        printf("      RESIDUAL %s\n", r.c_str());
    CHECK(g_scan.residual.empty(),
          "THE M0 GATE: mesh + weld leaves no uncovered column on any drawn "
          "cross-level pairing");

    printf("=== seam integration ");
    return check_summary();
}
