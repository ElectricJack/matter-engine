#pragma once
// tileset_part_collider.h — baked part -> ColliderFit, and fit arithmetic.
//
// `tileset_collider.h` fits a proxy to a vertex cloud but knows nothing about
// where parts live; this header is the layer that reads a baked part out of the
// parts cache and hands its triangles over, plus the two operations the settle
// pass needs on the result (uniform scale, vertical half-height).
//
// USED BY the tileset settle stage: `build_settle_plan` (tileset_bake.cpp)
// calls `collider_for_part` once per (child_hash, collider_override), derives
// per-scale copies with `scale_fit`, and uses `fit_half_height` to snap
// non-physics placements onto the base. `metric_info_of` in tileset_metrics.h
// consumes the same fits.
//
// COST. `collider_for_part` opens and parses a whole part bundle from disk; it
// is memoized by its callers for that reason and must not be called per
// instance. `scale_fit` and `fit_half_height` are pure arithmetic on a fit
// already in memory.
//
// SPACES AND UNITS. Metres, in the part's own local space. Nothing here applies
// an instance or world transform.
#include "tileset_collider.h"
#include <cstdint>
#include <string>

namespace tileset {

// Load a baked part from cache_dir and fit its collision proxy.
// cache_dir is the parts/ root parent (the directory that CONTAINS parts/).
// resolved_hash identifies the part; the file path is whatever
// part_asset::cache_path_resolved() returns, which today is
// cache_dir/parts/<16-hex>.bundle, loaded through part_asset::load_v2.
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
