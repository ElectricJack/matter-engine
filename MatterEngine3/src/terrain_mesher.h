#pragma once
// terrain_mesher.h — naive surface-nets sector mesher for infinite-world terrain.
// Pure CPU module: depends only on terrain_field.h. No JS, no GL.
// Used by Task 5 (terrainVolume verb) and Tasks 9-10 (WorldSector bake).

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
//   Skirts are emitted along all 4 borders (cross-rung seam cover).
// Returns false + err on degenerate config (rung outside 0..3, sector_size <= 0,
// y_min >= y_max).
bool mesh_sector(const terrain_field::FieldRuntime& field,
                 int64_t tx, int64_t tz, int rung,
                 float sector_size, float y_min, float y_max,
                 SectorMesh& out, std::string& err);

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
// Skirts follow the ACTUAL emitted border polyline (coarse vertices only on
// masked edges) at the same depth policy as mesh_sector, wound outward.
bool mesh_sector_heightfield(const terrain_field::FieldRuntime& field,
                             int64_t tx, int64_t tz, int lod, int edge_mask,
                             float sector_size, float y_min, float y_max,
                             SectorMesh& out, std::string& err);

// Edge-mask bit layout shared by the mesher, the streamer, and WorldSector.
enum EdgeMaskBits {
    kEdgePosX = 1,
    kEdgeNegX = 2,
    kEdgePosZ = 4,
    kEdgeNegZ = 8,
};

} // namespace terrain_mesher
