// MatterEngine3/tests/seam_weld_tests.cpp — M0-WP3: the runtime cross-level
// seam welder (docs/volumetric-sectors-design-2026-08-10.md §4.1).
//
// The welder is deliberately engine-free -- no sectors, no bakes, no frames --
// so this suite drives it with SYNTHETIC seam::FaceRecords built here from an
// analytic density function sampled at two resolutions. That is not a shortcut:
// the whole reason seams moved out of the bake is that their correctness must
// stop depending on streaming state, and a suite that needed a streamer to run
// would have conceded the point. It is also why this file is milliseconds.
//
// The eight properties gated below, in the order the design asks for them:
//   1. equal rungs emit nothing (those pairs already meet bitwise)
//   2. a rung gap of 2 is REJECTED, not approximated
//   3. a 2:1 pair over a flat surface: every crossing lands, and every emitted
//      position is one of the two records' own vertices (nothing synthesized)
//   4. winding agrees with the averaged vertex normals, on all three plane axes
//      and with either side finer -- the cross-check on the paper derivation in
//      seam_weld.h
//   5. the triangle/quad split matches the 2:1 collapse parity exactly, against
//      an oracle computed straight from the analytic field
//   6. a coarse cell with no vertex is COUNTED (missing_landing) and emits
//      nothing degenerate -- §4.1's honest residue
//   7. a side backed by TWO tiles welds identically to one -- the tile-corner
//      case that killed the previous baked-variant design
//   8. determinism: byte-identical output across runs

#include "check.h"
#include "../src/seam_weld.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace seam;

// ---------------------------------------------------------------------------
// Synthetic records
// ---------------------------------------------------------------------------

using DensFn = std::function<double(double, double, double)>;

// A tilted plane: solid below it. Chosen for test 3 because "does every fine
// crossing have a coarse landing site?" is PROVABLE here rather than merely
// likely -- if the surface passes through the interior of a fine cell it passes
// through the interior of the coarse cell containing it, and a plane through a
// cube's interior always separates at least one corner from the rest, so the
// coarse cell has a sign change and therefore a vertex.
static double dens_flat(double x, double y, double z) {
    return 3.7 - y - 0.25 * x + 0.13 * z;
}
// A sphere, solid inside. Off-lattice centre so no lattice sample lands on zero.
static double dens_sphere(double x, double y, double z) {
    const double dx = x - 0.31, dy = y + 0.22, dz = z - 0.17;
    return 7.63 * 7.63 - (dx * dx + dy * dy + dz * dz);
}

// Same fixed tangential-axis order seam::face_tangent_axes gives, keyed by the
// normal axis directly (the welder never needs the six-face enum).
static void axis_tangents(int axis, int& a_axis, int& b_axis) {
    if (axis == 0)      { a_axis = 1; b_axis = 2; }
    else if (axis == 1) { a_axis = 0; b_axis = 2; }
    else                { a_axis = 0; b_axis = 1; }
}

// Reference surface-nets pass over ONE cell layer adjacent to a shared plane --
// the same construction terrain_mesher performs, reduced to the boundary layer a
// tile would export.
//
//   axis          : plane normal axis
//   rung          : this side's rung (finer = larger)
//   plane         : global lattice index of the shared plane AT THIS RUNG
//   positive_side : the tile above the plane (cell layer = plane) or below it
//                   (cell layer = plane - 1)
static FaceRecord build_record(int axis, int rung, int64_t plane, bool positive_side,
                               int64_t a0, int64_t a1, int64_t b0, int64_t b1,
                               const DensFn& D) {
    FaceRecord rec;
    rec.face = (axis == 0) ? (positive_side ? kFacePosX : kFaceNegX)
             : (axis == 1) ? (positive_side ? kFacePosY : kFaceNegY)
                           : (positive_side ? kFacePosZ : kFaceNegZ);
    rec.plane = plane;
    rec.cell_layer = positive_side ? plane : plane - 1;

    const double v = rung_voxel(rung);
    int aa = 0, ba = 0;
    axis_tangents(axis, aa, ba);

    for (int64_t ca = a0; ca < a1; ++ca) {
        for (int64_t cb = b0; cb < b1; ++cb) {
            int64_t g[3];
            g[aa] = ca; g[ba] = cb; g[axis] = rec.cell_layer;

            double c[2][2][2];
            bool any_solid = false, any_air = false;
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j)
                    for (int k = 0; k < 2; ++k) {
                        c[i][j][k] = D(double(g[0] + i) * v,
                                       double(g[1] + j) * v,
                                       double(g[2] + k) * v);
                        if (c[i][j][k] > 0) any_solid = true; else any_air = true;
                    }
            if (!any_solid || !any_air) continue;   // no vertex in this cell

            // Centroid of the 12 lattice-edge crossings, in cell-local units.
            double acc[3] = {0, 0, 0};
            int cnt = 0;
            for (int e = 0; e < 3; ++e) {
                const int o1 = (e + 1) % 3, o2 = (e + 2) % 3;
                for (int u = 0; u < 2; ++u)
                    for (int w = 0; w < 2; ++w) {
                        int p0[3], p1[3];
                        p0[e] = 0; p1[e] = 1;
                        p0[o1] = u; p1[o1] = u;
                        p0[o2] = w; p1[o2] = w;
                        const double da = c[p0[0]][p0[1]][p0[2]];
                        const double db = c[p1[0]][p1[1]][p1[2]];
                        if ((da > 0) == (db > 0)) continue;
                        acc[e]  += da / (da - db);
                        acc[o1] += u;
                        acc[o2] += w;
                        ++cnt;
                    }
            }
            if (!cnt) continue;

            double wp[3];
            for (int ax = 0; ax < 3; ++ax)
                wp[ax] = (double(g[ax]) + acc[ax] / cnt) * v;

            BoundaryVert bv;
            bv.a = ca; bv.b = cb;
            bv.px = wp[0]; bv.py = wp[1]; bv.pz = wp[2];

            // Density is positive in solid, so its gradient points INTO the
            // solid; negate for the outward normal (same rule as the mesher).
            double gr[3];
            for (int ax = 0; ax < 3; ++ax) {
                double p[3] = {wp[0], wp[1], wp[2]};
                p[ax] = wp[ax] + v; const double dp = D(p[0], p[1], p[2]);
                p[ax] = wp[ax] - v; const double dm = D(p[0], p[1], p[2]);
                gr[ax] = dp - dm;
            }
            const double len = std::sqrt(gr[0]*gr[0] + gr[1]*gr[1] + gr[2]*gr[2]);
            if (len > 1e-12) {
                bv.nx = float(-gr[0] / len);
                bv.ny = float(-gr[1] / len);
                bv.nz = float(-gr[2] / len);
            } else { bv.nx = 0; bv.ny = 1; bv.nz = 0; }

            // corner_signs describe this cell's face ON THE PLANE -- lattice
            // index `plane` along the normal axis, from BOTH sides. bit index
            // = 2*(b offset) + (a offset).
            uint8_t sg = 0;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    int64_t gl[3];
                    gl[aa] = ca + i; gl[ba] = cb + j; gl[axis] = plane;
                    if (D(double(gl[0]) * v, double(gl[1]) * v, double(gl[2]) * v) > 0)
                        sg |= uint8_t(1u << (j * 2 + i));
                }
            bv.corner_signs = sg;

            // Two materials, so bucket creation order is itself under test.
            bv.material = uint32_t(1 + ((((ca * 3 + cb) % 2) + 2) % 2));
            rec.verts.push_back(bv);
        }
    }
    return rec;   // generated a-major / b-minor, i.e. already sorted by (a,b)
}

// A whole 2:1 pairing, held at a stable address so the WeldSide closures stay
// valid (std::function captures a pointer INTO this object).
struct Pairing {
    int axis = 2;
    int fine_rung = 2, coarse_rung = 1;
    bool neg_is_fine = true;
    FaceRecord fine_rec, coarse_rec;
    WeldSide neg, pos;
};

// plane_fine must be even so the coarse side has a lattice point there.
static std::unique_ptr<Pairing> make_pairing(int axis, int fine_rung, bool neg_is_fine,
                                             int64_t plane_fine, int64_t half_extent,
                                             const DensFn& D) {
    auto p = std::unique_ptr<Pairing>(new Pairing());
    p->axis = axis;
    p->fine_rung = fine_rung;
    p->coarse_rung = fine_rung - 1;
    p->neg_is_fine = neg_is_fine;

    const int64_t cf = half_extent;              // fine cells either side of 0
    const int64_t cc = half_extent / 2;          // same world extent, coarse
    p->fine_rec   = build_record(axis, p->fine_rung, plane_fine,
                                 /*positive_side=*/!neg_is_fine,
                                 -cf, cf, -cf, cf, D);
    p->coarse_rec = build_record(axis, p->coarse_rung, plane_fine / 2,
                                 /*positive_side=*/neg_is_fine,
                                 -cc, cc, -cc, cc, D);

    const FaceRecord* fr = &p->fine_rec;
    const FaceRecord* cr = &p->coarse_rec;
    WeldSide fine_side, coarse_side;
    fine_side.rung   = p->fine_rung;
    fine_side.at     = [fr](int64_t a, int64_t b) { return fr->find(a, b); };
    coarse_side.rung = p->coarse_rung;
    coarse_side.at   = [cr](int64_t a, int64_t b) { return cr->find(a, b); };

    if (neg_is_fine) { p->neg = fine_side; p->pos = coarse_side; }
    else             { p->neg = coarse_side; p->pos = fine_side; }
    return p;
}

// ---------------------------------------------------------------------------
// Small mesh helpers
// ---------------------------------------------------------------------------

static size_t tri_count(const WeldMesh& m) { return m.triangle_count(); }

static bool meshes_identical(const WeldMesh& a, const WeldMesh& b) {
    if (a.buckets.size() != b.buckets.size()) return false;
    for (size_t i = 0; i < a.buckets.size(); ++i) {
        if (a.buckets[i].material  != b.buckets[i].material)  return false;
        if (a.buckets[i].positions != b.buckets[i].positions) return false;
        if (a.buckets[i].normals   != b.buckets[i].normals)   return false;
    }
    return true;
}

// Every position the welder writes must be a verbatim copy of some record
// vertex. If this ever fails the welder has started inventing geometry, which is
// the one thing §4.1 forbids it to do.
static void collect_record_positions(const FaceRecord& r,
                                     std::set<std::array<float, 3>>& out) {
    for (const BoundaryVert& v : r.verts)
        out.insert({float(v.px), float(v.py), float(v.pz)});
}

// Returns the number of emitted positions that are NOT verbatim record vertices.
static size_t foreign_positions(const WeldMesh& m,
                                const std::set<std::array<float, 3>>& known,
                                size_t& checked) {
    size_t foreign = 0;
    checked = 0;
    for (const WeldBucket& b : m.buckets)
        for (size_t i = 0; i + 2 < b.positions.size(); i += 3) {
            ++checked;
            const std::array<float, 3> q = {b.positions[i], b.positions[i+1],
                                            b.positions[i+2]};
            if (!known.count(q)) ++foreign;
        }
    return foreign;
}

// Every triangle's geometric normal must agree with the mean of its three
// corners' stored normals. Slivers with no defined plane are skipped rather than
// guessed at. Returns the count that disagreed.
static int winding_disagreements(const WeldMesh& m, int& checked, double& worst) {
    int wrong = 0;
    checked = 0;
    worst = 1.0;
    for (const WeldBucket& b : m.buckets)
        for (size_t t = 0; t * 9 < b.positions.size(); ++t) {
            const float* P = &b.positions[t * 9];
            const float* N = &b.normals[t * 9];
            const double e1[3] = {P[3]-P[0], P[4]-P[1], P[5]-P[2]};
            const double e2[3] = {P[6]-P[0], P[7]-P[1], P[8]-P[2]};
            const double g[3] = {e1[1]*e2[2] - e1[2]*e2[1],
                                 e1[2]*e2[0] - e1[0]*e2[2],
                                 e1[0]*e2[1] - e1[1]*e2[0]};
            const double gl = std::sqrt(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
            if (gl < 1e-9) continue;
            double mn[3] = {0, 0, 0};
            for (int v = 0; v < 3; ++v)
                for (int c = 0; c < 3; ++c) mn[c] += N[v*3 + c];
            const double ml = std::sqrt(mn[0]*mn[0] + mn[1]*mn[1] + mn[2]*mn[2]);
            if (ml < 1e-6) continue;
            const double dot = (g[0]*mn[0] + g[1]*mn[1] + g[2]*mn[2]) / (gl * ml);
            ++checked;
            if (dot <= 0) ++wrong;
            worst = std::min(worst, dot);
        }
    return wrong;
}

// True if any emitted triangle has two identical corners.
static size_t sliver_count(const WeldMesh& m) {
    size_t slivers = 0;
    for (const WeldBucket& b : m.buckets)
        for (size_t t = 0; t * 9 < b.positions.size(); ++t) {
            const float* P = &b.positions[t * 9];
            for (int i = 0; i < 3; ++i)
                for (int j = i + 1; j < 3; ++j)
                    if (P[i*3+0] == P[j*3+0] && P[i*3+1] == P[j*3+1] &&
                        P[i*3+2] == P[j*3+2]) ++slivers;
        }
    return slivers;
}

int main() {
    printf("=== seam welder (M0-WP3) ===\n");

    // --- 1. equal rungs emit nothing ---------------------------------------
    // Equal-level pairs already meet bitwise through shared density samples
    // (design §4). A weld there would be duplicate surface, not a fix.
    {
        FaceRecord ra = build_record(2, 2, 0, false, -12, 12, -12, 12, dens_sphere);
        FaceRecord rb = build_record(2, 2, 0, true,  -12, 12, -12, 12, dens_sphere);
        WeldSide neg = side_from_record(2, ra), pos = side_from_record(2, rb);
        WeldMesh m; WeldStats s; std::string err;
        const bool ok = weld_face(2, neg, pos, -10, 10, -10, 10, m, s, err);
        CHECK(ok, err.c_str());
        CHECK(m.buckets.empty(), "equal rung: no buckets");
        CHECK(tri_count(m) == 0, "equal rung: no triangles");
        CHECK(s.crossings == 0 && s.quads == 0 && s.tris == 0 &&
              s.missing_landing == 0 && s.sign_conflicts == 0,
              "equal rung: stats all zero");
        printf("[1] equal rung (%zu + %zu record verts) -> %zu tris, crossings %d\n",
               ra.verts.size(), rb.verts.size(), tri_count(m), s.crossings);
    }

    // --- 2. a rung gap of two is rejected ----------------------------------
    // restrict_levels guarantees +/-1 across faces and the fan implements
    // exactly one level (§4.4). Welding a 4:1 face anyway would hide the
    // invariant breaking, which is the failure mode this whole work exists to
    // stop, so it is an error and not a best effort.
    {
        FaceRecord ra = build_record(2, 3, 0, false, -8, 8, -8, 8, dens_sphere);
        FaceRecord rb = build_record(2, 1, 0, true,  -8, 8, -8, 8, dens_sphere);
        WeldSide neg = side_from_record(3, ra), pos = side_from_record(1, rb);
        WeldMesh m; WeldStats s; std::string err;
        const bool ok = weld_face(2, neg, pos, -4, 4, -4, 4, m, s, err);
        CHECK(!ok, "rung gap 2: rejected");
        CHECK(!err.empty(), "rung gap 2: error text set");
        CHECK(tri_count(m) == 0, "rung gap 2: nothing emitted");
        printf("[2] rung gap 2 rejected: \"%s\"\n", err.c_str());

        // ...and the reverse direction, and a gap of two the other way round.
        WeldSide neg2 = side_from_record(1, rb), pos2 = side_from_record(3, ra);
        CHECK(!weld_face(2, neg2, pos2, -4, 4, -4, 4, m, s, err),
              "rung gap -2: rejected too");
    }

    // --- 3. a 2:1 pair over a flat surface ---------------------------------
    // Flat because the landing-site question is then provable (see dens_flat).
    // Asserts: every crossing produced geometry, missing_landing is zero, and
    // every emitted position is one of the two records' own vertices.
    {
        auto p = make_pairing(/*axis=*/2, /*fine_rung=*/2, /*neg_is_fine=*/true,
                              /*plane_fine=*/0, /*half_extent=*/24, dens_flat);
        WeldMesh m; WeldStats s; std::string err;
        CHECK(weld_face(p->axis, p->neg, p->pos, -20, 20, -20, 20, m, s, err),
              err.c_str());
        CHECK(s.crossings > 0, "flat 2:1: found crossings at all");
        CHECK(s.crossings == s.accounted(), "flat 2:1: every crossing accounted");
        CHECK(s.missing_landing == 0, "flat 2:1: no missing landing sites");
        CHECK(s.caps == 0 && s.missing_coarse_pair == 0 && s.missing_fine == 0,
              "flat 2:1: nothing absent, so no caps and no drops");
        CHECK(s.degenerate == 0, "flat 2:1: nothing fully degenerate");
        CHECK(s.sign_conflicts == 0, "flat 2:1: no sign conflicts");
        CHECK(s.crossings == s.quads + s.tris,
              "flat 2:1: every crossing edge emitted a triangle or a quad");

        std::set<std::array<float, 3>> known;
        collect_record_positions(p->fine_rec, known);
        collect_record_positions(p->coarse_rec, known);
        size_t checked = 0;
        const size_t foreign = foreign_positions(m, known, checked);
        CHECK(foreign == 0, "flat 2:1: welder synthesized no positions");
        printf("[3] flat 2:1 (fine rung 2 / coarse rung 1): crossings %d = quads %d"
               " + tris %d, missing %d, %zu tris, %zu emitted verts all from the"
               " %zu+%zu record verts\n",
               s.crossings, s.quads, s.tris, s.missing_landing, tri_count(m),
               checked, p->fine_rec.verts.size(), p->coarse_rec.verts.size());
    }

    // --- 4. winding ---------------------------------------------------------
    // The derivation in seam_weld.h says the quad's normal must run from solid
    // to air. Here it is cross-checked the only way that counts: against the
    // averaged vertex normals the bake already computed from the density
    // gradient. Swept over all three plane axes AND both fine-side choices,
    // because the reverse_frame table has a different entry for each.
    {
        int total_tris = 0, checked = 0, wrong = 0;
        double worst = 1.0;
        for (int axis = 0; axis < 3; ++axis)
            for (int which = 0; which < 2; ++which) {
                auto p = make_pairing(axis, 2, /*neg_is_fine=*/which == 0, 0, 24,
                                      dens_sphere);
                WeldMesh m; WeldStats s; std::string err;
                CHECK(weld_face(axis, p->neg, p->pos, -20, 20, -20, 20, m, s, err),
                      err.c_str());
                CHECK(s.crossings > 0, "winding sweep: crossings found");
                total_tris += int(tri_count(m));
                int c = 0; double w = 1.0;
                wrong += winding_disagreements(m, c, w);
                checked += c;
                worst = std::min(worst, w);
            }
        CHECK(wrong == 0, "winding: every triangle's geometric normal agrees with "
                          "its corners' averaged normals");
        printf("[4] winding sweep, 3 axes x 2 fine sides: %d tris, %d testable,"
               " %d backwards, worst cos = %.4f\n",
               total_tris, checked, wrong, worst);
    }

    // --- 5. the 2:1 collapse ratio -----------------------------------------
    // The fan is not a code path -- it is what happens when both fine cells
    // beside an edge map to ONE coarse cell, which happens exactly when the
    // OTHER tangential index is odd. Oracle computed straight from the analytic
    // field, so this checks the claim rather than the implementation's opinion
    // of itself.
    {
        const int axis = 2, fine_rung = 2;
        auto p = make_pairing(axis, fine_rung, /*neg_is_fine=*/false, 0, 24,
                              dens_sphere);
        WeldMesh m; WeldStats s; std::string err;
        CHECK(weld_face(axis, p->neg, p->pos, -20, 20, -20, 20, m, s, err),
              err.c_str());

        const double v = rung_voxel(fine_rung);
        int aa = 0, ba = 0; axis_tangents(axis, aa, ba);
        auto plane_sign = [&](int64_t A, int64_t B) {
            int64_t g[3]; g[aa] = A; g[ba] = B; g[axis] = 0;
            return dens_sphere(double(g[0])*v, double(g[1])*v, double(g[2])*v) > 0;
        };
        int exp_cross = 0, exp_tris = 0, exp_quads = 0;
        for (int64_t A = -20; A < 20; ++A)
            for (int64_t B = -20; B < 20; ++B) {
                const bool s00 = plane_sign(A, B);
                if (s00 != plane_sign(A + 1, B)) {          // a-edge: cells vary in b
                    ++exp_cross; ((B & 1) ? exp_tris : exp_quads)++;
                }
                if (s00 != plane_sign(A, B + 1)) {          // b-edge: cells vary in a
                    ++exp_cross; ((A & 1) ? exp_tris : exp_quads)++;
                }
            }
        CHECK(s.crossings == exp_cross, "collapse: crossing count matches the field");
        CHECK(s.missing_landing == 0 && s.degenerate == 0,
              "collapse: all crossings landed (ratio assertion is only meaningful then)");
        CHECK(s.tris == exp_tris, "collapse: odd-index crossings became triangles");
        CHECK(s.quads == exp_quads, "collapse: even-index crossings stayed quads");
        printf("[5] collapse parity: crossings %d (oracle %d), tris %d (oracle %d),"
               " quads %d (oracle %d) -> %.1f%% collapsed to the 2:1 fan\n",
               s.crossings, exp_cross, s.tris, exp_tris, s.quads, exp_quads,
               100.0 * double(s.tris) / double(s.crossings ? s.crossings : 1));
    }

    // --- 6. topology disagreement: caps and drops, told apart ---------------
    // Built by deleting one coarse vertex, which is exactly the shape of the real
    // case (the coarse cell covering a fine-only sign change produced none). One
    // deletion produces BOTH residue classes at once, which is why this fixture
    // is worth more than two separate ones:
    //
    //   crossings whose two fine cells map to DIFFERENT coarse cells (previously
    //     quads) now have exactly one coarse corner left -> CAP, one triangle;
    //   crossings whose two fine cells both map to the DELETED cell (previously
    //     the 2:1 aliasing collapse, one triangle) now have none -> dropped as
    //     missing_coarse_pair.
    //
    // That makes the whole outcome arithmetically determined from the baseline,
    // so this asserts exact identities rather than inequalities.
    {
        auto p = make_pairing(2, 2, /*neg_is_fine=*/true, 0, 24, dens_sphere);
        WeldMesh m0; WeldStats s0; std::string err;
        CHECK(weld_face(2, p->neg, p->pos, -20, 20, -20, 20, m0, s0, err),
              err.c_str());
        CHECK(s0.missing_landing == 0, "residue: baseline has no missing landings");
        CHECK(s0.caps == 0, "residue: baseline needs no caps");

        const size_t victim = p->coarse_rec.verts.size() / 2;
        const BoundaryVert gone = p->coarse_rec.verts[victim];
        p->coarse_rec.verts.erase(p->coarse_rec.verts.begin() + long(victim));

        WeldMesh m1; WeldStats s1;
        CHECK(weld_face(2, p->neg, p->pos, -20, 20, -20, 20, m1, s1, err),
              err.c_str());
        CHECK(s1.crossings == s0.crossings,
              "residue: signs come from the fine side, so crossings are unchanged");
        CHECK(s1.crossings == s1.accounted(), "residue: every crossing accounted");
        CHECK(s1.missing_landing == s1.missing_coarse_pair + s1.missing_fine,
              "residue: the sub-buckets partition missing_landing");
        CHECK(s1.missing_fine == 0, "residue: the fine record is untouched");
        CHECK(s1.caps > 0, "residue: the recoverable crossings were capped");
        CHECK(s1.missing_coarse_pair > 0,
              "residue: the aliased crossings had nothing to cap onto");
        CHECK(s1.caps <= s1.tris, "residue: caps are a subset of tris");
        CHECK(s1.degenerate == 0, "residue: nothing counted as degenerate");

        // The exact arithmetic described above.
        CHECK(s1.quads == s0.quads - s1.caps,
              "residue: every cap came out of a former quad");
        CHECK(s1.tris == s0.tris - s1.missing_coarse_pair + s1.caps,
              "residue: tris lost the aliased drops and gained the caps");
        CHECK(tri_count(m1) == tri_count(m0) - size_t(s1.caps)
                                             - size_t(s1.missing_coarse_pair),
              "residue: each cap costs one triangle, each drop costs one");
        CHECK(sliver_count(m1) == 0,
              "residue: no emitted triangle has a repeated corner");
        printf("[6] residue: removed coarse cell (%lld,%lld) -> %d caps + %d"
               " coarse-pair drops (+%d fine); tris %zu -> %zu; quads %d -> %d,"
               " tris %d -> %d\n",
               (long long)gone.a, (long long)gone.b, s1.caps,
               s1.missing_coarse_pair, s1.missing_fine, tri_count(m0),
               tri_count(m1), s0.quads, s1.quads, s0.tris, s1.tris);
    }

    // --- 6b. the collapsed cap, isolated ------------------------------------
    // §4.1 offers "a collapsed cap or drops the sliver"; the first revision of
    // the welder only ever dropped, and M0-WP6's scans measured the cost as a
    // full ~1-voxel column (7-14 m at the coarse levels) rather than the
    // sub-fine-voxel defect §4.1 predicted. This isolates ONE such crossing --
    // region narrowed to a single anchor -- and checks the cap on its own terms:
    // a triangle is emitted, all three of its corners are record vertices (the
    // cap must not become the one place the welder invents geometry), and its
    // winding still agrees with the averaged vertex normals even though the
    // collapse arrived by absence rather than by index aliasing.
    {
        auto p = make_pairing(2, 2, /*neg_is_fine=*/true, 0, 24, dens_sphere);
        std::string err;

        // Find an anchor whose b-edge crosses AND whose two coarse cells are
        // distinct (a == even, so floor_div2 does not alias them) -- i.e. one
        // that is a quad today and can therefore become a cap.
        int64_t hitA = 0, hitB = 0; bool found = false;
        const double v = rung_voxel(2);
        auto plane_sign = [&](int64_t A, int64_t B) {
            return dens_sphere(double(A) * v, double(B) * v, 0.0) > 0;
        };
        for (int64_t A = -18; A < 18 && !found; ++A)
            for (int64_t B = -18; B < 18 && !found; ++B) {
                if ((A & 1) != 0) continue;                       // would alias
                if (plane_sign(A, B) == plane_sign(A, B + 1)) continue;  // b-edge
                if (plane_sign(A, B) != plane_sign(A + 1, B)) continue;  // a-edge
                if (!p->coarse_rec.find(floor_div2(A - 1), floor_div2(B))) continue;
                if (!p->coarse_rec.find(floor_div2(A),     floor_div2(B))) continue;
                hitA = A; hitB = B; found = true;
            }
        CHECK(found, "cap: located a crossing with two distinct coarse corners");

        // The anchor carries exactly ONE crossing (the a-edge was required not to
        // cross), so every count below is a statement about that single quad.
        WeldMesh before; WeldStats sb;
        CHECK(weld_face(2, p->neg, p->pos, hitA, hitA + 1, hitB, hitB + 1,
                        before, sb, err), err.c_str());
        CHECK(sb.crossings == 1, "cap: the anchor is isolated to one crossing");
        CHECK(sb.quads == 1 && before.triangle_count() == 2,
              "cap: it is a full quad before the deletion");
        CHECK(sb.missing_landing == 0 && sb.caps == 0,
              "cap: the isolated anchor lands cleanly before the deletion");

        // Delete exactly one of the two coarse cells that crossing lands on.
        const int64_t ka = floor_div2(hitA - 1), kb = floor_div2(hitB);
        const BoundaryVert* target = p->coarse_rec.find(ka, kb);
        CHECK(target != nullptr, "cap: the victim coarse cell exists");
        const size_t idx = size_t(target - &p->coarse_rec.verts[0]);
        p->coarse_rec.verts.erase(p->coarse_rec.verts.begin() + long(idx));

        WeldMesh after; WeldStats sa;
        CHECK(weld_face(2, p->neg, p->pos, hitA, hitA + 1, hitB, hitB + 1,
                        after, sa, err), err.c_str());
        CHECK(sa.crossings == 1, "cap: the crossing is still found");
        CHECK(sa.caps == 1 && sa.tris == 1 && sa.quads == 0,
              "cap: the crossing was capped, not dropped");
        CHECK(sa.missing_landing == 0, "cap: nothing was dropped");
        CHECK(after.triangle_count() == 1, "cap: exactly one triangle was emitted");

        std::set<std::array<float, 3>> known;
        collect_record_positions(p->fine_rec, known);
        collect_record_positions(p->coarse_rec, known);
        size_t checked = 0;
        CHECK(foreign_positions(after, known, checked) == 0,
              "cap: all three corners are record vertices -- nothing synthesized");
        CHECK(sliver_count(after) == 0, "cap: the capped triangle is not a sliver");

        int wchecked = 0; double worst = 1.0;
        const int wrong = winding_disagreements(after, wchecked, worst);
        CHECK(wrong == 0, "cap: capped triangles wind the same way as the rest");
        CHECK(wchecked >= 1, "cap: the winding check actually looked at something");
        printf("[6b] cap: anchor (%lld,%lld), removed coarse cell (%lld,%lld) ->"
               " caps %d, missing %d, %zu -> %zu tris, %zu corners all from"
               " records, winding %d/%d ok (worst cos %.4f)\n",
               (long long)hitA, (long long)hitB, (long long)ka, (long long)kb,
               sa.caps, sa.missing_landing, before.triangle_count(),
               after.triangle_count(), checked, wchecked - wrong, wchecked, worst);
    }

    // --- 7. corner adjacency: one side backed by FOUR tiles -----------------
    // This is the case that killed the previous, baked design: a band quad near
    // a tile corner needs the OTHER axis's ghost vertices, so per-face baked
    // variants are not independent and the combinations explode. Here the welder
    // simply asks the caller's closure, which happens to be answering out of a
    // 2x2 block of tiles -- including plane edges whose two cells come from
    // different tiles, and a lattice corner whose four surrounding cells come
    // from four different tiles. The output must be INDISTINGUISHABLE from the
    // single-tile run, not merely gap-free.
    {
        const int axis = 2, fine_rung = 2;
        const int64_t split = 0;    // tile boundaries in BOTH tangential directions

        FaceRecord whole = build_record(axis, fine_rung, 0, false, -24, 24, -24, 24,
                                        dens_sphere);
        FaceRecord q[2][2];         // [a < split][b < split] -> [0]=low, [1]=high
        q[0][0] = build_record(axis, fine_rung, 0, false, -24, split, -24, split, dens_sphere);
        q[0][1] = build_record(axis, fine_rung, 0, false, -24, split, split, 24, dens_sphere);
        q[1][0] = build_record(axis, fine_rung, 0, false, split, 24, -24, split, dens_sphere);
        q[1][1] = build_record(axis, fine_rung, 0, false, split, 24, split, 24, dens_sphere);
        FaceRecord coarse = build_record(axis, fine_rung - 1, 0, true, -12, 12, -12, 12,
                                         dens_sphere);
        CHECK(q[0][0].verts.size() + q[0][1].verts.size() + q[1][0].verts.size() +
              q[1][1].verts.size() == whole.verts.size(),
              "corner: the 2x2 split covers exactly the same cells");

        WeldSide coarse_side = side_from_record(fine_rung - 1, coarse);

        WeldSide one = side_from_record(fine_rung, whole);
        WeldMesh m_one; WeldStats s_one; std::string err;
        CHECK(weld_face(axis, one, coarse_side, -20, 20, -20, 20, m_one, s_one, err),
              err.c_str());

        WeldSide four;
        four.rung = fine_rung;
        const FaceRecord (*tiles)[2] = q;
        four.at = [tiles](int64_t a, int64_t b) {
            return tiles[a < split ? 0 : 1][b < split ? 0 : 1].find(a, b);
        };
        WeldMesh m_four; WeldStats s_four;
        CHECK(weld_face(axis, four, coarse_side, -20, 20, -20, 20, m_four, s_four, err),
              err.c_str());

        CHECK(meshes_identical(m_one, m_four),
              "corner: four tiles weld byte-identically to one");
        CHECK(s_four.crossings == s_one.crossings && s_four.quads == s_one.quads &&
              s_four.tris == s_one.tris &&
              s_four.missing_landing == s_one.missing_landing,
              "corner: identical stats");
        CHECK(s_four.sign_conflicts == 0,
              "corner: the tiles agree on the shared lattice rows and columns");
        CHECK(s_four.missing_landing == 0, "corner: no gap at the tile-tile borders");

        // Count the crossings that genuinely straddle a tile border, so a pass
        // here cannot be vacuous: a b-edge anchored at a == split uses cell
        // (split-1, B) and (split, B), i.e. two different tiles; an a-edge
        // anchored at b == split does the same across the other border.
        const double v = rung_voxel(fine_rung);
        int aa = 0, ba = 0; axis_tangents(axis, aa, ba);
        auto plane_sign = [&](int64_t A, int64_t B) {
            int64_t g[3]; g[aa] = A; g[ba] = B; g[axis] = 0;
            return dens_sphere(double(g[0])*v, double(g[1])*v, double(g[2])*v) > 0;
        };
        int straddling = 0;
        for (int64_t B = -20; B < 20; ++B)
            if (plane_sign(split, B) != plane_sign(split, B + 1)) ++straddling;
        for (int64_t A = -20; A < 20; ++A)
            if (plane_sign(A, split) != plane_sign(A + 1, split)) ++straddling;
        CHECK(straddling > 0, "corner: the boundary rows carry crossings");
        printf("[7] corner adjacency: 2x2 split at a=b=%lld into %zu/%zu/%zu/%zu"
               " verts; %d crossings, %d straddling a tile border; output"
               " byte-identical to the single-tile weld (%zu tris)\n",
               (long long)split, q[0][0].verts.size(), q[0][1].verts.size(),
               q[1][0].verts.size(), q[1][1].verts.size(),
               s_four.crossings, straddling, tri_count(m_four));
    }

    // --- 7b. sign conflicts are DETECTED, not smoothed over -----------------
    // Two cells sharing a plane-edge endpoint must agree about its sign; that
    // agreement is what lets four sparse bits per entry replace a dense sign
    // image. A disagreement means the two records were built from different
    // samples, which is a bug upstream -- so the counter has to actually fire.
    // Without this the field is untested code that always reads zero.
    {
        auto p = make_pairing(2, 2, /*neg_is_fine=*/true, 0, 24, dens_sphere);
        WeldMesh m; WeldStats clean; std::string err;
        CHECK(weld_face(2, p->neg, p->pos, -20, 20, -20, 20, m, clean, err),
              err.c_str());
        CHECK(clean.sign_conflicts == 0, "conflicts: clean records have none");

        BoundaryVert& victim = p->fine_rec.verts[p->fine_rec.verts.size() / 2];
        const uint8_t was = victim.corner_signs;
        victim.corner_signs = uint8_t(was ^ 0x0F);

        WeldStats dirty;
        CHECK(weld_face(2, p->neg, p->pos, -20, 20, -20, 20, m, dirty, err),
              err.c_str());
        CHECK(dirty.sign_conflicts > 0,
              "conflicts: a corrupted corner_signs byte is caught");
        printf("[7b] sign conflicts: cell (%lld,%lld) signs 0x%X -> 0x%X gives"
               " %d conflicts (was %d)\n",
               (long long)victim.a, (long long)victim.b, was, victim.corner_signs,
               dirty.sign_conflicts, clean.sign_conflicts);
    }

    // --- 8. determinism -----------------------------------------------------
    {
        auto p = make_pairing(1, 2, /*neg_is_fine=*/false, 0, 24, dens_sphere);
        WeldMesh a, b; WeldStats sa, sb; std::string err;
        CHECK(weld_face(1, p->neg, p->pos, -18, 18, -18, 18, a, sa, err), err.c_str());
        CHECK(weld_face(1, p->neg, p->pos, -18, 18, -18, 18, b, sb, err), err.c_str());
        CHECK(meshes_identical(a, b), "determinism: byte-identical geometry");
        CHECK(a.buckets.size() > 1, "determinism: more than one material bucket");
        CHECK(std::memcmp(&sa, &sb, sizeof(WeldStats)) == 0,
              "determinism: identical stats");
        size_t bytes = 0;
        for (const WeldBucket& bk : a.buckets)
            bytes += (bk.positions.size() + bk.normals.size()) * sizeof(float);
        printf("[8] determinism: %zu buckets, %zu tris, %zu bytes, identical twice\n",
               a.buckets.size(), tri_count(a), bytes);
    }

    // --- 9. the overlap band (M0-WP7) ---------------------------------------
    //
    // The band is the second layer of the seam: geometry the mesher sampled
    // beyond its own ownership and did NOT put in its mesh, handed to the welder
    // so the drawn pair can decide whether to show it. Four things are the
    // welder's responsibility and are gated here.
    //
    //   VERBATIM. The welder copies; it does not reconstruct, filter, re-wind or
    //     re-material. Anything less and the band would stop being "the
    //     triangles the mesher would have emitted" and start being a second
    //     guess at them.
    //   REBASED, ONCE. Band positions are world-absolute doubles for the same
    //     reason BoundaryVert's are; the welder subtracts its own origin, which
    //     is the only place a weld spanning two differently-based tiles can do
    //     it.
    //   SUPPRESSED AT EQUAL RUNGS. The whole reason this cannot live in the
    //     mesh: there it would be duplicate coplanar surface on every
    //     equal-level seam, which is exactly what the retired `edge_mask` gate
    //     existed to prevent -- at the price of baking a neighbour guess.
    //   INERT WHEN ABSENT. A null band leaves the welder bit-for-bit what it was
    //     before WP7, so a caller that has not been taught about bands is not
    //     silently changed by their arrival.
    {
        auto p = make_pairing(2, 2, /*neg_is_fine=*/false, 0, 20, dens_sphere);

        // Two buckets, three triangles, positions far from the origin so the
        // rebase has something to cancel.
        OverlapBand band;
        band.buckets.resize(2);
        band.buckets[0].material = 7;
        band.buckets[1].material = 9;
        const double base = 12345.0;
        for (int t = 0; t < 3; ++t) {
            OverlapBucket& bk = band.buckets[t == 2 ? 1 : 0];
            for (int v = 0; v < 3; ++v) {
                bk.positions.push_back(base + t * 10 + v);
                bk.positions.push_back(base + t * 10 + v + 100);
                bk.positions.push_back(base + t * 10 + v + 200);
                bk.normals.push_back(float(v));
                bk.normals.push_back(float(t));
                bk.normals.push_back(1.0f);
            }
        }

        // Baseline: the same weld with no band at all.
        WeldMesh plain; WeldStats sp; std::string err;
        CHECK(weld_face(2, p->neg, p->pos, -14, 14, -14, 14, plain, sp, err),
              err.c_str());
        CHECK(sp.band_tris == 0, "[9] a null band emits nothing and counts nothing");

        // Cross-level, band on the FINE side (which here is `pos`).
        WeldSide fine_with = p->pos;
        fine_with.band = &band;
        WeldMesh withb; WeldStats sw;
        withb.origin_x = 1000.0; withb.origin_y = 2000.0; withb.origin_z = 3000.0;
        CHECK(weld_face(2, p->neg, fine_with, -14, 14, -14, 14, withb, sw, err),
              err.c_str());
        CHECK(sw.band_tris == 3, "[9] all three band triangles are emitted");
        CHECK(sw.crossings == sp.crossings && sw.quads == sp.quads &&
              sw.tris == sp.tris && sw.missing_landing == sp.missing_landing,
              "[9] the band does not disturb the fan's accounting by one crossing");
        CHECK(sw.crossings == sw.accounted(),
              "[9] band_tris stays OUT of the four-way partition");
        CHECK(withb.triangle_count() == plain.triangle_count() + 3,
              "[9] the band supplements the fan -- it adds, it does not replace");

        // Verbatim + rebased: every band vertex must appear in the output at
        // exactly (world - origin), with its normal and in its own material.
        size_t found = 0;
        for (size_t bi = 0; bi < band.buckets.size(); ++bi) {
            const OverlapBucket& src = band.buckets[bi];
            const WeldBucket* dst = nullptr;
            for (const WeldBucket& b : withb.buckets)
                if (b.material == src.material) dst = &b;
            CHECK(dst != nullptr, "[9] the band's material bucket exists in the weld");
            if (!dst) continue;
            for (size_t i = 0; i + 2 < src.positions.size(); i += 3) {
                const float wx = float(src.positions[i + 0] - withb.origin_x);
                const float wy = float(src.positions[i + 1] - withb.origin_y);
                const float wz = float(src.positions[i + 2] - withb.origin_z);
                bool hit = false;
                for (size_t j = 0; j + 2 < dst->positions.size(); j += 3)
                    if (dst->positions[j] == wx && dst->positions[j + 1] == wy &&
                        dst->positions[j + 2] == wz &&
                        dst->normals[j]     == src.normals[i] &&
                        dst->normals[j + 1] == src.normals[i + 1] &&
                        dst->normals[j + 2] == src.normals[i + 2]) { hit = true; break; }
                if (hit) ++found;
            }
        }
        CHECK(found == 9,
              "[9] every band vertex is emitted verbatim -- position rebased by "
              "the mesh origin and nothing else, normal and material untouched");

        // Equal rungs: the SAME band, handed to a pair that already meets
        // bitwise, must produce nothing.
        {
            WeldSide eq_neg = p->pos, eq_pos = p->pos;
            eq_neg.band = &band; eq_pos.band = &band;
            WeldMesh eq; WeldStats se;
            CHECK(weld_face(2, eq_neg, eq_pos, -14, 14, -14, 14, eq, se, err),
                  err.c_str());
            CHECK(eq.buckets.empty() && se.band_tris == 0 && se.crossings == 0,
                  "[9] equal rungs suppress the band as well as the fan: those "
                  "tiles meet through shared samples and the band would be "
                  "duplicate coplanar surface");
        }

        // The band on the COARSE side is ignored: the mesher never puts one on a
        // +x/+z face, and the welder must not invent a use for one.
        {
            WeldSide coarse_with = p->neg;
            coarse_with.band = &band;
            WeldMesh cm; WeldStats sc;
            CHECK(weld_face(2, coarse_with, p->pos, -14, 14, -14, 14, cm, sc, err),
                  err.c_str());
            CHECK(sc.band_tris == 0 && meshes_identical(cm, plain),
                  "[9] only the FINE side's band is drawn");
        }

        printf("[9] overlap band: %zu tris without, %zu with (%d band, %zu of 9 "
               "vertices verbatim after rebase); equal-rung and coarse-side "
               "bands both suppressed\n",
               plain.triangle_count(), withb.triangle_count(), sw.band_tris, found);
    }

    printf("=== seam welder ");
    return check_summary();
}
