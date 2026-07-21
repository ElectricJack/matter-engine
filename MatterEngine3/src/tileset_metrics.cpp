// tileset_metrics.cpp — pose-delta metric (settle-tick-optimizer.md §II.3).
#include "tileset_metrics.h"
#include "tileset_layout.h"  // kTorusN

#include <algorithm>
#include <cmath>
#include <vector>

namespace tileset {

namespace {

constexpr float kFallbackCharSize = 0.5f;      // meters
constexpr float kIsotropicRatio   = 1.2f;      // extent ratio below => 0.5 weight
constexpr float kRadToDeg         = 180.0f / 3.14159265358979323846f;

// Shortest signed displacement modulo extent (torus wrap). extent <= 0
// disables wrapping (plain difference).
float wrap_delta(float d, float extent) {
    if (extent <= 0.0f) return d;
    d = std::fmod(d, extent);
    if (d >  0.5f * extent) d -= extent;
    if (d < -0.5f * extent) d += extent;
    return d;
}

// Nearest-rank percentile on an ASCENDING-sorted vector: index ceil(q*N)-1.
float nearest_rank(const std::vector<float>& sorted, float q) {
    if (sorted.empty()) return 0.0f;
    int idx = (int)std::ceil(q * (double)sorted.size()) - 1;
    if (idx < 0) idx = 0;
    if (idx >= (int)sorted.size()) idx = (int)sorted.size() - 1;
    return sorted[(size_t)idx];
}

} // namespace

float char_size_of(const ColliderFit& fit) {
    float s = 0.0f;
    switch (fit.type) {
        case ColliderType::Sphere:
        case ColliderType::Capsule:
            s = fit.radius;
            break;
        case ColliderType::Box:
            s = std::max(fit.half_extent[0],
                std::max(fit.half_extent[1], fit.half_extent[2]));
            break;
        case ColliderType::Hull: {
            // Bounding radius: max distance of a hull point from the center.
            const size_t n = fit.hull_points.size() / 3;
            float max_sq = 0.0f;
            for (size_t i = 0; i < n; ++i) {
                float dx = fit.hull_points[i * 3 + 0] - fit.center[0];
                float dy = fit.hull_points[i * 3 + 1] - fit.center[1];
                float dz = fit.hull_points[i * 3 + 2] - fit.center[2];
                float sq = dx * dx + dy * dy + dz * dz;
                if (sq > max_sq) max_sq = sq;
            }
            s = std::sqrt(max_sq);
            // Degenerate hull (no points): fall back to the OBB extent.
            if (s <= 0.0f)
                s = std::max(fit.half_extent[0],
                    std::max(fit.half_extent[1], fit.half_extent[2]));
            break;
        }
    }
    return s > 0.0f ? s : kFallbackCharSize;
}

float orient_weight_of(const ColliderFit& fit) {
    if (fit.type == ColliderType::Sphere) return 0.0f;

    float e0, e1, e2;  // extents along the fit's principal axes
    if (fit.type == ColliderType::Capsule) {
        e0 = fit.seg_half + fit.radius;
        e1 = e2 = fit.radius;
    } else {  // Box, Hull: PCA-OBB half extents (descending)
        e0 = fit.half_extent[0];
        e1 = fit.half_extent[1];
        e2 = fit.half_extent[2];
    }
    float lo = std::min(e0, std::min(e1, e2));
    float hi = std::max(e0, std::max(e1, e2));
    if (lo <= 0.0f) return 1.0f;  // degenerate/flat: rotation is visible
    return (hi / lo < kIsotropicRatio) ? 0.5f : 1.0f;
}

PoseMetricInfo metric_info_of(const ColliderFit& fit) {
    return { char_size_of(fit), orient_weight_of(fit) };
}

PoseDeltaReport compare_settled(const SettledTorus& baseline,
                                const SettledTorus& candidate,
                                const PoseMetricMap& metric_info,
                                const PoseDeltaGate& gate) {
    PoseDeltaReport rep;

    const size_t nb = baseline.instances.size();
    const size_t nc = candidate.instances.size();
    const size_t n  = std::min(nb, nc);
    const bool count_match = (nb == nc);
    rep.skipped = (int)(std::max(nb, nc) - n);

    const float extent = (float)kTorusN * baseline.cfg.size;

    std::vector<float> pos_deltas;     pos_deltas.reserve(n);
    std::vector<float> orient_deltas;  orient_deltas.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        const SettledInstance& a = baseline.instances[i];
        const SettledInstance& b = candidate.instances[i];

        PoseMetricInfo info;  // 0.5 m / full weight fallback
        auto it = metric_info.find(a.child_hash);
        if (it != metric_info.end()) info = it->second;

        // Position: torus-wrapped in x/z, plain in y; normalized by char size.
        float dx = wrap_delta(b.pose.px - a.pose.px, extent);
        float dy = b.pose.py - a.pose.py;
        float dz = wrap_delta(b.pose.pz - a.pose.pz, extent);
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        float cs = info.char_size > 0.0f ? info.char_size : kFallbackCharSize;
        float nd = dist / cs;
        pos_deltas.push_back(nd);
        if (nd > 1.0f) ++rep.teleports;
        if (nd > rep.max) rep.max = nd;

        // Orientation: quaternion angle 2*acos(|dot|), weighted.
        if (info.orient_weight > 0.0f) {
            float dot = a.pose.qx * b.pose.qx + a.pose.qy * b.pose.qy +
                        a.pose.qz * b.pose.qz + a.pose.qw * b.pose.qw;
            float ad = std::fabs(dot);
            if (ad > 1.0f) ad = 1.0f;
            float angle_deg = 2.0f * std::acos(ad) * kRadToDeg;
            orient_deltas.push_back(info.orient_weight * angle_deg);
        }
    }
    rep.compared = (int)n;

    std::sort(pos_deltas.begin(), pos_deltas.end());
    std::sort(orient_deltas.begin(), orient_deltas.end());
    rep.median         = nearest_rank(pos_deltas, 0.50f);
    rep.p95            = nearest_rank(pos_deltas, 0.95f);
    rep.p95_orient_deg = nearest_rank(orient_deltas, 0.95f);

    rep.pass = count_match &&
               rep.p95 <= gate.p95 &&
               rep.teleports <= gate.teleports &&
               rep.p95_orient_deg <= gate.p95_orient_deg;
    return rep;
}

} // namespace tileset
