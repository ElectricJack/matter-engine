#pragma once

// vt_chart_gpu.h — the chart/triangle GPU streams shared by every VT page pass.
//
// WP-D's tier-1 compositor (vt_compositor.cpp / vt_composite.comp) and WP-H's
// tier-2 hemisphere enrichment (vt_enrich.cpp / vt_enrich_ao.comp) both have to
// answer the same question per page texel: "which surface point does this texel
// own?". Both therefore consume the same two SSBOs — the chart table with its
// plane basis precomputed, and the rung's triangles reordered chart-grouped
// with per-vertex plane coordinates baked in.
//
// This header is the ONE place that packing lives, so the two passes can never
// drift apart. The GLSL mirrors are shaders_vk/vt_chart_types.glsl (structs)
// and shaders_vk/vt_chart_resolve.glsl (the resolve itself); keep all three in
// lockstep.
//
// Header-only on purpose: vt_compositor_tests.exe links a deliberately minimal
// TU list (the compositor is standalone by contract) and must not grow a new
// translation unit just to see these structs.

#include <cstdint>
#include <cstring>
#include <vector>

#include "chart_atlas.h"
#include "vt_types.h"

namespace vt {

// std430 mirrors — every field is 16-byte packed so the C++ layout is exact.
struct GpuChart {
    float origin_tpm[4];     // xyz part-local plane origin, w texels_per_meter
    float tangent_ou[4];     // xyz T (unit),  w = dot(origin, T)
    float bitangent_ov[4];   // xyz B (unit),  w = dot(origin, B)
    uint32_t rect[4];        // finest-mip atlas texels: x, y, w, h
    uint32_t tri_range[4];   // x = first tri (into tris[]), y = count
};
static_assert(sizeof(GpuChart) == 80, "GpuChart must match std430 layout");

// Triangles already reordered by tri_order (chart-grouped), with per-vertex
// plane coordinates precomputed: plane U in p*.w, plane V in n*.w.
struct GpuTri {
    float p0[4], p1[4], p2[4];   // xyz part-local position, w = plane U
    float n0[4], n1[4], n2[4];   // xyz part-local normal,   w = plane V
    uint32_t mat[4];             // x = TriEx materialId
    // WP-F: per-vertex surfaces()-tape weights, 8 u8 columns per vertex
    // (kMaxSurfaceMaterials), 2 u32 per vertex:
    //   wA = {v0 cols 0-3, v0 cols 4-7, v1 cols 0-3, v1 cols 4-7}
    //   wB = {v2 cols 0-3, v2 cols 4-7, 0, 0}
    // All zero when the part carries no tape (weight mode never reads them).
    uint32_t wA[4];
    uint32_t wB[4];
};
static_assert(sizeof(GpuTri) == 144, "GpuTri must match std430 layout");

// Per-vertex tape weight columns packed into the triangle stream — must equal
// terrain_field::kMaxSurfaceMaterials and the shader packing width.
constexpr uint32_t kVtMaxSurfaceMaterials = 8;

// True when `ctx` carries a usable surfaces()-tape classification (WP-F).
inline bool vt_context_has_tape(const VtPartContext& ctx) {
    return ctx.surface_material_count > 0 &&
           ctx.surface_material_count <= kVtMaxSurfaceMaterials &&
           ctx.surface_weights != nullptr;
}

// Builds the two streams for one (variant, rung). Returns false when the
// result would be unusable (no charts, or every triangle rejected), leaving
// `out_charts` / `out_tris` in an unspecified but valid state.
//
// DETERMINISM: fixed iteration order (charts ascending, then the chart's
// tri_order range ascending). Same atlas + mesh => byte-identical streams.
inline bool vt_build_chart_gpu_streams(const chart_atlas::ChartAtlasRung& atlas,
                                       const VtPartContext& ctx,
                                       std::vector<GpuChart>& out_charts,
                                       std::vector<GpuTri>& out_tris) {
    out_charts.clear();
    out_tris.clear();
    if (atlas.charts.empty()) return false;
    if (!ctx.positions || !ctx.indices || ctx.triangle_count == 0) return false;

    const bool has_tape = vt_context_has_tape(ctx);
    const uint32_t tape_cols = has_tape ? ctx.surface_material_count : 0;

    out_charts.resize(atlas.charts.size());
    out_tris.reserve(atlas.tri_order.size());
    for (size_t ci = 0; ci < atlas.charts.size(); ++ci) {
        const chart_atlas::ChartEntry& c = atlas.charts[ci];
        GpuChart& g = out_charts[ci];
        const float ou = c.origin[0] * c.tangent[0] +
                         c.origin[1] * c.tangent[1] +
                         c.origin[2] * c.tangent[2];
        const float ov = c.origin[0] * c.bitangent[0] +
                         c.origin[1] * c.bitangent[1] +
                         c.origin[2] * c.bitangent[2];
        g.origin_tpm[0] = c.origin[0];
        g.origin_tpm[1] = c.origin[1];
        g.origin_tpm[2] = c.origin[2];
        g.origin_tpm[3] = c.texels_per_meter;
        g.tangent_ou[0] = c.tangent[0];
        g.tangent_ou[1] = c.tangent[1];
        g.tangent_ou[2] = c.tangent[2];
        g.tangent_ou[3] = ou;
        g.bitangent_ov[0] = c.bitangent[0];
        g.bitangent_ov[1] = c.bitangent[1];
        g.bitangent_ov[2] = c.bitangent[2];
        g.bitangent_ov[3] = ov;
        g.rect[0] = c.rect_x;
        g.rect[1] = c.rect_y;
        g.rect[2] = c.rect_w;
        g.rect[3] = c.rect_h;
        g.tri_range[0] = static_cast<uint32_t>(out_tris.size());
        uint32_t emitted = 0;
        for (uint32_t i = 0; i < c.tri_count; ++i) {
            const uint32_t oi = c.first_tri + i;
            if (oi >= atlas.tri_order.size()) break;
            const uint32_t ti = atlas.tri_order[oi];
            if (ti >= ctx.triangle_count) continue;
            GpuTri g_tri{};
            float* pdst[3] = {g_tri.p0, g_tri.p1, g_tri.p2};
            float* ndst[3] = {g_tri.n0, g_tri.n1, g_tri.n2};
            float pos[3][3];
            bool corners_ok = true;
            uint32_t corner0 = 0;
            uint32_t corners[3] = {0, 0, 0};
            for (int v = 0; v < 3; ++v) {
                const uint32_t corner = ctx.indices[3 * ti + v];
                if (corner >= ctx.vertex_count) { corners_ok = false; break; }
                corners[v] = corner;
                if (v == 0) corner0 = corner;
                for (int k = 0; k < 3; ++k)
                    pos[v][k] = ctx.positions[3 * corner + k];
                pdst[v][0] = pos[v][0];
                pdst[v][1] = pos[v][1];
                pdst[v][2] = pos[v][2];
                pdst[v][3] = pos[v][0] * c.tangent[0] +
                             pos[v][1] * c.tangent[1] +
                             pos[v][2] * c.tangent[2];   // plane U
                if (ctx.normals) {
                    ndst[v][0] = ctx.normals[3 * corner + 0];
                    ndst[v][1] = ctx.normals[3 * corner + 1];
                    ndst[v][2] = ctx.normals[3 * corner + 2];
                }
                ndst[v][3] = pos[v][0] * c.bitangent[0] +
                             pos[v][1] * c.bitangent[1] +
                             pos[v][2] * c.bitangent[2];  // plane V
            }
            if (!corners_ok) continue;
            if (!ctx.normals) {
                // Geometric face normal fallback (no vertex normals).
                const float e1[3] = {pos[1][0] - pos[0][0],
                                     pos[1][1] - pos[0][1],
                                     pos[1][2] - pos[0][2]};
                const float e2[3] = {pos[2][0] - pos[0][0],
                                     pos[2][1] - pos[0][1],
                                     pos[2][2] - pos[0][2]};
                const float fn[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                                     e1[2] * e2[0] - e1[0] * e2[2],
                                     e1[0] * e2[1] - e1[1] * e2[0]};
                for (int v = 0; v < 3; ++v) {
                    ndst[v][0] = fn[0];
                    ndst[v][1] = fn[1];
                    ndst[v][2] = fn[2];
                }
            }
            uint32_t mat = ctx.material_ids ? ctx.material_ids[corner0]
                                            : ctx.dominant_material;
            if (mat == 0xFFFFFFFFu) {
                mat = (ctx.dominant_material != 0xFFFFFFFFu)
                          ? ctx.dominant_material
                          : 0u;
            }
            g_tri.mat[0] = mat & 0xFFu;
            if (has_tape) {
                // Pack each corner's u8 weight columns: 2 u32 per vertex,
                // little-endian within the u32 (column k at bit 8*(k&3)).
                const auto pack_pair = [&](uint32_t corner, uint32_t out[2]) {
                    out[0] = 0;
                    out[1] = 0;
                    const uint8_t* w =
                        ctx.surface_weights + size_t(corner) * tape_cols;
                    for (uint32_t k = 0; k < tape_cols; ++k)
                        out[k >> 2] |= uint32_t(w[k]) << ((k & 3u) * 8u);
                };
                uint32_t pair[2];
                pack_pair(corners[0], pair);
                g_tri.wA[0] = pair[0];
                g_tri.wA[1] = pair[1];
                pack_pair(corners[1], pair);
                g_tri.wA[2] = pair[0];
                g_tri.wA[3] = pair[1];
                pack_pair(corners[2], pair);
                g_tri.wB[0] = pair[0];
                g_tri.wB[1] = pair[1];
            }
            out_tris.push_back(g_tri);
            ++emitted;
        }
        g.tri_range[1] = emitted;
        g.tri_range[2] = 0;
        g.tri_range[3] = 0;
    }
    return !out_tris.empty();
}

// Candidate charts for one page: every chart rect whose finest-mip footprint
// (expanded by a dilation margin) touches the page, in ascending chart index.
// When nothing touches, the single nearest chart by rect distance is emitted so
// gutter/border dilation always has content. Returns the number appended to
// `out` (0 only when the atlas has no charts).
//
// Shared by the compositor and the enricher so both passes resolve a texel
// against exactly the same candidate set.
inline uint32_t vt_page_candidate_charts(
    const chart_atlas::ChartAtlasRung& atlas, uint32_t page_x, uint32_t page_y,
    uint32_t mip, std::vector<uint32_t>& out) {
    const int64_t mip_scale = int64_t(1) << mip;
    const int64_t page_lo_x = (int64_t(page_x) * chart_atlas::kVtPagePayload -
                               chart_atlas::kVtPageBorder) * mip_scale;
    const int64_t page_lo_y = (int64_t(page_y) * chart_atlas::kVtPagePayload -
                               chart_atlas::kVtPageBorder) * mip_scale;
    const int64_t store = chart_atlas::kVtPagePayload +
                          2 * chart_atlas::kVtPageBorder;
    const int64_t page_hi_x = page_lo_x + store * mip_scale;
    const int64_t page_hi_y = page_lo_y + store * mip_scale;
    const int64_t margin = int64_t(32) * mip_scale;

    uint32_t appended = 0;
    int64_t nearest_d2 = INT64_MAX;
    uint32_t nearest_chart = 0;
    for (size_t ci = 0; ci < atlas.charts.size(); ++ci) {
        const chart_atlas::ChartEntry& c = atlas.charts[ci];
        const int64_t clo_x = c.rect_x, clo_y = c.rect_y;
        const int64_t chi_x = clo_x + c.rect_w, chi_y = clo_y + c.rect_h;
        int64_t dx = page_lo_x - margin - chi_x;
        const int64_t dx2 = clo_x - (page_hi_x + margin);
        if (dx2 > dx) dx = dx2;
        if (dx < 0) dx = 0;
        int64_t dy = page_lo_y - margin - chi_y;
        const int64_t dy2 = clo_y - (page_hi_y + margin);
        if (dy2 > dy) dy = dy2;
        if (dy < 0) dy = 0;
        if (dx == 0 && dy == 0) {
            out.push_back(static_cast<uint32_t>(ci));
            ++appended;
        } else {
            const int64_t d2 = dx * dx + dy * dy;
            if (d2 < nearest_d2) {
                nearest_d2 = d2;
                nearest_chart = static_cast<uint32_t>(ci);
            }
        }
    }
    if (appended == 0 && !atlas.charts.empty()) {
        out.push_back(nearest_chart);
        appended = 1;
    }
    return appended;
}

}  // namespace vt
