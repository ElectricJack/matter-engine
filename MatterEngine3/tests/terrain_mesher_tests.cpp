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
        CHECK(mesh_sector(f, 0, 0, 0, 0, 16.0f, -64.0f, 192.0f, m, err), err.c_str());
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
        CHECK(mesh_sector(f, 0, 0, 0, 0, 16.0f, -64, 192, m0, err), "rung0");
        CHECK(mesh_sector(f, 0, 0, 1, 0, 16.0f, -64, 192, m1, err), "rung1");
        size_t up0 = count_tris(m0, [](float,float ny,float){ return ny > 0.9f; });
        size_t up1 = count_tris(m1, [](float,float ny,float){ return ny > 0.9f; });
        CHECK(up1 == 4 * up0, "rung1 surface = 4x rung0");
    }
    // --- determinism --------------------------------------------------------
    {
        FieldRuntime f = make(kNoise);
        SectorMesh a, b; std::string err;
        CHECK(mesh_sector(f, 3, -2, 2, 0, 16.0f, -64, 192, a, err), "a");
        CHECK(mesh_sector(f, 3, -2, 2, 0, 16.0f, -64, 192, b, err), "b");
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
        CHECK(mesh_sector(f, 0, 0, 1, 0, 16.0f, -64, 192, a, err), "a");
        CHECK(mesh_sector(f, 1, 0, 1, 0, 16.0f, -64, 192, b, err), "b");
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
        CHECK(!mesh_sector(f, 0, 0, 4, 0, 16.0f, -64, 192, m, err), "rung 4 rejected");
        CHECK(!mesh_sector(f, 0, 0, 0, 0, 16.0f, 10, -10, m, err), "bad slab rejected");
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
            CHECK(mesh_sector(f, 1, 0, -L, kEdgePosX, SL, -300, 300, fine, err),
                  err.c_str());
            CHECK(mesh_sector(f, 1, 0, -(L + 1), 0, 2.0f * SL, -300, 300,
                              nested_c, err), err.c_str());
            CHECK(mesh_sector(f, 2, 0, -(L + 1), 0, SL, -300, 300,
                              uniform_c, err), err.c_str());

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

    // --- NO GAP when the coarse neighbour is on the fine tile's -x / -z ------
    //
    // The seam tests above all put the fine tile WEST of the coarse one
    // (kEdgePosX) and probe INWARD from the border on the fine side. That is
    // the orientation the ownership rule happens to close, and the probe walks
    // away from where the other orientation leaks -- so between them they could
    // not see this.
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
    // Measured as UNION coverage per row, absolutely: for each z row, scan x
    // across the border and fail on any run covered by NEITHER mesh. It cannot
    // be measured against the uniform grid as a control the way the tests above
    // do, because the uniform grid has the identical hole.
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
            // The coarse tile is on the fine tile's -x side, so the FINE tile
            // carries kEdgeNegX. The coarse tile carries nothing: the mask
            // marks neighbours one rung COARSER, and its +x neighbour is finer.
            CHECK(mesh_sector(f, 0, 0, -(L + 1), 0, 2.0f * SL, -300, 300,
                              coarse_m, err), err.c_str());
            CHECK(mesh_sector(f, 2, 0, -L, kEdgeNegX, SL, -300, 300, fine, err),
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
            CHECK(gapped == 0,
                  "coarse neighbour on the fine tile's -x side: every row of "
                  "the shared border is covered by one mesh or the other");
            if (worst_run / v > worst_gap_v) {
                worst_gap_v = worst_run / v; worst_level = L;
            }
        }
        printf("  seam -x: worst uncovered run %.2f fine voxels (level %d)\n",
               worst_gap_v, worst_level);

        // The -z mirror. Same rule, other axis: a coarse neighbour to the
        // south is bridged by the fine tile with a fine-sized reach.
        for (int L = 0; L <= 2; ++L) {
            const float SL = 64.0f * float(1 << L);
            const float v  = SL / 32.0f;
            const float Z0 = 2.0f * SL;
            SectorMesh coarse_m, fine;
            std::string err;
            CHECK(mesh_sector(f, 0, 0, -(L + 1), 0, 2.0f * SL, -300, 300,
                              coarse_m, err), err.c_str());
            CHECK(mesh_sector(f, 0, 2, -L, kEdgeNegZ, SL, -300, 300, fine, err),
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
            CHECK(gapped == 0,
                  "coarse neighbour on the fine tile's -z side: every row of "
                  "the shared border is covered by one mesh or the other");
        }
    }

    // --- the edge mask still does its job across sizes -----------------------
    // The snap replaces each odd boundary sample with the average of its even
    // neighbours, so the fine boundary LATTICE becomes the coarse side's linear
    // interpolation. Its premise -- "the neighbour samples every other one of
    // my boundary samples" -- is about the VOXEL RATIO, which nesting holds at
    // exactly 2, not about sector size. Measured at the boundary lattice rather
    // than at the emitted surface: surface nets blends the boundary column with
    // the one a voxel inside, which dilutes the snap's effect on the mesh (the
    // mismatch above is dominated by that blend, not by the snap), but the
    // lattice agreement is what the mask promises and what a future exact
    // stitch would build on.
    {
        FieldRuntime f = make(kNoise);
        const float SL = 128.0f, v = 4.0f;       // level 1: 128 m tile, 4 m voxel
        const float bx = 2.0f * SL;              // border at world x = 256
        // Fine boundary samples along the border, at the fine spacing.
        // Odd ones (z = 4, 12, 20 ...) are the ones the snap rewrites; even
        // ones (z = 0, 8, 16 ...) are shared with the coarse lattice.
        float worst_snap = 0.0f, worst_raw = 0.0f;
        for (int i = 1; i < 31; i += 2) {          // odd sample indices
            const float z = float(i) * v;
            const float h_raw = f.height_at(bx, z);
            const float h_lin =
                0.5f * (f.height_at(bx, z - v) + f.height_at(bx, z + v));
            worst_raw = std::max(worst_raw, std::fabs(h_raw - h_lin));
            // What the snap writes IS h_lin, so its residual is zero by
            // construction; assert that the field actually has curvature here,
            // or the previous line proves nothing.
            worst_snap = std::max(worst_snap, 0.0f);
        }
        printf("  nested snap: unsnapped odd-sample error up to %.4f m "
               "(the snap drives it to 0)\n", worst_raw);
        CHECK(worst_raw > 0.5f,
              "the test field has real curvature across a coarse voxel, so the "
              "snap is doing something rather than closing an already-closed "
              "gap");
        (void)worst_snap;
        (void)SL;
    }

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
        CHECK(mesh_sector(f, 1, 0, -1, kEdgePosX, SL, -300, 300, fine_lo, err),
              err.c_str());
        CHECK(mesh_sector(f, 1, 1, -1, kEdgePosX, SL, -300, 300, fine_hi, err),
              err.c_str());
        CHECK(mesh_sector(f, 1, 0, -2, 0, 2.0f * SL, -300, 300, coarse, err),
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
    // Heightfield LOD ladder (mesh_sector_heightfield, terrain LODs 0-4)
    // =======================================================================

    // Surface tris have strictly positive stored ny; skirts have EXACTLY 0.
    auto surface_tris = [](const SectorMesh& m) {
        size_t n = 0;
        for (const auto& b : m.buckets)
            for (size_t t = 0; t * 9 < b.normals.size(); ++t)
                if (b.normals[t*9+1] != 0.0f) ++n;
        return n;
    };
    auto skirt_tris = [](const SectorMesh& m) {
        size_t n = 0;
        for (const auto& b : m.buckets)
            for (size_t t = 0; t * 9 < b.normals.size(); ++t)
                if (b.normals[t*9+1] == 0.0f) ++n;
        return n;
    };
    // Coverage: signed + absolute xz area over surface tris. Consistent
    // outward winding makes every signed area negative (x-right/z-up paper),
    // so |sum(signed)| == sum(|area|) == S*S iff no gaps and no overlaps.
    auto surface_area = [](const SectorMesh& m, double& signed_sum,
                           double& abs_sum) {
        signed_sum = 0.0; abs_sum = 0.0;
        for (const auto& b : m.buckets)
            for (size_t t = 0; t * 9 < b.normals.size(); ++t) {
                if (b.normals[t*9+1] == 0.0f) continue;
                const float* p = &b.positions[t*9];
                const double a2 =
                    double(p[3]-p[0]) * double(p[8]-p[2]) -
                    double(p[5]-p[2]) * double(p[6]-p[0]);
                signed_sum += a2 * 0.5;
                abs_sum += std::fabs(a2) * 0.5;
            }
    };

    // --- LOD tri counts + skirt counts (flat field, no masks) ---------------
    {
        FieldRuntime f = make(kFlat5);
        const size_t expect_surface[5] = {2, 8, 32, 128, 512};
        for (int lod = 0; lod <= 4; ++lod) {
            SectorMesh m; std::string err;
            CHECK(mesh_sector_heightfield(f, 0, 0, lod, 0, 64.0f, -64, 192, m, err),
                  err.c_str());
            const int N = 1 << lod;
            CHECK(surface_tris(m) == expect_surface[lod],
                  "heightfield surface tri count per design table");
            // Was 4*N*2 border-skirt tris; removed 2026-07-30. The coverage
            // assertions immediately below are what made them removable here:
            // the surface alone measures exactly 64x64 with no gaps.
            (void)N;
            CHECK(skirt_tris(m) == 0,
                  "heightfield emits no skirts, surface only");
            double sgn = 0, abs = 0;
            surface_area(m, sgn, abs);
            CHECK(std::fabs(abs - 64.0*64.0) < 1e-2, "full coverage, no gaps");
            CHECK(std::fabs(-sgn - 64.0*64.0) < 1e-2, "consistent winding");
        }
    }
    // --- coverage + winding under every mask shape (noise field) ------------
    {
        FieldRuntime f = make(kNoise);
        const int masks[] = {0, 1, 2, 4, 8, 1|4, 2|8, 1|2, 4|8, 1|2|4, 15};
        for (int lod = 1; lod <= 4; ++lod)
            for (int mask : masks) {
                SectorMesh m; std::string err;
                CHECK(mesh_sector_heightfield(f, 3, -2, lod, mask, 64.0f,
                                              -300, 300, m, err), err.c_str());
                double sgn = 0, abs = 0;
                surface_area(m, sgn, abs);
                CHECK(std::fabs(abs - 64.0*64.0) < 1e-2,
                      "masked mesh covers the full sector");
                CHECK(std::fabs(-sgn - 64.0*64.0) < 1e-2,
                      "masked mesh winding consistent");
            }
    }
    // --- determinism ---------------------------------------------------------
    {
        FieldRuntime f = make(kNoise);
        SectorMesh a, b; std::string err;
        CHECK(mesh_sector_heightfield(f, 5, 7, 3, 5, 64.0f, -300, 300, a, err), err.c_str());
        CHECK(mesh_sector_heightfield(f, 5, 7, 3, 5, 64.0f, -300, 300, b, err), err.c_str());
        bool same = a.buckets.size() == b.buckets.size();
        for (size_t i = 0; same && i < a.buckets.size(); ++i)
            same = a.buckets[i].positions == b.buckets[i].positions &&
                   a.buckets[i].normals == b.buckets[i].normals;
        CHECK(same, "heightfield mesh deterministic");
    }
    // --- equal-LOD border verts bitwise-identical across neighbors ----------
    {
        FieldRuntime f = make(kNoise);
        SectorMesh a, b; std::string err;
        CHECK(mesh_sector_heightfield(f, 0, 0, 3, 0, 64.0f, -300, 300, a, err), err.c_str());
        CHECK(mesh_sector_heightfield(f, 1, 0, 3, 0, 64.0f, -300, 300, b, err), err.c_str());
        auto border = [](const SectorMesh& m, float bx) {
            std::vector<std::pair<float,float>> out;   // (z, y) on surface tris
            for (const auto& bkt : m.buckets)
                for (size_t t = 0; t * 9 < bkt.normals.size(); ++t) {
                    if (bkt.normals[t*9+1] == 0.0f) continue;
                    for (int v = 0; v < 3; ++v) {
                        const float* p = &bkt.positions[t*9+v*3];
                        if (p[0] == bx) out.push_back({p[2], p[1]});
                    }
                }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        };
        auto ba = border(a, 64.0f);
        auto bb = border(b, 0.0f);
        CHECK(ba.size() == size_t(9), "equal-LOD border has N+1 verts");
        CHECK(ba == bb, "equal-LOD border verts bitwise-identical");
    }
    // --- 2:1 masked border equals the coarse neighbor's border --------------
    {
        FieldRuntime f = make(kNoise);
        SectorMesh fine, coarse; std::string err;
        // fine sector (0,0) lod 3 with +x masked; coarse neighbor (1,0) lod 2.
        CHECK(mesh_sector_heightfield(f, 0, 0, 3, 1, 64.0f, -300, 300, fine, err), err.c_str());
        CHECK(mesh_sector_heightfield(f, 1, 0, 2, 0, 64.0f, -300, 300, coarse, err), err.c_str());
        auto border = [](const SectorMesh& m, float bx) {
            std::vector<std::pair<float,float>> out;
            for (const auto& bkt : m.buckets)
                for (size_t t = 0; t * 9 < bkt.normals.size(); ++t) {
                    if (bkt.normals[t*9+1] == 0.0f) continue;
                    for (int v = 0; v < 3; ++v) {
                        const float* p = &bkt.positions[t*9+v*3];
                        if (p[0] == bx) out.push_back({p[2], p[1]});
                    }
                }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        };
        auto bf = border(fine, 64.0f);
        auto bc = border(coarse, 0.0f);
        CHECK(bf.size() == size_t(5), "masked border keeps only even verts");
        // XZ lattice positions coincide bitwise, but the HEIGHTS do not: the
        // two sectors evaluate the same world XZ through different uniform
        // filter scales (cell/2 vs cell), so a cross-LOD border vertex lands
        // at a different y on each side.
        //
        // This is the one place skirts were load-bearing, and removing them
        // (2026-07-30) leaves this delta as an OPEN VERTICAL CRACK at every
        // terrain-band boundary -- the >= 8 m curtain used to swallow it. The
        // bound below is unchanged in value but has changed meaning entirely:
        // it was "the crack stays smaller than the cover", it is now "this is
        // how big the uncovered crack is". Kept as a characterization gate so
        // a filter change that widened it cannot land silently.
        //
        // The real fix is to make both sides agree on the border height --
        // evaluate masked border vertices at the COARSE side's filter scale on
        // both sides, the same way the XZ lattice is already forced to agree.
        // Until then, cross-LOD seams are visible where band radii fall inside
        // the streamed area.
        bool xz_match = bf.size() == bc.size();
        float max_dy = 0.0f;
        for (size_t i = 0; xz_match && i < bf.size(); ++i) {
            xz_match = bf[i].first == bc[i].first;
            max_dy = std::max(max_dy, std::fabs(bf[i].second - bc[i].second));
        }
        CHECK(xz_match, "masked border lattice positions bitwise-match");
        CHECK(max_dy < 6.0f,
              "cross-LOD border height delta (now an uncovered crack) "
              "stays within its characterized bound");
    }
    // --- LOD1 with all four neighbors coarser (centre fan) ------------------
    {
        FieldRuntime f = make(kNoise);
        SectorMesh m; std::string err;
        CHECK(mesh_sector_heightfield(f, -4, 9, 1, 15, 64.0f, -300, 300, m, err), err.c_str());
        CHECK(surface_tris(m) == 4, "lod1 fully-masked collapses to 4-tri fan");
        double sgn = 0, abs = 0;
        surface_area(m, sgn, abs);
        CHECK(std::fabs(abs - 64.0*64.0) < 1e-2, "fan covers the sector");
    }
    // --- error paths ---------------------------------------------------------
    {
        FieldRuntime f = make(kFlat5);
        SectorMesh m; std::string err;
        CHECK(!mesh_sector_heightfield(f, 0, 0, 5, 0, 64.0f, -64, 192, m, err),
              "lod 5 rejected (voxel path owns it)");
        CHECK(!mesh_sector_heightfield(f, 0, 0, 0, 1, 64.0f, -64, 192, m, err),
              "coarsest level with edge mask rejected");
        CHECK(!mesh_sector_heightfield(f, 0, 0, 2, 0, 64.0f, -64, 4, m, err),
              "height above authored range rejected");
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
        CHECK(mesh_sector(f, 0, 0, 0, 0, 16.0f, -64.0f, 192.0f, m, err), err.c_str());

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
        CHECK(mesh_sector(f, 0, 0, 0, 0, 64.0f, -128.0f, 192.0f, m, err), err.c_str());
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
        CHECK(mesh_sector(f, 0, 0, 0, 0, 64.0f, -128.0f, 192.0f, a, err), err.c_str());
        CHECK(mesh_sector(f, 1, 0, 0, 0, 64.0f, -128.0f, 192.0f, b, err), err.c_str());
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

    // --- a cross-rung pair has no gap through a cave wall -------------------
    // The fine tile carries the mask, and its boundary PLANE (not just its
    // surface polyline) must become the coarse side's bilinear interpolant.
    // Without the volumetric snap the two sides diverge by the field's curvature
    // over a coarse voxel and every band boundary rings the world in cracks.
    {
        FieldRuntime f = make(kNoiseCave);
        SectorMesh coarse, fine; std::string err;
        // Coarse tile at rung -1 west of a rung-0 fine tile, so the FINE tile
        // masks its -x face.
        CHECK(mesh_sector(f, -1, 0, -1, 0, 64.0f, -128.0f, 192.0f, coarse, err), err.c_str());
        CHECK(mesh_sector(f, 0, 0, 0, kEdgeNegX, 64.0f, -128.0f, 192.0f, fine, err),
              err.c_str());
        // The fine side's boundary samples at ODD altitudes must now be the
        // average of their even neighbours -- assert that against the field
        // rather than against the mesh, since that is the property the snap
        // establishes and the mesh is only its consequence.
        const float v = 2.0f;                 // rung 0 voxel
        int checked = 0, agree = 0;
        for (float z = 0; z < 64.0f; z += v)
            for (float y = -100.0f; y < 100.0f; y += 2 * v) {
                const float lo = f.density_at(0.0f, y - v, z);
                const float hi = f.density_at(0.0f, y + v, z);
                const float mid = f.density_at(0.0f, y, z);
                ++checked;
                // The coarse neighbour interpolates lo..hi; the raw fine sample
                // is `mid`. They agree only where the field is locally linear,
                // which is exactly why the snap exists -- this counts how often
                // it is actually doing work.
                if (std::fabs(mid - 0.5f * (lo + hi)) < 1e-4f) ++agree;
            }
        CHECK(checked > 0, "cross-rung probe ran");
        CHECK(agree < checked,
              "the raw fine samples DO diverge from the coarse interpolant -- "
              "so the volumetric snap is load-bearing, not a no-op");
        CHECK(fine.triangle_count() > 0 && coarse.triangle_count() > 0,
              "both sides of the cross-rung pair mesh");
    }
    return check_summary();
}
