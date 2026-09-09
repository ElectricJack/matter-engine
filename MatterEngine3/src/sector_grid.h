#pragma once
// MatterEngine3/src/sector_grid.h
//
// A fixed-pitch, axis-aligned 3-D grid over world space, plus the pass that
// bins a flattened world (`world_flatten::FlatInstance`) into it.
//
// This is the bake-time COMPOSITION grid, used by lod_select.h and
// provider/sector_resolver.h to cut a whole flattened world into
// sector-sized pieces. It is NOT the runtime streaming grid:
// `matter_stream::SectorStreamer` in sector_streamer.h has its own tile key
// format, level ladder, hysteresis and eviction rules, and the two agree on
// nothing beyond the idea of dividing the world by a pitch. The names are
// similar and the concepts are not.
//
// Units are world METRES throughout; `pitch` is a sector's edge length, the
// same on all three axes. Cells are half-open, [n*pitch, (n+1)*pitch), so a
// position exactly on a boundary belongs to the upper cell -- deterministically
// and including for negative coordinates, because floor (not truncation) is
// what does the division.
//
// Dependencies are just tri.h for `float3` (from libs/SpatialQueryLib, which
// despite its name owns the engine's core geometry types) and world_flatten.h
// for the instance record. No raylib, no Vulkan, no engine state.
#include "tri.h"          // float3, mat4
#include "world_flatten.h"
#include <cstdint>
#include <map>
#include <vector>

namespace sector_grid {

// A cell address: the floor of a world position divided by the pitch, per
// axis. Negative values are ordinary, not an edge case -- the grid is centred
// on the world origin and extends both ways on every axis.
//
// `operator<` below is a plain lexicographic (x, y, z) ordering. It exists to
// key the `Sectors` map, and that ordering fixes the sector iteration order of
// a bake, so it is a determinism dependency rather than a convenience.
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

// Sector -> the instances whose world translation falls inside it.
// Deliberately std::map rather than unordered_map: the sorted iteration order
// IS the bake's determinism guarantee (see SectorCoord's ordering above).
// Instances are held by value, so a Sectors is about as large as the flat
// instance list it was built from.
using Sectors = std::map<SectorCoord, std::vector<world_flatten::FlatInstance>>;

// Bin each flattened instance into its sector by world translation.
Sectors bin_instances(const std::vector<world_flatten::FlatInstance>& flat,
                      const SectorGrid& grid);

// World translation of a flattened instance (row-major cell[3,7,11]).
float3 instance_position(const world_flatten::FlatInstance& f);

} // namespace sector_grid
