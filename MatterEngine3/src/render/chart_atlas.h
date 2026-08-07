#pragma once

// Chart-space virtual texturing — chart sidecar schema (contract C1).
// Producer: WP-A (chart build in the part bake, per LOD rung).
// Consumers: WP-D (page compositor), WP-E (VT residency), WP-G (RT sampling).
//
// Engine-wide, no GPU types. Serialized as a versioned "CHRT" section of the
// .part artifact (see part_asset_v2.{h,cpp}); absence of the section means
// charts = 0 and the legacy (chartless) path.
//
// Coordinate convention: a part-local position p belonging to chart c maps to
// atlas texels as
//   texel.x = c.rect_x + kChartGutterTexels + (dot(p, c.tangent)   - dot(c.origin, c.tangent))   * c.texels_per_meter
//   texel.y = c.rect_y + kChartGutterTexels + (dot(p, c.bitangent) - dot(c.origin, c.bitangent)) * c.texels_per_meter
// and the vertex UV written into TriEx.uv0/1/2 (flowing to the render vertex
// surface.xy) is texel / (atlas_w, atlas_h) — normalized [0,1] over the atlas,
// not texels.

#include <cstdint>
#include <cstring>
#include <vector>

namespace chart_atlas {

// Page geometry (finest mip). Payload texels per page edge; border texels
// stored around each physical page so bilinear sampling stays inside the page.
constexpr uint32_t kVtPagePayload   = 128;
constexpr uint32_t kVtPageBorder    = 4;
// No variant's virtual atlas exceeds this dimension (texels, finest mip).
constexpr uint32_t kVtMaxAtlasDim   = 8192;
// Resident tail: atlas mips of dimension <= kVtTailDim are always resident.
constexpr uint32_t kVtTailDim       = 64;
// Gutter (dilated texels) around every chart's content inside its rect.
constexpr uint32_t kChartGutterTexels = 4;
// Default normal-cone half-angle for chart segmentation.
constexpr float    kChartNormalConeDeg = 45.0f;
// Version of the serialized chart section.
constexpr uint32_t kChartAtlasVersion = 1;

struct ChartEntry {
    float origin[3];        // part-local plane origin
    float tangent[3];       // T — atlas U direction, unit
    float bitangent[3];     // B — atlas V direction, unit
    uint32_t rect_x, rect_y, rect_w, rect_h;   // texels in the virtual atlas (finest mip)
    float texels_per_meter;
    uint32_t first_tri, tri_count;             // into the rung's chart-grouped triangle order
};

struct ChartAtlasRung {
    uint32_t atlas_w = 0, atlas_h = 0;         // finest-mip virtual dims, <= 8192
    std::vector<ChartEntry> charts;
    std::vector<uint32_t>   tri_order;         // triangle indices grouped by chart
};

// M6 (texture unification): the identity of a PARAMETERISATION — the mapping
// from a surface point to an atlas texel — folded from the table's content.
//
// VtResidency keys a variant layer on this instead of on the rung, which is
// what lets every rung of a part share one layer (and therefore one set of
// resident pages) when the bake gave them one table. Deriving it from CONTENT
// rather than from a flag is what makes that self-correcting: a unified ladder
// folds to one value and collapses; a per-rung ladder folds to different
// values and stays split, exactly as before. There is nothing to keep in sync
// with the bake, and no way for a half-unified part to alias two different
// parameterisations onto one layer.
//
// `tri_order` is DELIBERATELY EXCLUDED. It indexes one rung's own triangles,
// so folding it would give every rung a different id and defeat the whole
// purpose. What has to match is the surface-point-to-texel mapping, which is
// exactly the fields below.
inline uint64_t parameterisation_id(const ChartAtlasRung& atlas) {
    auto mix = [](uint64_t h, uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h;
    };
    auto mixf = [&](uint64_t h, float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        // -0.0f and +0.0f are the same parameterisation; normalise so two
        // tables differing only in a zero's sign bit still collapse.
        if (bits == 0x80000000u) bits = 0u;
        return mix(h, bits);
    };
    uint64_t h = 0xcbf29ce484222325ull;
    h = mix(h, atlas.atlas_w);
    h = mix(h, atlas.atlas_h);
    h = mix(h, atlas.charts.size());
    for (const ChartEntry& c : atlas.charts) {
        for (int i = 0; i < 3; ++i) {
            h = mixf(h, c.origin[i]);
            h = mixf(h, c.tangent[i]);
            h = mixf(h, c.bitangent[i]);
        }
        h = mix(h, c.rect_x); h = mix(h, c.rect_y);
        h = mix(h, c.rect_w); h = mix(h, c.rect_h);
        h = mixf(h, c.texels_per_meter);
    }
    return h;
}

// ---------------------------------------------------------------------------
// Serialization (used by the .part "CHRT" trailer and by tests).
// Layout (little-endian, after the caller's own tag/size framing):
//   u32 version (= kChartAtlasVersion)
//   u32 rung_count
//   per rung:
//     u32 atlas_w, u32 atlas_h
//     u32 chart_count, chart_count * ChartEntry (64 bytes each, field order:
//         origin[3], tangent[3], bitangent[3] (f32), rect_x/y/w/h (u32),
//         texels_per_meter (f32), first_tri, tri_count (u32))
//     u32 tri_order_count, tri_order_count * u32
// ---------------------------------------------------------------------------

inline void append_chart_rungs(std::vector<uint8_t>& out,
                               const std::vector<ChartAtlasRung>& rungs) {
    auto put_u32 = [&out](uint32_t v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof v);
    };
    auto put_f32 = [&out](float v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof v);
    };
    put_u32(kChartAtlasVersion);
    put_u32(static_cast<uint32_t>(rungs.size()));
    for (const ChartAtlasRung& r : rungs) {
        put_u32(r.atlas_w);
        put_u32(r.atlas_h);
        put_u32(static_cast<uint32_t>(r.charts.size()));
        for (const ChartEntry& c : r.charts) {
            for (int i = 0; i < 3; ++i) put_f32(c.origin[i]);
            for (int i = 0; i < 3; ++i) put_f32(c.tangent[i]);
            for (int i = 0; i < 3; ++i) put_f32(c.bitangent[i]);
            put_u32(c.rect_x); put_u32(c.rect_y); put_u32(c.rect_w); put_u32(c.rect_h);
            put_f32(c.texels_per_meter);
            put_u32(c.first_tri); put_u32(c.tri_count);
        }
        put_u32(static_cast<uint32_t>(r.tri_order.size()));
        for (uint32_t t : r.tri_order) put_u32(t);
    }
}

// Parses a chart section from [p, end). Returns false on truncation or a
// malformed count. An UNKNOWN version is not an error: rungs_out stays empty
// (forward-compatible readers fall back to charts = 0) and true is returned —
// the caller's size framing skips the payload it cannot interpret.
inline bool parse_chart_rungs(const uint8_t* p, const uint8_t* end,
                              std::vector<ChartAtlasRung>& rungs_out) {
    rungs_out.clear();
    auto get_u32 = [&p, end](uint32_t& v) -> bool {
        if (p > end || static_cast<size_t>(end - p) < sizeof v) return false;
        std::memcpy(&v, p, sizeof v); p += sizeof v; return true;
    };
    auto get_f32 = [&p, end](float& v) -> bool {
        if (p > end || static_cast<size_t>(end - p) < sizeof v) return false;
        std::memcpy(&v, p, sizeof v); p += sizeof v; return true;
    };
    uint32_t version = 0, rung_count = 0;
    if (!get_u32(version)) return false;
    if (version != kChartAtlasVersion) return true;   // unknown version -> charts = 0
    if (!get_u32(rung_count)) return false;
    // Sanity cap: a rung is at least 12 bytes; reject absurd counts up front.
    if (rung_count > 64) return false;
    std::vector<ChartAtlasRung> rungs;
    rungs.reserve(rung_count);
    for (uint32_t ri = 0; ri < rung_count; ++ri) {
        ChartAtlasRung r;
        uint32_t chart_count = 0, order_count = 0;
        if (!get_u32(r.atlas_w) || !get_u32(r.atlas_h) || !get_u32(chart_count))
            return false;
        if (chart_count > static_cast<size_t>(end - p) / 64u) return false;
        r.charts.resize(chart_count);
        for (uint32_t ci = 0; ci < chart_count; ++ci) {
            ChartEntry& c = r.charts[ci];
            bool ok = true;
            for (int i = 0; i < 3 && ok; ++i) ok = get_f32(c.origin[i]);
            for (int i = 0; i < 3 && ok; ++i) ok = get_f32(c.tangent[i]);
            for (int i = 0; i < 3 && ok; ++i) ok = get_f32(c.bitangent[i]);
            ok = ok && get_u32(c.rect_x) && get_u32(c.rect_y) &&
                 get_u32(c.rect_w) && get_u32(c.rect_h) &&
                 get_f32(c.texels_per_meter) &&
                 get_u32(c.first_tri) && get_u32(c.tri_count);
            if (!ok) return false;
        }
        if (!get_u32(order_count)) return false;
        if (order_count > static_cast<size_t>(end - p) / sizeof(uint32_t)) return false;
        r.tri_order.resize(order_count);
        for (uint32_t ti = 0; ti < order_count; ++ti)
            if (!get_u32(r.tri_order[ti])) return false;
        rungs.push_back(std::move(r));
    }
    rungs_out = std::move(rungs);
    return true;
}

} // namespace chart_atlas
