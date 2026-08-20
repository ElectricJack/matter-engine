// MatterEngine3/src/provider/resolvers.cpp
//
// Implements SectorLodResolver (declared in sector_resolver.h): the per-frame
// answer to "which instances render this frame, and at which LOD rung?".
//
// resolve() runs once per frame on the caller's thread and does three things:
//   1. Re-bin the world's instances into sector_grid sectors, but ONLY when
//      WorldState::version() changed since the last call. The cached binning is
//      the Stage 1 CPU-floor fix — re-binning ~44k instances into a std::map
//      every frame dominated the frame time.
//   2. lod_select::select_sector_lods_ex picks one rung per (sector, part) from
//      the camera position. This is exact per frame and never cached, so the
//      output matches the uncached implementation exactly.
//   3. Emit one ResolvedInstance per instance in every sector kept by the
//      activation test, plus expanded children for parts past their inline
//      cutover.
//
// Distances, not projected sizes. Both remaining comparisons here go through
// render/lod_distance.h — lod::normalized_switch_distance, lod::reach,
// lod::select_rep — which is THE single LOD rule in the engine (the Vulkan cull
// shader and the other CPU mirror call the same header). Do not reintroduce a
// projected-size comparison here; the in-function comment below carries the
// algebra proving the two forms agree.
//
// Units and spaces: transforms, sector centres and the camera position are all
// world space; pitch_ and active_radius_ are in the same units, and the
// distances handed to lod_distance are distances from the eye. Output order is
// sector-map order (std::map over SectorCoord), so it is deterministic for a
// given world version but is neither world order nor camera order.

#include "sector_resolver.h"
#include "matrix_math.h"
#include "render/lod_distance.h"   // lod::normalized_switch_distance / reach / select_rep

#include "world_flatten.h"     // world_flatten::FlatInstance
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

namespace viewer {

// Stable identity for a child emitted by inline-cutover expansion. Folds the
// parent's stable id, the child's part hash and the child's 1-based ordinal
// with the usual hash_combine constants, so the same child of the same parent
// gets the same id on every frame — anything upstream keyed on stable_id
// depends on that. A result of 0 is remapped to 1, keeping it distinct from the
// 0 that ResolvedInstance::stable_id default-initializes to.
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
    //
    // Both remaining decisions here — the inline cutover and the expanded
    // child's rung — were projected-size comparisons against the same `size`
    // select_sector_lods_ex computed. They are now distance comparisons through
    // the single rule in render/lod_distance.h, using the distance the choice
    // carries. With size = r_parent * pixel_budget / d:
    //
    //   cutover: size >= C
    //          <=> d <= normalized_switch_distance(C) * reach(r_parent, 1, G)
    //
    //   child:  child_size = size * r_child * child_scale / r_parent
    //                      = r_child * child_scale * G / d       (r_parent cancels)
    //           child_size >= thr[i]
    //          <=> d <= normalized_switch_distance(thr[i])
    //                     * reach(r_child, child_scale, G)
    //
    // The child is the one site with a real instance scale; the parent's own
    // selection keeps scale 1.0f, exactly as before.
    std::vector<ResolvedInstance> out;
    std::vector<float> child_switch_distances;   // scratch, reused across refs
    for (const auto& sk : sectors) {
        const sector_grid::SectorCoord& c = sk.first;
        float sx = (c.x + 0.5f) * pitch_;
        float sy = (c.y + 0.5f) * pitch_;
        float sz = (c.z + 0.5f) * pitch_;
        float dx = sx - cam_pos.x, dy = sy - cam_pos.y, dz = sz - cam_pos.z;
        // Activation is a sphere test on the sector CENTRE, not on its bounds:
        // a sector is dropped as soon as its centre leaves active_radius_, even
        // if part of it is still inside. That is why active_radius_ is derived
        // from the outermost terrain LOD band rather than dialled by hand.
        if (std::sqrt(dx*dx + dy*dy + dz*dz) > active_radius_) continue;

        static const std::map<uint64_t, lod_select::LodChoice> kNoLods;
        auto cit = chosen.find(c);
        const auto& lod_for_part = (cit != chosen.end()) ? cit->second : kNoLods;
        for (const auto& inst : sk.second) {
            // No entry means the part is absent from the LOD table, so the
            // cutover branch below cannot fire anyway; +inf is the distance
            // spelling of the 0.0f projected size this used to default to.
            int lod = 0;
            float dist_to_eye = std::numeric_limits<float>::infinity();
            auto it = lod_for_part.find(inst.resolved_hash);
            if (it != lod_for_part.end()) { lod = it->second.level; dist_to_eye = it->second.distance; }
            if (lod < 0) continue;

            auto pit = lods.find(inst.resolved_hash);
            const lod_select::PartLod* pl = (pit != lods.end()) ? &pit->second : nullptr;
            // Inside the cutover distance the parent is emitted as its TRUNK
            // ONLY (segment 0) and each of its refs is emitted beside it as a
            // separate instance with its own rung (segment 1). Outside it, the
            // parent is emitted whole (segment 1, the tail of the loop) and no
            // children are expanded. inline_cutover <= 0 disables the split.
            if (pl && pl->inline_cutover > 0.0f &&
                dist_to_eye <= lod::normalized_switch_distance(pl->inline_cutover)
                                   * lod::reach(pl->bound_radius, 1.0f, pixel_budget_)) {
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
                    // The parent radius cancels out of the child's size, but the
                    // > 0 guard is kept: it is what made the old division safe,
                    // and dropping it would start emitting a selected rung where
                    // the code used to hard-code 0.
                    if (child_it != lods.end() && pl->bound_radius > 0.0f) {
                        const auto& child_thresholds = child_it->second.thresholds;
                        child_switch_distances.clear();
                        child_switch_distances.reserve(child_thresholds.size());
                        for (float t : child_thresholds)
                            child_switch_distances.push_back(
                                lod::normalized_switch_distance(t));
                        cr.lod_level = lod::select_rep(
                            child_switch_distances.data(),
                            (int)child_switch_distances.size(), dist_to_eye,
                            lod::reach(child_it->second.bound_radius,
                                       ref.child_scale, pixel_budget_));
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
