#include "sector_grid.h"
#include <cmath>

namespace sector_grid {

SectorCoord SectorGrid::sector_of(const float3& p) const {
    return SectorCoord{
        (int)std::floor(p.x / pitch_),
        (int)std::floor(p.y / pitch_),
        (int)std::floor(p.z / pitch_)
    };
}

float3 instance_position(const world_flatten::FlatInstance& f) {
    return make_float3(f.world.cell[3], f.world.cell[7], f.world.cell[11]);
}

Sectors bin_instances(const std::vector<world_flatten::FlatInstance>& flat,
                      const SectorGrid& grid) {
    Sectors out;
    for (const auto& f : flat)
        out[grid.sector_of(instance_position(f))].push_back(f);
    return out;
}

} // namespace sector_grid
