#pragma once

// Opt-in CPU diagnosis of resident page packing. Counts the UNION of texel
// centers covered by projected triangles, not summed triangle area, atlas
// bounding-box occupancy, or the compositor's dilated (always-filled) output.
// Uses the chart_atlas.h projection and vt_chart_resolve.glsl center convention.
// This is a geometric diagnostic, not a bit-exact GPU rasterization oracle.
// Filtering can need texels outside this coverage; low coverage is not itself
// permission to remove gutters or physical borders. Never run in timing captures.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "vt_types.h"

namespace vt {

struct VtPageDensity {
    bool geometry_available = false;
    uint32_t atlas_texels = 0;
    uint32_t chart_block_texels = 0;
    uint32_t content_bounds_texels = 0;
    uint32_t gutter_bounds_texels = 0;
    uint32_t triangle_texels = 0;
};

inline VtPageDensity vt_measure_page_density(
    const chart_atlas::ChartAtlasRung& atlas, const VtPartContext& ctx,
    uint32_t mip, uint32_t page_x, uint32_t page_y) {
    VtPageDensity out;
    if (mip >= 32 || !atlas.atlas_w || !atlas.atlas_h) return out;
    constexpr int edge = int(chart_atlas::kVtPagePayload);
    constexpr uint8_t in_atlas = 1, in_block = 2, in_bounds = 4,
                      in_gutter = 8, in_triangle = 16;
    std::array<uint8_t, edge * edge> mask{}; // fixed 16 KiB, reused per page
    const double scale = double(uint64_t(1) << mip);
    const double page_left = double(page_x) * edge;
    const double page_top = double(page_y) * edge;
    const uint32_t width = std::max(1u, atlas.atlas_w >> mip);
    const uint32_t height = std::max(1u, atlas.atlas_h >> mip);
    for (int y = 0; y < edge; ++y)
        for (int x = 0; x < edge; ++x)
            if (page_left + x < width && page_top + y < height)
                mask[size_t(y) * edge + x] = in_atlas;

    // Coordinates passed here are in this page's payload texels. Include
    // centers on a geometric boundary; the union prevents double counting.
    auto lower = [](double v) {
        return int(std::clamp(std::ceil(v - 0.5), 0.0, double(edge)));
    };
    auto upper = [](double v) {
        return int(std::clamp(std::floor(v - 0.5) + 1.0, 0.0, double(edge)));
    };
    auto mark_rect = [&](double x0, double y0, double x1, double y1, uint8_t bit) {
        if (!std::isfinite(x0) || !std::isfinite(y0) ||
            !std::isfinite(x1) || !std::isfinite(y1)) return;
        for (int y = lower(y0), ye = upper(y1); y < ye; ++y)
            for (int x = lower(x0), xe = upper(x1); x < xe; ++x) {
                auto& value = mask[size_t(y) * edge + x];
                if (value & in_atlas) value |= bit;
            }
    };
    out.geometry_available = ctx.positions && ctx.indices && ctx.triangle_count &&
                             !atlas.charts.empty();
    for (const auto& chart : atlas.charts) {
        const double bx = double(chart.rect_x) / scale - page_left;
        const double by = double(chart.rect_y) / scale - page_top;
        mark_rect(bx, by, bx + chart.rect_w / scale,
                  by + chart.rect_h / scale, in_block);
        if (!out.geometry_available || !(chart.texels_per_meter > 0) ||
            !std::isfinite(chart.texels_per_meter)) continue;
        // Charts outside this page cannot contribute packed surface centers.
        if (bx > edge || by > edge || bx + chart.rect_w / scale < 0 ||
            by + chart.rect_h / scale < 0) continue;
        double ou = 0, ov = 0;
        for (int k = 0; k < 3; ++k) {
            ou += double(chart.origin[k]) * chart.tangent[k];
            ov += double(chart.origin[k]) * chart.bitangent[k];
        }
        double min_x = std::numeric_limits<double>::infinity(), min_y = min_x;
        double max_x = -min_x, max_y = -min_x;
        for (uint32_t i = 0; i < chart.tri_count; ++i) {
            const uint64_t oi = uint64_t(chart.first_tri) + i;
            if (oi >= atlas.tri_order.size()) break;
            const uint32_t ti = atlas.tri_order[size_t(oi)];
            if (ti >= ctx.triangle_count) continue;
            double x[3]{}, y[3]{};
            bool valid = true;
            for (int corner = 0; corner < 3; ++corner) {
                const uint32_t vi = ctx.indices[size_t(ti) * 3 + corner];
                if (vi >= ctx.vertex_count) { valid = false; break; }
                double u = 0, v = 0;
                for (int k = 0; k < 3; ++k) {
                    const double p = ctx.positions[size_t(vi) * 3 + k];
                    u += p * chart.tangent[k]; v += p * chart.bitangent[k];
                }
                x[corner] = bx + (chart_atlas::kChartGutterTexels +
                    (u - ou) * chart.texels_per_meter) / scale;
                y[corner] = by + (chart_atlas::kChartGutterTexels +
                    (v - ov) * chart.texels_per_meter) / scale;
                valid = valid && std::isfinite(x[corner]) && std::isfinite(y[corner]);
            }
            if (!valid) continue;
            const double x0 = std::min({x[0], x[1], x[2]});
            const double y0 = std::min({y[0], y[1], y[2]});
            const double x1 = std::max({x[0], x[1], x[2]});
            const double y1 = std::max({y[0], y[1], y[2]});
            min_x = std::min(min_x, x0); min_y = std::min(min_y, y0);
            max_x = std::max(max_x, x1); max_y = std::max(max_y, y1);
            const double area = (x[1] - x[0]) * (y[2] - y[0]) -
                                (y[1] - y[0]) * (x[2] - x[0]);
            if (std::abs(area) < 1e-18) continue;
            const double sign = area > 0 ? 1.0 : -1.0;
            for (int py = lower(y0), ye = upper(y1); py < ye; ++py)
                for (int px = lower(x0), xe = upper(x1); px < xe; ++px) {
                    auto& value = mask[size_t(py) * edge + px];
                    if (!(value & in_atlas) || (value & in_triangle)) continue;
                    bool inside = true;
                    for (int a = 0; a < 3; ++a) {
                        const int b = (a + 1) % 3;
                        const double e = (x[b] - x[a]) * (py + 0.5 - y[a]) -
                                         (y[b] - y[a]) * (px + 0.5 - x[a]);
                        if (e * sign < 0) { inside = false; break; }
                    }
                    if (inside) value |= in_triangle;
                }
        }
        mark_rect(min_x, min_y, max_x, max_y, in_bounds);
        const double gutter = chart_atlas::kChartGutterTexels / scale;
        // Padded bounds stay inside the allocated chart block.
        mark_rect(std::max(bx, min_x - gutter), std::max(by, min_y - gutter),
                  std::min(bx + chart.rect_w / scale, max_x + gutter),
                  std::min(by + chart.rect_h / scale, max_y + gutter), in_gutter);
    }
    for (uint8_t value : mask) {
        out.atlas_texels += (value & in_atlas) != 0;
        out.chart_block_texels += (value & in_block) != 0;
        out.content_bounds_texels += (value & in_bounds) != 0;
        out.gutter_bounds_texels += (value & in_gutter) != 0;
        out.triangle_texels += (value & in_triangle) != 0;
    }
    return out;
}

} // namespace vt
