// chart_atlas_tests.cpp — WP-A (chart-space virtual texturing) headless
// metrics gate. No GPU, no world bakes: synthetic fixtures only.
//
// Gates (per the 2026-07-29 plan, WP-A item 4):
//   - chart coverage == 100% of triangles
//   - zero chart-rect overlap in the atlas
//   - per-chart projection distortion < 2.5
//   - gutter validity (content inset by >= 4 texels; no two charts' content
//     within 4 texels of each other)
//   - vertex-split inflation < 15% (indexed weld with vs without chart UVs)
//   - determinism (same mesh => byte-identical chart table + UVs)
//   - correctness on a > 64k-vertex mesh (32-bit index path)
//   - CHRT .part sidecar round-trip; chartless files stay byte-identical and
//     load with charts = 0 (no version break)

#include "check.h"
#include "lod_bake.h"          // build_chart_rung, bake_lods, chart_atlas, part_asset_v2
#include "part_store.h"        // viewer::PartStore (flat fast path)
#include "raster_mesh.h"       // viewer::build_raster_mesh_data
#include "../../libs/MeshChartingLib/include/mesh_charting.h"
// M6: test_apply_chart_rung decimates a fixture to get a genuinely coarser
// rung to adopt the base parameterisation onto.
#include "../../libs/MatterSurfaceLib/include/mesh_indexed.hpp"
#include "../../libs/MatterSurfaceLib/include/mesh_simplifier.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

Tri make_tri(float3 a, float3 b, float3 c) {
    Tri t{};
    t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
    t.centroid = make_float3((a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f,
                             (a.z + b.z + c.z) / 3.0f);
    return t;
}

// Per-face-normal TriEx (materialId 1, neutral tint, zero UV, AO defaults).
std::vector<TriEx> face_normal_triex(const std::vector<Tri>& tris) {
    std::vector<TriEx> ex(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) {
        const Tri& t = tris[i];
        float3 n = cross(t.vertex1 - t.vertex0, t.vertex2 - t.vertex0);
        const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        n = l > 1e-12f ? make_float3(n.x / l, n.y / l, n.z / l) : make_float3(0, 1, 0);
        TriEx e{};
        e.uv0 = e.uv1 = e.uv2 = make_float2(0.0f);
        e.N0 = e.N1 = e.N2 = n;
        e.materialId = 1;
        e.tint = make_float4(1.0f, 1.0f, 1.0f, 0.0f);
        ex[i] = e;
    }
    return ex;
}

// Axis-aligned cube, edge 2 m, centered at origin (12 tris, hard edges).
std::vector<Tri> build_cube() {
    std::vector<Tri> out;
    const float s = 1.0f;
    const float3 v[8] = {
        make_float3(-s,-s,-s), make_float3( s,-s,-s), make_float3( s, s,-s), make_float3(-s, s,-s),
        make_float3(-s,-s, s), make_float3( s,-s, s), make_float3( s, s, s), make_float3(-s, s, s)};
    const int f[6][4] = {
        {0,3,2,1},  // -Z
        {4,5,6,7},  // +Z
        {0,1,5,4},  // -Y
        {3,7,6,2},  // +Y
        {0,4,7,3},  // -X
        {1,2,6,5}}; // +X
    for (int i = 0; i < 6; ++i) {
        out.push_back(make_tri(v[f[i][0]], v[f[i][1]], v[f[i][2]]));
        out.push_back(make_tri(v[f[i][0]], v[f[i][2]], v[f[i][3]]));
    }
    return out;
}

// Capped cylinder (radius 2, height 4, 48 segments) with a flared lip at the
// top whose underside faces down-outward — a genuine overhang that a world-XZ
// projection could not chart injectively.
std::vector<Tri> build_cylinder_overhang() {
    std::vector<Tri> out;
    const int   S = 48;
    const float R = 2.0f, H = 4.0f;
    const float LIP_R = 2.8f, LIP_Y = 3.4f, LIP_BOT = 2.8f;
    auto ring = [&](float r, float y, int s) {
        const float a = (float)s / (float)S * 6.28318530717958647692f;
        return make_float3(r * std::cos(a), y, r * std::sin(a));
    };
    for (int s = 0; s < S; ++s) {
        const int t = (s + 1) % S;
        // Side wall.
        out.push_back(make_tri(ring(R, 0, s), ring(R, H, s), ring(R, H, t)));
        out.push_back(make_tri(ring(R, 0, s), ring(R, H, t), ring(R, 0, t)));
        // Lip top: rim outward-down to the lip edge.
        out.push_back(make_tri(ring(R, H, s), ring(LIP_R, LIP_Y, s), ring(LIP_R, LIP_Y, t)));
        out.push_back(make_tri(ring(R, H, s), ring(LIP_R, LIP_Y, t), ring(R, H, t)));
        // Lip underside: overhang faces pointing mostly downward.
        out.push_back(make_tri(ring(LIP_R, LIP_Y, s), ring(R, LIP_BOT, s), ring(R, LIP_BOT, t)));
        out.push_back(make_tri(ring(LIP_R, LIP_Y, s), ring(R, LIP_BOT, t), ring(LIP_R, LIP_Y, t)));
        // Top cap fan.
        out.push_back(make_tri(make_float3(0, H, 0), ring(R, H, t), ring(R, H, s)));
        // Bottom cap fan.
        out.push_back(make_tri(make_float3(0, 0, 0), ring(R, 0, s), ring(R, 0, t)));
    }
    return out;
}

// Rolling heightfield: two octaves, combined slopes up to ~42 degrees —
// hilly-terrain-like relief without being an adversarial noise field (a
// greedy 45-degree normal-cone segmentation fragments white-noise normals
// into thousands of slivers, which no terrain sector resembles).
float sheet_height(float x, float z) {
    return 3.0f * std::sin(x * 0.15f) * std::cos(z * 0.13f) +
           1.0f * std::sin(x * 0.45f + 0.5f) * std::sin(z * 0.40f + 1.3f);
}

// Large subdivided heightfield-like sheet: (N+1)^2 vertices with N = 260 =>
// 68,121 unique welded vertices (> 64k, 16-bit indices impossible) and
// 135,200 triangles. Smooth per-vertex normals so the chartless weld actually
// merges shared corners (that makes the vertex-split metric meaningful).
void build_big_sheet(std::vector<Tri>& tris, std::vector<TriEx>& triex) {
    const int   N = 260;
    const float EXT = 64.0f;                 // meters, sector-sized
    const float step = EXT / (float)N;
    auto pos = [&](int i, int j) {
        const float x = -EXT * 0.5f + step * (float)i;
        const float z = -EXT * 0.5f + step * (float)j;
        return make_float3(x, sheet_height(x, z), z);
    };
    auto nrm = [&](int i, int j) {
        const float x = -EXT * 0.5f + step * (float)i;
        const float z = -EXT * 0.5f + step * (float)j;
        const float e = 0.01f;
        const float dhdx = (sheet_height(x + e, z) - sheet_height(x - e, z)) / (2 * e);
        const float dhdz = (sheet_height(x, z + e) - sheet_height(x, z - e)) / (2 * e);
        float3 n = make_float3(-dhdx, 1.0f, -dhdz);
        const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        return make_float3(n.x / l, n.y / l, n.z / l);
    };
    tris.clear(); triex.clear();
    tris.reserve((size_t)N * N * 2);
    triex.reserve((size_t)N * N * 2);
    for (int j = 0; j < N; ++j) {
        for (int i = 0; i < N; ++i) {
            const float3 p00 = pos(i, j),     p10 = pos(i + 1, j);
            const float3 p01 = pos(i, j + 1), p11 = pos(i + 1, j + 1);
            const float3 n00 = nrm(i, j),     n10 = nrm(i + 1, j);
            const float3 n01 = nrm(i, j + 1), n11 = nrm(i + 1, j + 1);
            tris.push_back(make_tri(p00, p11, p10));
            tris.push_back(make_tri(p00, p01, p11));
            TriEx a{}; a.uv0 = a.uv1 = a.uv2 = make_float2(0.0f);
            a.N0 = n00; a.N1 = n11; a.N2 = n10;
            a.materialId = 1; a.tint = make_float4(1, 1, 1, 0);
            TriEx b = a; b.N0 = n00; b.N1 = n01; b.N2 = n11;
            triex.push_back(a);
            triex.push_back(b);
        }
    }
}

// ---------------------------------------------------------------------------
// Metric helpers
// ---------------------------------------------------------------------------

// Soup positions/indices matching build_chart_rung's internal layout (needed
// for the distortion metric, which is measured through MeshChartingLib).
void soup_arrays(const std::vector<Tri>& tris, std::vector<float>& pos,
                 std::vector<unsigned int>& idx) {
    const size_t n = tris.size();
    pos.resize(n * 9);
    idx.resize(n * 3);
    for (size_t t = 0; t < n; ++t) {
        const float3* v[3] = { &tris[t].vertex0, &tris[t].vertex1, &tris[t].vertex2 };
        for (int k = 0; k < 3; ++k) {
            const size_t c = t * 3 + (size_t)k;
            pos[c * 3 + 0] = v[k]->x; pos[c * 3 + 1] = v[k]->y; pos[c * 3 + 2] = v[k]->z;
            idx[c] = (unsigned int)c;
        }
    }
}

size_t indexed_vertex_count(const std::vector<Tri>& tris, const std::vector<TriEx>& ex) {
    const viewer::RasterMeshData mesh = viewer::build_raster_mesh_data(
        tris.data(), ex.data(), (int)tris.size());
    return (size_t)mesh.vertex_count;
}

std::vector<uint8_t> serialize_rung(const chart_atlas::ChartAtlasRung& rung) {
    std::vector<uint8_t> bytes;
    chart_atlas::append_chart_rungs(bytes, {rung});
    return bytes;
}

// Run every geometric gate on one charted rung. `label` prefixes messages.
void check_rung_gates(const char* label,
                      const std::vector<Tri>& tris,
                      const std::vector<TriEx>& charted,
                      const chart_atlas::ChartAtlasRung& rung) {
    char msg[256];
    const size_t n = tris.size();
    const uint32_t page = chart_atlas::kVtPagePayload;
    const uint32_t gutter = chart_atlas::kChartGutterTexels;

    snprintf(msg, sizeof msg, "%s: atlas dims positive and <= %u", label,
             chart_atlas::kVtMaxAtlasDim);
    CHECK(rung.atlas_w > 0 && rung.atlas_h > 0 &&
          rung.atlas_w <= chart_atlas::kVtMaxAtlasDim &&
          rung.atlas_h <= chart_atlas::kVtMaxAtlasDim, msg);
    snprintf(msg, sizeof msg, "%s: atlas dims are page multiples", label);
    CHECK(rung.atlas_w % page == 0 && rung.atlas_h % page == 0, msg);

    // Coverage: tri_order is a permutation of [0, n) and the charts' ranges
    // partition it exactly.
    snprintf(msg, sizeof msg, "%s: tri_order covers every triangle exactly once", label);
    bool cover_ok = rung.tri_order.size() == n;
    if (cover_ok) {
        std::vector<uint8_t> seen(n, 0);
        for (uint32_t t : rung.tri_order) {
            if (t >= n || seen[t]) { cover_ok = false; break; }
            seen[t] = 1;
        }
    }
    CHECK(cover_ok, msg);
    snprintf(msg, sizeof msg, "%s: chart ranges partition tri_order", label);
    {
        uint64_t total = 0;
        bool ranges_ok = true;
        uint32_t expect_first = 0;
        for (const auto& c : rung.charts) {
            if (c.first_tri != expect_first) ranges_ok = false;
            expect_first += c.tri_count;
            total += c.tri_count;
        }
        CHECK(ranges_ok && total == n, msg);
    }

    // Page alignment + zero rect overlap + rects inside the atlas.
    snprintf(msg, sizeof msg, "%s: chart rects page-aligned and inside the atlas", label);
    bool align_ok = true;
    for (const auto& c : rung.charts) {
        if (c.rect_x % page || c.rect_y % page || c.rect_w % page || c.rect_h % page)
            align_ok = false;
        if (c.rect_w == 0 || c.rect_h == 0 ||
            c.rect_x + c.rect_w > rung.atlas_w || c.rect_y + c.rect_h > rung.atlas_h)
            align_ok = false;
    }
    CHECK(align_ok, msg);
    snprintf(msg, sizeof msg, "%s: zero chart-rect overlap", label);
    {
        bool overlap = false;
        for (size_t a = 0; a < rung.charts.size() && !overlap; ++a)
            for (size_t b = a + 1; b < rung.charts.size() && !overlap; ++b) {
                const auto& A = rung.charts[a];
                const auto& Bc = rung.charts[b];
                const bool disjoint = A.rect_x + A.rect_w <= Bc.rect_x ||
                                      Bc.rect_x + Bc.rect_w <= A.rect_x ||
                                      A.rect_y + A.rect_h <= Bc.rect_y ||
                                      Bc.rect_y + Bc.rect_h <= A.rect_y;
                if (!disjoint) overlap = true;
            }
        CHECK(!overlap, msg);
    }

    // UV range + gutter validity: every corner's texel position must sit
    // inside its chart's rect inset by the gutter. Disjoint rects + the inset
    // then guarantee no two charts' content is within 2*gutter texels.
    snprintf(msg, sizeof msg, "%s: UVs in [0,1] and content inset by the %u-texel gutter",
             label, gutter);
    {
        bool uv_ok = true;
        const float eps = 1e-3f;
        std::vector<int> chart_of(n, -1);
        for (size_t c = 0; c < rung.charts.size(); ++c)
            for (uint32_t k = 0; k < rung.charts[c].tri_count; ++k)
                chart_of[rung.tri_order[rung.charts[c].first_tri + k]] = (int)c;
        for (size_t t = 0; t < n && uv_ok; ++t) {
            const int ci = chart_of[t];
            if (ci < 0) { uv_ok = false; break; }
            const auto& c = rung.charts[ci];
            const float2 uvs[3] = { charted[t].uv0, charted[t].uv1, charted[t].uv2 };
            for (int k = 0; k < 3; ++k) {
                const float u = uvs[k].x, v = uvs[k].y;
                if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) { uv_ok = false; break; }
                const float tx = u * (float)rung.atlas_w;
                const float ty = v * (float)rung.atlas_h;
                if (tx < (float)(c.rect_x + gutter) - eps ||
                    tx > (float)(c.rect_x + c.rect_w - gutter) + eps ||
                    ty < (float)(c.rect_y + gutter) - eps ||
                    ty > (float)(c.rect_y + c.rect_h - gutter) + eps) { uv_ok = false; break; }
            }
        }
        CHECK(uv_ok, msg);
    }

    // Distortion < 2.5 per chart (max/min singular value of the projection).
    snprintf(msg, sizeof msg, "%s: per-chart projection distortion < 2.5", label);
    {
        std::vector<float> pos;
        std::vector<unsigned int> idx;
        soup_arrays(tris, pos, idx);
        float worst = 1.0f;
        for (const auto& c : rung.charts) {
            std::vector<int> list(c.tri_count);
            for (uint32_t k = 0; k < c.tri_count; ++k)
                list[k] = (int)rung.tri_order[c.first_tri + k];
            const float d = mesh_charting::projection_distortion(
                pos.data(), idx.data(), (int)n, list.data(), (int)list.size(),
                c.tangent, c.bitangent);
            if (d > worst) worst = d;
        }
        CHECK(worst < 2.5f, msg);
        printf("  [%s] charts=%zu atlas=%ux%u worst distortion=%.3f\n",
               label, rung.charts.size(), rung.atlas_w, rung.atlas_h, worst);
    }

    // Pack efficiency (reported, not gated).
    {
        double block_area = 0.0;
        for (const auto& c : rung.charts)
            block_area += (double)c.rect_w * (double)c.rect_h;
        const double atlas_area = (double)rung.atlas_w * (double)rung.atlas_h;
        printf("  [%s] pack efficiency (block area / atlas area) = %.1f%%\n",
               label, 100.0 * block_area / atlas_area);
    }
}

// Full fixture pass: chart build, gates, vertex-split inflation, determinism.
void run_fixture(const char* label, const std::vector<Tri>& tris,
                 const std::vector<TriEx>& base_ex, float tpm) {
    char msg[256];
    std::vector<TriEx> charted = base_ex;
    chart_atlas::ChartAtlasRung rung;
    snprintf(msg, sizeof msg, "%s: build_chart_rung succeeds", label);
    const bool ok = lod_bake::build_chart_rung(tris, charted, tpm,
                                               chart_atlas::kChartNormalConeDeg, rung);
    CHECK(ok, msg);
    if (!ok) return;

    check_rung_gates(label, tris, charted, rung);

    // Vertex-split inflation < 15% against the chartless weld.
    {
        const size_t before = indexed_vertex_count(tris, base_ex);
        const size_t after = indexed_vertex_count(tris, charted);
        snprintf(msg, sizeof msg, "%s: vertex-split inflation < 15%%", label);
        CHECK(before > 0 && (double)after <= (double)before * 1.15, msg);
        printf("  [%s] indexed verts %zu -> %zu (%.2f%% inflation)\n", label,
               before, after,
               before ? 100.0 * ((double)after / (double)before - 1.0) : 0.0);
    }

    // Determinism: a second run from the same inputs is byte-identical
    // (chart table AND uv stream).
    {
        std::vector<TriEx> charted2 = base_ex;
        chart_atlas::ChartAtlasRung rung2;
        const bool ok2 = lod_bake::build_chart_rung(
            tris, charted2, tpm, chart_atlas::kChartNormalConeDeg, rung2);
        snprintf(msg, sizeof msg, "%s: deterministic chart table", label);
        CHECK(ok2 && serialize_rung(rung) == serialize_rung(rung2), msg);
        snprintf(msg, sizeof msg, "%s: deterministic UVs", label);
        bool uv_same = ok2 && charted2.size() == charted.size();
        for (size_t i = 0; uv_same && i < charted.size(); ++i)
            uv_same = std::memcmp(&charted[i].uv0, &charted2[i].uv0, sizeof(float2)) == 0 &&
                      std::memcmp(&charted[i].uv1, &charted2[i].uv1, sizeof(float2)) == 0 &&
                      std::memcmp(&charted[i].uv2, &charted2[i].uv2, sizeof(float2)) == 0;
        CHECK(uv_same, msg);
    }

    // Chart UVs must reach the render-vertex surface stream: the indexed mesh's
    // surface_uvs (which matter_engine copies verbatim into VkRasterVertex
    // .surface.xy) must carry nonzero chart UVs.
    {
        const viewer::RasterMeshData mesh = viewer::build_raster_mesh_data(
            tris.data(), charted.data(), (int)tris.size());
        bool any_nonzero = false;
        for (float v : mesh.surface_uvs)
            if (v != 0.0f) { any_nonzero = true; break; }
        snprintf(msg, sizeof msg, "%s: chart UVs flow into the surface_uv vertex stream", label);
        CHECK(!mesh.surface_uvs.empty() && any_nonzero, msg);
    }
}

// ---------------------------------------------------------------------------
// Ladder + sidecar tests
// ---------------------------------------------------------------------------

// M6 (texture unification): apply_chart_rung gives a coarser rung rep 0's
// parameterisation instead of charting it independently.
//
// The keystone assertion is the IDENTITY one: fed rep 0's own geometry,
// apply_chart_rung must reproduce build_chart_rung's UVs BIT FOR BIT. The two
// write the same mapping from different sides — the builder from its internal
// minU/minV locals, the adopter from the ChartEntry fields the GPU reads — so
// if they ever disagree, the disagreement is exactly the gap between what the
// bake wrote and what the shader will resolve, which is unfindable from a
// screenshot.
void test_apply_chart_rung() {
    printf("=== test_apply_chart_rung ===\n");
    const std::vector<Tri> tris = build_cylinder_overhang();
    const std::vector<TriEx> base_ex = face_normal_triex(tris);

    std::vector<TriEx> charted = base_ex;
    chart_atlas::ChartAtlasRung base;
    const bool built = lod_bake::build_chart_rung(
        tris, charted, 16.0f, chart_atlas::kChartNormalConeDeg, base);
    CHECK(built, "apply: the base rung charts");
    if (!built) return;

    // (1) IDENTITY. Same mesh in, same UVs out, exactly.
    {
        std::vector<TriEx> adopted = base_ex;
        chart_atlas::ChartAtlasRung out;
        const bool ok = lod_bake::apply_chart_rung(tris, adopted, tris, base, out);
        CHECK(ok, "apply: adopting onto the base mesh succeeds");
        // Agreement is measured in TEXELS, not bits, and the reason is worth
        // stating because it is not sloppiness.
        //
        // build_chart_rung computes (dot(p,T) - minU) * tpm from its own local
        // minU. Nothing downstream ever sees minU: ChartEntry stores
        // origin = minU*T + minV*B, and both this function and the GPU resolve
        // recover it as dot(origin, T). T and B are orthonormal, so that is
        // minU*dot(T,T) + minV*dot(B,T) = minU in exact arithmetic and
        // minU + O(eps) in float. The residual is inherent to the round trip
        // through `origin`, which is the field the shader actually has.
        //
        // So the adopter cannot be bit-identical to the builder, and chasing
        // that would be chasing the wrong target: vt_chart_resolve.glsl makes
        // the SAME reconstruction, so this path agrees with the GPU at least
        // as closely as the builder's own vertex UVs do. What must hold is
        // that the disagreement is far below one texel — otherwise a rung
        // adoption would visibly shift the texture.
        double worst_texels = 0.0;
        size_t worst_tri = 0;
        bool sized = ok && adopted.size() == charted.size();
        if (sized) {
            for (size_t i = 0; i < charted.size(); ++i) {
                const float2* a[3] = { &adopted[i].uv0, &adopted[i].uv1, &adopted[i].uv2 };
                const float2* b[3] = { &charted[i].uv0, &charted[i].uv1, &charted[i].uv2 };
                for (int k = 0; k < 3; ++k) {
                    const double dx = std::fabs((double)a[k]->x - b[k]->x) * base.atlas_w;
                    const double dy = std::fabs((double)a[k]->y - b[k]->y) * base.atlas_h;
                    const double d = std::fmax(dx, dy);
                    if (d > worst_texels) { worst_texels = d; worst_tri = i; }
                }
            }
        }
        CHECK(sized, "apply: the adopted UV stream has one entry per triangle");
        CHECK(sized && worst_texels < 0.01,
              "apply: adopted UVs agree with the builder's to well under a texel");
        printf("  [apply] worst UV disagreement vs the builder: %.6f texels "
               "(triangle %zu of %zu)\n", worst_texels, worst_tri, charted.size());
        CHECK(ok && out.atlas_w == base.atlas_w && out.atlas_h == base.atlas_h,
              "apply: the atlas dimensions are the base's, not recomputed");
        CHECK(ok && out.charts.size() == base.charts.size(),
              "apply: the chart count is the base's");
        // The shared parameterisation must travel VERBATIM -- a chart whose
        // basis or rect drifted would put the same surface point on a
        // different texel per rung, which is the churn this milestone removes.
        bool basis_same = ok && out.charts.size() == base.charts.size();
        for (size_t c = 0; basis_same && c < base.charts.size(); ++c) {
            const auto& a = out.charts[c];
            const auto& b = base.charts[c];
            basis_same = std::memcmp(a.origin, b.origin, sizeof a.origin) == 0 &&
                         std::memcmp(a.tangent, b.tangent, sizeof a.tangent) == 0 &&
                         std::memcmp(a.bitangent, b.bitangent, sizeof a.bitangent) == 0 &&
                         a.rect_x == b.rect_x && a.rect_y == b.rect_y &&
                         a.rect_w == b.rect_w && a.rect_h == b.rect_h &&
                         a.texels_per_meter == b.texels_per_meter;
        }
        CHECK(basis_same, "apply: every chart's basis and rect survive verbatim");
    }

    // (2) A GENUINELY COARSER MESH. Decimate, adopt, and require that the
    // adopted UVs land inside the atlas and that the per-chart ranges still
    // partition the rung's triangles.
    {
        MeshIndexed fine = from_tri(tris, &base_ex);
        SimplifyOptions opts;
        opts.target_ratio = 0.4f;
        MeshIndexed coarse_m = simplify(fine, opts);
        std::vector<Tri> coarse; std::vector<TriEx> coarse_ex;
        to_tri(coarse_m, coarse, coarse_ex);
        CHECK(!coarse.empty() && coarse.size() < tris.size(),
              "apply: the fixture actually decimated");
        if (!coarse.empty()) {
            coarse_ex.resize(coarse.size());
            chart_atlas::ChartAtlasRung out;
            const bool ok = lod_bake::apply_chart_rung(coarse, coarse_ex, tris,
                                                       base, out);
            CHECK(ok, "apply: a decimated rung adopts the base parameterisation");
            if (ok) {
                CHECK(out.tri_order.size() == coarse.size(),
                      "apply: tri_order covers every triangle of the coarse rung");
                size_t total = 0;
                for (const auto& c : out.charts) total += c.tri_count;
                CHECK(total == coarse.size(),
                      "apply: the per-chart ranges partition the rung exactly");
                std::vector<char> seen(coarse.size(), 0);
                bool perm = true;
                for (uint32_t t : out.tri_order) {
                    if (t >= coarse.size() || seen[t]) { perm = false; break; }
                    seen[t] = 1;
                }
                CHECK(perm, "apply: tri_order is a permutation, no duplicates");

                // UVs stay on the atlas. A coarse triangle straddling a chart
                // boundary reaches past its rect into the gutter, which is the
                // documented approximation -- but it must not leave the atlas,
                // because that samples another chart outright. Measured, and
                // the overshoot is REPORTED rather than merely bounded, since
                // the gutter is the thing this milestone has to justify.
                float worst_over = 0.0f;
                bool on_atlas = true;
                for (size_t t = 0; t < coarse.size(); ++t) {
                    const float2* uv[3] = { &coarse_ex[t].uv0, &coarse_ex[t].uv1,
                                            &coarse_ex[t].uv2 };
                    const auto& e = out.charts[0];
                    (void)e;
                    for (int k = 0; k < 3; ++k) {
                        if (!(uv[k]->x >= 0.0f && uv[k]->x <= 1.0f &&
                              uv[k]->y >= 0.0f && uv[k]->y <= 1.0f))
                            on_atlas = false;
                        worst_over = std::fmax(worst_over,
                            std::fmax(std::fmax(-uv[k]->x, uv[k]->x - 1.0f),
                                      std::fmax(-uv[k]->y, uv[k]->y - 1.0f)));
                    }
                }
                CHECK(on_atlas, "apply: every adopted UV stays inside the atlas");
                printf("  coarse rung %zu tris, worst UV overshoot %.5f "
                       "(0 = fully inside)\n", coarse.size(),
                       std::fmax(0.0f, worst_over));
            }
        }
    }

    // (3) FAIL-CLOSED. A base with no charts, or a mismatched TriEx, must
    // return false and leave the caller's UVs untouched -- the same contract
    // build_chart_rung has, so a caller's fallback path is unchanged.
    {
        std::vector<TriEx> ex = base_ex;
        chart_atlas::ChartAtlasRung empty, out;
        CHECK(!lod_bake::apply_chart_rung(tris, ex, tris, empty, out),
              "apply: a chartless base fails closed");
        std::vector<TriEx> short_ex(tris.size() - 1);
        CHECK(!lod_bake::apply_chart_rung(tris, short_ex, tris, base, out),
              "apply: a TriEx that does not match the mesh fails closed");
        bool untouched = true;
        for (size_t i = 0; i < ex.size(); ++i)
            if (std::memcmp(&ex[i].uv0, &base_ex[i].uv0, sizeof(float2)) != 0)
                untouched = false;
        CHECK(untouched, "apply: a failed adoption leaves the UVs alone");
    }
    printf("PASSED\n");
}

// M6 step 2: ChartBakeOptions::unify_parameterisation makes a whole LADDER
// share one chart table instead of charting each rung. The assertions are the
// two halves of "rung-invariant": every rung reports the SAME parameterisation,
// and the flag is off by default so nothing changes for callers that have not
// opted in.
void test_unified_ladder_parameterisation() {
    printf("=== test_unified_ladder_parameterisation ===\n");
    // Cylinder-overhang, NOT the cube. The cube is 12 triangles and barely
    // decimates, so its rungs are the same mesh and chart identically with or
    // without the flag — which makes BOTH assertions below vacuous. The
    // failability check caught exactly that when this test was first written.
    const std::vector<Tri> tris = build_cylinder_overhang();
    const std::vector<TriEx> ex = face_normal_triex(tris);

    auto bake = [&](bool unify, std::vector<chart_atlas::ChartAtlasRung>& charts) {
        BLASManager blas;
        std::vector<BLASHandle> handles;
        lod_bake::ChartBakeOptions opts;
        opts.unify_parameterisation = unify;
        lod_bake::bake_lods(tris, lod_bake::BakeTargets{}, blas, &ex, nullptr,
                            &handles, &opts, &charts);
    };

    std::vector<chart_atlas::ChartAtlasRung> legacy, unified;
    bake(false, legacy);
    bake(true, unified);

    CHECK(legacy.size() == unified.size(), "unify: same rung count either way");
    CHECK(!unified.empty() && !unified[0].charts.empty(),
          "unify: the base rung still charts");
    if (unified.empty() || unified[0].charts.empty()) { printf("FAILED\n"); return; }

    // THE PROPERTY. Every charted rung reports the base's atlas and the base's
    // chart bases/rects. Only tri_order and the per-chart ranges may differ,
    // because those index this rung's own triangles.
    size_t charted_rungs = 0;
    bool all_shared = true;
    for (size_t r = 0; r < unified.size(); ++r) {
        const auto& u = unified[r];
        if (u.charts.empty()) continue;      // rung never charted; not a failure
        ++charted_rungs;
        if (u.atlas_w != unified[0].atlas_w || u.atlas_h != unified[0].atlas_h ||
            u.charts.size() != unified[0].charts.size()) { all_shared = false; break; }
        for (size_t c = 0; c < u.charts.size(); ++c) {
            const auto& a = u.charts[c];
            const auto& b = unified[0].charts[c];
            if (std::memcmp(a.origin, b.origin, sizeof a.origin) != 0 ||
                std::memcmp(a.tangent, b.tangent, sizeof a.tangent) != 0 ||
                std::memcmp(a.bitangent, b.bitangent, sizeof a.bitangent) != 0 ||
                a.rect_x != b.rect_x || a.rect_y != b.rect_y ||
                a.rect_w != b.rect_w || a.rect_h != b.rect_h ||
                a.texels_per_meter != b.texels_per_meter) { all_shared = false; break; }
        }
        if (!all_shared) break;
    }
    CHECK(charted_rungs >= 2,
          "unify: the fixture produced at least two charted rungs to compare");
    CHECK(all_shared,
          "unify: every rung reports the base's atlas, chart bases and rects");
    printf("  %zu charted rungs share one parameterisation\n", charted_rungs);

    // FAILABILITY. Without the flag the rungs must NOT all agree, or the
    // assertion above proves nothing -- a fixture whose rungs happen to chart
    // identically would pass it either way.
    {
        bool legacy_differs = false;
        for (size_t r = 1; r < legacy.size() && !legacy_differs; ++r) {
            if (legacy[r].charts.empty() || legacy[0].charts.empty()) continue;
            if (legacy[r].atlas_w != legacy[0].atlas_w ||
                legacy[r].atlas_h != legacy[0].atlas_h ||
                legacy[r].charts.size() != legacy[0].charts.size()) {
                legacy_differs = true; break;
            }
            for (size_t c = 0; c < legacy[r].charts.size(); ++c) {
                const auto& a = legacy[r].charts[c];
                const auto& b = legacy[0].charts[c];
                if (std::memcmp(a.origin, b.origin, sizeof a.origin) != 0 ||
                    a.rect_x != b.rect_x || a.rect_y != b.rect_y ||
                    a.rect_w != b.rect_w || a.rect_h != b.rect_h) {
                    legacy_differs = true; break;
                }
            }
        }
        CHECK(legacy_differs,
              "unify: WITHOUT the flag the rungs genuinely disagree (so the "
              "test above is not vacuous)");
    }

    // Default-off, checked on the struct rather than inferred from behaviour.
    CHECK(lod_bake::ChartBakeOptions{}.unify_parameterisation == false,
          "unify: off by default, so existing callers are untouched");

    // M6 step 3b: the bake half and the runtime half meet at
    // chart_atlas::parameterisation_id. VtResidency keys a variant LAYER on
    // this, so "the rungs share a table" only becomes "the rungs share their
    // resident pages" if the fold agrees. Asserting the two halves against
    // each other here is what makes the runtime behaviour follow from the bake
    // behaviour instead of merely being intended to.
    {
        uint64_t first_id = 0;
        bool have_first = false, all_same = true;
        for (const auto& r : unified) {
            if (r.charts.empty()) continue;
            const uint64_t id = chart_atlas::parameterisation_id(r);
            if (!have_first) { first_id = id; have_first = true; continue; }
            if (id != first_id) all_same = false;
        }
        CHECK(have_first && all_same,
              "unify: every unified rung folds to ONE parameterisation id "
              "(so VtResidency gives them one layer)");

        bool legacy_any_differs = false;
        uint64_t legacy_first = 0;
        bool legacy_have = false;
        for (const auto& r : legacy) {
            if (r.charts.empty()) continue;
            const uint64_t id = chart_atlas::parameterisation_id(r);
            if (!legacy_have) { legacy_first = id; legacy_have = true; continue; }
            if (id != legacy_first) legacy_any_differs = true;
        }
        CHECK(legacy_any_differs,
              "unify: per-rung rungs fold to DIFFERENT ids (so they keep one "
              "layer each, exactly as before)");

        // tri_order must not enter the fold: it indexes a rung's own
        // triangles, so folding it would give every rung a distinct id and
        // silently undo the whole milestone while every other test still
        // passed. Perturb it directly rather than trusting the comment.
        if (have_first && !unified.empty()) {
            chart_atlas::ChartAtlasRung perturbed = unified[0];
            perturbed.tri_order.push_back(12345u);
            CHECK(chart_atlas::parameterisation_id(perturbed) == first_id,
                  "unify: tri_order does not enter the parameterisation id");
            // ...but the MAPPING does. Move a chart's rect and the id must
            // move, or unrelated parameterisations would collapse onto one
            // layer and sample each other's texels.
            chart_atlas::ChartAtlasRung moved = unified[0];
            if (!moved.charts.empty()) {
                moved.charts[0].rect_x += 1u;
                CHECK(chart_atlas::parameterisation_id(moved) != first_id,
                      "unify: moving a chart's rect DOES change the id");
                chart_atlas::ChartAtlasRung denser = unified[0];
                denser.charts[0].texels_per_meter *= 2.0f;
                CHECK(chart_atlas::parameterisation_id(denser) != first_id,
                      "unify: changing texel density DOES change the id");
            }
        }
    }
    printf("PASSED\n");
}

void test_ladder_charts() {
    const std::vector<Tri> tris = build_cube();
    const std::vector<TriEx> ex = face_normal_triex(tris);

    BLASManager blas;
    std::vector<BLASHandle> handles;
    lod_bake::ChartBakeOptions opts;                 // props: 16 t/m every rung
    std::vector<chart_atlas::ChartAtlasRung> charts;
    const lod_bake::LodLevels lods = lod_bake::bake_lods(
        tris, lod_bake::BakeTargets{}, blas, &ex, nullptr, &handles, &opts, &charts);
    CHECK(lods.size() == charts.size(), "bake_lods: one chart table per rung");
    CHECK(!charts.empty() && !charts[0].charts.empty(),
          "bake_lods: rung 0 carries charts");
    // The registered rung-0 entry must carry the chart UVs (not the caller's
    // zero UVs) — that is what the loader turns into vertex data.
    if (!handles.empty()) {
        const auto* e = blas.get_entry(handles[0]);
        bool nonzero = false;
        if (e && e->tri_extra.size() == tris.size())
            for (const TriEx& t : e->tri_extra)
                if (t.uv0.x != 0.0f || t.uv0.y != 0.0f ||
                    t.uv1.x != 0.0f || t.uv1.y != 0.0f) { nonzero = true; break; }
        CHECK(nonzero, "bake_lods: registered rung-0 TriEx carries chart UVs");
    }
    // Caller's TriEx must NOT have been mutated (charting works on a copy).
    bool caller_untouched = true;
    for (const TriEx& t : ex)
        if (t.uv0.x != 0.0f || t.uv0.y != 0.0f) { caller_untouched = false; break; }
    CHECK(caller_untouched, "bake_lods: caller TriEx not mutated");

    // Terrain ladder density policy: 16 t/m at rung 0, halving per rung.
    std::vector<Tri> sheet;
    std::vector<TriEx> sheet_ex;
    build_big_sheet(sheet, sheet_ex);
    BLASManager blas2;
    lod_bake::ChartBakeOptions topts;
    topts.halve_per_rung = true;
    std::vector<chart_atlas::ChartAtlasRung> tcharts;
    std::vector<BLASHandle> thandles;
    const std::vector<uint8_t> no_skirts(sheet.size(), 0);
    const lod_bake::LodLevels tlods = lod_bake::bake_terrain_lods(
        sheet, no_skirts, 45.0f, lod_bake::TerrainBakeTargets{}, blas2,
        &sheet_ex, nullptr, &thandles, &topts, &tcharts);
    CHECK(tlods.size() == tcharts.size(), "bake_terrain_lods: one chart table per rung");
    bool densities_ok = tcharts.size() >= 2 &&
                        !tcharts[0].charts.empty() && !tcharts[1].charts.empty();
    if (densities_ok) {
        densities_ok = tcharts[0].charts[0].texels_per_meter == 16.0f &&
                       tcharts[1].charts[0].texels_per_meter == 8.0f;
    }
    CHECK(densities_ok, "bake_terrain_lods: 16 t/m rung 0, halved at rung 1");
}

std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::vector<uint8_t> bytes;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return bytes;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bytes.resize((size_t)(sz > 0 ? sz : 0));
    if (!bytes.empty() && std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
        bytes.clear();
    std::fclose(f);
    return bytes;
}

void test_sidecar_roundtrip() {
    const std::vector<Tri> tris = build_cube();
    std::vector<TriEx> charted = face_normal_triex(tris);
    chart_atlas::ChartAtlasRung rung;
    CHECK(lod_bake::build_chart_rung(tris, charted, 16.0f,
                                     chart_atlas::kChartNormalConeDeg, rung),
          "sidecar: fixture chart build succeeds");

    BLASManager blas;
    blas.register_triangles(const_cast<Tri*>(tris.data()), (int)tris.size(),
                            charted.data());
    TLASManager tlas(64);
    const uint64_t hash = 0x00C0FFEE12345678ull;
    const std::vector<part_asset::VolumeEmitter> no_emitters;
    const std::vector<chart_atlas::ChartAtlasRung> rungs = {rung, rung};

    // 1) With charts: CHRT round-trips bit-exactly through the .part.
    const std::string with_charts = "build/chart_atlas_with_charts.part";
    CHECK(part_asset::save_v2(with_charts, blas, tlas, nullptr, 0, {},
                              no_emitters, rungs, hash),
          "sidecar: save_v2 with CHRT succeeds");
    {
        BLASManager lb; TLASManager lt(64);
        std::vector<part_asset::ChildInstance> kids;
        part_asset::LodLevels lods;
        std::vector<part_asset::VolumeEmitter> emitters;
        std::vector<chart_atlas::ChartAtlasRung> loaded;
        CHECK(part_asset::load_v2(with_charts, hash, lb, lt, kids, lods,
                                  emitters, loaded),
              "sidecar: chart-aware load_v2 succeeds");
        std::vector<uint8_t> a, b;
        chart_atlas::append_chart_rungs(a, rungs);
        chart_atlas::append_chart_rungs(b, loaded);
        CHECK(a == b, "sidecar: chart table round-trips byte-identically");
    }
    // 2) Chart-unaware loaders must still accept a CHRT-bearing part
    //    (the strict suffix grammar skips the section).
    {
        BLASManager lb; TLASManager lt(64);
        std::vector<part_asset::ChildInstance> kids;
        part_asset::LodLevels lods;
        std::vector<part_asset::VolumeEmitter> emitters;
        std::optional<part_asset::PartAnimationLink> link;
        CHECK(part_asset::load_v2(with_charts, hash, lb, lt, kids, lods,
                                  emitters, link),
              "sidecar: legacy load_v2 accepts a CHRT-bearing part");
        CHECK(!link.has_value(), "sidecar: CHRT part is static (no ANLK)");
    }
    // 3) Compat guarantee: empty charts write byte-identical output to the
    //    legacy overload, and the chart-aware loader reads charts = 0 from it.
    const std::string legacy = "build/chart_atlas_legacy.part";
    const std::string empty_charts = "build/chart_atlas_empty_charts.part";
    CHECK(part_asset::save_v2(legacy, blas, tlas, nullptr, 0, {},
                              no_emitters, hash),
          "sidecar: legacy save_v2 succeeds");
    CHECK(part_asset::save_v2(empty_charts, blas, tlas, nullptr, 0, {},
                              no_emitters,
                              std::vector<chart_atlas::ChartAtlasRung>{}, hash),
          "sidecar: save_v2 with empty charts succeeds");
    {
        const auto a = read_file_bytes(legacy);
        const auto b = read_file_bytes(empty_charts);
        CHECK(!a.empty() && a == b,
              "sidecar: empty chart table writes byte-identical legacy output");
        BLASManager lb; TLASManager lt(64);
        std::vector<part_asset::ChildInstance> kids;
        part_asset::LodLevels lods;
        std::vector<part_asset::VolumeEmitter> emitters;
        std::vector<chart_atlas::ChartAtlasRung> loaded;
        CHECK(part_asset::load_v2(legacy, hash, lb, lt, kids, lods,
                                  emitters, loaded),
              "sidecar: chart-aware load_v2 accepts a chartless part");
        CHECK(loaded.empty(), "sidecar: chartless part loads with charts = 0");
    }
    std::remove(with_charts.c_str());
    std::remove(legacy.c_str());
    std::remove(empty_charts.c_str());
}

// ---------------------------------------------------------------------------
// Flat fast path: a part loaded via a .flat.part must come back with populated
// per-rung chart tables (parallel to lod_mesh_data) and chart UVs in its
// vertex streams, exactly like the stage_from_snapshot path.
// ---------------------------------------------------------------------------
void test_flat_load_charts() {
    namespace fs = std::filesystem;
    const fs::path root = fs::path("build") / "chart_atlas_flat_fixture";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "parts", ec);
    struct Cleanup {
        fs::path root;
        ~Cleanup() { std::error_code ignored; fs::remove_all(root, ignored); }
    } cleanup{root};
    constexpr uint64_t hash = 0xF1A7C4A75ull;

    // Canonical static .part (content is irrelevant to the flat, but the flat
    // fast path only engages beside a valid ANLK-free canonical Part).
    {
        BLASManager source;
        TLASManager tlas(4);
        const std::vector<Tri> tris = build_cube();
        const std::vector<TriEx> ex = face_normal_triex(tris);
        source.register_triangles(const_cast<Tri*>(tris.data()), (int)tris.size(),
                                  ex.data());
        CHECK(part_asset::save_v2((root / part_asset::cache_path_resolved(hash)).string(),
                                  source, tlas, nullptr, 0, {}, hash),
              "flat-path: canonical .part fixture saves");
    }
    // v3 flat: two clusters (cube prop + cylinder prop), two LOD levels each
    // (level 1 reuses the same BLAS entry — decimation is irrelevant here).
    {
        BLASManager flat_blas;
        TLASManager flat_tlas(4);
        const std::vector<Tri> cube = build_cube();
        const std::vector<TriEx> cube_ex = face_normal_triex(cube);
        const std::vector<Tri> cyl = build_cylinder_overhang();
        const std::vector<TriEx> cyl_ex = face_normal_triex(cyl);
        const uint32_t cube_index = (uint32_t)flat_blas.get_entries().size();
        flat_blas.register_triangles(const_cast<Tri*>(cube.data()), (int)cube.size(),
                                     cube_ex.data());
        const uint32_t cyl_index = (uint32_t)flat_blas.get_entries().size();
        flat_blas.register_triangles(const_cast<Tri*>(cyl.data()), (int)cyl.size(),
                                     cyl_ex.data());
        auto make_cluster = [](uint32_t blas_index, float extent) {
            part_asset::FlatCluster cl{};
            for (int k = 0; k < 3; ++k) { cl.aabb_min[k] = -extent; cl.aabb_max[k] = extent; }
            part_asset::LodLevel fine;  fine.screen_size_threshold = 0.5f;
            fine.blas_indices.push_back(blas_index);
            part_asset::LodLevel coarse; coarse.screen_size_threshold = 0.05f;
            coarse.blas_indices.push_back(blas_index);
            cl.lods.push_back(fine);
            cl.lods.push_back(coarse);
            return cl;
        };
        const std::vector<part_asset::FlatCluster> clusters = {
            make_cluster(cube_index, 1.0f), make_cluster(cyl_index, 3.0f)};
        CHECK(part_asset::save_flat_v3((root / part_asset::cache_path_flat(hash)).string(),
                                       flat_blas, flat_tlas, clusters, hash),
              "flat-path: .flat.part fixture saves");
    }

    viewer::PartStore store(root.string());
    const viewer::LoadedPart* lp = store.get_or_load(hash);
    CHECK(lp != nullptr, "flat-path: get_or_load succeeds");
    if (!lp) return;
    CHECK(!lp->clusters.empty() && lp->clusters.size() == 2,
          "flat-path: part came back through the flat fast path (2 clusters)");
    CHECK(lp->lod_charts.size() == lp->lod_mesh_data.size(),
          "flat-path: lod_charts parallel to lod_mesh_data");

    // Every mesh a cluster ladder step draws (chart_rung = mesh index) must
    // have a populated chart table AND nonzero chart UVs in its vertex stream.
    bool cluster_rungs_charted = true;
    bool cluster_uvs_flow = true;
    for (const viewer::LoadedCluster& cl : lp->clusters) {
        for (int mesh_idx : cl.lod_mesh) {
            if (mesh_idx < 0 || (size_t)mesh_idx >= lp->lod_charts.size()) {
                cluster_rungs_charted = false;
                continue;
            }
            const chart_atlas::ChartAtlasRung& rung = lp->lod_charts[mesh_idx];
            if (rung.charts.empty() || rung.atlas_w == 0) cluster_rungs_charted = false;
            const viewer::RasterMeshData& mesh = lp->lod_mesh_data[mesh_idx];
            bool nonzero = false;
            bool in_range = !mesh.surface_uvs.empty();
            for (float v : mesh.surface_uvs) {
                if (v != 0.0f) nonzero = true;
                if (v < 0.0f || v > 1.0f) in_range = false;
            }
            if (!nonzero || !in_range) cluster_uvs_flow = false;
            // Coverage: the rung's tri_order must span the mesh's triangles.
            if (rung.tri_order.size() != mesh.indices.size() / 3)
                cluster_rungs_charted = false;
        }
    }
    CHECK(cluster_rungs_charted, "flat-path: every cluster rung carries charts");
    CHECK(cluster_uvs_flow, "flat-path: chart UVs flow into cluster vertex streams");

    // The legacy whole-part rungs (RT view) are charted too.
    bool legacy_charted = true;
    for (size_t li = 0; li < lp->lod_blas.size() && li < lp->lod_charts.size(); ++li)
        if (lp->lod_charts[li].charts.empty()) legacy_charted = false;
    CHECK(legacy_charted, "flat-path: legacy whole-part rungs carry charts");

    // M6: the FLAT path is the one authored props take, and it charts at three
    // sites of its own that ChartBakeOptions never reaches. Step 2 unified the
    // two lod_bake ladders and left these three per-rung — so this asserts the
    // switch actually arrives here, on a real PartStore load, rather than
    // trusting that wiring five call sites hit all five.
    {
        auto ring_tables_agree = [](const viewer::LoadedPart& p,
                                    const viewer::LoadedCluster& cl) {
            const chart_atlas::ChartAtlasRung* first = nullptr;
            for (int mesh_idx : cl.lod_mesh) {
                if (mesh_idx < 0 || (size_t)mesh_idx >= p.lod_charts.size()) continue;
                const auto& r = p.lod_charts[mesh_idx];
                if (r.charts.empty()) continue;
                if (!first) { first = &r; continue; }
                if (r.atlas_w != first->atlas_w || r.atlas_h != first->atlas_h ||
                    r.charts.size() != first->charts.size())
                    return false;
                for (size_t c = 0; c < r.charts.size(); ++c) {
                    const auto& a = r.charts[c];
                    const auto& b = first->charts[c];
                    if (std::memcmp(a.origin, b.origin, sizeof a.origin) != 0 ||
                        a.rect_x != b.rect_x || a.rect_y != b.rect_y ||
                        a.rect_w != b.rect_w || a.rect_h != b.rect_h ||
                        a.texels_per_meter != b.texels_per_meter)
                        return false;
                }
            }
            return first != nullptr;
        };

        store.release(hash);
#ifdef _WIN32
        _putenv_s("MATTER_VT_UNIFY", "1");
#else
        setenv("MATTER_VT_UNIFY", "1", 1);
#endif
        viewer::PartStore ustore(root.string());
        const viewer::LoadedPart* up = ustore.get_or_load(hash);
        CHECK(up != nullptr, "flat-path/unify: reload succeeds");
        bool all_agree = up != nullptr && !up->clusters.empty();
        if (up)
            for (const viewer::LoadedCluster& cl : up->clusters)
                if (!ring_tables_agree(*up, cl)) all_agree = false;
        CHECK(all_agree,
              "flat-path/unify: every rung of a cluster shares one parameterisation");
        if (up) ustore.release(hash);
#ifdef _WIN32
        _putenv_s("MATTER_VT_UNIFY", "");
#else
        unsetenv("MATTER_VT_UNIFY");
#endif
        // The fixture's two LOD levels point at the SAME BLAS entry, so its
        // rungs are the same mesh and would chart identically either way —
        // which makes the assertion above true for the wrong reason. Say so
        // rather than let it read as a proof it is not: what this checks is
        // that the switch REACHES this path and nothing throws or empties,
        // and the per-rung-geometry proof lives in
        // test_unified_ladder_parameterisation on a fixture that decimates.
        printf("  [flat-path/unify] switch reaches the flat path; the "
               "differing-geometry proof is test_unified_ladder_parameterisation\n");
    }

    store.release(hash);
}

// ---------------------------------------------------------------------------
// Measurement mode (not part of the automated gate): chart a REAL .flat.part
// artifact's cluster rungs and print metrics. Usage:
//   chart_atlas_tests --measure-flat <path/to/<16-hex>.flat.part>
// ---------------------------------------------------------------------------
int measure_flat(const char* path_arg) {
    const std::string path = path_arg;
    const std::string base = std::filesystem::path(path).filename().string();
    uint64_t hash = 0;
    if (base.size() < 16 ||
        std::sscanf(base.c_str(), "%16llx", (unsigned long long*)&hash) != 1) {
        printf("measure-flat: cannot parse 16-hex hash from '%s'\n", base.c_str());
        return 2;
    }
    BLASManager blas;
    TLASManager tlas(65536);
    std::vector<part_asset::FlatCluster> clusters;
    std::vector<part_asset::FlatInstanceRef> refs;
    if (!part_asset::load_flat_v3(path, hash, blas, tlas, clusters, refs)) {
        printf("measure-flat: load_flat_v3 failed for %s (version %u)\n",
               path.c_str(), part_asset::peek_format_version(path));
        return 2;
    }
    const auto& entries = blas.get_entries();
    printf("measure-flat: %s — %zu clusters, %zu BLAS entries\n",
           path.c_str(), clusters.size(), entries.size());
    size_t rung_ordinal = 0;
    for (size_t ci = 0; ci < clusters.size(); ++ci) {
        for (size_t li = 0; li < clusters[ci].lods.size(); ++li) {
            std::vector<Tri> tris;
            std::vector<TriEx> triex;
            for (uint32_t bi : clusters[ci].lods[li].blas_indices) {
                if (bi >= entries.size()) continue;
                tris.insert(tris.end(), entries[bi]->triangles.begin(),
                            entries[bi]->triangles.end());
                triex.insert(triex.end(), entries[bi]->tri_extra.begin(),
                             entries[bi]->tri_extra.end());
            }
            if (tris.empty() || triex.size() != tris.size()) continue;
            const size_t before = indexed_vertex_count(tris, triex);
            std::vector<TriEx> charted = triex;
            chart_atlas::ChartAtlasRung rung;
            if (!lod_bake::build_chart_rung(tris, charted, 16.0f,
                                            chart_atlas::kChartNormalConeDeg, rung)) {
                printf("  cluster %zu lod %zu: %zu tris — CHART BUILD FAILED\n",
                       ci, li, tris.size());
                continue;
            }
            const size_t after = indexed_vertex_count(tris, charted);
            std::vector<float> pos;
            std::vector<unsigned int> idx;
            soup_arrays(tris, pos, idx);
            float worst = 1.0f;
            double block_area = 0.0;
            for (const auto& c : rung.charts) {
                std::vector<int> list(c.tri_count);
                for (uint32_t k = 0; k < c.tri_count; ++k)
                    list[k] = (int)rung.tri_order[c.first_tri + k];
                const float d = mesh_charting::projection_distortion(
                    pos.data(), idx.data(), (int)tris.size(), list.data(),
                    (int)list.size(), c.tangent, c.bitangent);
                if (d > worst) worst = d;
                block_area += (double)c.rect_w * (double)c.rect_h;
            }
            printf("  cluster %zu lod %zu: tris=%zu charts=%zu atlas=%ux%u "
                   "distortion=%.3f pack=%.1f%% verts %zu->%zu (%.2f%% inflation)\n",
                   ci, li, tris.size(), rung.charts.size(), rung.atlas_w,
                   rung.atlas_h, worst,
                   100.0 * block_area / ((double)rung.atlas_w * rung.atlas_h),
                   before, after,
                   before ? 100.0 * ((double)after / before - 1.0) : 0.0);
            ++rung_ordinal;
        }
    }
    printf("measure-flat: measured %zu rungs\n", rung_ordinal);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--measure-flat") == 0)
        return measure_flat(argv[2]);
    // Fixture 1: cube (hard edges, one chart per face at the 45-degree cone).
    {
        const std::vector<Tri> tris = build_cube();
        run_fixture("cube", tris, face_normal_triex(tris), 16.0f);
    }
    // Fixture 2: cylinder with an overhanging lip (curved side walls +
    // downward-facing underside; charts must stay injective and low-distortion).
    {
        const std::vector<Tri> tris = build_cylinder_overhang();
        run_fixture("cylinder-overhang", tris, face_normal_triex(tris), 16.0f);
    }
    // Fixture 3: > 64k-vertex heightfield sheet (32-bit index path).
    {
        std::vector<Tri> tris;
        std::vector<TriEx> ex;
        build_big_sheet(tris, ex);
        const size_t welded = indexed_vertex_count(tris, ex);
        char msg[128];
        snprintf(msg, sizeof msg, "big-sheet: fixture exceeds 64k welded vertices (%zu)",
                 welded);
        CHECK(welded > 65536, msg);
        run_fixture("big-sheet", tris, ex, 16.0f);
    }

    test_ladder_charts();
    test_apply_chart_rung();
    test_unified_ladder_parameterisation();
    test_sidecar_roundtrip();
    test_flat_load_charts();

    if (g_failures == 0) { printf("chart_atlas_tests: ALL PASS\n"); return 0; }
    printf("chart_atlas_tests: %d FAILURE(S)\n", g_failures);
    return 1;
}
