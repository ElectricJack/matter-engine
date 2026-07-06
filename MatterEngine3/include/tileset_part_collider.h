#pragma once
#include "tileset_collider.h"
#include <cstdint>
#include <string>

namespace tileset {

// Load a baked part from cache_dir and fit its collision proxy.
// cache_dir is the parts/ root parent (the directory that CONTAINS parts/).
// resolved_hash identifies the part (the file is cache_dir/parts/<16-hex>.part).
// override_kind is passed through to fit_collider (nullptr/"auto"/...); see tileset_collider.h.
// Returns false + err (naming the hash and failure reason) on load failure or zero triangles.
bool collider_for_part(const std::string& cache_dir, uint64_t resolved_hash,
                       const char* override_kind,
                       ColliderFit& out, std::string& err);

// Scale a ColliderFit uniformly by factor s.
// Scales: center, half_extent[3], radius, seg_half, all hull_points xyz. volume *= s^3.
ColliderFit scale_fit(const ColliderFit& f, float s);

// Vertical half-height of the fitted shape (distance from center to lowest/highest
// surface point along +Y at identity orientation).
// Sphere  -> radius
// Capsule -> seg_half + radius  (conservative: uses full half-length + cap radius)
// Box     -> sum_i half_extent[i] * |axis[i].y|
// Hull    -> max |pt.y - center.y| over hull points
float fit_half_height(const ColliderFit& f);

} // namespace tileset
