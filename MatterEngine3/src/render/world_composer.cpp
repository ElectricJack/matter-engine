#include "world_composer.h"
#include "matrix_math.h"

#include <cstring>

namespace viewer {

int WorldComposer::compose(const WorldState& state,
                           SectorResolver& resolver,
                           const lod_select::PartLodTable& lods,
                           const float3& cam_pos) {
    auto resolved = resolver.resolve(state, lods, cam_pos);

    std::vector<TLASManager::DrawInstance> insts;

    const size_t kMaxInstances = 200000;

    // Emit TLAS instances via the shared part-tree traversal (walk_part_tree,
    // depth-capped at 8).  At depth 0 we use the resolved LOD; children get
    // lod=0 (they are always rendered at full detail relative to the parent).
    // Geometry-less assembly parts (lod_blas empty) are skipped but their
    // children are still visited.
    for (const auto& r : resolved) {
        int resolved_lod = r.lod_level;
        walk_part_tree(r.part_hash,
            [&](uint64_t h) -> const LoadedPart* { return store_.get_or_load(h); },
            [&](const LoadedPart* lp, uint64_t /*hash*/, const float rel[16], int depth) {
                if (lp->lod_blas.empty()) return;
                if (insts.size() >= kMaxInstances) return;

                // World transform = r.transform * rel_transform (identity at depth 0).
                matter::Mat4f root{};
                matter::Mat4f relative{};
                std::memcpy(root.m, r.transform, sizeof root.m);
                std::memcpy(relative.m, rel, sizeof relative.m);
                const matter::Mat4f world = viewer::mat4_mul(root, relative);

                int use_lod = (depth == 0) ? resolved_lod : 0;
                if (use_lod < 0) use_lod = 0;
                if (use_lod >= (int)lp->lod_blas.size()) use_lod = (int)lp->lod_blas.size() - 1;

                TLASManager::DrawInstance di;
                di.blas_handle = lp->lod_blas[use_lod];
                di.material_id = 0;
                di.is_imposter = false;
                std::memcpy(di.transform.m, world.m, sizeof(di.transform.m));
                insts.push_back(di);
            });
    }

    // FNV-1a fingerprint over (blas_handle, transform) of every instance: when
    // nothing moved and no LOD switched, skip the full TLAS clear/refit/build.
    uint64_t fp = 1469598103934665603ull;
    auto fold = [&fp](const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) fp = (fp ^ b[i]) * 1099511628211ull;
    };
    for (const auto& di : insts) {
        fold(&di.blas_handle, sizeof di.blas_handle);
        fold(di.transform.m, sizeof di.transform.m);
    }
    if (last_count_ == (int)insts.size() && last_fingerprint_ == fp)
        return last_count_;
    last_count_ = (int)insts.size();
    last_fingerprint_ = fp;

    tlas_.clear();
    tlas_.draw_batch(insts);
    tlas_.build(store_.blas());
    return (int)insts.size();
}

} // namespace viewer
