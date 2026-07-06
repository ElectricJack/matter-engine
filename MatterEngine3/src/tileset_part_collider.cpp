#include "tileset_part_collider.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "bvh.h"    // Tri, float3

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace tileset {

bool collider_for_part(const std::string& cache_dir, uint64_t resolved_hash,
                       const char* override_kind,
                       ColliderFit& out, std::string& err)
{
    // Build the full path: cache_dir + "/parts/<16-hex>.part"
    // cache_path_resolved returns "parts/<16-hex>.part" (relative).
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
