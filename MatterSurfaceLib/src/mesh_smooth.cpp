// Taubin lambda/mu smoothing (shrink-free Laplacian) on MeshIndexed.
#include "mesh_smooth.hpp"

#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace {

std::pair<uint32_t, uint32_t> edge_key(uint32_t a, uint32_t b) {
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

} // namespace

SmoothResult smooth(const MeshIndexed& in, const SmoothOptions& opts) {
    SmoothResult r;
    if (in.positions.empty() || in.indices.empty()) { r.err = "smooth: empty mesh"; return r; }
    if (in.indices.size() % 3 != 0) { r.err = "smooth: index count not a multiple of 3"; return r; }
    const uint32_t nv = (uint32_t)in.positions.size();
    for (uint32_t i : in.indices)
        if (i >= nv) { r.err = "smooth: index out of range"; return r; }
    if (!in.triex.empty() && in.triex.size() != in.indices.size() / 3) {
        r.err = "smooth: triex not parallel to triangles"; return r;
    }
    if (opts.iterations < 1)    { r.err = "smooth: iterations must be >= 1"; return r; }
    if (!(opts.lambda > 0.0f))  { r.err = "smooth: lambda must be > 0";      return r; }
    if (!(opts.mu < 0.0f))      { r.err = "smooth: mu must be < 0";          return r; }

    // 1-ring adjacency and boundary flags. std::map over sorted edge keys makes
    // ring order deterministic -> byte-stable output.
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t t = 0; t + 2 < in.indices.size(); t += 3) {
        uint32_t a = in.indices[t], b = in.indices[t + 1], c = in.indices[t + 2];
        ++edges[edge_key(a, b)];
        ++edges[edge_key(b, c)];
        ++edges[edge_key(c, a)];
    }
    std::vector<std::vector<uint32_t>> ring(nv);
    std::vector<bool> boundary(nv, false);
    for (const auto& e : edges) {
        ring[e.first.first].push_back(e.first.second);
        ring[e.first.second].push_back(e.first.first);
        if (e.second != 2) {  // boundary or non-manifold edge: hold both ends fixed
            boundary[e.first.first]  = true;
            boundary[e.first.second] = true;
        }
    }

    r.mesh = in;
    std::vector<float3>& pos = r.mesh.positions;
    std::vector<float3> tmp(nv);

    auto umbrella_step = [&](const std::vector<float3>& src, float w,
                             std::vector<float3>& dst) {
        for (uint32_t v = 0; v < nv; ++v) {
            if (boundary[v] || ring[v].empty()) { dst[v] = src[v]; continue; }
            float ax = 0.0f, ay = 0.0f, az = 0.0f;
            for (uint32_t nb : ring[v]) { ax += src[nb].x; ay += src[nb].y; az += src[nb].z; }
            const float inv = 1.0f / (float)ring[v].size();
            ax *= inv; ay *= inv; az *= inv;
            dst[v] = make_float3(src[v].x + w * (ax - src[v].x),
                                 src[v].y + w * (ay - src[v].y),
                                 src[v].z + w * (az - src[v].z));
        }
    };

    for (int it = 0; it < opts.iterations; ++it) {
        umbrella_step(pos, opts.lambda, tmp);
        umbrella_step(tmp, opts.mu, pos);
    }

    // Per-corner normal recompute when TriEx is attached: area-weighted vertex
    // normals (raw cross-product accumulation). All other TriEx fields kept.
    if (!r.mesh.triex.empty()) {
        std::vector<float3> vn(nv, make_float3(0.0f, 0.0f, 0.0f));
        for (size_t t = 0; t + 2 < r.mesh.indices.size(); t += 3) {
            const float3 p0 = pos[r.mesh.indices[t]];
            const float3 p1 = pos[r.mesh.indices[t + 1]];
            const float3 p2 = pos[r.mesh.indices[t + 2]];
            const float ex1 = p1.x - p0.x, ey1 = p1.y - p0.y, ez1 = p1.z - p0.z;
            const float ex2 = p2.x - p0.x, ey2 = p2.y - p0.y, ez2 = p2.z - p0.z;
            const float fx = ey1 * ez2 - ez1 * ey2;   // 2*area-weighted face normal
            const float fy = ez1 * ex2 - ex1 * ez2;
            const float fz = ex1 * ey2 - ey1 * ex2;
            for (int k = 0; k < 3; ++k) {
                float3& acc = vn[r.mesh.indices[t + k]];
                acc = make_float3(acc.x + fx, acc.y + fy, acc.z + fz);
            }
        }
        for (float3& nrm : vn) {
            const float len = std::sqrt(nrm.x*nrm.x + nrm.y*nrm.y + nrm.z*nrm.z);
            nrm = (len > 1e-20f) ? make_float3(nrm.x/len, nrm.y/len, nrm.z/len)
                                 : make_float3(0.0f, 0.0f, 1.0f);
        }
        for (size_t t = 0; t + 2 < r.mesh.indices.size(); t += 3) {
            TriEx& e = r.mesh.triex[t / 3];
            e.N0 = vn[r.mesh.indices[t]];
            e.N1 = vn[r.mesh.indices[t + 1]];
            e.N2 = vn[r.mesh.indices[t + 2]];
        }
    }

    r.ok = true;
    return r;
}
