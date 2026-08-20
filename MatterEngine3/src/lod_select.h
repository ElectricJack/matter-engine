#pragma once
// MatterEngine3/src/lod_select.h
//
// Sector-granularity LOD selection on the CPU: given the streamed sector grid,
// a per-part LOD table and the camera position, choose each part's ladder rung
// per sector (or floor-cull it). The engine's resolvers use the result to
// decide which rung's geometry to emit for a sector.
//
// The selection rule itself is NOT here -- it is render/lod_distance.h, the
// one distance-valued rule the Vulkan cull shader also uses. This header is
// the CPU entry point onto it; lod_select.cpp holds the mapping between the
// two forms and the deliberate divergences.
//
// Units and conventions: distances and radii are world-space metres; a
// "projected size" is the dimensionless bound_radius / distance (a tan-approx
// angular radius), scaled by pixel_budget before comparison. Thresholds are on
// that same scale and are ordered fine -> coarse, index 0 finest.
#include "tri.h"
#include "sector_grid.h"
#include <cstdint>
#include <map>
#include <vector>

namespace lod_select {

// Projected screen size = bound_radius / distance (normalized angular extent;
// tan-approx of angular radius). Distance clamped to >= 1e-4 to avoid div-by-zero.
float projected_size(const float3& world_center, float bound_radius,
                     const float3& cam_pos);

// Pick the COARSEST level whose threshold is satisfied (projected_size >= thr).
// thresholds are ordered fine->coarse (index 0 = finest, largest threshold).
// If projected_size clears the finest threshold, returns 0 (finest). If it
// clears nothing, clamps to the coarsest (last) level.
//
// NOTE: no production caller remains — sector selection now goes through
// lod::select_rep (render/lod_distance.h), the single distance-valued rule.
// This is kept as the projected-size reference the tests compare against.
int select_level(float projected_size, const std::vector<float>& thresholds);

// A single instance-ref entry in a part's LOD table: identifies the child part
// and its placement relative to the parent, plus the child's uniform scale factor.
struct PartLodRef {
    uint64_t child_hash;
    float    rel_transform[16];
    float    child_scale;
};

// Per-part LOD metadata needed for selection.
struct PartLod {
    float bound_radius;
    std::vector<float> thresholds;   // matches the part's LodLevels, fine->coarse
    float inline_cutover = 0.0f;     // max over flat_refs with cutover>0; 0 = no refs
    std::vector<PartLodRef> refs;    // instanced-children refs (cutover>0 only)
};
using PartLodTable = std::map<uint64_t, PartLod>;     // resolved_hash -> PartLod

struct LodChoice {
    // Index into the part's LodLevels, 0 = finest. -1 means floor-culled:
    // the part is too small to be worth drawing here and resolvers emit
    // nothing for it. Parts at or above the never-cull radius (terrain-tile
    // sized) are clamped to their coarsest rung instead of ever reaching -1.
    int   level;
    // Distance from the camera to the sector's CLOSEST instance — the same `d`
    // selection used, handed to the caller so downstream decisions (the inline
    // cutover, the child rung) can stay in distance form too. It replaces the
    // projected size this struct used to carry: with the single selection rule
    // in render/lod_distance.h, every consumer compares a distance against
    // normalized_switch_distance(threshold) * reach(...), so nothing needs the
    // size any more.
    float distance;
};

// For each sector, find its CLOSEST instance to cam_pos, and for every distinct
// part hash present in the sector compute its chosen LOD level using the closest
// instance's distance. Parts whose projected size falls below min_projected_size
// get level -1 ("floor-culled": too small to matter — resolvers emit nothing for
// them). The 0.0f default disables the floor.
// pixel_budget scales the projected size before BOTH the floor check and level
// selection — the runtime quality/speed dial (Stage 2). Default 1.0 is bit-
// identical to the pre-budget behaviour.
// _ex returns sector -> (part hash -> LodChoice{level, distance}).
std::map<sector_grid::SectorCoord, std::map<uint64_t, LodChoice>>
select_sector_lods_ex(const sector_grid::Sectors& sectors,
                      const PartLodTable& parts, const float3& cam_pos,
                      float min_projected_size = 0.0f,
                      float pixel_budget = 1.0f);

// Thin wrapper: returns sector -> (part hash -> chosen level index, or -1).
std::map<sector_grid::SectorCoord, std::map<uint64_t,int>>
select_sector_lods(const sector_grid::Sectors& sectors,
                   const PartLodTable& parts, const float3& cam_pos,
                   float min_projected_size = 0.0f,
                   float pixel_budget = 1.0f);

} // namespace lod_select
