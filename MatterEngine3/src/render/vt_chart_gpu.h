#pragma once

// vt_chart_gpu.h — the chart/triangle GPU streams shared by every VT page pass.
//
// WP-D's tier-1 compositor (vt_compositor.cpp / vt_composite.comp) and WP-H's
// tier-2 hemisphere enrichment (vt_enrich.cpp / vt_enrich_ao.comp) both have to
// answer the same question per page texel: "which surface point does this texel
// own?". Both consume the same prepared chart/triangle geometry — the chart
// table with its plane basis precomputed, and chart-grouped triangles with
// per-vertex plane coordinates. The compositor stores mutable surface rows
// separately; the enricher retains the combined triangle stream format.
//
// This header is the ONE place that packing lives, so the two passes can never
// drift apart. The GLSL mirrors are shaders_vk/vt_chart_types.glsl (structs)
// and shaders_vk/vt_chart_resolve.glsl (the resolve itself); keep all three in
// lockstep.
//
// Header-only on purpose: vt_compositor_tests.exe links a deliberately minimal
// TU list (the compositor is standalone by contract) and must not grow a new
// translation unit just to see these structs.
//
// CONVENTIONS AND THREADING. Nothing here holds state: both entry points are
// pure functions of their arguments (they only write the caller's output
// containers), so they are safe to call from any thread — the compositor
// calls them while recording, the enricher from its own pass.
//
// Spaces and units:
//   * positions, normals and the chart plane origin/tangent/bitangent are
//     PART-LOCAL; positions are metres, the basis vectors are unit length.
//   * chart rects and the page footprint are ATLAS TEXELS at the FINEST mip
//     (mip 0); a coarser page's footprint is scaled by `1 << mip`.
//   * plane U/V (the `.w` lanes of GpuTri's p*/n* rows) are METRES along the
//     chart tangent/bitangent, not texels — the shader multiplies by
//     texels_per_meter (GpuChart::origin_tpm.w) to reach texel space, and
//     subtracts the chart origin's projection carried in tangent_ou.w /
//     bitangent_ov.w.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "../../../libs/MeshChartingLib/include/mesh_charting.h"
#include "chart_atlas.h"
#include "terrain_field.h"   // kMaxSurfaceMaterials (the source of truth)
#include "vt_types.h"

namespace vt {

// std430 mirrors — every field is 16-byte packed so the C++ layout is exact.
struct GpuChart {
    float origin_tpm[4];     // xyz part-local plane origin, w texels_per_meter
    float tangent_ou[4];     // xyz T (unit),  w = dot(origin, T)
    float bitangent_ov[4];   // xyz B (unit),  w = dot(origin, B)
    uint32_t rect[4];        // finest-mip atlas texels: x, y, w, h
    uint32_t tri_range[4];   // x = first tri (into tris[]), y = count
    // tri_range.y is the count actually EMITTED into tris[], which can be
    // lower than the source ChartEntry::tri_count — the builder below drops
    // triangles whose index or corner index is out of range for the mesh.
    // tri_range.z/.w hold the optional seed hierarchy's first node/count.
    // The stream builder initializes them to zero; CPU preparation fills them.
};
static_assert(sizeof(GpuChart) == 80, "GpuChart must match std430 layout");

// Triangles already reordered by tri_order (chart-grouped), with per-vertex
// plane coordinates precomputed: plane U in p*.w, plane V in n*.w.
struct GpuTri {
    float p0[4], p1[4], p2[4];   // xyz part-local position, w = plane U
    float n0[4], n1[4], n2[4];   // xyz part-local normal,   w = plane V
    uint32_t mat[4];             // x = TriEx materialId
    // Optional surface traversal: y/z/w = emitted neighbor triangle + 1
    // across (0,1)/(1,2)/(2,0); 0 = closed or connectivity not prepared.
    // vt_build_chart_gpu_streams leaves these zero. The explicit neighbor
    // builder below uses the retained corners; page composition ignores them.
    // Per-vertex tape payload. TWO packings share these rows; which one a
    // part carries is decided at stream-build time and travels with the
    // request's weight mode (the struct is internal to the page passes and
    // versions with them):
    //   MODE 2 (per-vertex u8 weight columns, kMaxSurfaceMaterials wide):
    //     wA = {v0 cols 0-3, v0 cols 4-7, v1 cols 0-3, v1 cols 4-7}
    //     wB = {v2 cols 0-3, v2 cols 4-7, 0, 0};  wC = 0
    //   MODE 3 (P2 texel-rate tape: per-vertex f16 FIELD LANES, up to
    //   kVtMaxSurfaceLanes = 8, two halves per u32, lane l in word l>>1 at
    //   bit (l&1)*16):
    //     wA = v0 lanes, wB = v1 lanes, wC = v2 lanes.
    // All zero when the part carries no tape (those modes never read them).
    //
    // Spec §4.3 deviation, documented: the spec claimed the 8-f16 lane block
    // fits the existing wA/wB region "16 B used of the 32" — that accounting
    // covers ONE vertex slot; three vertices need 48 B, so the struct grew
    // one 16 B row (144 -> 160) instead of silently capping lanes at 5.
    uint32_t wA[4];
    uint32_t wB[4];
    uint32_t wC[4];
};
static_assert(sizeof(GpuTri) == 160, "GpuTri must match std430 layout");

// The compositor retains the immutable prefix and replaces only the surface
// rows. The combined GpuTri remains the shared CPU packing/reference format
// and the enricher's stream format. No page encoding changes with this split.
struct GpuTriGeometry {
    float p0[4], p1[4], p2[4];
    float n0[4], n1[4], n2[4];
    uint32_t mat[4];
};
struct GpuTriSurface {
    uint32_t wA[4], wB[4], wC[4];
};
using VtTriangleCorners = std::array<uint32_t, 3>;
static_assert(sizeof(GpuTriGeometry) == 112 &&
              sizeof(GpuTriGeometry) == offsetof(GpuTri, wA),
              "geometry must be the exact combined triangle prefix");
static_assert(sizeof(GpuTriSurface) == 48 &&
              sizeof(GpuTriGeometry) + sizeof(GpuTriSurface) == sizeof(GpuTri),
              "surface rows must preserve the combined triangle layout");

// Per-vertex tape weight columns packed into the triangle stream. DERIVED
// from terrain_field::kMaxSurfaceMaterials rather than restated, because the
// two silently disagreeing is a mis-decode of every tape weight; the shader
// packing width must be changed with it.
constexpr uint32_t kVtMaxSurfaceMaterials =
    static_cast<uint32_t>(terrain_field::kMaxSurfaceMaterials);

// MODE-3 f16 field lanes one vertex can carry, taken from GpuTri's own rows
// rather than written out again: one 16 B row per vertex, two halves per u32.
// vt_surface_tape.h's kVtMaxSurfaceLanes must agree -- vt_compositor.cpp,
// which sees both headers, static_asserts that it does.
// (Split in two so the division is not a `sizeof(array) / sizeof(type)`
// idiom, which -Wsizeof-array-div flags because the element type differs.)
constexpr uint32_t kGpuTriLaneBytesPerVertex =
    static_cast<uint32_t>(sizeof(GpuTri::wA));
constexpr uint32_t kGpuTriLanesPerVertex =
    kGpuTriLaneBytesPerVertex / static_cast<uint32_t>(sizeof(uint16_t));

// True when `ctx` carries a usable surfaces()-tape classification (WP-F).
inline bool vt_context_has_tape(const VtPartContext& ctx) {
    return ctx.surface_material_count > 0 &&
           ctx.surface_material_count <= kVtMaxSurfaceMaterials &&
           ctx.surface_weights != nullptr;
}

// Pack only the mutable rows, using the original vertex indices retained by
// geometry preparation. Callers establish valid corners and packing bounds.
inline GpuTriSurface vt_pack_triangle_surface(
    const VtPartContext& ctx, const uint32_t* corners, uint32_t tape_cols,
    const uint16_t* lanes, uint32_t lane_count) {
    GpuTriSurface result{};
    if (lanes && lane_count) {
        uint32_t* rows[3] = {result.wA, result.wB, result.wC};
        for (uint32_t v = 0; v < 3; ++v) {
            const uint16_t* source = lanes + size_t(corners[v]) * lane_count;
            for (uint32_t l = 0; l < lane_count; ++l)
                rows[v][l >> 1] |= uint32_t(source[l]) << ((l & 1u) * 16u);
        }
    } else if (tape_cols) {
        uint32_t* rows[3] = {result.wA, result.wA + 2, result.wB};
        for (uint32_t v = 0; v < 3; ++v) {
            const uint8_t* source = ctx.surface_weights + size_t(corners[v]) * tape_cols;
            for (uint32_t k = 0; k < tape_cols; ++k)
                rows[v][k >> 2] |= uint32_t(source[k]) << ((k & 3u) * 8u);
        }
    }
    return result;
}

// Builds the two streams for one (variant, rung). Returns false when the
// result would be unusable (no charts, or every triangle rejected), leaving
// `out_charts` / `out_tris` in an unspecified but valid state.
//
// P2: when `lanes` is non-null the per-vertex tape payload rows (wA/wB/wC)
// carry `lane_count` f16 FIELD LANES per vertex (mode-3 packing; see the
// GpuTri comment) instead of the mode-2 u8 weight columns. The caller (the
// compositor) passes the part's precomputed lane stream only for parts it
// promoted to mode 3; every other caller (the enricher included) leaves the
// defaults and gets the historical packing byte-for-byte.
// `out_corners`, when supplied, retains the original three vertex indices for
// every emitted triangle, in the same order. Rejected triangles append nothing.
//
// DETERMINISM: fixed iteration order (charts ascending, then the chart's
// tri_order range ascending). Same atlas + mesh => byte-identical streams.
//
// Both output vectors are CLEARED on entry, so a caller may reuse them across
// calls. Cost is O(atlas.tri_order.size()) with one GpuTri (160 B) appended
// per accepted triangle — this is the repack the compositor profiles as
// `vt.chart_streams`. Details worth knowing:
//   * a triangle whose index, or any corner index, is out of range for `ctx`
//     is skipped SILENTLY (see the GpuChart::tri_range note above);
//   * with `ctx.normals == nullptr` the fallback is the geometric face
//     normal, and it is NOT normalized — the shaders normalize;
//   * the material id (per-triangle, taken from corner 0, falling back to
//     ctx.dominant_material and then 0) is truncated to its low 8 bits.
inline bool vt_build_chart_gpu_streams(const chart_atlas::ChartAtlasRung& atlas,
                                       const VtPartContext& ctx,
                                       std::vector<GpuChart>& out_charts,
                                       std::vector<GpuTri>& out_tris,
                                       const uint16_t* lanes = nullptr,
                                       uint32_t lane_count = 0,
                                       std::vector<VtTriangleCorners>* out_corners = nullptr) {
    out_charts.clear();
    out_tris.clear();
    if (out_corners) out_corners->clear();
    if (atlas.charts.empty()) return false;
    if (!ctx.positions || !ctx.indices || ctx.triangle_count == 0) return false;

    const bool has_tape = vt_context_has_tape(ctx);
    const uint32_t tape_cols = has_tape ? ctx.surface_material_count : 0;
    const bool pack_lanes = lanes != nullptr && lane_count > 0 &&
                            lane_count <= kGpuTriLanesPerVertex;

    out_charts.resize(atlas.charts.size());
    out_tris.reserve(atlas.tri_order.size());
    if (out_corners) out_corners->reserve(atlas.tri_order.size());
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
            const auto surface = vt_pack_triangle_surface(
                ctx, corners, tape_cols, pack_lanes ? lanes : nullptr,
                pack_lanes ? lane_count : 0u);
            std::memcpy(g_tri.wA, surface.wA, sizeof(surface.wA));
            std::memcpy(g_tri.wB, surface.wB, sizeof(surface.wB));
            std::memcpy(g_tri.wC, surface.wC, sizeof(surface.wC));
            out_tris.push_back(g_tri);
            if (out_corners) out_corners->push_back({corners[0], corners[1], corners[2]});
            ++emitted;
        }
        g.tri_range[1] = emitted;
        g.tri_range[2] = 0;
        g.tri_range[3] = 0;
    }
    return !out_tris.empty();
}

// Populate the immutable geometry prefix's reserved words for connected POM
// traversal. Call once for new geometry, after chart grouping/rejection, never
// for a surface-only edit. Page compositing itself ignores the neighbor words.
// No persistent structure grows (112-byte geometry / 160-byte combined rows).
// Neighbor indices address EMITTED triangles, including chart reordering and
// UV vertex splits. Actual boundaries and ambiguous edges encode zero.
inline bool vt_build_chart_surface_neighbors(const VtPartContext& ctx,
    const std::vector<VtTriangleCorners>& corners, std::vector<GpuTri>& triangles) {
    for (auto& tri : triangles) tri.mat[1] = tri.mat[2] = tri.mat[3] = 0;
    if (!ctx.positions || corners.size() != triangles.size() ||
        corners.size() > size_t(std::numeric_limits<int>::max())) return false;
    std::vector<uint32_t> indices;
    indices.reserve(corners.size() * 3u);
    for (const auto& tri : corners) for (uint32_t vertex : tri) {
        if (vertex >= ctx.vertex_count) return false;
        indices.push_back(vertex);
    }
    const auto adjacency = mesh_charting::build_surface_adjacency(
        ctx.positions, indices.data(), int(corners.size()));
    for (size_t i=0; i<triangles.size(); ++i)
        for (int edge=0; edge<3; ++edge)
            triangles[i].mat[edge+1] = uint32_t(adjacency[i].nbr[edge] + 1);
    return true;
}

// Candidate charts for one page: every chart rect whose finest-mip footprint
// (expanded by a dilation margin) touches the page, in ascending chart index.
// When nothing touches, the single nearest chart by rect distance is emitted so
// gutter/border dilation always has content. Returns the number appended to
// `out` (0 only when the atlas has no charts).
//
// Shared by the compositor and the enricher so both passes resolve a texel
// against exactly the same candidate set.
//
// `out` is APPENDED to, never cleared — callers pack many pages' candidate
// lists back to back into one GPU buffer and record the (offset, count) pair
// per page. `page_x`/`page_y` are page coordinates AT `mip`; the page's
// payload+border footprint and the 32-texel dilation margin are both
// converted to finest-mip texels by `1 << mip`. Cost is a linear scan of the
// whole chart table per page, so a heavily-charted variant pays it again on
// every page it fills.
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
