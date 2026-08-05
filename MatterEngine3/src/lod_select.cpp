#include "lod_select.h"
#include "render/lod_distance.h"   // lod::normalized_switch_distance / reach / select_rep
#include <cmath>
#include <limits>

namespace lod_select {

// Parts at least this large (terrain tiles: ~14 m) are never floor-culled,
// only clamped to their coarsest rung — a structural never-invisible
// guarantee inside the active radius (Stage 2.4). Small scatter (grass,
// pebbles) stays floor-cullable.
static constexpr float kNeverCullRadius = 4.0f;

static float dist(const float3& a, const float3& b) {
    float dx=a.x-b.x, dy=a.y-b.y, dz=a.z-b.z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

float projected_size(const float3& c, float r, const float3& cam) {
    float d = dist(c, cam);
    if (d < 1e-4f) d = 1e-4f;
    return r / d;
}

int select_level(float size, const std::vector<float>& thr) {
    if (thr.empty()) return 0;
    // thr is fine->coarse: thr[0] is the largest threshold. The smallest index i
    // where size >= thr[i] is the finest level the projected size clears.
    for (size_t i = 0; i < thr.size(); ++i)
        if (size >= thr[i]) return (int)i;
    return (int)thr.size() - 1;                // cleared nothing -> coarsest
}

// Sector LOD selection, expressed through the single distance-valued rule in
// render/lod_distance.h. This is an exact re-expression of the projected-size
// comparisons it replaces, not a behaviour change:
//
//   size := r / d * G                       (r = bound_radius, G = pixel_budget)
//
//   rung  : size >= thr[i]  <=>  d <= (1/thr[i]) * (r * 1 * G)
//                           <=>  d <= normalized_switch_distance(thr[i])
//                                       * reach(r, 1.0f, G)
//   floor : size <  F       <=>  d >  normalized_switch_distance(F)
//                                       * reach(r, 1.0f, G)
//
// F == 0 ("floor off") needs no special case: normalized_switch_distance(0) is
// INFINITY, so the floor test is `d > INFINITY` -> false for every d.
//
// The instance scale passed to reach() is deliberately 1.0f. The rule this
// mirrors never looked at the instance transform's scale either — that is a
// pre-existing divergence from cull.comp, and preserving it is what keeps this
// conversion inert.
std::map<sector_grid::SectorCoord, std::map<uint64_t, LodChoice>>
select_sector_lods_ex(const sector_grid::Sectors& sectors,
                      const PartLodTable& parts, const float3& cam,
                      float min_projected_size, float pixel_budget) {
    std::map<sector_grid::SectorCoord, std::map<uint64_t, LodChoice>> out;
    // Normalized switch distances for the part under consideration. Hoisted so
    // the per-instance loop below allocates at most once for the whole call.
    std::vector<float> switch_distances;
    const float floor_distance = lod::normalized_switch_distance(min_projected_size);
    for (const auto& kv : sectors) {
        const auto& coord = kv.first;
        const auto& insts = kv.second;
        float closest = std::numeric_limits<float>::max();
        for (const auto& f : insts) {
            float d = dist(sector_grid::instance_position(f), cam);
            if (d < closest) closest = d;
        }
        if (closest < 1e-4f) closest = 1e-4f;
        for (const auto& f : insts) {
            auto pit = parts.find(f.resolved_hash);
            if (pit == parts.end()) continue;
            const PartLod& part = pit->second;
            const float reach = lod::reach(part.bound_radius, 1.0f, pixel_budget);
            int level;
            if (closest > floor_distance * reach) {
                level = (part.bound_radius >= kNeverCullRadius)
                            ? (part.thresholds.empty()
                                   ? 0
                                   : (int)part.thresholds.size() - 1)
                            : -1;
            } else {
                switch_distances.clear();
                switch_distances.reserve(part.thresholds.size());
                for (float t : part.thresholds)
                    switch_distances.push_back(lod::normalized_switch_distance(t));
                level = lod::select_rep(switch_distances.data(),
                                        (int)switch_distances.size(),
                                        closest, reach);
            }
            out[coord][f.resolved_hash] = {level, closest};
        }
    }
    return out;
}

std::map<sector_grid::SectorCoord, std::map<uint64_t,int>>
select_sector_lods(const sector_grid::Sectors& sectors,
                   const PartLodTable& parts, const float3& cam,
                   float min_projected_size, float pixel_budget) {
    auto ex = select_sector_lods_ex(sectors, parts, cam, min_projected_size, pixel_budget);
    std::map<sector_grid::SectorCoord, std::map<uint64_t,int>> out;
    for (const auto& kv : ex)
        for (const auto& pv : kv.second)
            out[kv.first][pv.first] = pv.second.level;
    return out;
}

} // namespace lod_select
