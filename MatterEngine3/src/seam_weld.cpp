// seam_weld.cpp — runtime cross-level seam welder. See seam_weld.h for the
// algorithm, the winding derivation, and why this file has no engine includes.

#include "seam_weld.h"

#include <algorithm>

namespace seam {

WeldSide side_from_record(int rung, const FaceRecord& rec) {
    WeldSide s;
    s.rung = rung;
    const FaceRecord* p = &rec;
    s.at = [p](int64_t a, int64_t b) -> const BoundaryVert* { return p->find(a, b); };
    return s;
}

namespace {

WeldBucket& bucket_for(WeldMesh& m, uint32_t material) {
    for (WeldBucket& b : m.buckets)
        if (b.material == material) return b;
    m.buckets.push_back(WeldBucket{});
    m.buckets.back().material = material;
    return m.buckets.back();
}

// The welder NEVER synthesizes a position or a normal: both come verbatim from a
// record entry. That is what makes the band watertight against the tiles rather
// than merely close to them, and the test suite gates it by checking every
// emitted position against the two records' vertex sets.
void push_tri(const WeldMesh& m, WeldBucket& b,
              const BoundaryVert& v0, const BoundaryVert& v1, const BoundaryVert& v2) {
    const BoundaryVert* v[3] = {&v0, &v1, &v2};
    for (int i = 0; i < 3; ++i) {
        b.positions.push_back(float(v[i]->px - m.origin_x));
        b.positions.push_back(float(v[i]->py - m.origin_y));
        b.positions.push_back(float(v[i]->pz - m.origin_z));
    }
    for (int i = 0; i < 3; ++i) {
        b.normals.push_back(v[i]->nx);
        b.normals.push_back(v[i]->ny);
        b.normals.push_back(v[i]->nz);
    }
}

} // namespace

bool weld_face(int face_axis,
               const WeldSide& neg, const WeldSide& pos,
               int64_t region_a0, int64_t region_a1,
               int64_t region_b0, int64_t region_b1,
               WeldMesh& out, WeldStats& stats, std::string& err) {
    out.clear_geometry();
    stats = WeldStats{};
    err.clear();

    if (face_axis < 0 || face_axis > 2) {
        err = "seam_weld: face_axis must be 0, 1 or 2";
        return false;
    }
    const int diff = neg.rung - pos.rung;
    if (diff > 1 || diff < -1) {
        err = "seam_weld: rung difference exceeds one level (restrict_levels "
              "guarantees +/-1 across faces; the fan implements exactly one)";
        return false;
    }
    // Equal rungs already meet bitwise through shared samples -- nothing to do,
    // and emitting anything here would be duplicate surface, not a fix.
    if (diff == 0) return true;

    if (!neg.at || !pos.at) {
        err = "seam_weld: side lookup is empty";
        return false;
    }

    const bool neg_is_fine = neg.rung > pos.rung;
    const WeldSide& fine = neg_is_fine ? neg : pos;

    // ---- The fine side's overlap band (M0-WP7) ------------------------------
    //
    // Copied verbatim -- rebased against this mesh's origin and nothing else.
    // The welder does not reconstruct, filter or re-wind a band: the mesher
    // already emitted exactly the triangles it would have emitted with ownership
    // extended into those columns, with the mesher's own winding and materials,
    // which is what makes this a faithful revival of the retired `reach` rather
    // than a second guess at it.
    //
    // It SUPPLEMENTS the fan below rather than replacing it, and that is a
    // measured choice, not a hedge. The two fail in different places: the fan
    // stops where the coarse side has no vertex, and that is precisely the
    // `missing_coarse_pair` residue the band exists to cover; the band is raw
    // fine-resolution surface, so it deviates from the coarse one by the field's
    // curvature over a coarse voxel and cannot be the exact join the fan is.
    // seam_integration_tests.cpp scans all three combinations (mesh+fan,
    // mesh+band, mesh+fan+band) and reports each, so the claim is checkable
    // rather than asserted.
    //
    // Only the FINE side's band is consulted. The coarse side of a plane is the
    // tile to its -x/-z, so the plane is that tile's +x/+z face -- a face the
    // mesher never puts a band on, because the [1..n] rule already makes the
    // +side tile the one that bridges.
    if (fine.band) {
        for (const OverlapBucket& src : fine.band->buckets) {
            const size_t n = src.positions.size() / 9;
            if (!n) continue;
            WeldBucket& dst = bucket_for(out, src.material);
            for (size_t i = 0; i < n * 9; i += 3) {
                dst.positions.push_back(float(src.positions[i + 0] - out.origin_x));
                dst.positions.push_back(float(src.positions[i + 1] - out.origin_y));
                dst.positions.push_back(float(src.positions[i + 2] - out.origin_z));
            }
            dst.normals.insert(dst.normals.end(),
                               src.normals.begin(), src.normals.begin() + n * 9);
            stats.band_tris += int(n);
        }
    }

    // Resolve a FINE-rung tangential cell on one side. The coarse side is asked
    // about floor_div2 of the fine index -- and that is the entire 2:1 fan: two
    // fine cells that map to one coarse cell resolve to one vertex, and the quad
    // below degenerates to a triangle on its own.
    const auto resolve = [](const WeldSide& s, bool is_fine,
                            int64_t a, int64_t b) -> const BoundaryVert* {
        return is_fine ? s.at(a, b) : s.at(floor_div2(a), floor_div2(b));
    };

    // Base cyclic order of the four cells around a plane edge, as (T, side)
    // pairs where T is the OTHER tangential index (see seam_weld.h):
    //   0: (T-1, neg)   1: (T, neg)   2: (T, pos)   3: (T-1, pos)
    // Triangulated (0,1,2) + (0,2,3); `flip` swaps 1 and 3, which reverses the
    // winding while keeping the 0-2 diagonal -- exactly terrain_mesher's
    // emit_quad.
    for (int64_t A = region_a0; A < region_a1; ++A) {
        for (int64_t B = region_b0; B < region_b1; ++B) {
            for (int dir = 0; dir < 2; ++dir) {
                const bool along_a = (dir == 0);

                // The two tangential cells adjacent to this edge.
                int64_t a_lo, b_lo, a_hi, b_hi;     // "lo" = the T-1 cell
                if (along_a) { a_lo = A;     b_lo = B - 1; a_hi = A; b_hi = B; }
                else         { a_lo = A - 1; b_lo = B;     a_hi = A; b_hi = B; }

                // Endpoint signs, from the FINE side only: the coarse side has no
                // sample at odd fine lattice points. corner_signs bits are
                //   0:(a,b)  1:(a+1,b)  2:(a,b+1)  3:(a+1,b+1)
                // so for an a-edge anchored at (A,B) the low endpoint (A,B) is
                // bit 0 of cell (A,B) and bit 2 of cell (A,B-1); the high endpoint
                // (A+1,B) is bit 1 and bit 3 respectively. For a b-edge the pairs
                // are (0,2) on cell (A,B) and (1,3) on cell (A-1,B).
                const int bit_lo_hi_cell = 0;
                const int bit_hi_hi_cell = along_a ? 1 : 2;
                const int bit_lo_lo_cell = along_a ? 2 : 1;
                const int bit_hi_lo_cell = 3;

                const BoundaryVert* f_hi = fine.at(a_hi, b_hi);
                const BoundaryVert* f_lo = fine.at(a_lo, b_lo);

                int s0 = -1, s1 = -1;   // sign at the low / high endpoint, 1 = solid
                if (f_hi) {
                    s0 = (f_hi->corner_signs >> bit_lo_hi_cell) & 1;
                    s1 = (f_hi->corner_signs >> bit_hi_hi_cell) & 1;
                }
                if (f_lo) {
                    const int t0 = (f_lo->corner_signs >> bit_lo_lo_cell) & 1;
                    const int t1 = (f_lo->corner_signs >> bit_hi_lo_cell) & 1;
                    if (s0 >= 0) {
                        // Both cells see the same two lattice points. Disagreement
                        // means the two records were built from different samples
                        // -- a bug upstream, never something to paper over.
                        if (t0 != s0 || t1 != s1) ++stats.sign_conflicts;
                    } else {
                        s0 = t0; s1 = t1;
                    }
                }
                // Neither adjacent fine cell is in the record. A crossing edge
                // forces a sign change in BOTH its adjacent cells, so both would
                // be present; absence therefore means no crossing here.
                if (s0 < 0) continue;
                if (s0 == s1) continue;      // no sign change -> no surface here
                ++stats.crossings;

                const BoundaryVert* q[4];
                q[0] = resolve(neg, neg_is_fine,  a_lo, b_lo);
                q[1] = resolve(neg, neg_is_fine,  a_hi, b_hi);
                q[2] = resolve(pos, !neg_is_fine, a_hi, b_hi);
                q[3] = resolve(pos, !neg_is_fine, a_lo, b_lo);

                // §4.1's residue, split three ways by what is actually absent --
                // the three cases have different meanings and different fixes, so
                // one counter for all of them cannot be acted on. See the long
                // note in seam_weld.h.
                const int fine_i0   = neg_is_fine ? 0 : 2;
                const int fine_i1   = neg_is_fine ? 1 : 3;
                const int coarse_i0 = neg_is_fine ? 2 : 0;
                const int coarse_i1 = neg_is_fine ? 3 : 1;

                // A fine corner is absent: the cell belongs to a tile the
                // caller's lookup does not cover. Capping here would stretch
                // geometry over ground a tile is about to occupy.
                if (!q[fine_i0] || !q[fine_i1]) {
                    ++stats.missing_fine;
                    ++stats.missing_landing;
                    continue;
                }
                // Both coarse corners absent: a feature smaller than a coarse
                // voxel, with nothing on the coarse side to land on at all.
                if (!q[coarse_i0] && !q[coarse_i1]) {
                    ++stats.missing_coarse_pair;
                    ++stats.missing_landing;
                    continue;
                }
                // Exactly one absent -> COLLAPSED CAP (§4.1). Duplicate the
                // present coarse corner into the absent slot. Everything below is
                // then unchanged: the dedup turns [a,b,c,c] into one triangle,
                // which is precisely what the floor_div2 aliasing collapse
                // already produces. Note this MUST precede the flip -- the flip
                // swaps slots 1 and 3 by cell position, so substituting after it
                // would fill the wrong corner.
                bool capped = false;
                if (!q[coarse_i0]) { q[coarse_i0] = q[coarse_i1]; capped = true; }
                else if (!q[coarse_i1]) { q[coarse_i1] = q[coarse_i0]; capped = true; }

                // Material: flat-sample the COARSE side (§4.2 v1) -- welds sit
                // exactly where the LOD ladder already drops fidelity. Index 0 is
                // a neg-side corner and index 2 a pos-side one, and neither moves
                // under the flip below, so this is order-independent.
                const uint32_t material = (neg_is_fine ? q[2] : q[0])->material;

                // The frame table. For an edge along `a`, the four cells around
                // it differ in (b, normal), and the base corner order is CCW in
                // the (b, n) plane -- so the emitted normal is `b x n`, which
                // equals +a exactly when (a, b, n) is right-handed. With
                // face_tangent_axes' ordering that holds for x-normal (y,z,x)
                // and z-normal (x,y,z) but NOT y-normal (x,z,y). Along `b` the
                // roles swap and so does the answer.
                //
                // MEASURED, not asserted (2026-08-11). Inverting the y-normal
                // entries alone takes the single-sheet y reversals from 29/617
                // to 559/617 -- the table is right for ~95% of crossings, so
                // the residue is a PER-CASE condition and not a table entry.
                // Perturbing an entry across all axes is worse still (x 0->182,
                // z 0->43, y 29->278). Do not "fix" the reversals here.
                const bool reverse_frame = along_a ? (face_axis == 1)
                                                   : (face_axis != 1);
                const bool flip = reverse_frame != (s0 == 0);
                if (flip) std::swap(q[1], q[3]);

                const bool t_a = (q[0] != q[1] && q[1] != q[2] && q[0] != q[2]);
                const bool t_b = (q[0] != q[2] && q[2] != q[3] && q[0] != q[3]);
                if (!t_a && !t_b) { ++stats.degenerate; continue; }

                WeldBucket& bucket = bucket_for(out, material);
                // Diagnosis side-channel; null in production (seam_weld.h).
                const uint8_t prov =
                    uint8_t((along_a ? WeldProvenance::kAlongA : 0) |
                            (s0 == 1 ? WeldProvenance::kSolidLo : 0) |
                            (capped  ? WeldProvenance::kCapped  : 0) |
                            (flip    ? WeldProvenance::kFlip    : 0));
                if (t_a) {
                    if (out.provenance)
                        out.provenance->add(prov, material,
                                            bucket.positions.size() / 9);
                    push_tri(out, bucket, *q[0], *q[1], *q[2]);
                }
                if (t_b) {
                    if (out.provenance)
                        out.provenance->add(
                            uint8_t(prov | WeldProvenance::kSecond), material,
                            bucket.positions.size() / 9);
                    push_tri(out, bucket, *q[0], *q[2], *q[3]);
                }
                if (t_a && t_b) {
                    ++stats.quads;      // unreachable when `capped` -- a cap always
                                        // duplicates a corner, killing one triangle
                } else {
                    ++stats.tris;
                    if (capped) ++stats.caps;
                }
            }
        }
    }
    return true;
}

} // namespace seam
