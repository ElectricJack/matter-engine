#include "lod_select.h"
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

std::map<sector_grid::SectorCoord, std::map<uint64_t, LodChoice>>
select_sector_lods_ex(const sector_grid::Sectors& sectors,
                      const PartLodTable& parts, const float3& cam,
                      float min_projected_size, float pixel_budget) {
    std::map<sector_grid::SectorCoord, std::map<uint64_t, LodChoice>> out;
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
            float size = pit->second.bound_radius / closest * pixel_budget;
            int level;
            if (size < min_projected_size) {
                level = (pit->second.bound_radius >= kNeverCullRadius)
                            ? (pit->second.thresholds.empty()
                                   ? 0
                                   : (int)pit->second.thresholds.size() - 1)
                            : -1;
            } else {
                level = select_level(size, pit->second.thresholds);
            }
            out[coord][f.resolved_hash] = {level, size};
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
