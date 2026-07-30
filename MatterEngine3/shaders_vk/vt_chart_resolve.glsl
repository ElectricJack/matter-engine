#ifndef VT_CHART_RESOLVE_GLSL
#define VT_CHART_RESOLVE_GLSL

// Chart-space virtual texturing — "which surface point owns this page texel?".
//
// The tier-1 compositor (vt_composite.comp) and the tier-2 hemisphere
// enrichment (vt_enrich_ao.comp) MUST agree on this exactly: the enrichment
// multiplies its result into a page the compositor produced, so a texel that
// resolves to a different triangle in the two passes bakes occlusion belonging
// to somewhere else. Sharing the code is the mechanism that makes that
// impossible, not a convenience.
//
// Include contract — before including, declare (any set/binding):
//   readonly buffer ChartsBuf { GpuChart charts[]; };
//   readonly buffer TrisBuf   { GpuTri   tris[];   };
//   readonly buffer CandBuf   { uint cand_charts[]; };
// and include vt_chart_types.glsl first.
//
// DETERMINISM: fixed iteration orders (candidates ascending, then the chart's
// triangle range ascending), ties broken by strict '<' so the first candidate
// wins. Identical inputs => identical result.

// ---------------------------------------------------------------------------
// Closest point on a 2D triangle. Returns squared distance (0 when q is
// inside) and the barycentrics of the closest point. Fixed evaluation order
// (inside test, then edges AB, BC, CA with strict '<') for determinism.
// ---------------------------------------------------------------------------
float closest_tri_2d(vec2 q, vec2 a, vec2 b, vec2 c, out vec3 bary) {
    vec2 ab = b - a, ac = c - a, aq = q - a;
    float d00 = dot(ab, ab), d01 = dot(ab, ac), d11 = dot(ac, ac);
    float d20 = dot(aq, ab), d21 = dot(aq, ac);
    float denom = d00 * d11 - d01 * d01;
    if (abs(denom) > 1e-12) {
        float v = (d11 * d20 - d01 * d21) / denom;
        float w = (d00 * d21 - d01 * d20) / denom;
        if (v >= 0.0 && w >= 0.0 && v + w <= 1.0) {
            bary = vec3(1.0 - v - w, v, w);
            return 0.0;
        }
    }
    // Outside (or degenerate): closest point over the three edge segments.
    float best_d2 = 1e30;
    bary = vec3(1.0, 0.0, 0.0);
    // edge AB
    {
        float t = clamp(dot(q - a, ab) / max(d00, 1e-12), 0.0, 1.0);
        vec2 p = a + ab * t;
        float d2 = dot(q - p, q - p);
        if (d2 < best_d2) { best_d2 = d2; bary = vec3(1.0 - t, t, 0.0); }
    }
    // edge BC
    {
        vec2 bc = c - b;
        float t = clamp(dot(q - b, bc) / max(dot(bc, bc), 1e-12), 0.0, 1.0);
        vec2 p = b + bc * t;
        float d2 = dot(q - p, q - p);
        if (d2 < best_d2) { best_d2 = d2; bary = vec3(0.0, 1.0 - t, t); }
    }
    // edge CA
    {
        float t = clamp(dot(q - c, -ac) / max(d11, 1e-12), 0.0, 1.0);
        vec2 p = c - ac * t;
        float d2 = dot(q - p, q - p);
        if (d2 < best_d2) { best_d2 = d2; bary = vec3(t, 0.0, 1.0 - t); }
    }
    return best_d2;
}

// The resolved surface point behind one physical page texel.
struct VtSurfacePoint {
    bool  found;
    uint  chart;         // index into charts[]
    uint  tri;           // index into tris[]
    vec3  bary;
    vec3  pos;           // part-local metres
    vec3  nrm;           // part-local unit normal
    vec2  plane_uv;      // the owning chart's plane coordinates
    float footprint_m;   // page-texel edge length in metres at this mip
};

// `store_texel` is the physical texel inside the 136^2 page store (border
// included); `page` the page coords at `mip`; `cand_offset`/`cand_count` the
// slice of cand_charts[] the CPU produced for this page (see
// vt_page_candidate_charts in vt_chart_gpu.h).
//
// Texels outside every triangle (gutters, borders, coarse-mip inter-chart
// space) take the globally nearest triangle point — that IS the dilation fill.
VtSurfacePoint vt_resolve_page_texel(uvec2 store_texel, uvec2 page, uint mip,
                                     uint cand_offset, uint cand_count) {
    VtSurfacePoint sp;
    sp.found = false;
    sp.chart = 0u;
    sp.tri = 0u;
    sp.bary = vec3(1.0, 0.0, 0.0);
    sp.pos = vec3(0.0);
    sp.nrm = vec3(0.0, 1.0, 0.0);
    sp.plane_uv = vec2(0.0);
    sp.footprint_m = 0.0;

    // Virtual texel at this mip (border texels go negative / past the atlas
    // edge — the nearest-triangle search handles them as dilation).
    ivec2 vt_texel = ivec2(page) * VT_PAGE_PAYLOAD - VT_PAGE_BORDER
                   + ivec2(store_texel);
    float scale = float(1u << mip);
    vec2 fin = (vec2(vt_texel) + 0.5) * scale;  // finest-mip texel center

    float best_d2 = 1e30;
    uint  best_chart = 0xFFFFFFFFu;
    vec3  best_bary = vec3(1.0, 0.0, 0.0);
    uint  best_tri = 0u;
    bool  inside = false;
    for (uint ci = 0u; ci < cand_count && !inside; ++ci) {
        uint chart_idx = cand_charts[cand_offset + ci];
        GpuChart c = charts[chart_idx];
        float tpm = c.origin_tpm.w;
        // Invert the chart_atlas.h texel<->position convention:
        //   texel = rect + gutter + (dot(p, T/B) - dot(origin, T/B)) * tpm
        vec2 q = (fin - vec2(c.rect.xy) - float(VT_CHART_GUTTER)) / tpm
               + vec2(c.tangent_ou.w, c.bitangent_ov.w);
        uint first = c.tri_range.x, count = c.tri_range.y;
        for (uint ti = 0u; ti < count; ++ti) {
            GpuTri tr = tris[first + ti];
            vec2 a = vec2(tr.p0.w, tr.n0.w);
            vec2 b = vec2(tr.p1.w, tr.n1.w);
            vec2 cc = vec2(tr.p2.w, tr.n2.w);
            // Cheap reject: plane-space bbox distance vs current best.
            vec2 lo = min(a, min(b, cc)), hi = max(a, max(b, cc));
            vec2 dv = max(max(lo - q, q - hi), vec2(0.0));
            if (dot(dv, dv) >= best_d2) continue;
            vec3 bary;
            float d2 = closest_tri_2d(q, a, b, cc, bary);
            if (d2 < best_d2) {
                best_d2 = d2;
                best_chart = chart_idx;
                best_tri = first + ti;
                best_bary = bary;
                if (d2 <= 0.0) { inside = true; break; }
            }
        }
    }
    if (best_chart == 0xFFFFFFFFu) return sp;

    GpuChart chart = charts[best_chart];
    GpuTri tri = tris[best_tri];
    vec3 pos = best_bary.x * tri.p0.xyz + best_bary.y * tri.p1.xyz
             + best_bary.z * tri.p2.xyz;
    vec3 nrm = best_bary.x * tri.n0.xyz + best_bary.y * tri.n1.xyz
             + best_bary.z * tri.n2.xyz;
    float nlen = length(nrm);
    nrm = (nlen > 1e-6) ? nrm / nlen : vec3(0.0, 1.0, 0.0);

    sp.found = true;
    sp.chart = best_chart;
    sp.tri = best_tri;
    sp.bary = best_bary;
    sp.pos = pos;
    sp.nrm = nrm;
    sp.plane_uv = vec2(best_bary.x * tri.p0.w + best_bary.y * tri.p1.w
                           + best_bary.z * tri.p2.w,
                       best_bary.x * tri.n0.w + best_bary.y * tri.n1.w
                           + best_bary.z * tri.n2.w);
    sp.footprint_m = scale / chart.origin_tpm.w;
    return sp;
}

#endif  // VT_CHART_RESOLVE_GLSL
