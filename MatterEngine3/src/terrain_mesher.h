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
// The negative half lets the terrain ladder stay voxel at distance instead of
// switching to the heightfield mesher below, which is what produced a visible
// seam between near and far terrain -- two different surfaces rather than two
// resolutions of one.
// Returns false + err on degenerate config (rung outside -5..3, sector_size <= 0,
// y_min >= y_max).
bool mesh_sector(const terrain_field::FieldRuntime& field,
                 int64_t tx, int64_t tz, int rung,
                 float sector_size, float y_min, float y_max,
                 SectorMesh& out, seam::SectorBoundary* boundary_out,
                 std::string& err);

// Heightfield terrain LOD ladder (2026-07-28 alpine design, LODs 0-4).
//
// Meshes one sector as an N x N regular height grid, N = 1 << lod
// (lod 0 -> one quad, lod 4 -> 16x16 cells). Evaluates height_at once per
// X/Z lattice point (plus four fixed-step probes per USED vertex for the
// gradient normal) and never allocates a voxel density volume. Positions are
// sector-local in x/z, world-absolute in y — same contract as mesh_sector.
//
// edge_mask marks cardinal neighbors that are exactly ONE terrain LOD
// coarser (bit 0 = +x, bit 1 = -x, bit 2 = +z, bit 3 = -z). On a masked
// edge the odd boundary vertices are dropped and the border cell rows are
// re-triangulated against the coarse neighbor's boundary vertices (even
// lattice points), so the shared border polyline is bitwise-identical to
// the coarse neighbor's own edge — watertight, with no T-vertices. Unmasked
// edges keep the full grid boundary, which is bitwise-identical between
// equal-LOD neighbors. lod 0 is the coarsest level and must pass
// edge_mask 0.
//
// Lattice coordinates are computed as double(S) * i / N (exact for the
// power-of-two N), so a coarse neighbor's lattice point and the fine
// sector's even lattice point produce identical height_at arguments.
// Normal probes use a fixed 2 m step regardless of lod, so a border vertex
// shades identically at every level.
//
// No border skirts (removed 2026-07-30): the masked-edge re-triangulation
// above is already watertight, so there was never a crack to cover.
bool mesh_sector_heightfield(const terrain_field::FieldRuntime& field,
                             int64_t tx, int64_t tz, int lod, int edge_mask,
                             float sector_size, float y_min, float y_max,
                             SectorMesh& out, std::string& err);

// Edge-mask bit layout. Now used ONLY by mesh_sector_heightfield above (and by
// the WorldSector.js copies that still compute a mask for the voxel verb, where
// it is accepted and ignored). The voxel path stopped taking a mask in
// M0-WP1 -- see the long note on mesh_sector. Ordered to match seam::Face's
// first four entries so a mask bit and a face index are trivially convertible
// while the two coexist.
enum EdgeMaskBits {
    kEdgePosX = 1,
    kEdgeNegX = 2,
    kEdgePosZ = 4,
    kEdgeNegZ = 8,
};

} // namespace terrain_mesher
