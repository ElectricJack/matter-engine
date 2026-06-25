#pragma once
#include "bvh.h"          // float3, mat4
#include "world_flatten.h"
#include <cstdint>
#include <map>
#include <vector>

namespace sector_grid {

struct SectorCoord { int x, y, z; };
inline bool operator<(const SectorCoord& a, const SectorCoord& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
}
inline bool operator==(const SectorCoord& a, const SectorCoord& b) {
    return a.x==b.x && a.y==b.y && a.z==b.z;
}

// Fixed-pitch axis-aligned grid centered on the world origin. Half-open cells
// [n*pitch, (n+1)*pitch) via floor -> boundary points are assigned deterministically.
class SectorGrid {
public:
    explicit SectorGrid(float pitch) : pitch_(pitch) {}
    SectorCoord sector_of(const float3& world_pos) const;
    float pitch() const { return pitch_; }
private:
    float pitch_;
};

using Sectors = std::map<SectorCoord, std::vector<world_flatten::FlatInstance>>;

// Bin each flattened instance into its sector by world translation.
Sectors bin_instances(const std::vector<world_flatten::FlatInstance>& flat,
                      const SectorGrid& grid);

// World translation of a flattened instance (row-major cell[3,7,11]).
float3 instance_position(const world_flatten::FlatInstance& f);

} // namespace sector_grid
