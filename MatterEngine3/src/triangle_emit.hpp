#pragma once
// Legacy include/build boundary. The triangle emitter is owned by
// ProceduralGeometry; the cache-coupled variation recorder stays with the
// engine because it uses part_asset_v2's resolved hash.
#include "../../libs/ProceduralGeometry/src/triangle_emit.hpp"
#include "part_asset_v2.h"  // SP-1: part_asset::ChildInstance, part_asset::compute_resolved_hash

namespace tri_emit {

// Variation binding. SP-1 (part_asset_v2.h) is implemented at execution time, so
// we consume its real ChildInstance + compute_resolved_hash rather than a local
// mirror; they are re-exported into tri_emit so callers/tests use one name. The
// struct layout matches the SP-1 spec byte-for-byte (8 + 64 = 72 bytes).
using ChildInstance = part_asset::ChildInstance;
using part_asset::compute_resolved_hash;

// Records instance(child, variation) calls. "variation" = the params bytes bound
// at instance time; they fold into the child's resolved hash so identical params
// dedup to one artifact (consistent with SP-3). Independent of LOD.
class VariationRecorder {
public:
    // Records a child instance at the current transform; returns the child's
    // resolved hash (the cache key / artifact identity).
    uint64_t instance(const void* child_source, size_t source_len,
                      const void* variation_params, size_t params_len,
                      const mat4& transform);

    const std::vector<ChildInstance>& children() const { return children_; }
    void clear() { children_.clear(); }

private:
    std::vector<ChildInstance> children_;
};

} // namespace tri_emit
