#pragma once
#include "script_host.h"
#include "matter/solid_face_projection.h"
#include <algorithm>
#include <cmath>

namespace script_host {
struct FiniteSurfaceFace {
    gpu_meshing::FaceJob geometry;
    // Receiver plane: dot(position - frame.origin, frame.n) == datum_m.
    float datum_m = 0;
};
// Six outward source/receiver frames: +Z,-Z,+X,-X,+Y,-Y. Jobs borrow the
// evaluated recipe's ops; keep it alive and unchanged until work completes.
// Geometry identity excludes appearance text and the containing part hash.
inline bool plan_finite_surface_faces(const EvaluatedFiniteSurface& source,
                                      std::array<FiniteSurfaceFace, 6>& out,
                                      gpu_meshing::Error& error) {
    const auto fail = [&](const char* text) {
        error = {gpu_meshing::ErrorCode::InvalidInput, text}; return false;
    };
    if (!source.present || !std::isfinite(source.pixel_m) || source.pixel_m < .0001f)
        return fail("finite surface requires a complete recipe and pixelM >= 0.0001");
    std::array<float, 3> half{}, center{};
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float lo = source.bounds_min_m[axis], hi = source.bounds_max_m[axis];
        if (!std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo))
            return fail("finite surface bounds must be finite with positive extent");
        half[axis] = (hi - lo) * .5f; center[axis] = lo + half[axis];
    }
    std::array<FiniteSurfaceFace, 6> result;
    uint64_t pixels = 0;
    for (unsigned side = 0; side < 6; ++side) {
        auto& f = result[side]; auto& j = f.geometry;
        j.source = source.geometry.job();
        j.source_identity = source.geometry.recipe_digest;
        j.frame.origin_m = {center[0], center[1], center[2]};
        unsigned u = 0, v = 1, n = 2;
        const float sign = side % 2 ? -1.f : 1.f;
        j.frame.u = {sign, 0, 0}; j.frame.n = {0, 0, sign};
        if (side == 2 || side == 3) {
            u = 2; n = 0;
            j.frame.u = {0, 0, -sign}; j.frame.n = {sign, 0, 0};
        } else if (side == 4 || side == 5) {
            v = 2; n = 1;
            j.frame.u = {1, 0, 0}; j.frame.v = {0, 0, -sign};
            j.frame.n = {0, sign, 0};
        }
        const auto padded = [&](unsigned axis) {
            return std::ceil(half[axis] / source.pixel_m + 2) * source.pixel_m;
        };
        j.u_max_m = padded(u); j.u_min_m = -j.u_max_m;
        j.v_max_m = padded(v); j.v_min_m = -j.v_max_m;
        j.height_max_m = padded(n); j.height_min_m = -j.height_max_m;
        j.pixel_m = source.pixel_m; j.max_steps = 2048;
        f.datum_m = half[n];
        if (source.version >= 2) continue; // Receiver frames do not project a whole-wall solid.
        gpu_meshing::FaceLayout layout;
        std::vector<gpu_meshing::FaceRegion> regions;
        if (!gpu_meshing::plan_face_regions(j, {}, layout, regions, error)) return false;
        // The source grid includes mesher padding; it is not an exact solid
        // bound. The projector checks clipped-inside ray entries at bake time.
        pixels += uint64_t(layout.width) * layout.height;
        // Bounds aggregate preparation memory before allocating any texels.
        if (pixels > 1024 * 1024)
            return fail("finite surface exceeds the aggregate one-million-pixel source budget");
    }
    error = {}; out = std::move(result); return true;
}
} // namespace script_host
