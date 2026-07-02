#include "world_composer.h"

#include <cstring>
#include <functional>

namespace viewer {

// Row-major 4x4 multiply, matching the ChildInstance/ResolvedInstance convention.
static void mul16(const float* a, const float* b, float* out) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i*4+k] * b[k*4+j];
            out[i*4+j] = s;
        }
}

int WorldComposer::compose(const WorldState& state,
                           SectorResolver& resolver,
                           const lod_select::PartLodTable& lods,
                           const float3& cam_pos) {
    auto resolved = resolver.resolve(state, lods, cam_pos);

    std::vector<TLASManager::DrawInstance> insts;

    const int kMaxDepth = 8;
    const size_t kMaxInstances = 200000;

    // Recursive emit: this node's own BLAS at `world`, then each child at world*child.
    std::function<void(uint64_t, const float*, int, int)> emit =
        [&](uint64_t hash, const float* world, int lod, int depth) {
            if (depth > kMaxDepth || insts.size() >= kMaxInstances) return;
            const LoadedPart* lp = store_.get_or_load(hash);
            if (!lp) return;
            // A geometry-less "assembly" part (e.g. a Tree that only places a
            // Trunk + branches) has no BLAS of its own, but its children must
            // still be expanded -- so only emit an instance when there IS geometry.
            if (!lp->lod_blas.empty()) {
                int use_lod = lod;
                if (use_lod < 0) use_lod = 0;
                if (use_lod >= (int)lp->lod_blas.size()) use_lod = (int)lp->lod_blas.size() - 1;

                TLASManager::DrawInstance di;
                di.blas_handle = lp->lod_blas[use_lod];
                di.material_id = 0;
                di.is_imposter = false;
                std::memcpy(di.transform.m, world, sizeof(di.transform.m));
                insts.push_back(di);
            }

            for (const auto& c : lp->children) {
                float child_world[16];
                mul16(world, c.transform, child_world);
                emit(c.child_resolved_hash, child_world, 0, depth + 1);
            }
        };

    for (const auto& r : resolved) {
        emit(r.part_hash, r.transform, r.lod_level, 0);
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
