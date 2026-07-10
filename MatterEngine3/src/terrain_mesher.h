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

} // namespace terrain_mesher
