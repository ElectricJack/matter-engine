#ifndef VIEWER_SECTOR_RESOLVER_H
#define VIEWER_SECTOR_RESOLVER_H

#include "world_source.h"
#include "lod_select.h"        // lod_select::PartLodTable; also brings in float3/make_float3
#include "sector_grid.h"       // sector_grid::SectorGrid, bin_instances (transitively precomp.h)

#include <cstdint>
#include <vector>

namespace viewer {

// An instance the composer should record this frame, with its chosen LOD level.
struct ResolvedInstance {
    uint64_t part_hash  = 0;
    int      lod_level  = 0;          // index into the part's LOD levels
    float    transform[16] = {0};     // row-major world placement
};

// Strategy: "given the camera, which instances render and at what LOD?"
class SectorResolver {
public:
    virtual ~SectorResolver() = default;
    virtual std::vector<ResolvedInstance>
        resolve(const WorldState& state,
                const lod_select::PartLodTable& lods,
                const float3& cam_pos) = 0;
    virtual const char* name() const = 0;
};

// Baseline: all instances active at LOD 0, no culling. Correctness reference.
class PassThroughResolver : public SectorResolver {
public:
    std::vector<ResolvedInstance>
        resolve(const WorldState&, const lod_select::PartLodTable&, const float3&) override;
    const char* name() const override { return "PassThrough"; }
};

// Bins instances into sectors, picks per-sector LOD via lod_select, and
// activates sectors within `active_radius_` of the camera.
class SectorLodResolver : public SectorResolver {
public:
    SectorLodResolver(float pitch, float active_radius)
        : pitch_(pitch), active_radius_(active_radius) {}
    std::vector<ResolvedInstance>
        resolve(const WorldState&, const lod_select::PartLodTable&, const float3&) override;
    const char* name() const override { return "SectorLod"; }
    void set_active_radius(float r) { active_radius_ = r; }
    void set_min_projected_size(float v) { min_projected_size_ = v; }

private:
    float pitch_;
    float active_radius_;
    float min_projected_size_ = 0.0f;
};

} // namespace viewer

#endif // VIEWER_SECTOR_RESOLVER_H
