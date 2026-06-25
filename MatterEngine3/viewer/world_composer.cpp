#include "world_composer.h"

#include <cstring>

namespace viewer {

int WorldComposer::compose(const WorldState& state,
                           SectorResolver& resolver,
                           const lod_select::PartLodTable& lods,
                           const float3& cam_pos) {
    auto resolved = resolver.resolve(state, lods, cam_pos);

    std::vector<TLASManager::DrawInstance> insts;
    insts.reserve(resolved.size());
    for (const auto& r : resolved) {
        const LoadedPart* lp = store_.get_or_load(r.part_hash);
        if (!lp || lp->lod_blas.empty()) continue;
        int lod = r.lod_level;
        if (lod < 0) lod = 0;
        if (lod >= (int)lp->lod_blas.size()) lod = (int)lp->lod_blas.size() - 1;

        TLASManager::DrawInstance di;
        di.blas_handle = lp->lod_blas[lod];
        di.material_id = 0;          // per-triangle materials live in the BLAS; 0 is the TLAS default slot
        di.is_imposter = false;
        std::memcpy(di.transform.m, r.transform, sizeof(di.transform.m));
        insts.push_back(di);
    }

    tlas_.clear();
    tlas_.draw_batch(insts);
    tlas_.build(store_.blas());
    return (int)insts.size();
}

} // namespace viewer
