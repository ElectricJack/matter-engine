// MatterEngine3/tests/terrain_mesher_tests.cpp — Task 5: native surface nets
#include "check.h"
#include "../src/terrain_field.h"
#include "../src/terrain_mesher.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using namespace terrain_field;
using namespace terrain_mesher;

static FieldRuntime make(const char* text) {
    FieldProgram p; std::string err;
    if (!FieldProgram::parse(text, p, err)) printf("parse err: %s\n", err.c_str());
    return FieldRuntime(std::move(p));
}
static const char* kFlat5 =
    "const 5\nconst 0.5\nconst 0.5\n"
    "height r0\nmoisture r1\nrelief r2\nseaLevel 0\nbiome 0.65 0.35\n";
static const char* kNoise =
    "noise2 1234 0.02 4 0.5 2.0\nconst 20\nmul r0 r1\nconst 0.5\nconst 0.5\n"
    "height r2\nmoisture r3\nrelief r4\nseaLevel -100\nbiome 0.65 0.35\n";

// Count tris whose (stored) normal satisfies pred.
template <typename P>
static size_t count_tris(const SectorMesh& m, P pred) {
    size_t n = 0;
    for (const auto& b : m.buckets)
        for (size_t t = 0; t * 9 < b.normals.size(); ++t)
            if (pred(b.normals[t*9+0], b.normals[t*9+1], b.normals[t*9+2])) ++n;
    return n;
}

int main() {
    // --- flat field, rung 0: counts, height, orientation -------------------
    {
        FieldRuntime f = make(kFlat5);
        SectorMesh m; std::string err;
        CHECK(mesh_sector(f, 0, 0, 0, 16.0f, -64.0f, 192.0f, m, nullptr, err), err.c_str());
        size_t up    = count_tris(m, [](float, float ny, float){ return ny >  0.9f; });
        // Border skirts were removed on 2026-07-30 (see terrain_mesher.cpp): a
        // flat sector is surface and nothing else. This was 64 -- 4 sides x 8
        // segments x 2 tris -- and is now the regression gate that they stay
        // gone, since a returning curtain shows up as a dark band along every
        // seam once Ground POM displaces the rim below the datum.
        size_t vertical = count_tris(m,
            [](float, float ny, float){ return std::fabs(ny) < 0.3f; });
        CHECK(up == 128, "flat rung0: 8x8 cells -> 128 surface tris");
        CHECK(vertical == 0, "flat rung0: no skirt curtains, surface only");
        bool y_ok = true, xz_ok = true;
        for (const auto& b : m.buckets)
            for (size_t t = 0; t * 9 < b.positions.size(); ++t) {
                float ny = b.normals[t*9+1];
                for (int v = 0; v < 3; ++v) {
                    float x = b.positions[t*9+v*3+0], y = b.positions[t*9+v*3+1],
                          z = b.positions[t*9+v*3+2];
                    if (ny > 0.9f && std::fabs(y - 5.0f) > 1e-3f) y_ok = false;
                    if (x < -2.1f || x > 18.1f || z < -2.1f || z > 18.1f) xz_ok = false;
                }
            }
        CHECK(y_ok, "surface verts at y=5");
        CHECK(xz_ok, "verts in sector-local range");
    }
    // --- rung scaling: rung1 = 4x surface tris of rung0 --------------------
    {
        FieldRuntime f = make(kFlat5);
        SectorMesh m0, m1; std::string err;
        CHECK(mesh_sector(f, 0, 0, 0, 16.0f, -64, 192, m0, nullptr, err), "rung0");
        CHECK(mesh_sector(f, 0, 0, 1, 16.0f, -64, 192, m1, nullptr, err), "rung1");
        size_t up0 = count_tris(m0, [](float,float ny,float){ return ny > 0.9f; });
        size_t up1 = count_tris(m1, [](float,float ny,float){ return ny > 0.9f; });
        CHECK(up1 == 4 * up0, "rung1 surface = 4x rung0");
    }
    // --- determinism --------------------------------------------------------
    {
        FieldRuntime f = make(kNoise);
        SectorMesh a, b; std::string err;
        CHECK(mesh_sector(f, 3, -2, 2, 16.0f, -64, 192, a, nullptr, err), "a");
        CHECK(mesh_sector(f, 3, -2, 2, 16.0f, -64, 192, b, nullptr, err), "b");
        CHECK(a.buckets.size() == b.buckets.size(), "same bucket count");
        bool same = true;
        for (size_t i = 0; i < a.buckets.size(); ++i)
            if (a.buckets[i].positions != b.buckets[i].positions) same = false;
        CHECK(same, "byte-identical positions");
    }
    // --- same-rung seam closes on SURFACE geometry alone ---------------------
    // This test used to assert that A's +x skirt and B's -x skirt agreed, on
    // the premise (stated in its own comment) that ownership-based face
    // emission "leaves a one-voxel gap at the shared border" which "skirts
    // fill". That premise was wrong, and the flat-field case above always
    // disproved it: its verts span local -2.1..18.1 on a 16 m sector, i.e. the
    // surface OVERSHOOTS the border by about a voxel on each side. Neighbours
    // therefore OVERLAP at the seam; they never gapped. Skirts were covering
    // nothing here, which is why they could be removed.
    //
    // So the property worth gating is the real one: each sector's surface must
    // reach at least to the shared border from its own side, with no reliance
    // on any vertical geometry. If a future ownership change pulls the mesh
    // back inside the border, this fails and the seam is a genuine hole.
    {
        FieldRuntime f = make(kNoise);
        SectorMesh a, b; std::string err;
        CHECK(mesh_sector(f, 0, 0, 1, 16.0f, -64, 192, a, nullptr, err), "a");
        CHECK(mesh_sector(f, 1, 0, 1, 16.0f, -64, 192, b, nullptr, err), "b");
        // Neither sector's surface stops AT the border: the shared border cell
        // row (world x in [15,16] at rung 1) is emitted by BOTH sectors, from
        // the same world samples. Measured: A spans local x -0.5000..15.6741
        // (world 15.6741) and B spans -0.5955..15.9985 (world x from 15.4045),
        // so their coverage OVERLAPS by 0.27 m rather than meeting at 16.
        //
        // Note the two sides are NOT stitched vertex-for-vertex: their border
        // columns sit at different x (surface-nets places each sector's vertex
        // from its own owned cells), so this is an OVERLAP, not a shared edge.
        // An overlap is all the seam needs -- there is no line of sight to the
        // background through it -- and it is why the skirts could go.
        auto world_x_range = [](const SectorMesh& m, float origin_x,
                                float& lo, float& hi) {
            lo = 1e30f; hi = -1e30f;
            for (const auto& bkt : m.buckets)
                for (size_t i = 0; i + 2 < bkt.positions.size(); i += 3) {
                    const float wx = origin_x + bkt.positions[i];
                    lo = std::min(lo, wx);
                    hi = std::max(hi, wx);
                }
        };
        float a_lo = 0, a_hi = 0, b_lo = 0, b_hi = 0;
        world_x_range(a,  0.0f, a_lo, a_hi);
        world_x_range(b, 16.0f, b_lo, b_hi);
        // Measured at rung 1: A reaches world 15.674, B starts at world 15.405,
        // so the two surfaces overlap by ~0.27 m across the world-16 border.
        CHECK(b_lo <= a_hi,
              "same-rung seam: neighbour surfaces overlap across the border, "
              "so no skirt is needed to keep it opaque");
        // And no border curtains remain on either side.
        auto vertical_tris = [](const SectorMesh& m) {
            return count_tris(m,
                [](float, float ny, float){ return ny == 0.0f; });
        };
        CHECK(vertical_tris(a) == 0 && vertical_tris(b) == 0,
              "no skirt curtains on either side of the seam");
    }
    // --- degenerate config fails loudly -------------------------------------
    {
        FieldRuntime f = make(kFlat5);
        SectorMesh m; std::string err;
        CHECK(!mesh_sector(f, 0, 0, 4, 16.0f, -64, 192, m, nullptr, err), "rung 4 rejected");
        CHECK(!mesh_sector(f, 0, 0, 0, 16.0f, 10, -10, m, nullptr, err), "bad slab rejected");
    }

    // =======================================================================
    // NESTED SECTOR LOD (docs/terrain-nested-sector-lod-2026-08-08.md, WP3)
    //
    // The nested design meshes level L as (rung = -L, sector_size = S_0 << L),
    // so cells-per-tile is constant and a coarse tile's edge is shared by TWO
    // fine tiles. The design's claim is that the mesher needs no changes.
    //
    // The claim under test is NOT that a cross-size pair meets perfectly. No
    // cross-rung pair does, on any grid: surface nets places each side's border
    // vertices from its OWN cells, so the two surfaces overlap across the
    // border with a vertical mismatch of order (voxel x local slope). That is
    // today's behaviour on the uniform grid and this migration does not promise
    // to fix it.
    //
    // The claim is that SECTOR SIZE DROPS OUT -- that a cross-size pair seams
    // exactly like the cross-rung pair the uniform grid already produces at the
    // same two voxels. So each case below is measured against its own uniform
    // control, built to share the same border line, the same field, the same
    // voxel pair, and literally the same fine mesh; the only difference is how
    // big the tile on the coarse side is.
    // =======================================================================

    // Vertical-ray surface height of a mesh at SECTOR-LOCAL (x, z): the highest
    // triangle covering that column in XZ projection, or -inf if none does.
    // Vertex lists cannot be compared across a seam -- neither side puts a
    // vertex on the border -- but where a plumb line lands can be.
    auto surface_y_at = [](const SectorMesh& m, float x, float z) -> float {
        float best = -1e30f;
        for (const auto& b : m.buckets)
            for (size_t t = 0; t * 9 < b.positions.size(); ++t) {
                const float* p = &b.positions[t * 9];
                const float x0 = p[0], z0 = p[2];
                const float x1 = p[3], z1 = p[5];
                const float x2 = p[6], z2 = p[8];
                const float det = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2);
                if (std::fabs(det) < 1e-9f) continue;   // vertical: no XZ area
                const float l0 =
                    ((z1 - z2) * (x - x2) + (x2 - x1) * (z - z2)) / det;
                const float l1 =
                    ((z2 - z0) * (x - x2) + (x0 - x2) * (z - z2)) / det;
                const float l2 = 1.0f - l0 - l1;
                const float eps = -1e-4f;
                if (l0 < eps || l1 < eps || l2 < eps) continue;
                best = std::max(best, l0 * p[1] + l1 * p[4] + l2 * p[7]);
            }
        return best;
    };

    // Walk a shared border line and report how the two surfaces meet there.
    //   covered  — columns where BOTH sides have surface (the watertightness
    //              measure: a column covered by neither, or by one side only,
    //              is a line of sight to the background)
    //   worst    — the largest vertical mismatch over those columns
    // Each side overshoots the border by part of a voxel and the two OVERLAP
    // across it (the same-rung case above measures this, and it is why the
    // skirts could be removed), so the probe walks INWARD from the border on
    // the fine side until it finds a column both meshes cover.
    struct SeamStat { int covered; int probed; float worst; };
    auto seam_stats = [&](const SectorMesh& fine, float fine_ox, float fine_oz,
                          const SectorMesh& coarse_m, float coarse_ox,
                          float coarse_oz, float bx, float z_lo, float z_hi,
                          float v_fine) -> SeamStat {
        SeamStat s{0, 0, 0.0f};
        for (float wz = z_lo; wz <= z_hi; wz += v_fine) {
            ++s.probed;
            for (int k = 0; k <= 4; ++k) {
                const float wx = bx - 0.25f * float(k) * v_fine;
                const float yf = surface_y_at(fine, wx - fine_ox, wz - fine_oz);
                const float yc =
                    surface_y_at(coarse_m, wx - coarse_ox, wz - coarse_oz);
                if (yf < -1e29f || yc < -1e29f) continue;
                ++s.covered;
                s.worst = std::max(s.worst, std::fabs(yf - yc));
                break;
            }
        }
        return s;
    };

    // --- size drops out: nested pair vs its uniform control, every level -----
    //
    // For each level L, two configurations sharing one border line at x = 2*S_L:
    //
    //   nested   fine  tile 1 @ S_L,   rung -L      x [S_L, 2 S_L)
    //            coarse tile 1 @ 2 S_L, rung -(L+1)  x [2 S_L, 4 S_L)
    //   uniform  fine  tile 1 @ S_L,   rung -L      x [S_L, 2 S_L)   <- same mesh
    //            coarse tile 2 @ S_L,   rung -(L+1)  x [2 S_L, 3 S_L)
    //
    // Identical fine mesh, identical border, identical voxels. The coarse tile
    // is twice as wide in the nested case and its z-extent covers the fine
    // tile's twice over -- the half-edge case. If sector size is irrelevant to
    // the seam, the two rows of numbers agree.
    {
        FieldRuntime f = make(kNoise);
        float worst_ratio = 0.0f;
        int worst_level = -1;
        for (int L = 0; L <= 4; ++L) {
            const float SL = 64.0f * float(1 << L);
            const float v = SL / 32.0f;              // this level's voxel
            const float bx = 2.0f * SL;              // the shared border
            SectorMesh fine, nested_c, uniform_c;
            std::string err;
            CHECK(mesh_sector(f, 1, 0, -L, SL, -300, 300, fine, nullptr, err),
                  err.c_str());
            CHECK(mesh_sector(f, 1, 0, -(L + 1), 2.0f * SL, -300, 300,
                              nested_c, nullptr, err), err.c_str());
            CHECK(mesh_sector(f, 2, 0, -(L + 1), SL, -300, 300,
                              uniform_c, nullptr, err), err.c_str());

            const SeamStat ns = seam_stats(fine, SL, 0.0f, nested_c,
                                           2.0f * SL, 0.0f, bx,
                                           v, SL - v, v);
            const SeamStat us = seam_stats(fine, SL, 0.0f, uniform_c,
                                           2.0f * SL, 0.0f, bx,
                                           v, SL - v, v);
            printf("  nested L%d/%d (voxel %.0f/%.0f m): nested %d/%d cols, "
                   "step %.4f | uniform %d/%d cols, step %.4f\n",
                   L, L + 1, v, 2 * v, ns.covered, ns.probed, ns.worst,
                   us.covered, us.probed, us.worst);
            // Same voxels, same border, same fine mesh: the coarse tile's SIZE
            // is the only variable, and it must not matter. Both quantities are
            // compared against the control rather than against an absolute
            // bound, because both are properties of the cross-rung ladder that
            // this migration inherits rather than creates.
            CHECK(ns.covered == us.covered,
                  "nested seam: the same columns are covered by both tiles as "
                  "on the uniform grid");
            CHECK(std::fabs(ns.worst - us.worst) < 1e-3f,
                  "nested seam: the same vertical mismatch as the uniform "
                  "grid's cross-rung seam at the same voxels");
            // A floor, so a change that collapsed coverage on BOTH paths could
            // not slip through the equality above.
            CHECK(ns.covered * 10 >= ns.probed * 9,
                  "nested seam: at least 90% of border columns are covered by "
                  "both tiles");
            const float ratio = us.worst > 1e-6f ? ns.worst / us.worst : 1.0f;
            if (ratio > worst_ratio) { worst_ratio = ratio; worst_level = L; }
        }
        printf("  nested vs uniform: worst mismatch ratio %.4f (level %d)\n",
               worst_ratio, worst_level);
        CHECK(worst_ratio < 1.05f,
              "nested seam is no worse than the uniform grid's cross-rung seam "
              "at the same voxels -- sector size drops out");
    }

    // --- THE STRIP when the coarse neighbour is on the fine tile's -x / -z ---
    //
    // The seam tests above all put the fine tile WEST of the coarse one and
    // probe INWARD from the border on the fine side. That is the orientation
    // the ownership rule happens to close, and the probe walks away from where
    // the other orientation leaks -- so between them they could not see this.
    //
    // Reported from a real session: adjacent tiles of DIFFERENT SIZE show a
    // one-triangle-strip gap along the shared edge.
    //
    // Why it is direction-asymmetric. Face ownership is `i in [1..n]`, and a
    // +y quad at i uses cells ci in {i-1, i} -- so a tile's surface reaches one
    // of ITS OWN voxels back past its -x border and stops one half voxel short
    // of its +x border. Every shared plane is therefore bridged by the tile on
    // its EAST side, using that tile's own voxel size:
    //
    //   fine west / coarse east: the coarse tile bridges with a 2v-wide ring
    //     cell, reaching back to ~X0-v, well past the fine mesh's last vertex
    //     at ~X0-v/2. Overlaps. Closed.
    //   coarse west / fine east: the FINE tile bridges, reaching back only
    //     ~v/2, while the coarse mesh's last vertex sits at ~X0-v (its own
    //     ring cell is 2v wide). The strip between them belongs to neither.
    //     THE GAP.
    //
    // So this is not a nested-sectors bug at all -- the same hole exists on the
    // uniform grid whenever a coarse tile sits west or south of a fine one.
    // Nesting made those borders common and put them near the camera.
    //
    // WHAT THIS BLOCK ASSERTS, AND WHY IT CHANGED (M0-WP1, 2026-08-10). It used
    // to gate the gap at ZERO, and it passed, because mesh_sector took an
    // `edge_mask` and extended its lattice one coarse voxel outward on a masked
    // -x/-z face so the fine bridge overshot the coarse tile's last vertex. The
    // mask is deleted: it named the neighbour level the streamer WANTED, not
    // the tile drawn, so the closure it bought held only in the steady state,
    // and it put a neighbour guess into the tile's bake identity. See the
    // retraction at the top of mesh_sector.
    //
    // So the strip is open again BY CONSTRUCTION and this is no longer a gate
    // on the mesher -- the mesher is not what closes it. What survives is the
    // measurement, kept whole: the same union-coverage probe, the same printf,
    // and a bound saying the hole is still the known one-voxel strip and not
    // something wider that an ownership change quietly opened. The ZERO-GAP
    // GATE RETURNS IN THE WELDER SUITE, where mesh_sector's output and
    // seam_weld's band are measured together -- which is the only place the
    // question "is the seam closed" now has a meaningful answer.
    {
        FieldRuntime f = make(kNoise);
        int worst_level = -1;
        float worst_gap_v = 0.0f;
        for (int L = 0; L <= 3; ++L) {
            const float SL = 64.0f * float(1 << L);
            const float v  = SL / 32.0f;        // the FINE voxel
            const float X0 = 2.0f * SL;         // shared plane
            SectorMesh coarse_m, fine;
            std::string err;
            // coarse tile 0 at 2*SL: x,z in [0, 2 SL), rung -(L+1)
            // fine   tile 2 at SL:   x in [2 SL, 3 SL), z in [0, SL), rung -L
            // The coarse tile is on the fine tile's -x side. Neither tile is
            // told that: mesh_sector takes no mask, so both mesh exactly as
            // they would beside an equal-rung neighbour, and the resulting
            // cross-rung strip is the runtime welder's to close.
            CHECK(mesh_sector(f, 0, 0, -(L + 1), 2.0f * SL, -300, 300,
                              coarse_m, nullptr, err), err.c_str());
            CHECK(mesh_sector(f, 2, 0, -L, SL, -300, 300, fine, nullptr, err),
                  err.c_str());

            const float step = v / 8.0f;
            int rows = 0, gapped = 0;
            float worst_run = 0.0f;
            for (float wz = v; wz <= SL - v; wz += v) {
                ++rows;
                float run = 0.0f, run_max = 0.0f;
                for (float wx = X0 - 2.5f * v; wx <= X0 + 2.0f * v; wx += step) {
                    const float yc = surface_y_at(coarse_m, wx, wz);
                    const float yf = surface_y_at(fine, wx - X0, wz);
                    if (yc > -1e29f || yf > -1e29f) run = 0.0f;
                    else { run += step; run_max = std::max(run_max, run); }
                }
                if (run_max > 0.25f * v) ++gapped;
                worst_run = std::max(worst_run, run_max);
            }
            printf("  seam -x L%d/%d (voxel %.0f/%.0f m): %d/%d rows gapped, "
                   "worst run %.3f m (%.2f fine voxels)\n",
                   L, L + 1, v, 2 * v, gapped, rows, worst_run, worst_run / v);
            // Characterization, not a gate. 1.5 fine voxels is the known
            // one-voxel strip with headroom: measured 0.88-1.12 at every level
            // both before the reach-back existed and after it was deleted. A
            // failure here means the hole grew to something the welder's
            // one-cell-wide band cannot span, which IS a mesher regression.
            CHECK(worst_run <= 1.5f * v,
                  "coarse neighbour on the fine tile's -x side: the uncovered "
                  "strip is still the known ~1 fine voxel, not wider");
            if (worst_run / v > worst_gap_v) {
                worst_gap_v = worst_run / v; worst_level = L;
            }
        }
        printf("  seam -x: worst uncovered run %.2f fine voxels (level %d) "
               "-- open by construction until the welder runs\n",
               worst_gap_v, worst_level);

        // The -z mirror. Same rule, other axis: a coarse neighbour to the
        // south is bridged by the fine tile with a fine-sized reach.
        for (int L = 0; L <= 2; ++L) {
            const float SL = 64.0f * float(1 << L);
            const float v  = SL / 32.0f;
            const float Z0 = 2.0f * SL;
            SectorMesh coarse_m, fine;
            std::string err;
            CHECK(mesh_sector(f, 0, 0, -(L + 1), 2.0f * SL, -300, 300,
                              coarse_m, nullptr, err), err.c_str());
            CHECK(mesh_sector(f, 0, 2, -L, SL, -300, 300, fine, nullptr, err),
                  err.c_str());
            const float step = v / 8.0f;
            int rows = 0, gapped = 0;
            float worst_run = 0.0f;
            for (float wx = v; wx <= SL - v; wx += v) {
                ++rows;
                float run = 0.0f, run_max = 0.0f;
                for (float wz = Z0 - 2.5f * v; wz <= Z0 + 2.0f * v; wz += step) {
                    const float yc = surface_y_at(coarse_m, wx, wz);
                    const float yf = surface_y_at(fine, wx, wz - Z0);
                    if (yc > -1e29f || yf > -1e29f) run = 0.0f;
                    else { run += step; run_max = std::max(run_max, run); }
                }
                if (run_max > 0.25f * v) ++gapped;
                worst_run = std::max(worst_run, run_max);
            }
            printf("  seam -z L%d/%d: %d/%d rows gapped, worst run %.3f m "
                   "(%.2f fine voxels)\n",
                   L, L + 1, gapped, rows, worst_run, worst_run / v);
            CHECK(worst_run <= 1.5f * v,
                  "coarse neighbour on the fine tile's -z side: the uncovered "
                  "strip is still the known ~1 fine voxel, not wider");
        }
    }

    // --- RETIRED: "the edge mask still does its job across sizes" ------------
    //
    // What used to be here proved the SNAP WAS LOAD-BEARING: it sampled the
    // field along a shared border at the fine spacing and showed that a raw odd
    // sample differs from the average of its two even neighbours by up to
    // 1.665 m on kNoise -- i.e. that the coarse side's linear interpolation and
    // the fine side's raw lattice genuinely disagree across a coarse voxel, so
    // rewriting the fine boundary samples onto that interpolation was closing a
    // real divergence rather than an already-closed one.
    //
    // The measurement was true and stays true. Its PREMISE is retired, not
    // forgotten: making the fine lattice agree with the coarse one requires
    // knowing which neighbour is coarse at bake time, and the mask that
    // answered was a statement about the level map the streamer wanted rather
    // than about the tile on screen. A snap aimed at a neighbour that is not
    // drawn bends the fine surface for nothing. Agreement across levels now
    // comes from the runtime welder, which reads BOTH sides' actual boundary
    // records (the export tests at the end of this file) and therefore cannot
    // be wrong about who is next door.
    //
    // The field-curvature fact this rested on -- ~1.7 m of divergence over a
    // 4 m coarse voxel on kNoise -- is exactly what makes the welded band a
    // real interpolation problem rather than a formality, so it is recorded
    // here rather than deleted outright.

    // --- three-tile corner: two fine siblings meeting one coarse edge --------
    // World (256, 128) is the corner shared by both level-1 fine tiles and is
    // sample 16 of 32 along the level-2 coarse tile's -x edge -- a real coarse
    // sample, not an interpolated point. The corner is where a quadtree is most
    // likely to leak, so it gets its own coverage gate.
    {
        FieldRuntime f = make(kNoise);
        const float SL = 128.0f, v = 4.0f;
        SectorMesh fine_lo, fine_hi, coarse;
        std::string err;
        // Fine tiles (1,0) and (1,1) at 128 m: x [128,256), z [0,128) and
        // [128,256). Coarse tile (1,0) at 256 m: x [256,512), z [0,256).
        CHECK(mesh_sector(f, 1, 0, -1, SL, -300, 300, fine_lo, nullptr, err),
              err.c_str());
        CHECK(mesh_sector(f, 1, 1, -1, SL, -300, 300, fine_hi, nullptr, err),
              err.c_str());
        CHECK(mesh_sector(f, 1, 0, -2, 2.0f * SL, -300, 300, coarse, nullptr, err),
              err.c_str());

        const SeamStat lo = seam_stats(fine_lo, SL, 0.0f, coarse,
                                       2.0f * SL, 0.0f, 2.0f * SL,
                                       v, SL - v, v);
        const SeamStat hi = seam_stats(fine_hi, SL, SL, coarse,
                                       2.0f * SL, 0.0f, 2.0f * SL,
                                       SL + v, 2.0f * SL - v, v);
        printf("  nested corner: lower %d/%d cols step %.4f, "
               "upper %d/%d cols step %.4f\n",
               lo.covered, lo.probed, lo.worst, hi.covered, hi.probed, hi.worst);
        CHECK(lo.covered * 10 >= lo.probed * 9 && hi.covered * 10 >= hi.probed * 9,
              "nested corner: BOTH fine siblings cover their half of the coarse "
              "tile's edge, all the way to the corner where they meet");
        // The two siblings are equal-level neighbours of each other, which is
        // the existing equal-rung case: identical world samples on their shared
        // column, so their surfaces meet as any same-rung pair does.
        const float y_lo = surface_y_at(fine_lo, SL - v, SL - 0.5f * v);
        const float y_hi = surface_y_at(fine_hi, SL - v, 0.5f * v);
        CHECK(y_lo > -1e29f && y_hi > -1e29f,
              "nested corner: both siblings cover the column just inside their "
              "shared edge");
    }

    // =======================================================================
    // VOLUMETRIC TERRAIN. A density that reads world y directly: tunnels,
    // caverns, overhangs. The mesher's heightfield shortcuts (narrow Y slab,
    // analytic gy, boundary-polyline seam snap) are all provable consequences
    // of `density = h - y` and none of them survives here.
    // =======================================================================

    // A flat surface at y=40 with a horizontal slab of air carved out of it
    // between y=-20 and y=+4. The carve is a function of y ALONE, so the answer
    // is exact and hand-checkable: two horizontal faces (a cave floor at -20 and
    // a cave roof at +4) plus the surface at 40, and nothing else.
    //
    //   solid = min(40 - y, max(y - 4, -20 - y))
    // written out: r5 = 40 - y, r8 = y - 4, r11 = -20 - y.
    static const char* kSlabCave =
        "const 40\n"                 // r0  surface height
        "input wy\n"                 // r1
        "const 0.5\n"                // r2
        "sub r0 r1\n"                // r3  40 - y      (above/below surface)
        "const 4\n"                  // r4
        "sub r1 r4\n"                // r5  y - 4       (above the cave roof)
        "const -20\n"                // r6
        "sub r6 r1\n"                // r7  -20 - y     (below the cave floor)
        "max r5 r7\n"                // r8  solid outside the slab
        "min r3 r8\n"                // r9
        "height r0\ndensity r9\nmoisture r2\nrelief r2\n"
        "seaLevel -1000\nbiome 0.65 0.35\n";
    {
        FieldRuntime f = make(kSlabCave);
        CHECK(!f.is_heightfield(), "a y-reading density is volumetric");
        SectorMesh m; std::string err;
        CHECK(mesh_sector(f, 0, 0, 0, 16.0f, -64.0f, 192.0f, m, nullptr, err), err.c_str());

        // THE SLAB MUST REACH THE CAVE. A heightfield slab would have started
        // ~4 voxels under y=40 and never sampled y=4 or y=-20 at all, so this
        // single count is the regression gate on the whole volumetric path.
        size_t up   = count_tris(m, [](float, float ny, float){ return ny >  0.9f; });
        size_t down = count_tris(m, [](float, float ny, float){ return ny < -0.9f; });
        CHECK(up == 256, "surface (y=40) + cave floor (y=-20): 2 x 128 up-facing tris");
        CHECK(down == 128, "cave roof (y=+4): 128 down-facing tris -- an OVERHANG, "
                           "which is exactly what a heightfield cannot express");

        // The roof normal is the numeric gy earning its keep: with the analytic
        // -2*eps shortcut every normal in this mesh would point up, including
        // the roof's.
        bool roof_ok = false;
        for (const auto& b : m.buckets)
            for (size_t t = 0; t * 9 < b.normals.size(); ++t)
                if (b.normals[t*9+1] < -0.9f &&
                    std::fabs(b.positions[t*9+1] - 4.0f) < 1.5f) roof_ok = true;
        CHECK(roof_ok, "the down-facing tris sit at the roof altitude, y~4");
    }

    // --- the cavity is CLOSED ------------------------------------------------
    // A vertical ray through a watertight solid crosses its boundary an even
    // number of times. Cast rays down through a 3D noise cave field and count
    // sign changes in the density: an odd count would mean a surface the mesher
    // must emit and the field says nothing about. This checks the FIELD is sane
    // (the mesher's own watertightness is the seam tests below).
    // The FLOOR (r14) is not decoration. Without it the tunnel field keeps
    // opening voids all the way past the bottom of the authored range, so a
    // downward ray can end in air and "is the cavity closed" has no answer --
    // the first version of this fixture had no floor and the closure check
    // failed on a field that was behaving exactly as written.
    static const char* kNoiseCave =
        "noise2 1234 0.02 4 0.5 2.0\nconst 20\nmul r0 r1\n"   // r2 surface base
        "const 30\nadd r2 r3\n"                               // r4 surface
        "input wy\n"                                          // r5
        "sub r4 r5\n"                                         // r6 h - y
        "ridge3 4242 0.02 2 0.5 2\n"                          // r7
        "const 0.45\nsub r7 r8\n"                             // r9
        "const -1\nmul r9 r10\n"                              // r11 solid outside tunnels
        "min r6 r11\n"                                        // r12
        "const -100\nsub r13 r5\n"                            // r14 -100 - y
        "max r12 r14\n"                                       // r15 solid below -100
        "const 0.5\n"                                         // r16
        "height r4\ndensity r15\nmoisture r16\nrelief r16\n"
        "seaLevel -1000\nbiome 0.65 0.35\n";
    {
        FieldRuntime f = make(kNoiseCave);
        SectorMesh m; std::string err;
        CHECK(mesh_sector(f, 0, 0, 0, 64.0f, -128.0f, 192.0f, m, nullptr, err), err.c_str());
        CHECK(m.triangle_count() > 0, "the noise cave meshes something");
        // Down-facing triangles are the proof that caves were meshed at all: a
        // heightfield world of this surface has none.
        size_t down = count_tris(m, [](float, float ny, float){ return ny < -0.5f; });
        CHECK(down > 0, "the noise cave has roofs");

        int odd = 0, sampled = 0;
        for (int i = 0; i < 40; ++i)
            for (int k = 0; k < 40; ++k) {
                const float x = i * 1.6f, z = k * 1.6f;
                int crossings = 0;
                float prev = f.density_at(x, 192.0f, z);
                for (float y = 192.0f - 0.25f; y > -128.0f; y -= 0.25f) {
                    const float d = f.density_at(x, y, z);
                    if ((d > 0) != (prev > 0)) ++crossings;
                    prev = d;
                }
                ++sampled;
                if (crossings & 1) ++odd;   // ends inside solid: fine, it is the floor
            }
        // Every ray starts in air (above the surface) and ends in rock (below
        // the cave field's reach at -128), so an ODD crossing count is the
        // correct answer and an even one would mean a ray that ended in air.
        CHECK(odd == sampled,
              "every downward ray ends in solid: the cavities are closed, not "
              "open to the bottom of the world");
    }

    // --- equal-rung tiles are watertight through a cave wall ----------------
    // Two neighbours meshing the same volumetric field must produce identical
    // vertices on their shared plane. The [1..n] ownership rule gives this for
    // free on a heightfield because the boundary is a shared polyline; here the
    // boundary is a whole plane of density, so it is worth asserting.
    {
        FieldRuntime f = make(kNoiseCave);
        SectorMesh a, b; std::string err;
        CHECK(mesh_sector(f, 0, 0, 0, 64.0f, -128.0f, 192.0f, a, nullptr, err), err.c_str());
        CHECK(mesh_sector(f, 1, 0, 0, 64.0f, -128.0f, 192.0f, b, nullptr, err), err.c_str());
        // Surface-nets vertices sit at CELL CENTROIDS, not on lattice planes, so
        // there is no vertex exactly at world x = 64 to compare -- the first
        // version of this check looked for one and found nothing. What IS shared
        // is the border CELL ROW: a's cell i=n and b's bridging cell i=0 span
        // the same world span [62, 64] and sample the same lattice points, so
        // their vertices must come out bit-identical. Compare that band.
        auto band = [](const SectorMesh& m, float ox,
                       std::vector<std::array<float,3>>& out) {
            for (const auto& bk : m.buckets)
                for (size_t v = 0; v * 3 < bk.positions.size(); ++v) {
                    const float wx = ox + bk.positions[v*3+0];
                    if (wx > 62.0f && wx <= 64.0f)
                        out.push_back({wx, bk.positions[v*3+1],
                                       bk.positions[v*3+2]});
                }
        };
        std::vector<std::array<float,3>> av, bv;
        band(a, 0.0f, av);
        band(b, 64.0f, bv);
        CHECK(!av.empty() && !bv.empty(), "both tiles reach their shared cell row");
        // WHAT THIS CAN AND CANNOT ASSERT, because the first version got it
        // wrong in a way worth recording. The two tiles do NOT emit the same
        // vertex set over the shared cell column (measured: 612 against 342),
        // and that is correct behaviour rather than a hole: a vertex reaches the
        // mesh only when some OWNED quad uses it, and the +x-edge quads over
        // this column are owned by `a` alone (b's would sit at i=0, which fails
        // the i >= 1 ownership rule). Exactly one tile emits each face, which is
        // the point of the rule.
        //
        // What must hold is that where BOTH tiles emitted a cell, they put its
        // vertex in the same place -- the two computed it from the same world
        // samples, so any disagreement means the volumetric fill is reading
        // different coordinates on the two sides. Match on (y, z), which
        // identifies the cell, then compare x.
        //
        // Agreement, not bitwise identity: positions are stored SECTOR-LOCAL, so
        // a reaches 62.0022 as (32.0011 - 1) * 2 while b reaches it as
        // (0.0011 - 1) * 2 plus a 64 m tile origin. Same real number, different
        // float arithmetic.
        double worst_x = 0; int paired = 0;
        for (const auto& p : av)
            for (const auto& q : bv)
                if (std::fabs(p[1] - q[1]) < 1e-4f && std::fabs(p[2] - q[2]) < 1e-4f) {
                    worst_x = std::max(worst_x, std::fabs(double(p[0]) - double(q[0])));
                    ++paired;
                    break;
                }
        printf("  volumetric equal-rung seam: a=%zu b=%zu shared-row verts, "
               "%d shared cells, worst x disagreement %.3g m\n",
               av.size(), bv.size(), paired, worst_x);
        CHECK(paired > 100, "the two tiles share most of the boundary cell column");
        CHECK(worst_x < 1e-3,
              "a cell emitted by both tiles gets the same vertex from each -- the "
              "volumetric fill reads identical world coordinates on both sides");
    }

    // --- REPURPOSED: the volumetric cross-rung pair --------------------------
    //
    // This block used to prove the VOLUMETRIC snap was load-bearing, in the same
    // shape as the heightfield snap retired alongside it: it walked a shared
    // boundary plane and counted how often a raw fine density sample differs
    // from the average of the two even samples the coarse neighbour would have
    // interpolated between. It always differed, which was the point -- without
    // the snap, a cave wall crossing a band boundary parts on the two sides.
    //
    // The premise is retired with the mask that carried it. What is NOT retired
    // is the fact underneath: across a cave field a coarse voxel's worth of
    // curvature is large, so the cross-rung boundary really does need
    // reconciling and the welder really does have work to do. Kept as that
    // measurement, plus the plain gate that both sides of a cross-rung pair
    // still mesh through a cave at all now that neither knows about the other.
    {
        FieldRuntime f = make(kNoiseCave);
        SectorMesh coarse, fine; std::string err;
        // Coarse tile at rung -1 west of a rung-0 fine tile. Neither is told.
        CHECK(mesh_sector(f, -1, 0, -1, 64.0f, -128.0f, 192.0f, coarse, nullptr, err), err.c_str());
        CHECK(mesh_sector(f, 0, 0, 0, 64.0f, -128.0f, 192.0f, fine, nullptr, err),
              err.c_str());
        const float v = 2.0f;                 // rung 0 voxel
        int checked = 0, agree = 0;
        float worst = 0.0f;
        for (float z = 0; z < 64.0f; z += v)
            for (float y = -100.0f; y < 100.0f; y += 2 * v) {
                const float lo = f.density_at(0.0f, y - v, z);
                const float hi = f.density_at(0.0f, y + v, z);
                const float mid = f.density_at(0.0f, y, z);
                ++checked;
                const float e = std::fabs(mid - 0.5f * (lo + hi));
                worst = std::max(worst, e);
                if (e < 1e-4f) ++agree;
            }
        printf("  volumetric cross-rung plane: %d/%d samples where the raw fine "
               "density equals the coarse interpolant, worst delta %.4f\n",
               agree, checked, worst);
        CHECK(checked > 0, "cross-rung probe ran");
        CHECK(agree < checked,
              "the raw fine samples DO diverge from the coarse interpolant, so "
              "the cross-rung boundary is a real reconciliation problem -- it "
              "is now the welder's, not the mesher's");
        CHECK(fine.triangle_count() > 0 && coarse.triangle_count() > 0,
              "both sides of the cross-rung pair mesh through the cave with "
              "neither told anything about the other");
    }

    // =======================================================================
    // BOUNDARY RECORD EXPORT (M0-WP2, seam_boundary.h)
    //
    // What a tile hands the seam welder in place of the baked-in stitch the
    // edge mask used to do. Every property below is about ONE tile or about two
    // tiles that never spoke to each other, which is the whole point: the
    // record is not part of the bake identity and cannot be, because no field
    // of it depends on a neighbour.
    // =======================================================================

    // --- (a) a face record holds exactly the surface-crossing cells ----------
    //
    // On kFlat5 the answer is exact and hand-checkable, which is why it is the
    // fixture: a flat surface at y = 5 crosses every column ONCE, so each of the
    // n cells along a face's cell layer contributes exactly one entry and no
    // other cell in the layer contributes any. n = 8 at rung 0 on a 16 m tile.
    // Anything else means the layer is being scanned over the wrong range.
    {
        FieldRuntime f = make(kFlat5);
        SectorMesh m; seam::SectorBoundary sb; std::string err;
        CHECK(mesh_sector(f, 3, -2, 0, 16.0f, -64.0f, 192.0f, m, &sb, err), err.c_str());
        CHECK(sb.rung == 0 && sb.cells == 8, "record carries rung and cells/axis");
        CHECK(sb.tx == 3 && sb.tz == -2 && sb.ty == 0,
              "record carries the tile index; ty is 0 until Y is tiled (M2)");
        CHECK(sb.faces[seam::kFacePosY].verts.empty() &&
              sb.faces[seam::kFaceNegY].verts.empty(),
              "the +y/-y faces stay empty in M0");
        const seam::FaceRecord& nx = sb.faces[seam::kFaceNegX];
        const seam::FaceRecord& px = sb.faces[seam::kFacePosX];
        printf("  boundary flat: -x %zu verts (plane %lld, layer %lld), "
               "+x %zu verts (plane %lld, layer %lld)\n",
               nx.verts.size(), (long long)nx.plane, (long long)nx.cell_layer,
               px.verts.size(), (long long)px.plane, (long long)px.cell_layer);
        CHECK(nx.verts.size() == 8 && px.verts.size() == 8,
              "flat field: one crossing cell per column, n per face");
        CHECK(sb.faces[seam::kFaceNegZ].verts.size() == 8 &&
              sb.faces[seam::kFacePosZ].verts.size() == 8,
              "and the same on the two z faces");
        // Indexing, checked against the derivation rather than against itself:
        // tx = 3, n = 8, so -x sits on global lattice/cell 24 and +x on 32/31.
        CHECK(nx.plane == 24 && nx.cell_layer == 24,
              "-x: plane == cell_layer == tx*n");
        CHECK(px.plane == 32 && px.cell_layer == 31,
              "+x: plane == (tx+1)*n, cell layer one below it");
        // (a, b) on a +-x face is (global y cell, global z cell): one y value
        // shared by all eight, and the eight consecutive z cells of tz = -2.
        bool one_a = true, b_ok = true;
        for (size_t i = 0; i < nx.verts.size(); ++i) {
            if (nx.verts[i].a != nx.verts[0].a) one_a = false;
            if (nx.verts[i].b != -2 * 8 + int64_t(i)) b_ok = false;
        }
        CHECK(one_a, "flat surface: every -x entry is in the same y cell layer");
        CHECK(b_ok, "-x entries run over the tile's own z cells, tz*n .. tz*n+n-1");
        // corner_signs on a flat field is fully determined: density = 5 - y, so
        // the cell's lower corners (a, b) and (a, b+1) are solid and its upper
        // ones (a+1, b) and (a+1, b+1) are air -> bits 0 and 2 set, 1 and 3 not.
        bool signs_ok = true;
        for (const seam::BoundaryVert& v : nx.verts)
            if (v.corner_signs != 0x5) signs_ok = false;
        CHECK(signs_ok,
              "flat field: bit0|bit2 (the two lower plane corners) solid, "
              "bit1|bit3 air -- the sign layout the welder reads edges from");
        // World position, not tile-local: the tile origin is x = 48, and the
        // -x cell's vertex sits half a voxel east of the plane at the surface.
        bool pos_ok = true;
        for (const seam::BoundaryVert& v : nx.verts)
            if (std::fabs(v.px - 49.0) > 1e-6 || std::fabs(v.py - 5.0) > 1e-3)
                pos_ok = false;
        CHECK(pos_ok,
              "positions are WORLD-absolute doubles: x = 48 + voxel/2, y = 5");
        // Sorted by (a, b): FaceRecord::find binary-searches it.
        bool sorted = true;
        for (size_t i = 1; i < nx.verts.size(); ++i)
            if (!(nx.verts[i-1].a < nx.verts[i].a ||
                  (nx.verts[i-1].a == nx.verts[i].a &&
                   nx.verts[i-1].b < nx.verts[i].b))) sorted = false;
        CHECK(sorted, "-x record is sorted ascending by (a, b)");
        CHECK(nx.find(nx.verts[0].a, nx.verts[0].b) == &nx.verts[0] &&
              nx.find(nx.verts[0].a, nx.verts[0].b - 1) == nullptr,
              "FaceRecord::find locates a present cell and rejects an absent one");
    }

    // --- (a2) every entry sits inside the cell its indices name --------------
    // The flat case above pins the indexing where the answer is known; this one
    // holds it on a real cave field, where the only thing that can be asserted
    // without re-implementing the mesher is the invariant that matters: an
    // entry's WORLD position must lie in the cell (a, b, cell_layer) claims it
    // is in. If a/b were off by one, or the tile origin were added at the wrong
    // rung, every entry would fall outside its own box.
    {
        FieldRuntime f = make(kNoiseCave);
        SectorMesh m; seam::SectorBoundary sb; std::string err;
        CHECK(mesh_sector(f, -3, 2, -1, 128.0f, -128.0f, 192.0f, m, &sb, err),
              err.c_str());
        const double v = seam::rung_voxel(-1);
        CHECK(v == 4.0, "rung -1 is a 4 m voxel");
        int outside = 0, total = 0;
        double worst = 0.0;
        for (int fa = 0; fa < 4; ++fa) {          // the four XZ faces
            const seam::FaceRecord& fr = sb.faces[fa];
            const int axis = seam::face_axis(fa);
            for (const seam::BoundaryVert& bv : fr.verts) {
                ++total;
                // Cell box from the indices alone.
                const double nlo = double(fr.cell_layer) * v;   // normal axis
                const double ylo = double(axis == 0 ? bv.a : bv.b) * v - 128.0;
                const double tlo = double(axis == 0 ? bv.b : bv.a) * v;
                const double px = axis == 0 ? bv.px : bv.pz;    // along normal
                const double pt = axis == 0 ? bv.pz : bv.px;    // tangential
                const double e = std::max(std::max(
                        std::max(nlo - px, px - (nlo + v)),
                        std::max(ylo - bv.py, bv.py - (ylo + v))),
                        std::max(tlo - pt, pt - (tlo + v)));
                worst = std::max(worst, e);
                if (e > 1e-3) ++outside;
            }
        }
        printf("  boundary cell-box: %d entries, %d outside their own cell, "
               "worst overshoot %.6f m\n", total, outside, worst);
        CHECK(total > 100, "the cave tile exported a substantial boundary");
        CHECK(outside == 0,
              "every entry's world position lies inside the cell its global "
              "(a, b) and cell_layer name -- the indexing is self-consistent");
    }

    // --- (b) two bakes of one tile give byte-identical records ---------------
    // Records ride on the bake artifact, so this is the same determinism gate
    // the mesh itself has: a record that reorders between bakes invalidates
    // every cache comparison downstream of it.
    {
        FieldRuntime f = make(kNoiseCave);
        SectorMesh m1, m2; seam::SectorBoundary s1, s2; std::string err;
        CHECK(mesh_sector(f, 1, 1, 0, 64.0f, -128.0f, 192.0f, m1, &s1, err), err.c_str());
        CHECK(mesh_sector(f, 1, 1, 0, 64.0f, -128.0f, 192.0f, m2, &s2, err), err.c_str());
        bool same = s1.vert_count() == s2.vert_count();
        size_t differing = 0;
        for (int fa = 0; fa < seam::kFaceCount && same; ++fa) {
            const auto& A = s1.faces[fa];
            const auto& B = s2.faces[fa];
            if (A.verts.size() != B.verts.size() || A.plane != B.plane ||
                A.cell_layer != B.cell_layer) { same = false; break; }
            for (size_t i = 0; i < A.verts.size(); ++i)
                if (std::memcmp(&A.verts[i], &B.verts[i],
                                sizeof(seam::BoundaryVert)) != 0) ++differing;
        }
        printf("  boundary determinism: %zu verts across 6 faces, %zu differing "
               "bytes-wise\n", s1.vert_count(), differing);
        CHECK(same && differing == 0,
              "two bakes of one tile produce byte-identical boundary records");
        CHECK(s1.vert_count() > 0, "the cave tile actually exported something");
    }

    // --- (c) THE CROSS-TILE INVARIANT: equal-rung neighbours agree -----------
    //
    // This is the one that matters. Tile A's +x record and tile B's -x record
    // describe the SAME world plane, and neither tile was told the other
    // exists. If the global indexing is right they name it with the same
    // `plane`, their (a, b) index sets coincide, and -- because both sampled
    // the identical world points -- their corner_signs match BIT FOR BIT. Any
    // disagreement here is an indexing bug that would make the welder join the
    // wrong cells, silently, at every seam in the world.
    {
        FieldRuntime f = make(kNoiseCave);
        SectorMesh ma, mb; seam::SectorBoundary sa, sb; std::string err;
        CHECK(mesh_sector(f, 0, 0, 0, 64.0f, -128.0f, 192.0f, ma, &sa, err), err.c_str());
        CHECK(mesh_sector(f, 1, 0, 0, 64.0f, -128.0f, 192.0f, mb, &sb, err), err.c_str());
        const seam::FaceRecord& A = sa.faces[seam::kFacePosX];
        const seam::FaceRecord& B = sb.faces[seam::kFaceNegX];
        CHECK(A.plane == B.plane,
              "equal-rung neighbours name the shared plane identically");
        CHECK(A.plane == 32 && A.cell_layer == 31 && B.cell_layer == 32,
              "A's +x cell layer is its last, B's -x cell layer is its first, "
              "and the plane sits between them");
        int shared = 0, sign_mismatch = 0;
        double worst_pos = 0.0;
        for (const seam::BoundaryVert& va : A.verts) {
            const seam::BoundaryVert* vb = B.find(va.a, va.b);
            if (!vb) continue;
            ++shared;
            if (va.corner_signs != vb->corner_signs) ++sign_mismatch;
            // The two cells are DIFFERENT cells -- one either side of the plane
            // -- so their vertices are NOT the same point and nothing here
            // claims they should be. Only the plane-corner signs, which are
            // literally the same eight world samples read twice, must agree.
            // The separation is printed as context for the welder, which has to
            // span exactly this distance.
            const double dx = va.px - vb->px, dy = va.py - vb->py,
                         dz = va.pz - vb->pz;
            worst_pos = std::max(worst_pos, std::sqrt(dx*dx + dy*dy + dz*dz));
        }
        printf("  boundary equal-rung: A+x %zu verts, B-x %zu verts, %d shared "
               "(a,b), %d sign mismatches, worst vertex separation %.3f m\n",
               A.verts.size(), B.verts.size(), shared, sign_mismatch, worst_pos);
        CHECK(shared > 50, "the two records overlap over most of the plane");
        CHECK(sign_mismatch == 0,
              "corner_signs match bit-for-bit where the two records overlap -- "
              "both tiles sampled the same world points on the shared plane");
        // The index SETS coincide too, not just their intersection: a cell with
        // a crossing on the plane has a sign change on both sides of it.
        int a_only = 0, b_only = 0;
        for (const seam::BoundaryVert& va : A.verts)
            if (!B.find(va.a, va.b)) ++a_only;
        for (const seam::BoundaryVert& vb : B.verts)
            if (!A.find(vb.a, vb.b)) ++b_only;
        printf("  boundary equal-rung: %d cells only in A, %d only in B "
               "(cells whose crossing is off the shared plane)\n",
               a_only, b_only);
        CHECK(a_only + b_only < shared,
              "the shared cells dominate: the two records describe one plane");
    }

    // --- (d) a 2:1 pair maps through floor_div2 ------------------------------
    // The welder's fan rests on `coarse = floor_div2(fine)` -- a coarse cell at
    // rung r-1 covers exactly the fine cells 2g and 2g+1 at rung r. Assert that
    // the fine face's indices land inside the coarse record's range under that
    // map, on a NESTED pair (fine 64 m tile at rung 0, coarse 128 m tile at
    // rung -1) where the tile sizes differ as well as the voxels.
    {
        FieldRuntime f = make(kNoiseCave);
        SectorMesh mf, mc; seam::SectorBoundary sf, sc; std::string err;
        // Coarse tile 0 at 128 m / rung -1 covers world x,z in [0, 128).
        // Fine tile 2 at 64 m / rung 0 covers x in [128, 192), z in [0, 64).
        // They share the plane at world x = 128.
        CHECK(mesh_sector(f, 0, 0, -1, 128.0f, -128.0f, 192.0f, mc, &sc, err), err.c_str());
        CHECK(mesh_sector(f, 2, 0, 0, 64.0f, -128.0f, 192.0f, mf, &sf, err), err.c_str());
        const seam::FaceRecord& F = sf.faces[seam::kFaceNegX];   // fine, -x
        const seam::FaceRecord& C = sc.faces[seam::kFacePosX];   // coarse, +x
        // Planes are in DIFFERENT units (each is a lattice index at its own
        // rung), so they compare through the same halving: world x = 128 is
        // fine lattice 64 at 2 m and coarse lattice 32 at 4 m.
        CHECK(F.plane == 64 && C.plane == 32,
              "each side names the shared plane at its own rung");
        CHECK(seam::floor_div2(F.plane) == C.plane,
              "floor_div2 carries the fine plane index onto the coarse one");
        // The coarse side's TANGENTIAL DOMAIN, from its own tile identity rather
        // than from which of its cells happen to hold a vertex: b on a +-x face
        // is the global z cell, and a tile owns exactly [tz*n, tz*n + n - 1].
        // That is the structural claim -- every fine boundary cell must halve
        // into a z cell the coarse tile actually owns, or the welder would be
        // asked to fan onto a tile that does not cover the seam.
        const int64_t c_b_lo = sc.tz * sc.cells;
        const int64_t c_b_hi = c_b_lo + sc.cells - 1;
        int64_t c_a_lo = 0, c_a_hi = 0, f_a_lo = 0, f_a_hi = 0;
        bool first = true;
        for (const seam::BoundaryVert& v : C.verts) {
            if (first) { c_a_lo = c_a_hi = v.a; first = false; }
            c_a_lo = std::min(c_a_lo, v.a); c_a_hi = std::max(c_a_hi, v.a);
        }
        first = true;
        for (const seam::BoundaryVert& v : F.verts) {
            if (first) { f_a_lo = f_a_hi = v.a; first = false; }
            f_a_lo = std::min(f_a_lo, v.a); f_a_hi = std::max(f_a_hi, v.a);
        }
        int in_b = 0, hit = 0;
        for (const seam::BoundaryVert& v : F.verts) {
            const int64_t ca = seam::floor_div2(v.a), cb = seam::floor_div2(v.b);
            if (cb >= c_b_lo && cb <= c_b_hi) ++in_b;
            if (C.find(ca, cb)) ++hit;
        }
        printf("  boundary 2:1: fine %zu verts (y cells %lld..%lld -> %lld..%lld), "
               "coarse %zu verts (y cells %lld..%lld); %d/%zu land in the coarse "
               "z domain, %d onto a coarse cell\n",
               F.verts.size(), (long long)f_a_lo, (long long)f_a_hi,
               (long long)seam::floor_div2(f_a_lo),
               (long long)seam::floor_div2(f_a_hi),
               C.verts.size(), (long long)c_a_lo, (long long)c_a_hi,
               in_b, F.verts.size(), hit);
        CHECK(!F.verts.empty() && !C.verts.empty(), "both sides exported a face");
        CHECK(in_b == int(F.verts.size()),
              "every fine boundary cell halves onto a z cell the coarse tile "
              "owns -- the 2:1 fan has somewhere to land");
        CHECK(seam::floor_div2(f_a_lo) >= c_a_lo - 1 &&
              seam::floor_div2(f_a_hi) <= c_a_hi + 1,
              "the fine face's y-cell span halves onto the coarse face's, to "
              "within the one cell the two slabs' different voxel heights allow");
        // Not every fine cell lands ON a coarse cell that HAS a vertex: a
        // feature thinner than a coarse voxel is resolved on one side only.
        // That is precisely the case the welder must synthesize geometry for,
        // so it is measured rather than gated.
        CHECK(hit > 0, "fine cells do find their coarse parent");
    }

    // =======================================================================
    // M0-WP7 — the overlap band, and the proof it does not move the mesh
    // =======================================================================
    //
    // WHAT CHANGED. `mesh_sector` now samples two extra lattice columns on its
    // -x and -z sides -- the retired `reach = 2`, with no mask and no condition
    // -- and exports the triangles it WOULD have emitted with ownership extended
    // into them as `FaceRecord::band` on kFaceNegX / kFaceNegZ. Ownership in
    // world terms is untouched, so the emitted mesh is supposed to be bitwise
    // what it was. "Supposed to be" is not good enough for a change that widens
    // an array everything else is indexed by: shifting a base by one would still
    // produce a plausible mesh, and the seam suites would still pass.
    //
    // So the mesh is pinned. The hashes below were taken from the build IMMEDIATELY
    // BEFORE the band existed, over configurations chosen to exercise the parts
    // most likely to move: a flat field where every vertex is at a known height,
    // a NEGATIVE tile index (the direction the band extends), both halves of a
    // cross-level pair, and volumetric tiles (whose Y slab is derived from the
    // sampled height range -- the one quantity that WOULD have moved if the band
    // columns were allowed into h_min/h_max, dragging every vertex in the tile
    // with it via y0).
    //
    // The record is pinned alongside the mesh, because the band arrives on the
    // record and a change there would be just as silent.
    //
    // If one of these fires, the band has shifted an index; it is not a licence
    // to re-record the constants.
    {
        static const char* kCave =
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

        // FNV-1a over the raw bytes. Any stable hash would do; this one is here
        // rather than in a shared header so the constants and the function that
        // produced them cannot drift apart.
        struct H {
            unsigned long long h = 1469598103934665603ULL;
            void feed(const void* p, size_t n) {
                const unsigned char* b = (const unsigned char*)p;
                for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
            }
        };
        const auto hash_mesh = [](const SectorMesh& m) {
            H s;
            unsigned long long nb = m.buckets.size(); s.feed(&nb, sizeof nb);
            for (const MaterialBucket& b : m.buckets) {
                s.feed(&b.material, sizeof b.material);
                unsigned long long np = b.positions.size(); s.feed(&np, sizeof np);
                if (np) s.feed(b.positions.data(), np * sizeof(float));
                unsigned long long nn = b.normals.size(); s.feed(&nn, sizeof nn);
                if (nn) s.feed(b.normals.data(), nn * sizeof(float));
            }
            return s.h;
        };
        const auto hash_record = [](const seam::SectorBoundary& sb) {
            H s;
            s.feed(&sb.rung, sizeof sb.rung); s.feed(&sb.cells, sizeof sb.cells);
            s.feed(&sb.tx, sizeof sb.tx); s.feed(&sb.tz, sizeof sb.tz);
            for (const seam::FaceRecord& fr : sb.faces) {
                s.feed(&fr.face, sizeof fr.face);
                s.feed(&fr.plane, sizeof fr.plane);
                s.feed(&fr.cell_layer, sizeof fr.cell_layer);
                unsigned long long n = fr.verts.size(); s.feed(&n, sizeof n);
                for (const seam::BoundaryVert& v : fr.verts) {
                    s.feed(&v.a, sizeof v.a); s.feed(&v.b, sizeof v.b);
                    s.feed(&v.px, sizeof v.px); s.feed(&v.py, sizeof v.py);
                    s.feed(&v.pz, sizeof v.pz);
                    s.feed(&v.nx, sizeof v.nx); s.feed(&v.ny, sizeof v.ny);
                    s.feed(&v.nz, sizeof v.nz);
                    s.feed(&v.material, sizeof v.material);
                    s.feed(&v.corner_signs, sizeof v.corner_signs);
                }
            }
            return s.h;
        };

        struct Pin {
            const char* tag; int field;      // 0 flat, 1 noise, 2 cave
            long long tx, tz; int rung;
            float S, y0, y1;
            unsigned long long mesh, rec;
        };
        // Recorded from the pre-band build. DO NOT re-record to make a failure
        // go away -- see the note above.
        static const Pin kPins[] = {
            {"flat rung0",       0,  0,  0,  0,  16.0f,  -64.0f, 192.0f,
             0xf365ecbc92387da2ULL, 0x30e41a9fde0d546aULL},
            {"flat rung2 (3,-2)",0,  3, -2,  2,  16.0f,  -64.0f, 192.0f,
             0x50d71bf78f1283aaULL, 0xf933a0523773e7aaULL},
            {"noise coarse L2",  1,  0,  0, -3, 512.0f, -300.0f, 300.0f,
             0x94b8bb96f2e6bd14ULL, 0x43b395d3b009718cULL},
            {"noise fine L2",    1,  2,  0, -2, 256.0f, -300.0f, 300.0f,
             0xaee3860b7951bd1bULL, 0xc918717a908ded36ULL},
            {"cave rung0 (1,1)", 2,  1,  1,  0,  64.0f, -128.0f, 192.0f,
             0x75518d8f48d68a54ULL, 0x3a13b481acc73546ULL},
            {"cave rung-1(-3,2)",2, -3,  2, -1, 128.0f, -128.0f, 192.0f,
             0x654236bd591865a9ULL, 0x7a22b1626ce78d5fULL},
        };
        FieldRuntime ff = make(kFlat5), fn = make(kNoise), fc = make(kCave);
        int pinned = 0;
        for (const Pin& p : kPins) {
            const FieldRuntime& f = p.field == 0 ? ff : (p.field == 1 ? fn : fc);
            SectorMesh m; seam::SectorBoundary sb; std::string err;
            CHECK(mesh_sector(f, p.tx, p.tz, p.rung, p.S, p.y0, p.y1, m, &sb, err),
                  err.c_str());
            const unsigned long long hm = hash_mesh(m), hr = hash_record(sb);
            if (hm != p.mesh || hr != p.rec)
                printf("      PIN %-18s mesh %016llx (want %016llx)  record %016llx"
                       " (want %016llx)\n", p.tag, hm, p.mesh, hr, p.rec);
            CHECK(hm == p.mesh,
                  "M0-WP7: the overlap band does not move the MESH by one byte");
            CHECK(hr == p.rec,
                  "M0-WP7: the overlap band does not move the boundary RECORD "
                  "by one byte");
            ++pinned;
        }
        printf("  M0-WP7 mesh identity: %d configurations pinned bitwise against "
               "the pre-band build (mesh + boundary record)\n", pinned);

        // Passing a null boundary_out must produce the same mesh as asking for
        // one. It is the only remaining way the band could leak into geometry,
        // and it is the path the engine takes for tiles it will never weld.
        {
            SectorMesh a, b; seam::SectorBoundary sb; std::string err;
            CHECK(mesh_sector(fc, 1, 1, 0, 64.0f, -128.0f, 192.0f, a, nullptr, err),
                  err.c_str());
            CHECK(mesh_sector(fc, 1, 1, 0, 64.0f, -128.0f, 192.0f, b, &sb, err),
                  err.c_str());
            CHECK(hash_mesh(a) == hash_mesh(b),
                  "asking for a boundary record (and therefore a band) does not "
                  "change the mesh that comes back");
        }

        // --- what the band actually IS ---------------------------------------
        {
            const float S = 64.0f;
            SectorMesh m; seam::SectorBoundary sb; std::string err;
            CHECK(mesh_sector(fc, 2, 3, 0, S, -128.0f, 192.0f, m, &sb, err),
                  err.c_str());
            const double v = seam::rung_voxel(0);
            const seam::OverlapBand& bx = sb.faces[seam::kFaceNegX].band;
            const seam::OverlapBand& bz = sb.faces[seam::kFaceNegZ].band;
            CHECK(bx.triangle_count() > 0 && bz.triangle_count() > 0,
                  "a tile with surface at its -x/-z borders exports both bands");
            CHECK(sb.faces[seam::kFacePosX].band.empty() &&
                  sb.faces[seam::kFacePosZ].band.empty() &&
                  sb.faces[seam::kFacePosY].band.empty() &&
                  sb.faces[seam::kFaceNegY].band.empty(),
                  "bands exist on the -x and -z faces ONLY: the [1..n] rule is "
                  "direction-asymmetric and the +side is bridged by the "
                  "neighbour, so a band there would be overlap on overlap");

            // The band lies WEST of the tile's own -x border and reaches about
            // 2.5 fine voxels back -- one coarse voxel past the ~v/2 the mesh
            // itself already bridges. That is the number the whole mechanism is
            // for: the coarse neighbour's last vertex sits at ~v back, so the
            // band must clear it.
            const double x0 = double(2) * double(S);   // this tile's -x border
            double west = 0.0, east = -1e30;
            for (const seam::OverlapBucket& bk : bx.buckets)
                for (size_t i = 0; i < bk.positions.size(); i += 3) {
                    const double dx = bk.positions[i] - x0;
                    west = std::min(west, dx);
                    east = std::max(east, dx);
                }
            printf("  M0-WP7 band extent: -x band %zu tris spanning x-x0 in "
                   "[%.2f, %.2f] m = [%.2f, %.2f] voxels; -z band %zu tris\n",
                   bx.triangle_count(), west, east, west / v, east / v,
                   bz.triangle_count());
            CHECK(west <= -1.5 * v,
                  "the -x band reaches at least 1.5 fine voxels west of the "
                  "tile's border -- past the coarse neighbour's last vertex at "
                  "~1 voxel, which is the entire point of it");
            CHECK(east <= 0.5 * v + 1e-6,
                  "the band stays WEST: it is the strip the tile does not own, "
                  "not a second copy of the tile's own surface");

            // Determinism, on the same terms as the record's: two bakes of the
            // same tile are byte-identical, so a band can ride a cache entry.
            SectorMesh m2; seam::SectorBoundary sb2;
            CHECK(mesh_sector(fc, 2, 3, 0, S, -128.0f, 192.0f, m2, &sb2, err),
                  err.c_str());
            const seam::OverlapBand& bx2 = sb2.faces[seam::kFaceNegX].band;
            bool same = bx.buckets.size() == bx2.buckets.size();
            for (size_t i = 0; same && i < bx.buckets.size(); ++i)
                same = bx.buckets[i].material == bx2.buckets[i].material &&
                       bx.buckets[i].positions == bx2.buckets[i].positions &&
                       bx.buckets[i].normals == bx2.buckets[i].normals;
            CHECK(same, "two bakes of one tile produce byte-identical bands");
        }
    }

    // =======================================================================
    // M2 — Y BECOMES A TILED AXIS (`mesh_sector_tiled`)
    // Design: docs/volumetric-sectors-design-2026-08-10.md §3.3.
    //
    // Everything above this line is the COLUMN path and stays exactly what it
    // was -- including the six bitwise pins immediately above, which are the
    // gate on that claim. This block is the SECOND regime: the tile is a cube
    // [ty*S, (ty+1)*S) and Y is indexed, owned and welded exactly as X and Z
    // are. The two coexist because a Y-tiled tile covers 64 m of height, so
    // covering a world takes a vertical STACK of requests -- which is the
    // streamer's job (M3, gated on `volumetricSectors`, default off).
    // =======================================================================

    // A volumetric fixture whose TOP SURFACE straddles y = 0: kNoiseCave with
    // its `const 30` height offset replaced by `const 0`, so h = 20*noise runs
    // roughly [-20, 20] instead of [10, 50], with the same ridge tunnels and the
    // same hard floor at -100. Needed because every vertical test wants a tile
    // BOUNDARY the surface actually crosses, and kNoiseCave's surface sits
    // entirely inside one 64 m tile.
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

    // --- M2 (a) tile-local Y, and the tile IS the extent ---------------------
    //
    // The column path returns world-absolute y and meshes [y_min, surface]. The
    // tiled path returns TILE-LOCAL y in [0, S] (plus the ghost/band reach on the
    // low side) and meshes nothing outside its own cube. On kFlat5 both are
    // hand-checkable: the surface is the plane y = 5, so it lands in exactly one
    // tile of any stack, at local height 5 - ty*S.
    {
        FieldRuntime f = make(kFlat5);
        const float S = 4.0f;              // n = 4 at rung 1 (1 m voxel)
        std::string err;
        // ty = 1 covers world y in [4, 8): it holds the surface, at local y = 1.
        SectorMesh m1;
        CHECK(mesh_sector_tiled(f, 0, 1, 0, 1, S, m1, nullptr, err), err.c_str());
        CHECK(m1.triangle_count() > 0, "the tile holding the surface meshes it");
        float ylo = 1e30f, yhi = -1e30f;
        for (const MaterialBucket& b : m1.buckets)
            for (size_t i = 1; i < b.positions.size(); i += 3) {
                ylo = std::min(ylo, b.positions[i]);
                yhi = std::max(yhi, b.positions[i]);
            }
        printf("  M2 tile-local Y: ty=1 (world y in [4,8)) surface at local y in "
               "[%.3f, %.3f] -- world 5 means local 1\n", ylo, yhi);
        CHECK(std::fabs(ylo - 1.0f) < 1e-3f && std::fabs(yhi - 1.0f) < 1e-3f,
              "mesh positions are TILE-LOCAL in y: the y=5 plane comes back at "
              "local y = 1 in the tile whose origin is y = 4");
        // ...and the same field through the COLUMN path still reports world y.
        SectorMesh mc;
        CHECK(mesh_sector(f, 0, 0, 1, S, -64.0f, 192.0f, mc, nullptr, err),
              err.c_str());
        float cy = -1e30f;
        for (const MaterialBucket& b : mc.buckets)
            for (size_t i = 1; i < b.positions.size(); i += 3)
                cy = std::max(cy, b.positions[i]);
        CHECK(std::fabs(cy - 5.0f) < 1e-3f,
              "the column path is untouched: it still returns world-absolute y");

        // Tiles above and below the surface are EMPTY, and that is a normal
        // outcome rather than an error -- §3.3's "resident, no geometry". The
        // column path could not express it: its slab always contained the
        // surface, and a tile that did not was a hard failure ("sampled height
        // outside authored Y range"). That check is gone here on purpose; the
        // octree's Y extent is validated at world load instead (M3).
        for (int64_t ty : {int64_t(3), int64_t(-3)}) {
            SectorMesh me; seam::SectorBoundary sbe;
            CHECK(mesh_sector_tiled(f, 0, ty, 0, 1, S, me, &sbe, err), err.c_str());
            CHECK(me.triangle_count() == 0,
                  "a tile wholly above or below the surface yields no geometry");
            CHECK(sbe.empty() && sbe.band_triangle_count() == 0,
                  "...and no boundary record and no band either: nothing to weld");
        }
        // The column path's Y-range check still fires, unchanged.
        {
            SectorMesh mm; std::string e2;
            CHECK(!mesh_sector(f, 0, 0, 1, S, 10.0f, 20.0f, mm, nullptr, e2),
                  "the column path still rejects a slab that misses the surface");
        }
    }

    // --- M2 (b) THE VERTICAL CROSS-TILE INVARIANT ----------------------------
    //
    // The vertical analogue of (c) above, and the same strongest-form gate:
    // tile A's +y record and tile B's -y record describe the SAME world plane,
    // neither tile was told the other exists, and if the Y indexing is right they
    // name it with the same `plane`, their (a, b) sets coincide, and -- because
    // the dyadic-double construction makes both sides read BITWISE identical
    // samples -- their corner_signs match bit for bit.
    //
    // This is the assertion the whole "no weld for an equal-level pair" claim
    // rests on in the vertical direction. If it fails, every stacked pair in a
    // volumetric world is welded against the wrong cells, silently.
    {
        FieldRuntime f = make(kCave0);
        SectorMesh ma, mb; seam::SectorBoundary sa, sb; std::string err;
        // Stacked equal-level pair sharing the plane y = 0.
        CHECK(mesh_sector_tiled(f, 0, -1, 0, 0, 64.0f, ma, &sa, err), err.c_str());
        CHECK(mesh_sector_tiled(f, 0,  0, 0, 0, 64.0f, mb, &sb, err), err.c_str());
        CHECK(sa.y_tiled && sb.y_tiled && sa.ty == -1 && sb.ty == 0,
              "the record identifies its regime and carries a real ty");
        const seam::FaceRecord& A = sa.faces[seam::kFacePosY];   // lower tile, top
        const seam::FaceRecord& B = sb.faces[seam::kFaceNegY];   // upper tile, base
        CHECK(!A.verts.empty() && !B.verts.empty(),
              "both sides of the horizontal plane exported a face -- the +-y "
              "records are populated at last (they are empty on the column path)");
        CHECK(A.plane == B.plane,
              "stacked equal-rung neighbours name the shared horizontal plane "
              "identically");
        CHECK(A.plane == 0 && A.cell_layer == -1 && B.cell_layer == 0,
              "y = 0 is global lattice 0; the lower tile's top cell layer is -1 "
              "and the upper tile's base layer is 0, with the plane between them");
        int shared = 0, sign_mismatch = 0;
        for (const seam::BoundaryVert& va : A.verts) {
            const seam::BoundaryVert* vb = B.find(va.a, va.b);
            if (!vb) continue;
            ++shared;
            if (va.corner_signs != vb->corner_signs) ++sign_mismatch;
        }
        int a_only = 0, b_only = 0;
        for (const seam::BoundaryVert& va : A.verts)
            if (!B.find(va.a, va.b)) ++a_only;
        for (const seam::BoundaryVert& vb : B.verts)
            if (!A.find(vb.a, vb.b)) ++b_only;
        printf("  M2 vertical equal-rung: A+y %zu verts, B-y %zu verts, %d shared "
               "(a,b), %d sign mismatches, %d only-A, %d only-B\n",
               A.verts.size(), B.verts.size(), shared, sign_mismatch,
               a_only, b_only);
        CHECK(shared > 20, "the two records overlap over most of the plane");
        CHECK(sign_mismatch == 0,
              "corner_signs match BIT FOR BIT across the horizontal plane: both "
              "tiles evaluated the field at the same doubles, which is the "
              "dyadic-double property the whole seam contract rests on");
        CHECK(a_only + b_only < shared,
              "the shared cells dominate: the two records describe one plane");

        // The bridge itself: the lower tile owns the shared cell layer and the
        // UPPER tile is the one whose geometry crosses the plane. Measured off
        // the meshes rather than argued: the upper tile has vertices BELOW its
        // own origin (in its ghost cell), the lower tile has none above its top.
        float b_min = 1e30f, a_max = -1e30f;
        for (const MaterialBucket& bk : mb.buckets)
            for (size_t i = 1; i < bk.positions.size(); i += 3)
                b_min = std::min(b_min, bk.positions[i]);
        for (const MaterialBucket& bk : ma.buckets)
            for (size_t i = 1; i < bk.positions.size(); i += 3)
                a_max = std::max(a_max, bk.positions[i]);
        const float v0 = 2.0f;   // rung 0
        printf("  M2 bridge direction: upper tile reaches to local y %.3f "
               "(%.2f voxels below its origin); lower tile tops out at local y "
               "%.3f (%.2f voxels below its top)\n",
               b_min, b_min / v0, a_max, (64.0f - a_max) / v0);
        CHECK(b_min < 0.0f && b_min >= -1.01f * v0,
              "THE TOP TILE BRIDGES: the upper tile's surface reaches back into "
              "the ghost cell below its own origin, by at most one of its own "
              "voxels -- the vertical form of the east/north rule");
        CHECK(a_max <= 64.0f - 0.0f && a_max < 64.0f,
              "...and the lower tile stops short of its +y border rather than "
              "crossing it, so the plane is bridged exactly once");
    }

    // --- M2 (c) determinism on the tiled path --------------------------------
    // Same gate the column path has: a tiled tile's mesh, record AND bands must
    // be byte-identical across two bakes, or nothing downstream can cache it.
    {
        FieldRuntime f = make(kCave0);
        SectorMesh m1, m2; seam::SectorBoundary s1, s2; std::string err;
        CHECK(mesh_sector_tiled(f, 1, -1, 2, 0, 64.0f, m1, &s1, err), err.c_str());
        CHECK(mesh_sector_tiled(f, 1, -1, 2, 0, 64.0f, m2, &s2, err), err.c_str());
        bool mesh_same = m1.buckets.size() == m2.buckets.size();
        for (size_t i = 0; mesh_same && i < m1.buckets.size(); ++i)
            mesh_same = m1.buckets[i].material == m2.buckets[i].material &&
                        m1.buckets[i].positions == m2.buckets[i].positions &&
                        m1.buckets[i].normals   == m2.buckets[i].normals;
        size_t differing = 0;
        bool rec_same = s1.vert_count() == s2.vert_count() &&
                        s1.ty == s2.ty && s1.y_tiled == s2.y_tiled;
        for (int fa = 0; fa < seam::kFaceCount && rec_same; ++fa) {
            const seam::FaceRecord& X = s1.faces[fa];
            const seam::FaceRecord& Y = s2.faces[fa];
            if (X.verts.size() != Y.verts.size() || X.plane != Y.plane ||
                X.cell_layer != Y.cell_layer) { rec_same = false; break; }
            for (size_t i = 0; i < X.verts.size(); ++i)
                if (std::memcmp(&X.verts[i], &Y.verts[i],
                                sizeof(seam::BoundaryVert)) != 0) ++differing;
            if (X.band.buckets.size() != Y.band.buckets.size()) { rec_same = false; break; }
            for (size_t i = 0; i < X.band.buckets.size(); ++i)
                if (X.band.buckets[i].material  != Y.band.buckets[i].material ||
                    X.band.buckets[i].positions != Y.band.buckets[i].positions ||
                    X.band.buckets[i].normals   != Y.band.buckets[i].normals)
                    rec_same = false;
        }
        printf("  M2 determinism: %zu tris, %zu boundary verts, %zu band tris; "
               "%zu differing record bytes\n", m1.triangle_count(),
               s1.vert_count(), s1.band_triangle_count(), differing);
        CHECK(m1.triangle_count() > 0 && s1.vert_count() > 0,
              "the fixture tile actually produced something to compare");
        CHECK(mesh_same, "two bakes of one tiled tile give an identical MESH");
        CHECK(rec_same && differing == 0,
              "...and an identical boundary RECORD and BANDS");
    }

    // --- M2 (d) the -y overlap band ------------------------------------------
    //
    // WHICH Y FACE, derived: this tile bridges DOWNWARD (see (b)), so its bridge
    // reaches ~v/2 below its base while a neighbour one level coarser stops
    // ~V/2 = v short of the same plane. The strip between them belongs to
    // neither -- exactly the -x/-z situation, one axis over -- so the band goes
    // on kFaceNegY. The +y face is bridged by the tile above it, and a band
    // there would be overlap on top of overlap.
    {
        FieldRuntime f = make(kCave0);
        SectorMesh m; seam::SectorBoundary sb; std::string err;
        const float S = 64.0f;
        CHECK(mesh_sector_tiled(f, 1, 0, 1, 0, S, m, &sb, err), err.c_str());
        const seam::OverlapBand& by = sb.faces[seam::kFaceNegY].band;
        CHECK(by.triangle_count() > 0,
              "a tile with surface at its -y border exports a -y band");
        CHECK(sb.faces[seam::kFacePosY].band.empty() &&
              sb.faces[seam::kFacePosX].band.empty() &&
              sb.faces[seam::kFacePosZ].band.empty(),
              "bands exist on the NEGATIVE faces only, on all three axes");
        CHECK(!sb.faces[seam::kFaceNegX].band.empty() &&
              !sb.faces[seam::kFaceNegZ].band.empty(),
              "and the two horizontal bands are still there alongside it");
        // The band lies BELOW the tile's own -y border and reaches about 2.5
        // fine voxels down -- one coarse voxel past the ~v/2 the mesh itself
        // already bridges, which is the number the mechanism exists for.
        const double v = seam::rung_voxel(0);
        const double y0 = 0.0;              // ty = 0, so the -y border is y = 0
        double below = 0.0, above = -1e30;
        for (const seam::OverlapBucket& bk : by.buckets)
            for (size_t i = 1; i < bk.positions.size(); i += 3) {
                const double dy = bk.positions[i] - y0;
                below = std::min(below, dy);
                above = std::max(above, dy);
            }
        printf("  M2 -y band: %zu tris spanning y-y0 in [%.2f, %.2f] m = "
               "[%.2f, %.2f] voxels\n", by.triangle_count(), below, above,
               below / v, above / v);
        CHECK(below <= -1.5 * v,
              "the -y band reaches at least 1.5 fine voxels below the tile's "
              "base -- past a coarse neighbour's last vertex at ~1 voxel");
        CHECK(above <= 0.5 * v + 1e-6,
              "the band stays BELOW: it is the strip the tile does not own, not "
              "a second copy of the tile's own surface");
        // Band positions are WORLD-absolute even though the mesh is tile-local:
        // a band spans two tiles with two origins, so it cannot inherit either.
        SectorMesh m2; seam::SectorBoundary sb2;
        CHECK(mesh_sector_tiled(f, 1, 1, 1, 0, S, m2, &sb2, err), err.c_str());
        double b2_min = 1e30;
        for (const seam::OverlapBucket& bk : sb2.faces[seam::kFaceNegY].band.buckets)
            for (size_t i = 1; i < bk.positions.size(); i += 3)
                b2_min = std::min(b2_min, bk.positions[i]);
        CHECK(sb2.faces[seam::kFaceNegY].band.triangle_count() == 0 ||
              b2_min > S - 3.0 * v,
              "the ty=1 tile's -y band sits at world y just under 64, not just "
              "under 0: band positions are world-absolute doubles");

        // Asking for no record still gives the same mesh -- the band must never
        // leak into geometry, on the tiled path as on the column one.
        SectorMesh mn;
        CHECK(mesh_sector_tiled(f, 1, 0, 1, 0, S, mn, nullptr, err), err.c_str());
        bool same = mn.buckets.size() == m.buckets.size();
        for (size_t i = 0; same && i < mn.buckets.size(); ++i)
            same = mn.buckets[i].material == m.buckets[i].material &&
                   mn.buckets[i].positions == m.buckets[i].positions;
        CHECK(same,
              "asking for a boundary record (and therefore three bands) does not "
              "change the tiled mesh that comes back");
    }

    // --- M2 (e) the vertical 2:1 map -----------------------------------------
    //
    // The welder's fan is `coarse = floor_div2(fine)` on BOTH tangential axes.
    // On a +-y face those are x and z, and -- unlike a +-x face, where one of
    // them was the untiled Y and had to be read out of the record -- both now
    // come from tile identity. That is the half of the design §3.3 promises:
    // "Y gets the identical ownership treatment as X/Z".
    {
        FieldRuntime f = make(kCave0);
        SectorMesh mf, mc; seam::SectorBoundary sf, sc; std::string err;
        // Coarse tile (0,-1,0) at 128 m / rung -1: x,z in [0,128), y in [-128,0).
        // Fine tile   (0, 0,0) at  64 m / rung  0: x,z in [0, 64), y in [0, 64).
        // They share the horizontal plane y = 0.
        CHECK(mesh_sector_tiled(f, 0, -1, 0, -1, 128.0f, mc, &sc, err), err.c_str());
        CHECK(mesh_sector_tiled(f, 0,  0, 0,  0,  64.0f, mf, &sf, err), err.c_str());
        const seam::FaceRecord& F = sf.faces[seam::kFaceNegY];
        const seam::FaceRecord& C = sc.faces[seam::kFacePosY];
        CHECK(!F.verts.empty() && !C.verts.empty(), "both sides exported a face");
        CHECK(F.plane == 0 && C.plane == 0,
              "y = 0 is lattice 0 at every rung, so both sides name it 0");
        CHECK(seam::floor_div2(F.plane) == C.plane,
              "floor_div2 carries the fine plane index onto the coarse one");
        // Both tangential domains from the coarse tile's identity.
        const int64_t ca_lo = sc.tx * sc.cells, ca_hi = ca_lo + sc.cells - 1;
        const int64_t cb_lo = sc.tz * sc.cells, cb_hi = cb_lo + sc.cells - 1;
        int in_box = 0, hit = 0;
        for (const seam::BoundaryVert& v : F.verts) {
            const int64_t a = seam::floor_div2(v.a), b = seam::floor_div2(v.b);
            if (a >= ca_lo && a <= ca_hi && b >= cb_lo && b <= cb_hi) ++in_box;
            if (C.find(a, b)) ++hit;
        }
        printf("  M2 vertical 2:1: fine %zu verts, coarse %zu verts; %d/%zu halve "
               "into the coarse tile's (x,z) cell box, %d onto a coarse cell\n",
               F.verts.size(), C.verts.size(), in_box, F.verts.size(), hit);
        CHECK(in_box == int(F.verts.size()),
              "every fine -y boundary cell halves onto an (x, z) cell the coarse "
              "tile below actually owns -- the vertical fan has somewhere to land");
        CHECK(hit > 0, "fine cells do find their coarse parent");
    }

    return check_summary();
}
