#include "sector_resolver.h"
#include "matrix_math.h"

#include "world_flatten.h"     // world_flatten::FlatInstance
#include <cmath>
#include <cstring>
#include <map>

namespace viewer {

static uint64_t child_stable_id(uint64_t parent, uint64_t part_hash,
                                uint32_t ordinal) {
    uint64_t hash = parent ^ (part_hash + 0x9e3779b97f4a7c15ull +
                              (parent << 6) + (parent >> 2));
    hash ^= static_cast<uint64_t>(ordinal) + 0x9e3779b97f4a7c15ull +
            (hash << 6) + (hash >> 2);
    return hash == 0 ? 1 : hash;
}

static ResolvedInstance to_resolved(const WorldManifestEntry& e, int lod) {
    ResolvedInstance r;
    r.part_hash = e.part_hash;
    r.stable_id = e.instance_id;
    r.lod_level = lod;
    std::memcpy(r.transform, e.transform, sizeof(r.transform));
    return r;
}

std::vector<ResolvedInstance>
PassThroughResolver::resolve(const WorldState& state,
                             const lod_select::PartLodTable&, const float3&) {
    std::vector<ResolvedInstance> out;
    out.reserve(state.entries().size());
    for (const auto& e : state.entries())
        out.push_back(to_resolved(e, 0));
    return out;
}

std::vector<ResolvedInstance>
SectorLodResolver::resolve(const WorldState& state,
                           const lod_select::PartLodTable& lods,
                           const float3& cam_pos) {
    // 1+2. (Re)build the sector binning only when the world content changed.
    // LOD selection below stays exact per frame — identical output to the
    // uncached implementation (Stage 1 constraint).
    if (state.version() != cached_version_) {
        std::vector<world_flatten::FlatInstance> flat;
        flat.reserve(state.entries().size());
        for (const auto& e : state.entries()) {
            world_flatten::FlatInstance fi;
            fi.resolved_hash = e.part_hash;
            fi.stable_id = e.instance_id;
            std::memcpy(fi.world.cell, e.transform, sizeof(fi.world.cell));  // mat4::cell[16]
            flat.push_back(fi);
        }
        sector_grid::SectorGrid grid(pitch_);
        sectors_ = sector_grid::bin_instances(flat, grid);
        cached_version_ = state.version();
        ++rebin_count_;
    }
    const sector_grid::Sectors& sectors = sectors_;
    auto chosen = lod_select::select_sector_lods_ex(sectors, lods, cam_pos,
                                                    min_projected_size_, pixel_budget_);

    // 3. Emit instances only for sectors within the activation sphere.
    std::vector<ResolvedInstance> out;
    for (const auto& sk : sectors) {
        const sector_grid::SectorCoord& c = sk.first;
        float sx = (c.x + 0.5f) * pitch_;
        float sy = (c.y + 0.5f) * pitch_;
        float sz = (c.z + 0.5f) * pitch_;
        float dx = sx - cam_pos.x, dy = sy - cam_pos.y, dz = sz - cam_pos.z;
        if (std::sqrt(dx*dx + dy*dy + dz*dz) > active_radius_) continue;

        static const std::map<uint64_t, lod_select::LodChoice> kNoLods;
        auto cit = chosen.find(c);
        const auto& lod_for_part = (cit != chosen.end()) ? cit->second : kNoLods;
        for (const auto& inst : sk.second) {
            int lod = 0; float ps = 0.0f;
            auto it = lod_for_part.find(inst.resolved_hash);
            if (it != lod_for_part.end()) { lod = it->second.level; ps = it->second.projected_size; }
            if (lod < 0) continue;

            auto pit = lods.find(inst.resolved_hash);
            const lod_select::PartLod* pl = (pit != lods.end()) ? &pit->second : nullptr;
            if (pl && pl->inline_cutover > 0.0f && ps >= pl->inline_cutover) {
                ResolvedInstance r;
                r.part_hash = inst.resolved_hash;
                r.stable_id = inst.stable_id;
                r.lod_level = lod;
                r.segment = 0;
                std::memcpy(r.transform, inst.world.cell, sizeof(r.transform));
                out.push_back(r);
                for (size_t ref_index = 0; ref_index < pl->refs.size();
                     ++ref_index) {
                    const auto& ref = pl->refs[ref_index];
                    ResolvedInstance cr;
                    cr.part_hash = ref.child_hash;
                    cr.stable_id = child_stable_id(
                        inst.stable_id, ref.child_hash,
                        static_cast<uint32_t>(ref_index + 1));
                    cr.segment = 1;
                    matter::Mat4f parent{};
                    matter::Mat4f relative{};
                    std::memcpy(parent.m, inst.world.cell, sizeof parent.m);
                    std::memcpy(relative.m, ref.rel_transform, sizeof relative.m);
                    const matter::Mat4f child = mat4_mul(parent, relative);
                    std::memcpy(cr.transform, child.m, sizeof cr.transform);
                    auto child_it = lods.find(ref.child_hash);
                    if (child_it != lods.end() && pl->bound_radius > 0.0f) {
                        float child_ps = ps * child_it->second.bound_radius * ref.child_scale
                                         / pl->bound_radius;
                        cr.lod_level = lod_select::select_level(child_ps, child_it->second.thresholds);
                    } else {
                        cr.lod_level = 0;
                    }
                    out.push_back(cr);
                }
                continue;
            }
            ResolvedInstance r;
            r.part_hash = inst.resolved_hash;
            r.stable_id = inst.stable_id;
            r.lod_level = lod;
            r.segment = 1;
            std::memcpy(r.transform, inst.world.cell, sizeof(r.transform));
            out.push_back(r);
        }
    }
    return out;
}

} // namespace viewer
