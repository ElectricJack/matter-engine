// tileset_part_collider.cpp — baked part -> collision proxy, and proxy algebra.
//
// The bridge between two layers that otherwise do not know about each other:
// `part_asset_v2` (how a baked part is stored) and `tileset_collider.h` (how a
// vertex cloud becomes a primitive). Everything the tileset settle pass needs
// from a child part's geometry comes through here.
//
// Three functions, and only the first touches the disk:
//   `collider_for_part`  load `<cache_dir>/parts/<hash>.bundle` and fit it.
//   `scale_fit`          uniform scale of an existing fit, no I/O.
//   `fit_half_height`    how far the fit extends vertically from its centre --
//                        the offset the non-physics snap path uses to sit a
//                        prop on the ground (see tileset_bake.cpp).
//
// COST AND CACHING. `collider_for_part` loads and parses an entire part bundle
// (BLAS, TLAS, child instances, LOD levels) to look at triangles, and every
// vertex of every BLAS entry is copied into one flat array before fitting. That
// is far too expensive to repeat per instance, which is why `build_settle_plan`
// memoizes the result per (child_hash, collider_override) and derives per-scale
// variants with `scale_fit` rather than reloading. Do not call it in a loop
// over placements.
//
// SPACES. Everything stays in the part's own local space, in metres. Nothing
// here applies a world or instance transform.
//
// No shared state; safe to call concurrently for different parts.

#include "tileset_part_collider.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "tri.h"    // Tri, float3

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace tileset {

// The vertex cloud is the union of the triangles of EVERY BLAS entry in the
// part, not just the first -- a part is routinely many entries, and fitting
// only one would proxy a fraction of the object. The TLAS and child-instance
// tables are loaded because `load_v2` requires somewhere to put them, but they
// are not consulted: the triangles go into the fit exactly as stored, with no
// placement transform applied.
//
// Vertices are NOT deduplicated, so a shared vertex is weighted once per
// triangle that uses it. That biases the PCA frame toward densely tessellated
// regions; it is accepted because the output is a settle-time proxy, not a
// measurement.
//
// Fails (false + `err` naming the hash) when the bundle cannot be loaded or the
// part has no triangles at all. Both are hard errors for the settle pass --
// there is no empty-collider fallback.
bool collider_for_part(const std::string& cache_dir, uint64_t resolved_hash,
                       const char* override_kind,
                       ColliderFit& out, std::string& err)
{
    // Build the full path: cache_dir + "/parts/<16-hex>.bundle"
    // cache_path_resolved returns "parts/<16-hex>.bundle" (relative).
    std::string rel = part_asset::cache_path_resolved(resolved_hash);
    std::string path = cache_dir + "/" + rel;

    BLASManager blas;
    TLASManager tlas(64);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;

    if (!part_asset::load_v2(path, resolved_hash, blas, tlas, children, lods)) {
        std::ostringstream ss;
        ss << "collider_for_part: failed to load part "
           << std::hex << resolved_hash << " from " << path;
        err = ss.str();
        return false;
    }

    // Gather all triangle vertices into a flat xyz array.
    std::vector<float> xyz;
    for (const auto& entry : blas.get_entries()) {
        for (const auto& t : entry->triangles) {
            xyz.push_back(t.vertex0.x); xyz.push_back(t.vertex0.y); xyz.push_back(t.vertex0.z);
            xyz.push_back(t.vertex1.x); xyz.push_back(t.vertex1.y); xyz.push_back(t.vertex1.z);
            xyz.push_back(t.vertex2.x); xyz.push_back(t.vertex2.y); xyz.push_back(t.vertex2.z);
        }
    }

    size_t vertex_count = xyz.size() / 3;
    if (vertex_count == 0) {
        std::ostringstream ss;
        ss << "collider_for_part: part " << std::hex << resolved_hash << " has zero triangles";
        err = ss.str();
        return false;
    }

    out = fit_collider(xyz.data(), vertex_count, override_kind);
    return true;
}

// `axis` is deliberately NOT scaled: those are unit basis vectors and a uniform
// scale leaves the frame's orientation alone. `type` is preserved too -- scaling
// cannot turn a capsule into a sphere -- so the scaled fit is the same primitive
// at a different size, which is what makes memoizing one base fit per child and
// deriving every scale from it correct.
//
// `s` is applied without validation: a negative or zero factor produces a
// degenerate fit rather than an error.
ColliderFit scale_fit(const ColliderFit& f, float s)
{
    ColliderFit r = f;
    // Scale center.
    r.center[0] = f.center[0] * s;
    r.center[1] = f.center[1] * s;
    r.center[2] = f.center[2] * s;
    // Scale half extents.
    r.half_extent[0] = f.half_extent[0] * s;
    r.half_extent[1] = f.half_extent[1] * s;
    r.half_extent[2] = f.half_extent[2] * s;
    // Scale sphere/capsule radii.
    r.radius   = f.radius   * s;
    r.seg_half = f.seg_half * s;
    // Scale hull points (xyz triples).
    r.hull_points.resize(f.hull_points.size());
    for (size_t i = 0; i < f.hull_points.size(); ++i)
        r.hull_points[i] = f.hull_points[i] * s;
    // Volume scales cubically.
    r.volume = f.volume * s * s * s;
    return r;
}

// Half-height about the fit's own centre, in metres, at identity orientation --
// NOT a distance to the ground. The non-physics snap in tileset_bake.cpp uses it
// as `y = base_height + fh - embed * 2 * fh`, so a value of 0 sits the prop's
// centre exactly on the base. A `Hull` fit with no points therefore returns 0
// and half-sinks the prop; that is the degenerate case to look for if a
// non-physics layer renders buried.
//
// The `Hull` loop walks indices 1, 4, 7, ... -- the y component of each xyz
// triple -- and inherits `hull_points`' subsampling, so it can under-report a
// spike that the decimation dropped (see tileset_collider.h).
float fit_half_height(const ColliderFit& f)
{
    switch (f.type) {
    case ColliderType::Sphere:
        return f.radius;

    case ColliderType::Capsule:
        // Conservative: full seg_half + cap radius.
        return f.seg_half + f.radius;

    case ColliderType::Box: {
        // Sum of half_extent[i] * |axis[i].y| (axis[i][1] is the Y component).
        float h = 0.0f;
        for (int i = 0; i < 3; ++i)
            h += f.half_extent[i] * std::fabs(f.axis[i][1]);
        return h;
    }

    case ColliderType::Hull: {
        // Max |pt.y - center.y| over hull points.
        float h = 0.0f;
        const float cy = f.center[1];
        for (size_t i = 1; i < f.hull_points.size(); i += 3) {
            float dy = std::fabs(f.hull_points[i] - cy);
            if (dy > h) h = dy;
        }
        return h;
    }

    default:
        return 0.0f;
    }
}

} // namespace tileset
