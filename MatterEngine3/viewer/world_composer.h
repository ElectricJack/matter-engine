#ifndef VIEWER_WORLD_COMPOSER_H
#define VIEWER_WORLD_COMPOSER_H

#include "world_source.h"
#include "part_store.h"
#include "sector_resolver.h"
#include "tlas_manager.hpp"     // MSL TLASManager

namespace viewer {

// Per frame: resolve active instances, record each into the TLAS at its LOD's
// BLAS handle, and build. Returns the count of recorded instances.
class WorldComposer {
public:
    WorldComposer(PartStore& store, size_t tlas_capacity)
        : store_(store), tlas_(static_cast<int>(tlas_capacity)) {}

    int compose(const WorldState& state,
                SectorResolver& resolver,
                const lod_select::PartLodTable& lods,
                const float3& cam_pos);

    TLASManager& tlas() { return tlas_; }

private:
    PartStore&  store_;
    TLASManager tlas_;
    // Last built instance set's fingerprint: compose() skips the TLAS rebuild
    // when the (blas_handle, transform) set is unchanged frame-over-frame.
    uint64_t last_fingerprint_ = 0;
    int      last_count_ = -1;
};

} // namespace viewer

#endif // VIEWER_WORLD_COMPOSER_H
