// MatterEngine3/src/tileset_placement.cpp — see tileset_placement.h for the
// contract; this file documents the internals only.
//
// Every generator here is rejection-based and bounded: it draws candidates
// from `rng` until it has `target` accepted points or its attempt budget runs
// out, whichever happens first. The budgets (16x, 30x and 16x the target,
// plus 64) exist so that a pathological domain -- a density whose Poisson
// min-distance can never be satisfied, or clear disks covering most of the
// rect -- terminates with fewer points instead of spinning forever.
//
// The RNG draw ORDER is the determinism contract. Each generator consumes
// `rng` in a fixed sequence, so changing the number or order of
// next_unit()/next_u64() calls in any of them re-rolls the layout of every
// existing tileset. Treat these bodies as frozen unless that is the intent.

#include "tileset_placement.h"
#include "dsl_rng.h"
#include <cmath>

namespace tileset {

uint64_t placement_seed(uint64_t master, uint32_t layer_index, uint32_t domain_id) {
    // Two SplitMix64 avalanche folds; constants match dsl_rng.h.
    auto mix = [](uint64_t z) {
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    // The +1 on both indices matters: without it layer 0 would fold nothing
    // into `master` and domain 0 would fold nothing into the layer seed, so
    // the first layer's first domain would just be the master seed.
    uint64_t s = mix(master + 0x9E3779B97F4A7C15ull * (uint64_t)(layer_index + 1));
    return mix(s ^ (0xD1B54A32D192ED03ull * (uint64_t)(domain_id + 1)));
}

namespace {

// True when (x, z) is outside every clear disk. The distance test is strict,
// so a candidate exactly `clear_radius` away is accepted. Runs once per
// candidate inside every generator's attempt loop, i.e. O(disks) per draw --
// strip domains carry two disks, interior domains none.
bool clears_disks(const PlacementDomain& d, float x, float z) {
    for (const Point2& c : d.clear_disks) {
        float dx = x - c.x, dz = z - c.z;
        if (dx * dx + dz * dz < d.clear_radius * d.clear_radius) return false;
    }
    return true;
}

float frange(dsl::Rng& r, float a, float b) { return a + (float)r.next_unit() * (b - a); }

std::vector<Point2> scatter_uniform(const PlacementDomain& d, int target, dsl::Rng& rng) {
    std::vector<Point2> out;
    out.reserve((size_t)target);
    int attempts = 0, max_attempts = target * 16 + 64;
    while ((int)out.size() < target && attempts++ < max_attempts) {
        float x = frange(rng, d.x0, d.x1), z = frange(rng, d.z0, d.z1);
        if (!clears_disks(d, x, z)) continue;
        out.push_back({ x, z });
    }
    return out;
}

// Naive dart throwing: each candidate is compared against every point already
// accepted, so this is O(target^2) with no acceleration grid. That is fine for
// the tens-to-hundreds of points one tile domain holds; do not reuse it for a
// world-scale scatter. `density` is used only to pick the min distance -- the
// point count comes from `target`, which the caller already derived from it.
std::vector<Point2> scatter_poisson(const PlacementDomain& d, int target, float density, dsl::Rng& rng) {
    // Dart throwing with min distance r = 0.7/sqrt(density).
    const float r = 0.7f / std::sqrt(density > 1e-6f ? density : 1e-6f);
    const float r2 = r * r;
    std::vector<Point2> out;
    out.reserve((size_t)target);
    int attempts = 0, max_attempts = target * 30 + 64;
    while ((int)out.size() < target && attempts++ < max_attempts) {
        float x = frange(rng, d.x0, d.x1), z = frange(rng, d.z0, d.z1);
        if (!clears_disks(d, x, z)) continue;
        bool ok = true;
        for (const Point2& p : out) {
            float dx = x - p.x, dz = z - p.z;
            if (dx * dx + dz * dz < r2) { ok = false; break; }
        }
        if (ok) out.push_back({ x, z });
    }
    return out;
}

// sigma is a fixed 0.15 in domain-local units (metres), NOT a fraction of the
// domain, so clumps have the same absolute spread in a thin edge-strip domain
// as in a wide interior one. Offsets that leave the rect are rejected rather
// than clamped or wrapped, which thins the result near the domain edges.
std::vector<Point2> scatter_cluster(const PlacementDomain& d, int target, dsl::Rng& rng) {
    // Cluster centers (uniform), then gaussian-ish offsets (Box-Muller from rng).
    std::vector<Point2> out;
    out.reserve((size_t)target);
    int n_centers = target / 8 + 1;
    std::vector<Point2> centers;
    for (int i = 0; i < n_centers; ++i)
        centers.push_back({ frange(rng, d.x0, d.x1), frange(rng, d.z0, d.z1) });
    const float sigma = 0.15f;
    int attempts = 0, max_attempts = target * 16 + 64;
    while ((int)out.size() < target && attempts++ < max_attempts) {
        const Point2& c = centers[(size_t)(rng.next_u64() % (uint64_t)centers.size())];
        float u1 = (float)rng.next_unit(), u2 = (float)rng.next_unit();
        if (u1 < 1e-7f) u1 = 1e-7f;
        float mag = sigma * std::sqrt(-2.0f * std::log(u1));
        float x = c.x + mag * std::cos(6.2831853f * u2);
        float z = c.z + mag * std::sin(6.2831853f * u2);
        if (x < d.x0 || x >= d.x1 || z < d.z0 || z >= d.z1) continue;
        if (!clears_disks(d, x, z)) continue;
        out.push_back({ x, z });
    }
    return out;
}

} // namespace

// The target count is rounded, so a domain whose density * area lands below
// 0.5 yields nothing at all rather than a single instance. An unrecognized
// kind falls through to the uniform scatter.
std::vector<Point2> scatter(PlacementKind kind, const PlacementDomain& dom,
                            float density, uint64_t seed) {
    float area = (dom.x1 - dom.x0) * (dom.z1 - dom.z0);
    int target = (int)std::lround(density * (area > 0.0f ? area : 0.0f));
    if (target <= 0) return {};
    dsl::Rng rng(seed);
    switch (kind) {
        case PlacementKind::Poisson: return scatter_poisson(dom, target, density, rng);
        case PlacementKind::Cluster: return scatter_cluster(dom, target, rng);
        default:                     return scatter_uniform(dom, target, rng);
    }
}

} // namespace tileset
