#include "part_ao_bake.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace part_ao {
namespace {

struct VertKey {                       // position+normal bits: hard edges keep
    uint32_t px, py, pz, nx, ny, nz;   // separate AO from coincident positions
    bool operator==(const VertKey& o) const {
        return px==o.px && py==o.py && pz==o.pz && nx==o.nx && ny==o.ny && nz==o.nz;
    }
};
struct VertKeyHash {
    size_t operator()(const VertKey& k) const {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t w : {k.px, k.py, k.pz, k.nx, k.ny, k.nz}) {
            h ^= w; h *= 1099511628211ull;
        }
        return static_cast<size_t>(h);
    }
};
VertKey key_of(const float3& p, const float3& n) {
    VertKey k;
    std::memcpy(&k.px, &p.x, 4); std::memcpy(&k.py, &p.y, 4);
    std::memcpy(&k.pz, &p.z, 4);
    std::memcpy(&k.nx, &n.x, 4); std::memcpy(&k.ny, &n.y, 4);
    std::memcpy(&k.nz, &n.z, 4);
    return k;
}

// Deterministic ONB (Duff et al. branchless).
void onb(const float3& n, float3& t, float3& b) {
    const float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + n.z);
    t = make_float3(1.0f + sign * n.x * n.x * a, sign * n.x * n.y * a, -sign * n.x);
    b = make_float3(n.x * n.y * a, sign + n.y * n.y * a, -n.y);
}

float ao_at(BVH& bvh, const float3& p, const float3& n,
            uint32_t rays, float radius, uint32_t seed) {
    constexpr float kGolden = 2.39996323f;            // golden-angle spiral
    const float azimuth0 = (seed & 0xFFFFu) * (6.2831853f / 65536.0f);
    float3 t, b; onb(n, t, b);
    const float eps = 1e-3f * radius;
    float occlusion = 0.0f;
    for (uint32_t i = 0; i < rays; ++i) {
        // Spherical-Fibonacci cosine-weighted hemisphere sample.
        const float u = (i + 0.5f) / rays;            // stratified in [0,1)
        const float cos_theta = std::sqrt(1.0f - u);
        const float sin_theta = std::sqrt(u);
        const float phi = azimuth0 + i * kGolden;
        const float3 d_local = make_float3(std::cos(phi) * sin_theta,
                                           std::sin(phi) * sin_theta, cos_theta);
        float3 d = t * d_local.x + b * d_local.y + n * d_local.z;
        BVHRay ray;
        ray.O = p + n * eps;
        ray.D = d;
        ray.rD = make_float3(1.0f / d.x, 1.0f / d.y, 1.0f / d.z);
        ray.hit.t = 1e30f;   // ctor leaves hit uninitialized; Intersect only lowers it
        bvh.Intersect(ray, 0);
        if (ray.hit.t < radius)
            occlusion += 1.0f - ray.hit.t / radius;    // distance-attenuated
    }
    return std::max(0.0f, 1.0f - occlusion / rays);
}

}  // namespace

void bake_part_ao(const std::vector<const std::vector<Tri>*>& group_tris,
                  const std::vector<std::vector<TriEx>*>& group_triex,
                  const AoBakeParams& params, AoBakeStats* stats) {
    if (stats) *stats = AoBakeStats{};
    if (params.quality <= 0.0f) return;

    size_t total = 0;
    for (const auto* g : group_tris) total += g->size();
    if (total == 0) return;

    // Combined part-local soup, 64-aligned for the SIMD Tri type (movaps).
    BvhMesh mesh;
    mesh.triCount = static_cast<int>(total);
    mesh.tri = static_cast<Tri*>(MALLOC64(total * sizeof(Tri)));
    {
        size_t off = 0;
        for (const auto* g : group_tris) {
            if (g->empty()) continue;
            std::memcpy(mesh.tri + off, g->data(), g->size() * sizeof(Tri));
            off += g->size();
        }
    }
    BVH bvh(&mesh);   // ctor allocates + Build()s; centroids computed inside

    // Pass 1: count unique welded (position,normal) keys. The budget is charged
    // against these — the vertices actually raycast — not the raw corner count,
    // which is ~6x larger on welded meshes and used to over-halve rays/vertex
    // into blotchy noise on big parts.
    std::unordered_map<VertKey, float, VertKeyHash> cache;
    cache.reserve(total * 3);
    for (size_t gi = 0; gi < group_tris.size(); ++gi) {
        const std::vector<Tri>& tris = *group_tris[gi];
        const std::vector<TriEx>& triex = *group_triex[gi];
        if (triex.size() != tris.size()) continue;
        for (size_t ti = 0; ti < tris.size(); ++ti) {
            const float3 ps[3] = {tris[ti].vertex0, tris[ti].vertex1,
                                  tris[ti].vertex2};
            const float3 ns[3] = {triex[ti].N0, triex[ti].N1, triex[ti].N2};
            for (int c = 0; c < 3; ++c)
                cache.emplace(key_of(ps[c], ns[c]), -1.0f);
        }
    }
    const uint64_t unique_count = cache.size();
    if (stats) stats->unique_positions = unique_count;

    uint32_t rays = static_cast<uint32_t>(
        std::clamp(std::lround(params.quality * 32.0f), 4l, 128l));
    while (static_cast<uint64_t>(rays) * unique_count > params.max_total_rays &&
           rays > 4)
        rays /= 2;                                     // adaptive: halve to budget
    if (stats) stats->rays_per_vertex = rays;

    // Pass 2: bake each unique vertex once; fan the value out to every corner.
    for (size_t gi = 0; gi < group_tris.size(); ++gi) {
        const std::vector<Tri>& tris = *group_tris[gi];
        std::vector<TriEx>& triex = *group_triex[gi];
        if (triex.size() != tris.size()) continue;
        for (size_t ti = 0; ti < tris.size(); ++ti) {
            const float3 ps[3] = {tris[ti].vertex0, tris[ti].vertex1,
                                  tris[ti].vertex2};
            const float3 ns[3] = {triex[ti].N0, triex[ti].N1, triex[ti].N2};
            float* aos[3] = {&triex[ti].ao0, &triex[ti].ao1, &triex[ti].ao2};
            for (int c = 0; c < 3; ++c) {
                const VertKey k = key_of(ps[c], ns[c]);
                auto found = cache.find(k);
                if (found->second < 0.0f) {
                    const uint32_t seed =
                        static_cast<uint32_t>(VertKeyHash{}(k));
                    found->second = ao_at(bvh, ps[c], ns[c], rays,
                                          params.radius, seed);
                }
                *aos[c] = found->second;
            }
        }
    }

    // BVH and BvhMesh have no destructors (bvh.h) — free what the ctor and we
    // allocated, or every part bake leaks its whole soup + node pool.
    FREE64(bvh.bvhNode);
    delete[] bvh.triIdx;
    FREE64(mesh.tri);
}

}  // namespace part_ao
