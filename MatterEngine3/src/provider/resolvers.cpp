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
#include "profile.h"
#include "render/lod_distance.h"   // lod::normalized_switch_distance / reach / select_rep

#include "world_flatten.h"     // world_flatten::FlatInstance
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
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

const std::vector<ResolvedInstance>&
SectorLodResolver::resolve(const WorldState& state,
                           const lod_select::PartLodTable& lods,
                           const float3& cam_pos) {
    // 1+2. (Re)build the sector binning only when the world content changed.
    // LOD selection below stays exact per frame — identical output to the
    // uncached implementation (Stage 1 constraint).
    if (&state != cached_state_ || state.version() != cached_version_) {
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
        distinct_parts_.clear();
        for (const auto& sector : sectors_) {
            std::set<uint64_t> hashes;
            for (const auto& instance : sector.second) hashes.insert(instance.resolved_hash);
            distinct_parts_[sector.first].assign(hashes.begin(), hashes.end());
        }
        cached_state_ = &state;
        cached_version_ = state.version();
        output_reusable_ = false;
        ++rebin_count_;
    }
    const sector_grid::Sectors& sectors = sectors_;
    // Timed here rather than inside lod_select.cpp: this is its only per-frame
    // caller, and lod_select.cpp stays free of a ProfileLib link dependency for
    // the hand-picked test link lines that compile it.
    PROFILE_SCOPE_NAMED(sector_lod_scope, "resolve.sector_lod");
    auto chosen = lod_select::select_sector_lods_ex(sectors, lods, cam_pos,
        min_projected_size_, pixel_budget_, &distinct_parts_);
    sector_lod_scope.stop();
    // select_sector_lods_ex reads every binned instance once (its closest-
    // instance distance pass), so what it scanned is the binned total.
    size_t sector_lod_scanned = 0;
    for (const auto& sector : sectors) sector_lod_scanned += sector.second.size();
    PROFILE_COUNT("instances.sector_lod_scanned", sector_lod_scanned);

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
    PROFILE_SCOPE("resolve.emit");
    const auto sector_active = [&](const sector_grid::SectorCoord& c) {
        const float dx = (c.x + 0.5f) * pitch_ - cam_pos.x;
        const float dy = (c.y + 0.5f) * pitch_ - cam_pos.y;
        const float dz = (c.z + 0.5f) * pitch_ - cam_pos.z;
        return !(std::sqrt(dx*dx + dy*dy + dz*dz) > active_radius_);
    };
    size_t active_count = 0;
    Selection selection;
    for (const auto& sector : sectors) {
        if (!sector_active(sector.first)) continue;
        active_count += sector.second.size();
        // Keep even an empty/unknown-part sector in the activation signature:
        // the resolver emits its unknown instances at the default rung.
        auto& levels = selection[sector.first];
        auto found = chosen.find(sector.first);
        if (found != chosen.end())
            for (const auto& part : found->second) levels.emplace(part.first, part.second.level);
    }
    bool reusable = true;
    for (const auto& part : lods)
        if (part.second.inline_cutover > 0.0f) { reusable = false; break; }
    // Without inline expansion, output is determined entirely by world
    // content, active sectors and chosen rungs. Distance may change each frame
    // without changing any of those values. Inline children depend on extra
    // distance/ref data and deliberately keep the full expansion path.
    if (reusable && output_reusable_ && selection == output_selection_) return output_;
    output_reusable_ = false;
    output_selection_ = std::move(selection);
    ++output_rebuild_count_;
    auto& out = output_;
    out.clear();
    // Reserve the active source count once. Growing a multi-million-entry
    // forest vector from zero copied hundreds of megabytes each frame.
    // Inline children may grow it further through the normal vector policy.
    out.reserve(active_count);
    std::vector<float> child_switch_distances;   // scratch, reused across refs
    for (const auto& sk : sectors) {
        const sector_grid::SectorCoord& c = sk.first;
        // Activation is a sphere test on the sector CENTRE, not on its bounds:
        // a sector is dropped as soon as its centre leaves active_radius_, even
        // if part of it is still inside. That is why active_radius_ is derived
        // from the outermost terrain LOD band rather than dialled by hand.
        if (!sector_active(c)) continue;

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
    output_reusable_ = reusable;
    return out;
}

} // namespace viewer
