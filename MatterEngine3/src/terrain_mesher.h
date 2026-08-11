#pragma once
// terrain_mesher.h — naive surface-nets sector mesher for infinite-world terrain.
// Pure CPU module: depends only on terrain_field.h. No JS, no GL.
// Used by Task 5 (terrainVolume verb) and Tasks 9-10 (WorldSector bake).

#include "seam_boundary.h"
#include "terrain_field.h"
#include <cstdint>
#include <string>
#include <vector>

namespace terrain_mesher {

// One material bucket: flat triangle list. x/z are sector-LOCAL
// (world minus sector origin); y is world-absolute.
struct MaterialBucket {
    uint32_t material = 0;         // terrain_field::FieldRuntime::Material cast to uint32_t
    std::vector<float> positions;  // 9 floats per triangle (3 verts x xyz)
    std::vector<float> normals;    // 9 floats per triangle (gradient normals)
};

struct SectorMesh {
    std::vector<MaterialBucket> buckets;
    size_t triangle_count() const {
        size_t n = 0;
        for (const auto& b : buckets) n += b.positions.size() / 9;
        return n;
    }
};

// Naive surface nets over one sector slab.
//   voxel = 2.0f / (1 << rung)   (rung 0..3 -> 2.0 / 1.0 / 0.5 / 0.25)
//   tx, tz: sector tile indices (world origin = tx * sector_size, tz * sector_size)
//   Positions are sector-local (subtract sector origin from world); y is world-absolute.
//   Normals are gradient normals (from the density field).
//   No border skirts: the [1..n] ownership rule makes an EQUAL-rung pair
//   watertight on its own (skirts removed 2026-07-30; see the note at the end
//   of mesh_sector).
//
// A TILE'S GEOMETRY NO LONGER DEPENDS ON ITS NEIGHBOURS (M0-WP1, 2026-08-10).
// This function used to take an `edge_mask` naming the cardinal neighbours that
// were exactly one rung coarser, and it closed the unequal-rung seam at BAKE
// time -- by snapping the fine side's boundary samples onto the coarse side's
// interpolant, and by extending the lattice one coarse voxel outward on the
// -x/-z faces so the fine bridge reached past the coarse tile's last vertex.
// Both are gone, along with the mask itself. Two reasons, and the first is
// fatal on its own:
//
//   The mask described the DESIRED neighbour level, not the DRAWN one. The
//   streamer computes it from the level map it wants; what is on screen is
//   whatever has finished baking, been published, and not been parked. Every
//   transient disagreement -- mid-split, mid-merge, parked, still baking -- is a
//   tile snapped for a neighbour that is not there, and each one opens the very
//   one-voxel strip the mask existed to close. The seam was closed only in the
//   steady state, which a streaming world is rarely in.
//
//   It put a guess about the neighbourhood into the tile's BAKE IDENTITY. The
//   same tile at the same rung had to be re-baked and re-cached under a
//   different key when a neighbour four hundred metres away changed level, and
//   two tiles that differ only in their mask cannot share a cache entry.
//
// Cross-level seams are now generated at RUNTIME by the seam welder, from the
// sparse boundary records both sides export (see `boundary_out` below and
// seam_boundary.h): the welder joins the two tiles that are ACTUALLY drawn, so
// it cannot disagree with them, and it costs the bake nothing. Equal-level
// pairs need no weld at all and never did -- they meet by the shared-sample
// `[1..n]` ownership rule, exactly and bitwise, and that rule is untouched
// here. So is the direction-asymmetric bridging it produces: with `reach` gone,
// a tile's surface reaches one of its OWN voxels back past its -x/-z border and
// stops half a voxel short of its +x/+z border, which is what makes an
// equal-level pair overlap rather than gap. Across levels that asymmetry
// reopens the known ~1-voxel strip on the coarse-west/fine-east side; closing
// it is the welder's job, not this function's.
//
// `boundary_out` (optional, may be null) receives the sparse per-face record
// the welder consumes: for each of the four XZ faces, the boundary cells that
// produced a vertex, keyed by GLOBAL cell index at this tile's rung, with world
// positions, normals, materials and the four plane-corner density signs. It is
// NOT part of the tile's bake identity -- no field of it depends on any
// neighbour. +y/-y stay empty until Y is tiled (M2).
//
// It ALSO receives the per-face OVERLAP BAND on kFaceNegX and kFaceNegZ
// (M0-WP7): the triangles this function would have emitted if ownership ran one
// coarse voxel further back on those two sides. The mesher samples those columns
// unconditionally -- no mask, nothing about a neighbour -- and keeps them out of
// the mesh, so THE EMITTED MESH IS BITWISE WHAT IT WAS; the welder decides at
// runtime whether the drawn pair is cross-level and therefore whether to draw
// the band. That covers the one thing the vertex fan provably cannot: a boundary
// cell where the coarse tile, at twice the voxel, sees no sign change at all and
// so has no surface to land on. Passing null skips the band's emission (the
// extra sampling happens regardless); measured cost of the whole thing is
// +14.6% heightfield / +18.2% volumetric, of which about +11% is the sampling.
//
// `rung` is a power-of-two voxel ladder about a 2 m base, in BOTH directions:
//   3 -> 0.25 m, 2 -> 0.5 m, 1 -> 1 m, 0 -> 2 m, -1 -> 4 m ... -5 -> 64 m.
// THE NEGATIVE HALF IS THE ENTIRE DISTANCE LADDER, not a fallback. The streamer
// pairs a level-L tile (`sector_size << L` metres across) with rung -L, so
// cells-per-tile is CONSTANT at every level and one mesher covers the camera to
// the fog wall. Take the negative rungs away and either tiles stop growing (the
// part explosion nested sector LOD exists to fix) or cells-per-tile grows with
// them (a 2 m voxel out to 2.5 km, measured at 45 GB). `terrain_bands` is a
// radius->rung table, and cross-rung tile pairs are the only reason the seam
// welder exists.
//
// This is also what let the SEPARATE HEIGHTFIELD MESHER be deleted (see the
// note at the end of this header): coarse terrain used to come from its LOD 0-4
// height grids, and having two surfaces meet mid-world printed a visible seam
// between near and far terrain. One representation at two resolutions cannot.
// Returns false + err on degenerate config (rung outside -5..3, sector_size <= 0,
// y_min >= y_max).
bool mesh_sector(const terrain_field::FieldRuntime& field,
                 int64_t tx, int64_t tz, int rung,
                 float sector_size, float y_min, float y_max,
                 SectorMesh& out, seam::SectorBoundary* boundary_out,
                 std::string& err);

// ---------------------------------------------------------------------------
// mesh_sector_tiled — the same surface nets with Y AS A TILED AXIS (M2)
// ---------------------------------------------------------------------------
//
// Design: docs/volumetric-sectors-design-2026-08-10.md §3.3.
//
// `mesh_sector` above meshes a COLUMN: one tile covers [y_min, surface] for its
// (tx, tz), which is why `yMin` is a streamed world's dominant cost dial (a
// StreamCaverns level-0 tile samples ~590 rows of density to reach -1000 m).
// This function meshes a CUBE: the tile is `[ty*S, (ty+1)*S)` in y as well as
// x and z, so it samples at most (n+5)^3 and a tile away from the surface or a
// cavern shell costs one all-air/all-solid sampling pass and yields nothing.
//
// BOTH EXIST ON PURPOSE, AND WHICH ONE RUNS IS THE CALLER'S EXPLICIT CHOICE.
// A Y-tiled tile covers 64 m of height. A world whose streamer still asks for
// one tile per (tx, tz) column would therefore mesh a 64 m band and nothing
// else -- so this cannot simply replace the column path, and it is not a
// migration that can be half-done. The streamer side (a vertical stack of
// requests, 8-child octree descent, `transform[7] = ty * sector_size` on the
// publish transform) is M3 and is gated on the world flag `volumetricSectors`,
// default off. Until then every live world runs `mesh_sector`, whose output is
// pinned BITWISE -- mesh and boundary record, six configurations -- in
// terrain_mesher_tests.cpp. Do not "unify" the two behind an abstraction that
// moves those bytes.
//
// WHAT CHANGES, all of it a straight extension of what x and z already do:
//
//   * Tile origin `oy = double(ty) * double(sector_size)`, sample
//     `y = oy + (j-1) * voxel` -- the same dyadic-`double` construction as x/z,
//     which makes a shared lattice point BITWISE identical from any tile at any
//     level. terrain_mesher.cpp derives that in full; it is the basis of the
//     whole seam contract and not a coincidence worth re-deriving by hand.
//   * Mesh positions are TILE-LOCAL in y as well as x/z.
//   * Ownership is `j in [1..n]`, identically to i and k, and its consequence
//     (derived at `owned` in the .cpp, not assumed) is that THE TOP TILE BRIDGES
//     each horizontal shared plane -- the natural extension of the east/north
//     rule, and what makes an equal-level vertical pair watertight with no weld.
//   * The boundary record covers all six faces symmetrically, so a ±y pair
//     welds through exactly the same `seam::weld_face` fan as a ±x one.
//   * The overlap band (M0-WP7) gains its Y counterpart on kFaceNegY -- the
//     face whose neighbour the `[1..n]` rule under-reaches, by the same argument
//     that put bands on -x/-z. Sampled unconditionally, kept out of the mesh,
//     drawn by the welder only for a cross-level pair.
//
// WHAT IS DELIBERATELY DROPPED: the `h_min < y_min` check ("sampled height
// outside authored Y range"). It asks whether a slab contains the whole surface,
// and a Y-tiled tile is one cube of a stack -- the surface leaving through its
// top or bottom is the normal case. §3.3 moves that validation to world-load
// time, against the octree's Y extent, which is M3's.
//
// Returns false + err on degenerate config (rung outside -5..3, sector_size<=0).
// An all-air or all-solid tile is NOT an error: it returns true with an empty
// mesh and an empty record, which is the "resident, no geometry" outcome §3.3
// requires of the publish path.
bool mesh_sector_tiled(const terrain_field::FieldRuntime& field,
                       int64_t tx, int64_t ty, int64_t tz, int rung,
                       float sector_size,
                       SectorMesh& out, seam::SectorBoundary* boundary_out,
                       std::string& err);

// THERE IS NO SECOND MESHER, AND NO EDGE MASK (both deleted 2026-08-11).
//
// `mesh_sector_heightfield` used to sit here: an N x N regular height grid
// (LODs 0-4) that meshed `height_at` directly instead of a density volume, with
// an `edge_mask`/`EdgeMaskBits` scheme that re-triangulated the border rows
// against a coarser neighbour's even lattice points. Both are gone.
//
// The mesher went because a world cannot be two surfaces. The voxel ladder's
// negative rungs (see `rung` above) already carry terrain out to 64 m voxels,
// so nothing had to switch representation at distance -- and while both existed
// the switch printed a visible seam between near and far terrain. Its last
// caller was the `terrainHeightfield` DSL verb, which no scene in the tree ever
// called.
//
// The mask went in M0-WP1 for the reasons set out at length on `mesh_sector`
// above: it named the level the streamer WANTED next door rather than the tile
// actually drawn, and it put a neighbour guess into the tile's bake identity.
// It outlived the voxel path by a few weeks only as an accepted-and-ignored
// argument, until the ~9 WorldSector.js copies passing it were swept.
//
// A face index into `seam::Face` is now the only way to name a boundary
// direction; the mask bits used to shadow its first four entries.

} // namespace terrain_mesher
