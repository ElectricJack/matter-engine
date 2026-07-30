#ifndef VT_CHART_TYPES_GLSL
#define VT_CHART_TYPES_GLSL

// Chart-space virtual texturing — page geometry + the chart/triangle stream
// structs shared by every page pass.
//
// GLSL mirror of MatterEngine3/src/render/vt_chart_gpu.h. Consumers declare
// their own `charts` / `tris` SSBOs (their set/binding differ) using these
// struct types, then include vt_chart_resolve.glsl for the resolve itself.
// Keep this file, vt_chart_gpu.h and chart_atlas.h in lockstep.

// Page geometry — MUST match chart_atlas.h (kVtPagePayload/kVtPageBorder/
// kChartGutterTexels).
#define VT_PAGE_PAYLOAD 128
#define VT_PAGE_BORDER  4
#define VT_PAGE_STORE   136
#define VT_CHART_GUTTER 4

struct GpuChart {
    vec4 origin_tpm;     // xyz part-local plane origin, w texels_per_meter
    vec4 tangent_ou;     // xyz T (unit), w = dot(origin, T)
    vec4 bitangent_ov;   // xyz B (unit), w = dot(origin, B)
    uvec4 rect;          // finest-mip atlas texels: x, y, w, h
    uvec4 tri_range;     // x = first tri (into tris[], chart-grouped), y = count
};

// Triangles already reordered by tri_order (chart-grouped) CPU-side, with
// per-vertex plane coordinates precomputed: plane U in p*.w, plane V in n*.w.
struct GpuTri {
    vec4 p0, p1, p2;     // xyz part-local position, w = plane U
    vec4 n0, n1, n2;     // xyz part-local normal,   w = plane V
    uvec4 mat;           // x = TriEx materialId
    // WP-F surfaces()-tape weights: 8 u8 columns per vertex, 2 u32 each.
    // wA = {v0 cols 0-3, v0 cols 4-7, v1 cols 0-3, v1 cols 4-7};
    // wB = {v2 cols 0-3, v2 cols 4-7, 0, 0}. Zero when the part has no tape.
    uvec4 wA;
    uvec4 wB;
};

#endif  // VT_CHART_TYPES_GLSL
