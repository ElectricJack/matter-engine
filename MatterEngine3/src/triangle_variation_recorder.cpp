// The triangle emitter implementation is owned by ProceduralGeometry and is
// compiled from libs/ProceduralGeometry/src/triangle_emit.cpp;
// VariationRecorder::instance stays here because it folds into the
// part_asset_v2 cache identity.
#include "triangle_emit.hpp"

namespace tri_emit {

uint64_t VariationRecorder::instance(const void* child_source, size_t source_len,
                                     const void* variation_params, size_t params_len,
                                     const mat4& transform) {
    // No grandchildren resolved here (SP-3 territory); fold an empty child list.
    // compute_resolved_hash is SP-1's part_asset:: helper (re-exported into
    // tri_emit), so the variation -> artifact identity matches the rest of the
    // pipeline byte-for-byte.
    uint64_t rh = compute_resolved_hash(child_source, source_len,
                                        variation_params, params_len,
                                        nullptr, 0);
    ChildInstance ci{};
    ci.child_resolved_hash = rh;
    for (int i = 0; i < 16; ++i) ci.transform[i] = transform.cell[i];
    children_.push_back(ci);
    return rh;
}

} // namespace tri_emit
