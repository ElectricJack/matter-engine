#include <memory>
#include <cstdlib>
#include <cstdio>
#include "lod_bake.h"
#include "bake_trace.h"        // Bake Lab task 1.5: LOD ladder spans + counters
#include "bake_trace_names.h"  // kSpanLod, kSpanLodRung
#include "../../libs/MatterSurfaceLib/include/mesh_simplifier.hpp"
#include "../../libs/MatterSurfaceLib/include/mesh_indexed.hpp"
#include "../../libs/MatterSurfaceLib/include/mesh_transform.hpp"  // reproject_triex
#include "../../libs/MeshChartingLib/include/mesh_charting.h"      // WP-A chart build
#include <algorithm>  // std::min (first_rung clamp)
#include <atomic>   // ladder_census() backing counters
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>   // M6: apply_chart_rung's centroid grid

namespace lod_bake {

// Task 11 (Phase 5 autoremesher integration): decimate_tris/decimate_to_error
// route through MSL's MeshIndexed pipeline directly. The previous
// Tri → raylib::Mesh → simplify_mesh → raylib::Mesh → Tri double round-trip
// (via the local tris_to_mesh/mesh_to_tris helpers) has been removed — the
// weld now happens once in from_tri and the intermediate raylib::Mesh is gone.
// simplify(MeshIndexed) internally shims to simplify_mesh for the QEM step;
// when that shim is removed in a later task, this file will already be on the
// final MeshIndexed API. Public API of decimate_tris/decimate_to_error is
// unchanged: input/output still std::vector<Tri>, callers see no difference.

std::vector<Tri> decimate_tris(const std::vector<Tri>& tris, float keep_ratio) {
    if (tris.empty()) return {};

    MeshIndexed in = from_tri(tris, nullptr);
    SimplifyOptions opts;
    opts.target_ratio  = keep_ratio;
    // Topological boundary lock: open-edge vertices are never moved or
    // collapsed, so an open mesh's rim polyline is identical at every LOD.
    // Streamed terrain sectors render through THIS ladder (PartStore's
    // load-time re-bake, not part_flatten), and adjacent sectors share
    // bitwise-identical rim verts — locking keeps them watertight at any
    // LOD pairing. Closed meshes have no open edges and are unaffected.
    opts.lock_boundary = true;

    MeshIndexed out = simplify(in, opts, nullptr);

    std::vector<Tri>   out_tris;
    std::vector<TriEx> out_triex_unused;
    to_tri(out, out_tris, out_triex_unused);
    // Fallback semantics: simplifier returned degenerate (empty) output —
    // treat as identity and hand back the caller's input unchanged. Matches
    // the pre-refactor behavior.
    return out_tris.empty() ? tris : out_tris;
}

std::vector<Tri> decimate_to_error(const std::vector<Tri>& tris, float epsilon,
                                   bool use_aabb_bounds) {
    if (tris.empty()) return {};

    MeshIndexed in = from_tri(tris, nullptr);
    SimplifyOptions opts;
    // target_ratio 0 -> targetTri clamps to 1, so the collapse loop runs until
    // the min heap cost exceeds max_error (the error bound is the ONLY stop).
    opts.target_ratio  = 0.0f;
    opts.max_error     = epsilon * epsilon;  // QEM cost is squared distance
    // lock_boundary=true activates the topological boundary lock (Task 8):
    // open-edge vertices are never collapsed, regardless of CellBounds.
    opts.lock_boundary = true;

    CellBounds cb{};
    const CellBounds* bounds_ptr = nullptr;
    if (use_aabb_bounds) {
        // Pass the mesh's own AABB so vertices on its face planes
        // (tile borders, sheet rims) are frozen — correct for terrain tiles and
        // whole-mesh flattens where the boundary IS the world border.
        // (When use_aabb_bounds=false, bounds_ptr stays nullptr and ONLY the
        // topological boundary lock (open-edge vertices) is active — correct
        // for cluster interiors whose AABB does NOT represent a world border.)
        float minx=1e30f, maxx=-1e30f;
        float miny=1e30f, maxy=-1e30f;
        float minz=1e30f, maxz=-1e30f;
        for (const Tri& t : tris) {
            const float3* vs[3] = { &t.vertex0, &t.vertex1, &t.vertex2 };
            for (const float3* v : vs) {
                if (v->x < minx) minx = v->x; if (v->x > maxx) maxx = v->x;
                if (v->y < miny) miny = v->y; if (v->y > maxy) maxy = v->y;
                if (v->z < minz) minz = v->z; if (v->z > maxz) maxz = v->z;
            }
        }
        cb.min_bound = { minx, miny, minz };
        cb.max_bound = { maxx, maxy, maxz };
        bounds_ptr = &cb;
    }

    MeshIndexed out = simplify(in, opts, bounds_ptr);

    std::vector<Tri>   out_tris;
    std::vector<TriEx> out_triex_unused;
    to_tri(out, out_tris, out_triex_unused);
    // Same fallback semantics as decimate_tris: empty output -> identity.
    return out_tris.empty() ? tris : out_tris;
}

// NOTE (Task 8, Phase 5 autoremesher integration): `reproject_triex` moved to
// MatterSurfaceLib (see MatterSurfaceLib/src/mesh_transform.cpp). Callers in
// part_flatten still work at the Tri/TriEx boundary and wrap through MSL's
// from_tri/to_tri to call the MeshIndexed-shaped reprojector; that wrap is
// the minimum needed to bridge the public API's Tri boundary and the MSL
// interior — Task 11 kept it as a thin adapter rather than collapsing it,
// because lod_bake's public functions still return std::vector<Tri>.

// ---- Chart-space virtual texturing (WP-A) ----------------------------------
//
// Charts a single rung mesh: normal-cone segmentation (MeshChartingLib),
// per-chart planar projection at `texels_per_meter`, page-aligned shelf pack
// (kVtPagePayload grid, kChartGutterTexels gutters, clamped to kVtMaxAtlasDim
// by halving the density), then atlas UVs written into triex.uv0/1/2
// normalized [0,1] over the atlas. Purely a TriEx.uv rewrite — positions,
// normals, materials, tint, AO are untouched, and downstream vertex welding
// (indexed_part_geometry keys on the UV) performs the vertex split between
// charts automatically.
// M6: adopt rep 0's chart table for a coarser rung. See the header for why
// this needs no texel transfer — the parameterisation is analytic.
bool apply_chart_rung(const std::vector<Tri>& tris, std::vector<TriEx>& triex,
                      const std::vector<Tri>& base_tris,
                      const chart_atlas::ChartAtlasRung& base,
                      chart_atlas::ChartAtlasRung& out) {
    out = {};
    const size_t n = tris.size();
    const size_t bn = base_tris.size();
    if (n == 0 || triex.size() != n || bn == 0 || base.charts.empty() ||
        base.atlas_w == 0 || base.atlas_h == 0 ||
        base.tri_order.size() != bn)
        return false;

    // Recover rep 0's per-triangle chart id from the table's own grouping —
    // ChartEntry::first_tri/tri_count index into tri_order, so this is the
    // inverse of the counting sort build_chart_rung wrote. Reading it back
    // rather than plumbing a parallel array keeps the table the single
    // authority on which triangle is in which chart.
    std::vector<uint32_t> base_cid(bn, UINT32_MAX);
    for (size_t c = 0; c < base.charts.size(); ++c) {
        const chart_atlas::ChartEntry& e = base.charts[c];
        const uint64_t end = (uint64_t)e.first_tri + e.tri_count;
        if (end > bn) return false;                    // table disagrees with the mesh
        for (uint64_t i = e.first_tri; i < end; ++i) {
            const uint32_t t = base.tri_order[(size_t)i];
            if (t >= bn) return false;
            base_cid[t] = (uint32_t)c;
        }
    }
    for (uint32_t c : base_cid) if (c == UINT32_MAX) return false;  // unassigned triangle

    // Uniform grid over rep 0's centroids for the nearest-triangle lookup.
    // Same shape as the reprojection index's centroid grid; local here because
    // that one is private to mesh_transform and carries donor machinery this
    // does not need.
    std::vector<float3> bc(bn);
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < bn; ++i) {
        const Tri& t = base_tris[i];
        bc[i] = make_float3((t.vertex0.x + t.vertex1.x + t.vertex2.x) / 3.0f,
                            (t.vertex0.y + t.vertex1.y + t.vertex2.y) / 3.0f,
                            (t.vertex0.z + t.vertex1.z + t.vertex2.z) / 3.0f);
        const float p[3] = { bc[i].x, bc[i].y, bc[i].z };
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::fmin(mn[k], p[k]);
            mx[k] = std::fmax(mx[k], p[k]);
        }
    }
    const float span = std::fmax(mx[0] - mn[0],
                                 std::fmax(mx[1] - mn[1], mx[2] - mn[2]));
    // ~1 triangle per cell on average, clamped so a degenerate span cannot
    // divide by zero or explode the cell count.
    const float cell = std::fmax(span / std::fmax(1.0f, std::cbrt((float)bn)),
                                 1e-6f);
    auto cell_of = [&](const float3& p, int& cx, int& cy, int& cz) {
        cx = (int)std::floor((p.x - mn[0]) / cell);
        cy = (int)std::floor((p.y - mn[1]) / cell);
        cz = (int)std::floor((p.z - mn[2]) / cell);
    };
    auto key_of = [](int cx, int cy, int cz) -> uint64_t {
        return ((uint64_t)(uint32_t)cx * 0x9E3779B97F4A7C15ull) ^
               ((uint64_t)(uint32_t)cy * 0xC2B2AE3D27D4EB4Full) ^
               ((uint64_t)(uint32_t)cz * 0x165667B19E3779F9ull);
    };
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    grid.reserve(bn * 2);
    for (size_t i = 0; i < bn; ++i) {
        int cx, cy, cz; cell_of(bc[i], cx, cy, cz);
        grid[key_of(cx, cy, cz)].push_back((uint32_t)i);
    }

    // Nearest base triangle by centroid, widening the ring until something is
    // found. Ties break on the LOWER index so the result is order-independent
    // and two cold bakes agree (the .gtex double-bake discipline).
    auto nearest_base = [&](const float3& p) -> uint32_t {
        int cx, cy, cz; cell_of(p, cx, cy, cz);
        uint32_t best = UINT32_MAX;
        float best_d = 1e30f;
        for (int r = 0; r < 64; ++r) {
            for (int dz = -r; dz <= r; ++dz)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        // Shell only: the interior was covered by smaller r.
                        if (r > 0 && std::abs(dx) != r && std::abs(dy) != r &&
                            std::abs(dz) != r)
                            continue;
                        auto it = grid.find(key_of(cx + dx, cy + dy, cz + dz));
                        if (it == grid.end()) continue;
                        for (uint32_t i : it->second) {
                            const float ddx = bc[i].x - p.x;
                            const float ddy = bc[i].y - p.y;
                            const float ddz = bc[i].z - p.z;
                            const float d = ddx * ddx + ddy * ddy + ddz * ddz;
                            if (d < best_d || (d == best_d && i < best)) {
                                best_d = d; best = i;
                            }
                        }
                    }
            // One extra ring past the first hit: a nearer centroid can sit in
            // a diagonal neighbour of the cell that produced it.
            if (best != UINT32_MAX && r > 0) break;
        }
        return best;
    };

    std::vector<uint32_t> cid(n, 0);
    for (size_t t = 0; t < n; ++t) {
        const Tri& tr = tris[t];
        const float3 c = make_float3(
            (tr.vertex0.x + tr.vertex1.x + tr.vertex2.x) / 3.0f,
            (tr.vertex0.y + tr.vertex1.y + tr.vertex2.y) / 3.0f,
            (tr.vertex0.z + tr.vertex1.z + tr.vertex2.z) / 3.0f);
        const uint32_t nb = nearest_base(c);
        if (nb == UINT32_MAX) return false;
        cid[t] = base_cid[nb];
    }

    // The shared parameterisation, verbatim. Only tri_order and the per-chart
    // ranges are this rung's own.
    out.atlas_w = base.atlas_w;
    out.atlas_h = base.atlas_h;
    out.charts = base.charts;

    const size_t nc = out.charts.size();
    std::vector<uint32_t> first(nc, 0), count(nc, 0);
    for (size_t t = 0; t < n; ++t) count[cid[t]]++;
    uint32_t running = 0;
    for (size_t c = 0; c < nc; ++c) { first[c] = running; running += count[c]; }
    out.tri_order.resize(n);
    {
        std::vector<uint32_t> cursor = first;
        for (size_t t = 0; t < n; ++t) out.tri_order[cursor[cid[t]]++] = (uint32_t)t;
    }
    for (size_t c = 0; c < nc; ++c) {
        out.charts[c].first_tri = first[c];
        out.charts[c].tri_count = count[c];
    }

    // The UV write, arithmetically identical to build_chart_rung's. It states
    // the mapping through ChartEntry rather than through the builder's locals
    // (minU/minV live on as dot(origin, T/B), T and B being orthonormal), so
    // this and the GPU resolve read the same fields.
    const float inv_w = 1.0f / (float)out.atlas_w;
    const float inv_h = 1.0f / (float)out.atlas_h;
    const float gutter = (float)chart_atlas::kChartGutterTexels;
    for (size_t t = 0; t < n; ++t) {
        const chart_atlas::ChartEntry& e = out.charts[cid[t]];
        const float3 T = make_float3(e.tangent[0], e.tangent[1], e.tangent[2]);
        const float3 B = make_float3(e.bitangent[0], e.bitangent[1], e.bitangent[2]);
        const float3 O = make_float3(e.origin[0], e.origin[1], e.origin[2]);
        const float minU = dot(O, T);
        const float minV = dot(O, B);
        const float3* v[3] = { &tris[t].vertex0, &tris[t].vertex1, &tris[t].vertex2 };
        float2* uv[3] = { &triex[t].uv0, &triex[t].uv1, &triex[t].uv2 };
        for (int k = 0; k < 3; ++k) {
            const float tx = (float)e.rect_x + gutter +
                             (dot(*v[k], T) - minU) * e.texels_per_meter;
            const float ty = (float)e.rect_y + gutter +
                             (dot(*v[k], B) - minV) * e.texels_per_meter;
            uv[k]->x = tx * inv_w;
            uv[k]->y = ty * inv_h;
        }
    }
    return true;
}

bool build_chart_rung(const std::vector<Tri>& tris, std::vector<TriEx>& triex,
                      float texels_per_meter, float cone_deg,
                      chart_atlas::ChartAtlasRung& out) {
    namespace mc = mesh_charting;
    out = {};
    const int n = (int)tris.size();
    if (n <= 0 || triex.size() != tris.size() || !(texels_per_meter > 0.0f) ||
        !(cone_deg > 0.0f) || cone_deg >= 90.0f)
        return false;

    // Triangle soup -> position/index arrays (welding happens inside
    // build_adjacency by exact position, so soup corners are fine and the
    // 32-bit path carries any sector-sized mesh).
    std::vector<float> pos((size_t)n * 9);
    std::vector<unsigned int> idx((size_t)n * 3);
    for (int t = 0; t < n; ++t) {
        const float3* v[3] = { &tris[t].vertex0, &tris[t].vertex1, &tris[t].vertex2 };
        for (int k = 0; k < 3; ++k) {
            const size_t corner = (size_t)t * 3 + k;
            pos[corner * 3 + 0] = v[k]->x;
            pos[corner * 3 + 1] = v[k]->y;
            pos[corner * 3 + 2] = v[k]->z;
            idx[corner] = (unsigned int)corner;
        }
    }

    const auto adj = mc::build_adjacency(pos.data(), idx.data(), n);
    int n_charts = 0;
    const auto cid = mc::segment_charts(pos.data(), idx.data(), n, adj, cone_deg, n_charts);
    if (n_charts <= 0) return false;

    // Per-chart plane basis from the area-weighted average normal.
    const auto normals = mc::chart_average_normals(pos.data(), idx.data(), n, cid, n_charts);
    std::vector<float> T((size_t)n_charts * 3), B((size_t)n_charts * 3);
    for (int c = 0; c < n_charts; ++c)
        mc::plane_basis(&normals[(size_t)c * 3], &T[(size_t)c * 3], &B[(size_t)c * 3]);

    // Per-chart planar extents (meters in plane space).
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> minU((size_t)n_charts,  inf), minV((size_t)n_charts,  inf);
    std::vector<float> maxU((size_t)n_charts, -inf), maxV((size_t)n_charts, -inf);
    auto project = [&](int c, const float3& p, float& u, float& v) {
        const float* tc = &T[(size_t)c * 3];
        const float* bc = &B[(size_t)c * 3];
        u = p.x * tc[0] + p.y * tc[1] + p.z * tc[2];
        v = p.x * bc[0] + p.y * bc[1] + p.z * bc[2];
    };
    for (int t = 0; t < n; ++t) {
        const int c = cid[t];
        const float3* v[3] = { &tris[t].vertex0, &tris[t].vertex1, &tris[t].vertex2 };
        for (int k = 0; k < 3; ++k) {
            float u, w;
            project(c, *v[k], u, w);
            if (u < minU[c]) minU[c] = u;
            if (u > maxU[c]) maxU[c] = u;
            if (w < minV[c]) minV[c] = w;
            if (w > maxV[c]) maxV[c] = w;
        }
    }

    // Pack at the requested density; halve until the page-aligned atlas fits
    // within kVtMaxAtlasDim (the clamp policy). Floor: 1/64 texel per meter.
    const int page   = (int)chart_atlas::kVtPagePayload;
    const int gutter = (int)chart_atlas::kChartGutterTexels;
    const int maxdim = (int)chart_atlas::kVtMaxAtlasDim;
    float tpm = texels_per_meter;
    int atlas_w = 0, atlas_h = 0;
    std::vector<mc::PagedChartSize> sizes((size_t)n_charts);
    std::vector<mc::PagedChartPlacement> placements;
    bool packed = false;
    while (tpm >= 1.0f / 64.0f) {
        for (int c = 0; c < n_charts; ++c) {
            const float eu = maxU[c] - minU[c];
            const float ev = maxV[c] - minV[c];
            sizes[c].content_w = std::max(1, (int)std::ceil((double)eu * tpm));
            sizes[c].content_h = std::max(1, (int)std::ceil((double)ev * tpm));
        }
        if (mc::pack_charts_paged(sizes, page, gutter, maxdim,
                                  atlas_w, atlas_h, placements)) {
            packed = true;
            break;
        }
        tpm *= 0.5f;
    }
    if (!packed) return false;

    // Chart-grouped triangle order (counting sort — deterministic).
    std::vector<uint32_t> first((size_t)n_charts, 0), count((size_t)n_charts, 0);
    for (int t = 0; t < n; ++t) count[cid[t]]++;
    uint32_t running = 0;
    for (int c = 0; c < n_charts; ++c) { first[c] = running; running += count[c]; }
    out.tri_order.resize((size_t)n);
    {
        std::vector<uint32_t> cursor = first;
        for (int t = 0; t < n; ++t) out.tri_order[cursor[cid[t]]++] = (uint32_t)t;
    }

    out.atlas_w = (uint32_t)atlas_w;
    out.atlas_h = (uint32_t)atlas_h;
    out.charts.resize((size_t)n_charts);
    for (int c = 0; c < n_charts; ++c) {
        chart_atlas::ChartEntry& e = out.charts[c];
        const float* tc = &T[(size_t)c * 3];
        const float* bc = &B[(size_t)c * 3];
        for (int i = 0; i < 3; ++i) {
            e.origin[i]    = minU[c] * tc[i] + minV[c] * bc[i];
            e.tangent[i]   = tc[i];
            e.bitangent[i] = bc[i];
        }
        e.rect_x = (uint32_t)placements[c].x;
        e.rect_y = (uint32_t)placements[c].y;
        e.rect_w = (uint32_t)placements[c].w;
        e.rect_h = (uint32_t)placements[c].h;
        e.texels_per_meter = tpm;
        e.first_tri = first[c];
        e.tri_count = count[c];
    }

    // Chart UVs into TriEx, normalized [0,1] over the atlas (not texels).
    const float inv_w = 1.0f / (float)atlas_w;
    const float inv_h = 1.0f / (float)atlas_h;
    for (int t = 0; t < n; ++t) {
        const int c = cid[t];
        const float3* v[3] = { &tris[t].vertex0, &tris[t].vertex1, &tris[t].vertex2 };
        float2* uv[3] = { &triex[t].uv0, &triex[t].uv1, &triex[t].uv2 };
        for (int k = 0; k < 3; ++k) {
            float u, w;
            project(c, *v[k], u, w);
            const float tx = (float)placements[c].x + (float)gutter + (u - minU[c]) * tpm;
            const float ty = (float)placements[c].y + (float)gutter + (w - minV[c]) * tpm;
            uv[k]->x = tx * inv_w;
            uv[k]->y = ty * inv_h;
        }
    }
    return true;
}

LodLevels bake_lods(const std::vector<Tri>& tris, const BakeTargets& targets,
                    BLASManager& blas, const std::vector<TriEx>* triex,
                    BakeObserver* observer,
                    std::vector<BLASHandle>* out_handles,
                    const ChartBakeOptions* chart_opts,
                    std::vector<chart_atlas::ChartAtlasRung>* out_charts) {
    LodLevels out;
    if (out_handles) out_handles->clear();
    if (out_charts) out_charts->clear();
    // Bake Lab task 1.5 (docs/bake-lab.md §II.1): one kSpanLod around the
    // ladder, one kSpanLodRung child per level with tris_in/tris_out/keep_ratio
    // counters. Observation-only; no-op without a current collector.
    BAKE_SPAN(bake_trace::kSpanLod);

    // No cascade and no first_rung here, unlike bake_terrain_lods. BakeTargets
    // targets a keep_ratio, which is RELATIVE to the decimator's input: cascading
    // 0.1 then 0.01 yields 0.001 of the original, so the ladder would have to
    // renormalize every ratio against the previous rung and would then produce
    // different triangle counts than it does today. This is also the prop ladder,
    // not the streamed-sector one — it is not the 114 ms/sector in the profile —
    // so the change would be all risk and no measured win.

    // Every decimated rung reprojects against the SAME full-res source, so the
    // welded source mesh and everything reproject_triex derives from it (source
    // centroids, the centroid hash, face normals, and the triangle-AABB overlap
    // grid) are ladder-invariant. Building them per rung is what made a world
    // sector's ladder cost seconds, and PartStore re-bakes that ladder on every
    // load — see ReprojectSource in mesh_transform.hpp. Built lazily so a ladder
    // with no usable TriEx, or one that never decimates, pays nothing.
    const bool reproject_usable = triex && triex->size() == tris.size();
    std::unique_ptr<MeshIndexed> src_m;
    std::unique_ptr<ReprojectSource> src_index;

    for (size_t lvl = 0; lvl < targets.keep_ratio.size(); ++lvl) {
        BAKE_SPAN(bake_trace::kSpanLodRung);
        // W3: per-rung wall time for the observer's status line (decimation
        // cost only; excludes BLAS registration below). std::chrono rather
        // than the BakeTrace span's own timer so this works even when no
        // collector is current (observer is independent of tracing).
        const auto rung_t0 = std::chrono::steady_clock::now();
        float keep = targets.keep_ratio[lvl];
        bool full = (keep >= 0.999f);
        // Perf fix: for the undecimated (full) level, avoid copying `tris` by
        // passing its data directly via const_cast (register_triangles reads only).
        // For decimated levels, the QEM output is already a fresh vector.
        // MATTER_LOD_BAKE_PROFILE: per-rung split. PartStore re-bakes this
        // ladder inside the GL-thread publish job, where it measured 3-6 s per
        // sector; the rung timer below only ever fed the observer's status line,
        // so nothing attributed that time to a stage.
        const bool lb_prof = std::getenv("MATTER_LOD_BAKE_PROFILE") != nullptr;
        auto lb_mark = std::chrono::steady_clock::now();
        auto lb_split = [&lb_mark]() -> double {
            const auto now = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(now - lb_mark).count();
            lb_mark = now;
            return ms;
        };
        double t_decimate = 0, t_reproject = 0, t_register = 0, t_indexscan = 0;

        std::vector<Tri> decimated;
        if (!full) {
            decimated = decimate_tris(tris, keep);
            if (decimated.empty()) decimated = tris;  // never register empty geometry
        }
        t_decimate = lb_split();
        const std::vector<Tri>& geo = full ? tris : decimated;
        BAKE_COUNT("tris_in",    (double)tris.size());
        BAKE_COUNT("tris_out",   (double)geo.size());
        BAKE_COUNT("keep_ratio", (double)keep);
        // Per-triangle TriEx (materialId/tint/normals/AO) is parallel to `tris`, so
        // it describes `geo` directly only at the undecimated level. Decimation
        // reorders and merges triangles, so a decimated level must have its TriEx
        // REPROJECTED from the source rather than dropped: reproject_triex matches
        // each surviving triangle to its nearest source triangle (carrying
        // materialId/tint/uv/AO) and samples the source's authored shading
        // normals at the rung's corners (SampleSource).
        //
        // Passing nullptr here instead — which is what this did until 2026-07-28 —
        // is what made a coarse rung render in the fallback instance material with
        // flat, per-face shading while the fine rung beside it kept the authored
        // one. On AnimatedRigGallery that showed up as the creature turning from red
        // to grey and going faceted as the camera pulled back. part_flatten.cpp has
        // reprojected across its own ladder since 2026-07-07; this is the same
        // idiom, applied to the ladder script_host uses for animated parts.
        //
        // SampleSource because a rung must inherit the authored shading
        // character, not recompute it blind: recomputing smooth vertex normals
        // over the rung (issue ef7053be) melted every box's 90° edges into a
        // gradient the moment it popped below the full level, while the spheres
        // beside it looked fine.
        std::vector<TriEx> reprojected;
        if (!full && reproject_usable) {
            if (!src_index) {
                src_m = std::make_unique<MeshIndexed>(from_tri(tris, triex));
                src_index = std::make_unique<ReprojectSource>(
                    *src_m, ReprojectNormals::SampleSource);
            }
            MeshIndexed tgt_m = from_tri(geo, nullptr);
            reproject_triex(*src_index, tgt_m);
            // to_tri emits one triangle per 3 indices in order, so `reprojected`
            // lines up with `geo`; the unwelded tris themselves are redundant.
            std::vector<Tri> geo_unwelded_unused;
            to_tri(tgt_m, geo_unwelded_unused, reprojected);
        }
        t_reproject = lb_split();
        const TriEx* ex = nullptr;
        if (full && triex && triex->size() == geo.size())      ex = triex->data();
        else if (!full && reprojected.size() == geo.size())    ex = reprojected.data();
        // WP-A chart build: rewrite this rung's TriEx UVs with chart-atlas UVs
        // before registration. Charted TriEx lives in a local copy so the
        // caller's `triex` (full level) stays const. Fail-closed: on failure
        // the rung registers unchanged UVs and its chart table stays empty.
        std::vector<TriEx> charted_ex;
        chart_atlas::ChartAtlasRung rung_charts;
        if (chart_opts && ex) {
            charted_ex.assign(ex, ex + geo.size());
            float tpm = chart_opts->texels_per_meter;
            if (chart_opts->halve_per_rung && lvl > 0)
                tpm /= (float)(1u << (lvl < 30 ? lvl : 30));
            if (build_chart_rung(geo, charted_ex, tpm, chart_opts->cone_deg,
                                 rung_charts))
                ex = charted_ex.data();
            else
                rung_charts = {};
        }
        // register_triangles may deduplicate (returning an existing handle), so we
        // must NOT pre-record entries().size() as the index — it would be off-by-N
        // if prior identical geometry already occupies that slot. Look up the returned
        // handle's actual position in the entries array after registration instead.
        // register_triangles reads but does not modify the Tri array; const_cast safe.
        BLASHandle h = blas.register_triangles(const_cast<Tri*>(geo.data()), (int)geo.size(), ex);
        t_register = lb_split();
        if (out_handles) out_handles->push_back(h);
        // blas_indices is still filled because script_host serializes it, and
        // there `blas` is a per-part manager whose absolute index IS the
        // part-local one written to disk. Callers holding a live shared manager
        // should read out_handles instead -- see the header.
        uint32_t idx = UINT32_MAX;
        const auto& entries = blas.get_entries();
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i]->handle == h) { idx = (uint32_t)i; break; }
        }
        t_indexscan = lb_split();
        if (lb_prof) {
            std::fprintf(stderr,
                "[lod-rung] lvl=%zu keep=%.3f tris=%zu->%zu entries=%zu "
                "decimate=%.1f reproject=%.1f register=%.1f indexscan=%.1f ms\n",
                lvl, (double)keep, tris.size(), geo.size(), entries.size(),
                t_decimate, t_reproject, t_register, t_indexscan);
        }
        LodLevel L;
        L.screen_size_threshold = targets.threshold[lvl];
        if (idx != UINT32_MAX) L.blas_indices.push_back(idx);
        out.push_back(std::move(L));
        if (out_charts) out_charts->push_back(std::move(rung_charts));

        if (observer) {
            const double rung_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - rung_t0).count();
            observer->on_rung_ready((int)lvl, (int)geo.size(), rung_ms);
        }
    }
    return out;
}

size_t count_terrain_skirt_tris(const std::vector<Tri>& tris,
                                std::vector<uint8_t>* mask) {
    if (mask) mask->assign(tris.size(), 0);
    size_t count = 0;
    for (size_t i = 0; i < tris.size(); ++i) {
        const Tri& t = tris[i];
        const float3* v[3] = { &t.vertex0, &t.vertex1, &t.vertex2 };
        bool vertical = false;
        for (int a = 0; a < 3 && !vertical; ++a) {
            const int b = (a + 1) % 3;
            vertical = v[a]->x == v[b]->x && v[a]->z == v[b]->z &&
                       v[a]->y != v[b]->y;
        }
        if (vertical) {
            ++count;
            if (mask) (*mask)[i] = 1;
        }
    }
    return count;
}

namespace {
// Backing store for ladder_census(). Relaxed atomics: readers want an
// aggregate, not a coherent instant.
std::atomic<uint64_t> g_ladder_rungs[kMaxCensusRungs],
    g_ladder_decimate_us[kMaxCensusRungs], g_ladder_reproject_us[kMaxCensusRungs],
    g_ladder_chart_us[kMaxCensusRungs], g_ladder_blas_us[kMaxCensusRungs],
    g_ladder_tris_in[kMaxCensusRungs], g_ladder_tris_out[kMaxCensusRungs];

size_t census_slot(size_t lvl) {
    return lvl < kMaxCensusRungs ? lvl : kMaxCensusRungs - 1;
}

// Elapsed microseconds since `mark`, advancing `mark` to now.
uint64_t split_us(std::chrono::steady_clock::time_point& mark) {
    const auto now = std::chrono::steady_clock::now();
    const uint64_t us = (uint64_t)std::chrono::duration_cast<
        std::chrono::microseconds>(now - mark).count();
    mark = now;
    return us;
}
}  // namespace

LadderCensus ladder_census() {
    LadderCensus c;
    for (size_t i = 0; i < kMaxCensusRungs; ++i) {
        c.rungs[i]        = g_ladder_rungs[i].load(std::memory_order_relaxed);
        c.decimate_us[i]  = g_ladder_decimate_us[i].load(std::memory_order_relaxed);
        c.reproject_us[i] = g_ladder_reproject_us[i].load(std::memory_order_relaxed);
        c.chart_us[i]     = g_ladder_chart_us[i].load(std::memory_order_relaxed);
        c.blas_us[i]      = g_ladder_blas_us[i].load(std::memory_order_relaxed);
        c.tris_in[i]      = g_ladder_tris_in[i].load(std::memory_order_relaxed);
        c.tris_out[i]     = g_ladder_tris_out[i].load(std::memory_order_relaxed);
    }
    return c;
}

LodLevels bake_terrain_lods(const std::vector<Tri>& tris,
                            const std::vector<uint8_t>& skirt_mask,
                            float bound_radius,
                            const TerrainBakeTargets& targets,
                            BLASManager& blas,
                            const std::vector<TriEx>* triex,
                            BakeObserver* observer,
                            std::vector<BLASHandle>* out_handles,
                            const ChartBakeOptions* chart_opts,
                            std::vector<chart_atlas::ChartAtlasRung>* out_charts) {
    LodLevels out;
    if (out_handles) out_handles->clear();
    if (out_charts) out_charts->clear();
    BAKE_SPAN(bake_trace::kSpanLod);

    const bool triex_usable = triex && triex->size() == tris.size();
    const bool lb_prof = std::getenv("MATTER_LOD_BAKE_PROFILE") != nullptr;

    // MATTER_LOD_CASCADE=0 restores the pre-2026-07-30 path where every coarse
    // rung decimates the FULL-RES surface. Kept as a kill-switch because the
    // cascade changes what the coarsest rung's geometry IS (not just how long
    // it takes to build) — if a distant tile ever looks wrong this isolates the
    // cascade from the rest of the ladder without a rebuild.
    // Read once into a function-local static, not per rung: bake_terrain_lods
    // runs on every streaming worker concurrently and getenv is not safe
    // against a concurrent setenv, so 3 getenv calls per sector x N workers is
    // both wasteful and the wrong shape. C++11 magic statics make the
    // initialization itself thread-safe. (lb_prof above is per call and stays
    // that way — it is the pre-existing MATTER_LOD_BAKE_PROFILE idiom and only
    // gates an fprintf that already dwarfs the getenv.)
    static const bool cascade_enabled = []() {
        const char* v = std::getenv("MATTER_LOD_CASCADE");
        return !(v && v[0] == '0');
    }();

    // Surface-only source (skirts removed) for every decimated level.
    std::vector<Tri> surface;
    std::vector<TriEx> surface_ex;
    surface.reserve(tris.size());
    if (triex_usable) surface_ex.reserve(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) {
        if (i < skirt_mask.size() && skirt_mask[i]) continue;
        surface.push_back(tris[i]);
        if (triex_usable) surface_ex.push_back((*triex)[i]);
    }

    // Reproject source built once from the full-res surface (ladder-invariant,
    // same reasoning as bake_lods).
    std::unique_ptr<MeshIndexed> src_m;
    std::unique_ptr<ReprojectSource> src_index;

    // Cascade state: the previous BAKED rung's decimated output and the eps it
    // was produced at. Empty until a decimated rung has actually run, so the
    // first decimated rung — whichever one first_rung makes it — always starts
    // from `surface`, never from a rung that was skipped.
    std::vector<Tri> cascade_prev;
    float cascade_prev_eps = 0.0f;

    const size_t rung_count = targets.eps_ratio.size();
    // Clamp so the ladder is never empty. A part with zero rungs registers no
    // BLAS at all, and PartStore then treats it as a pure assembler — the
    // sector publishes as an invisible hole, which is strictly worse than a
    // too-coarse mesh. "Skip everything finer than X" therefore degrades to
    // "bake only the coarsest rung I have", not to "bake nothing".
    const size_t first_rung =
        rung_count > 0 ? std::min(targets.first_rung, rung_count - 1) : 0;
    // Everything a rung costs — decimation, reprojection, charting, BLAS
    // registration, and every census counter — lives inside this loop, so
    // starting at first_rung skips all of it and a skipped rung leaves no trace
    // in `out`, `out_handles`, `out_charts` or the census. The three outputs
    // stay parallel and equal-length (one push per iteration, unconditionally),
    // which is what part_store's assert(lod_handles.size() == lods.size())
    // checks.
    //
    // Surviving rungs keep their OWN targets.threshold[lvl] — deliberately NOT
    // renumbered. lod_select picks by threshold VALUE and clamps to the finest
    // entry it has, so a ladder that starts at rung 1 simply never offers
    // anything finer than 0.05 and a close camera draws that. Compacting the
    // thresholds would instead tell the selector the coarse mesh is the
    // near-field one, which is a different (wrong) statement.
    //
    // With first_rung > 0 the eps==0 `full` rung is skipped, so terrain_mesher's
    // skirt curtains — which only ever ship on that rung — are absent from every
    // baked rung. Intended: skirts hide transient streaming holes at close
    // range, and setting first_rung is precisely the caller asserting this part
    // is never drawn at close range. Every surviving rung is still decimated
    // from `surface` (the full-res, skirt-free mesh built above), so skipping
    // rung 0 costs detail nowhere except the skirts.
    for (size_t lvl = first_rung; lvl < rung_count; ++lvl) {
        BAKE_SPAN(bake_trace::kSpanLodRung);
        const auto rung_t0 = std::chrono::steady_clock::now();
        auto census_mark = rung_t0;
        const float eps = targets.eps_ratio[lvl] * bound_radius;
        const bool full = eps <= 0.0f;

        std::vector<Tri> decimated;
        std::vector<TriEx> reprojected;
        size_t dec_in = 0;        // triangles actually handed to the decimator
        bool   cascaded = false;  // ...and whether they came from a coarser rung
        if (!full) {
            // CASCADE. Rung N decimates rung N-1's output, not `surface`. QEM
            // cost tracks the INPUT size, which is why rung 2 cost as much as
            // rung 1 on StreamMountain (24.0 vs 22.2 ms) while producing a
            // third of the triangles — both were collapsing the same
            // 6131-triangle full-res surface.
            //
            // Watertightness is unaffected, and the reason is structural rather
            // than empirical. decimate_to_error runs with lock_boundary=true,
            // and in mesh_simplifier a boundary-locked vertex:
            //   - is never removed. buildEdge rejects an edge whose ends are
            //     both locked, and when exactly one end is locked it makes that
            //     end the SURVIVOR; only the non-survivor is ever marked
            //     removed.
            //   - is never moved. The survivor's position is written under
            //     `if (!verts[e.vi].locked)`.
            // And a rim edge (both ends locked) cannot be eaten indirectly: if
            // an interior apex c collapses into rim vertex a, the triangle
            // carrying rim edge (a,b) degenerates, but the second triangle at
            // the manifold edge (b,c) retargets c->a and carries (a,b) forward.
            // So the open boundary of the input is the open boundary of the
            // output, bit for bit — the rim is a FIXED POINT of this pass. A
            // fixed point composes: rim(decimate(decimate(S))) == rim(S).
            // Cascading therefore hands sector N's rung 2 exactly the rim
            // polyline sector N+1's rung 1 has, which is the property every LOD
            // pairing has always relied on.
            //
            // use_aabb_bounds=false is load-bearing here. The face-plane lock
            // keys off the INPUT mesh's AABB, and a decimated intermediate can
            // have a slightly smaller AABB than full-res — locking a different
            // vertex set on the cascaded pass than on the direct one. Terrain
            // passes false (only the topological lock is live), so the lock set
            // is derived from topology, which is what the fixed-point argument
            // above needs.
            //
            // The one caveat, stated rather than glossed: the cascade adds a
            // from_tri weld (1e-4) over a mesh that now contains QEM-OPTIMAL
            // positions, which the direct path never re-welds. If such a
            // computed vertex landed within 0.1 mm of a rim vertex and came
            // first in triangle order it would become the weld representative
            // and shift the rim by up to 1e-4 m. That is the same tolerance the
            // ladder already welds full-res at, it is bounded (not compounding
            // — each pass re-welds at the same absolute epsilon), and 0.1 mm at
            // a coarse rung's switch-in distance of hundreds of metres is many
            // orders below a pixel. It is not, however, the bitwise identity the
            // paragraph above establishes for the lock itself, so: watertight by
            // construction, with a 1e-4 weld floor inherited from the pipeline.
            //
            // What DOES change is the error semantics, and we do not paper over
            // it. eps is still ABSOLUTE (targets.eps_ratio[lvl] * bound_radius)
            // but it is now measured against the rung being collapsed, not
            // against full-res, and mesh_simplifier rebuilds its quadrics from
            // scratch on every call (decimate() zeroes v.q before seeding), so
            // pass N does not inherit pass N-1's accumulated error. A cascaded
            // rung's true deviation from full-res is bounded by the SUM of the
            // eps values along its chain — 0.015R + 0.05R at the default
            // targets, not 0.05R. That looser bound is not enforced anywhere
            // and is not claimed anywhere; MATTER_LOD_CASCADE=0 buys the tight
            // one back at ~12 ms/sector.
            //
            // Only cascade to a strictly coarser rung: a ladder whose eps does
            // not increase monotonically would be asking an already-coarser
            // mesh for detail it no longer has, so such a rung falls back to
            // full-res rather than silently under-delivering.
            cascaded = cascade_enabled && !cascade_prev.empty() &&
                       eps > cascade_prev_eps;
            const std::vector<Tri>& dec_src = cascaded ? cascade_prev : surface;
            dec_in = dec_src.size();
            decimated = decimate_to_error(dec_src, eps,
                                          /*use_aabb_bounds=*/false);
            if (decimated.empty()) decimated = dec_src;
            // Hand this rung's output to the next coarser one. A copy, not a
            // move: `decimated` is still this rung's geometry for reprojection,
            // charting and registration below. ~70 KB at rung 1, against the
            // ~12 ms the cascade takes off rung 2.
            if (cascade_enabled) {
                cascade_prev     = decimated;
                cascade_prev_eps = eps;
            }
            g_ladder_decimate_us[census_slot(lvl)].fetch_add(
                split_us(census_mark), std::memory_order_relaxed);
            // Reprojection still sources from the FULL-RES surface via
            // src_index, NEVER from the cascade intermediate. Material, tint,
            // UV and shading-normal error would otherwise compound down the
            // chain the way position error does, and unlike position error it
            // is directly visible: a rung two reprojections removed from the
            // authored data drifts material IDs across biome boundaries and
            // smears the sampled normals it was supposed to inherit.
            if (!surface_ex.empty()) {
                if (!src_index) {
                    src_m = std::make_unique<MeshIndexed>(
                        from_tri(surface, &surface_ex));
                    src_index = std::make_unique<ReprojectSource>(
                        *src_m, ReprojectNormals::SampleSource);
                }
                MeshIndexed tgt_m = from_tri(decimated, nullptr);
                reproject_triex(*src_index, tgt_m);
                std::vector<Tri> unwelded_unused;
                to_tri(tgt_m, unwelded_unused, reprojected);
                g_ladder_reproject_us[census_slot(lvl)].fetch_add(
                    split_us(census_mark), std::memory_order_relaxed);
            }
        }
        census_mark = std::chrono::steady_clock::now();
        const std::vector<Tri>& geo = full ? tris : decimated;
        BAKE_COUNT("tris_in",    (double)tris.size());
        BAKE_COUNT("tris_out",   (double)geo.size());
        BAKE_COUNT("keep_ratio", tris.empty()
                       ? 0.0 : (double)geo.size() / (double)tris.size());
        const TriEx* ex = nullptr;
        if (full && triex_usable)                                ex = triex->data();
        else if (!full && reprojected.size() == geo.size() &&
                 !reprojected.empty())                           ex = reprojected.data();
        // WP-A chart build (terrain policy: density halves per coarser rung).
        // Same fail-closed contract as bake_lods above.
        std::vector<TriEx> charted_ex;
        chart_atlas::ChartAtlasRung rung_charts;
        if (chart_opts && ex) {
            charted_ex.assign(ex, ex + geo.size());
            float tpm = chart_opts->texels_per_meter;
            if (chart_opts->halve_per_rung && lvl > 0)
                tpm /= (float)(1u << (lvl < 30 ? lvl : 30));
            if (build_chart_rung(geo, charted_ex, tpm, chart_opts->cone_deg,
                                 rung_charts))
                ex = charted_ex.data();
            else
                rung_charts = {};
        }
        g_ladder_chart_us[census_slot(lvl)].fetch_add(
            split_us(census_mark), std::memory_order_relaxed);
        BLASHandle h = blas.register_triangles(
            const_cast<Tri*>(geo.data()), (int)geo.size(), ex);
        g_ladder_blas_us[census_slot(lvl)].fetch_add(
            split_us(census_mark), std::memory_order_relaxed);
        g_ladder_rungs[census_slot(lvl)].fetch_add(1, std::memory_order_relaxed);
        g_ladder_tris_in[census_slot(lvl)].fetch_add(
            tris.size(), std::memory_order_relaxed);
        g_ladder_tris_out[census_slot(lvl)].fetch_add(
            geo.size(), std::memory_order_relaxed);
        if (out_handles) out_handles->push_back(h);
        uint32_t idx = UINT32_MAX;
        const auto& entries = blas.get_entries();
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i]->handle == h) { idx = (uint32_t)i; break; }
        }
        if (lb_prof) {
            // `dec_in` is what the QEM pass actually collapsed, which under
            // cascading is the previous rung, not `surface` — the one number
            // that says whether the cascade is live for this rung.
            std::fprintf(stderr,
                "[terrain-rung] lvl=%zu eps=%.2f tris=%zu->%zu (surface %zu, "
                "skirts %zu, decimated %zu%s)\n",
                lvl, (double)eps, tris.size(), geo.size(), surface.size(),
                tris.size() - surface.size(), dec_in,
                cascaded ? " cascaded" : "");
        }
        LodLevel L;
        L.screen_size_threshold = targets.threshold[lvl];
        if (idx != UINT32_MAX) L.blas_indices.push_back(idx);
        out.push_back(std::move(L));
        if (out_charts) out_charts->push_back(std::move(rung_charts));

        if (observer) {
            const double rung_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - rung_t0).count();
            observer->on_rung_ready((int)lvl, (int)geo.size(), rung_ms);
        }
    }
    return out;
}

} // namespace lod_bake
