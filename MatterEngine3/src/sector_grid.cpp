// MatterEngine3/src/sector_grid.cpp
//
// Implementation of the fixed-pitch spatial binning declared in
// sector_grid.h: floor-division from a world position to a `SectorCoord`, and
// the bulk pass that groups a flattened world's instances by sector.
//
// This is the BAKE-TIME composition grid -- it is consumed by lod_select.h and
// provider/sector_resolver.h. It is NOT the runtime streaming grid;
// `matter_stream::SectorStreamer` (sector_streamer.h) has its own tile keys,
// level ladder and hysteresis, and the two share nothing but the idea of
// dividing the world by a pitch. Do not reason from one to the other.
//
// Pure CPU: no engine state, no I/O, no threads. Every function here is a
// deterministic function of its arguments, which is what lets a bake's
// per-sector output order be reproducible.
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

// Group every flattened instance by the sector its world TRANSLATION lands in.
//
// Binning is by the instance origin alone: a part whose geometry straddles a
// sector boundary still lands wholly in the sector containing its translation,
// so consumers must not assume a sector's contents are geometrically inside
// it.
//
// COPIES each FlatInstance into the returned map -- `Sectors` is a value
// container, not a view -- so the result is roughly the size of `flat` again.
// Iteration order is the std::map ordering on SectorCoord (x, then y, then z),
// which is the determinism guarantee downstream bakes depend on.
Sectors bin_instances(const std::vector<world_flatten::FlatInstance>& flat,
                      const SectorGrid& grid) {
    Sectors out;
    for (const auto& f : flat)
        out[grid.sector_of(instance_position(f))].push_back(f);
    return out;
}

} // namespace sector_grid
