#include "vertex_ao.h"

#include <cmath>
#include <cstdint>
#include <cstring>

static inline SlotCoord slot_of(const AoGrid& g, float3 p) {
    return SlotCoord{
        (int)lroundf((p.x - g.origin.x) / g.spacing),
        (int)lroundf((p.y - g.origin.y) / g.spacing),
        (int)lroundf((p.z - g.origin.z) / g.spacing)};
}

static float vertex_ao(float3 p, float3 n, const Occupancy& occ,
                       const AoGrid& g, const AoParams& params) {
    const float R = params.radius;
    if (R <= 0.0f || g.spacing <= 0.0f) return 1.0f;
    // Bound the dense box scan: a misconfigured (R >> spacing) would otherwise blow
    // up to (2*reach+1)^3 lookups per vertex. Occupancy is sparse, so beyond a few
    // slots the dense scan is the wrong tool anyway; clamp to keep the bake bounded.
    int reach = (int)std::ceil(R / g.spacing);
    if (reach > 64) reach = 64;
    const SlotCoord c = slot_of(g, p);
    float accum = 0.0f;
    for (int dz = -reach; dz <= reach; ++dz)
    for (int dy = -reach; dy <= reach; ++dy)
    for (int dx = -reach; dx <= reach; ++dx) {
        const SlotCoord s{c.x + dx, c.y + dy, c.z + dz};
        if (!occ.occupied(s)) continue;
        const float3 sp = make_float3(g.origin.x + s.x * g.spacing,
                                      g.origin.y + s.y * g.spacing,
                                      g.origin.z + s.z * g.spacing);
        const float3 d = make_float3(sp.x - p.x, sp.y - p.y, sp.z - p.z);
        const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (dist <= 1e-5f || dist > R) continue;
        const float inv = 1.0f / dist;
        const float align = (d.x * n.x + d.y * n.y + d.z * n.z) * inv; // dot(normalize(d), n)
        if (align <= 0.0f) continue;          // occluder is behind the surface
        const float falloff = 1.0f - dist / R; // linear falloff over the radius
        accum += align * falloff;
    }
    float ao = 1.0f - params.strength * accum;
    if (ao < 0.0f) ao = 0.0f;
    if (ao > 1.0f) ao = 1.0f;
    return ao;
}

void bake_vertex_ao(const std::vector<Tri>& tris, std::vector<TriEx>& triEx,
                    const Occupancy& occ, const AoGrid& grid, const AoParams& params) {
    const size_t n = tris.size() < triEx.size() ? tris.size() : triEx.size();
    for (size_t i = 0; i < n; ++i) {
        const Tri& t = tris[i];
        TriEx& ex = triEx[i];
        ex.ao0 = vertex_ao(t.vertex0, ex.N0, occ, grid, params);
        ex.ao1 = vertex_ao(t.vertex1, ex.N1, occ, grid, params);
        ex.ao2 = vertex_ao(t.vertex2, ex.N2, occ, grid, params);
    }
}

float pack_ao_w(float ao0, float ao1, float ao2) {
    // Floor each channel to 1 so a baked word is never all-zero: the shader uses
    // bits==0 as the "never written / AO disabled" sentinel, so a genuinely
    // fully-occluded vertex (AO 0) must not collide with it (1/255 is imperceptible).
    auto q = [](float v) -> uint32_t {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        uint32_t b = (uint32_t)(v * 255.0f + 0.5f);
        return b < 1u ? 1u : b;
    };
    const uint32_t bits = q(ao0) | (q(ao1) << 8) | (q(ao2) << 16);
    float f; std::memcpy(&f, &bits, sizeof(f));
    return f;
}
