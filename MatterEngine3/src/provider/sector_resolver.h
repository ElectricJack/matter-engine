#ifndef VIEWER_SECTOR_RESOLVER_H
#define VIEWER_SECTOR_RESOLVER_H

// MatterEngine3/src/provider/sector_resolver.h
//
// Declares ResolvedInstance and SectorLodResolver: the per-frame LOD and
// activation pass that sits between the authoritative WorldState
// (world_source.h) and the composer that turns instances into draw records.
// The implementation is in resolvers.cpp.
//
// Depends on lod_select (per-sector rung choice), sector_grid (spatial binning)
// and, through the .cpp, render/lod_distance.h — the one LOD rule. Nothing here
// touches the GPU or the filesystem.
//
// Use: construct once with the sector pitch and an activation radius, keep the
// instance alive across frames (the cached sector binning is the whole point of
// keeping it), then call resolve(state, lods, cam_pos) each frame and hand the
// result to the composer. The setters below may be called between frames; none
// of them invalidates the binning cache, which keys on WorldState::version()
// alone.

#include "world_source.h"
#include "lod_select.h"        // lod_select::PartLodTable; also brings in float3/make_float3
#include "sector_grid.h"       // sector_grid::SectorGrid, bin_instances (transitively precomp.h)

#include <cstdint>
#include <vector>

namespace viewer {

// An instance the composer should record this frame, with its chosen LOD level.
struct ResolvedInstance {
    uint64_t part_hash  = 0;
    uint64_t stable_id  = 0;          // authoritative world/path identity
    int      lod_level  = 0;          // index into the part's LOD levels
    int      segment    = 1;          // 0 = fine (trunk only), 1 = coarse/merged (default)
    float    transform[16] = {0};     // row-major world placement
};

// THE resolver: "given the camera, which instances render and at what LOD?"
//
// There is one. A PassThroughResolver used to sit beside this -- every world
// entry emitted at LOD 0, no binning, no culling -- as a correctness reference,
// and it was the default for every world but Meadow. Two resolve paths meant
// two places every knob had to be remembered, and the sub-pixel floor was
// wired to only one of them for long enough that the editor's slider was
// silently inert on the world being tuned with it. SectorLod is a strict
// superset: the same instances, plus per-sector rung selection and
// inline-cutover child expansion.
//
// Bins instances into sectors, picks per-sector LOD via lod_select, and
// activates sectors within `active_radius_` of the camera.
class SectorLodResolver {
public:
    SectorLodResolver(float pitch, float active_radius)
        : pitch_(pitch), active_radius_(active_radius) {}
    // The per-frame pass: re-bin (only if the world version changed), select a
    // rung per sector, emit the instances inside the activation radius plus any
    // inline-cutover children. Allocates and returns a fresh vector every call
    // and is O(active instances) even on the cached path. The float3 is the
    // camera position in world space.
    std::vector<ResolvedInstance>
        resolve(const WorldState&, const lod_select::PartLodTable&, const float3&);
    const char* name() const { return "SectorLod"; }
    // The world's outermost terrain LOD band -- see RenderOptions in
    // matter/world_session.h for why this is derived rather than dialled.
    // Infinity for a world with no bands, which emits every sector.
    void set_active_radius(float r) { active_radius_ = r; }
    void set_min_projected_size(float v) { min_projected_size_ = v; }
    void set_pixel_budget(float b) { pixel_budget_ = b; }
    // Times the sector table was (re)built — bumps only when WorldState::version()
    // changes, never on camera motion.
    int rebin_count() const { return rebin_count_; }

private:
    float pitch_;
    float active_radius_;
    float min_projected_size_ = 0.0f;
    float pixel_budget_ = 1.0f;
    // Binning cache (Stage 1): re-binning ~44k instances into a std::map every
    // frame dominated the CPU floor. Sectors only change when the world does.
    sector_grid::Sectors sectors_;
    uint64_t cached_version_ = UINT64_MAX;
    int      rebin_count_    = 0;
};

} // namespace viewer

#endif // VIEWER_SECTOR_RESOLVER_H
