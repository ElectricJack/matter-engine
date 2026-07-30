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
#include <chrono>
#include <cmath>
#include <limits>

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

    for (size_t lvl = 0; lvl < targets.eps_ratio.size(); ++lvl) {
        BAKE_SPAN(bake_trace::kSpanLodRung);
        const auto rung_t0 = std::chrono::steady_clock::now();
        const float eps = targets.eps_ratio[lvl] * bound_radius;
        const bool full = eps <= 0.0f;

        std::vector<Tri> decimated;
        std::vector<TriEx> reprojected;
        if (!full) {
            decimated = decimate_to_error(surface, eps,
                                          /*use_aabb_bounds=*/false);
            if (decimated.empty()) decimated = surface;
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
            }
        }
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
        BLASHandle h = blas.register_triangles(
            const_cast<Tri*>(geo.data()), (int)geo.size(), ex);
        if (out_handles) out_handles->push_back(h);
        uint32_t idx = UINT32_MAX;
        const auto& entries = blas.get_entries();
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i]->handle == h) { idx = (uint32_t)i; break; }
        }
        if (lb_prof) {
            std::fprintf(stderr,
                "[terrain-rung] lvl=%zu eps=%.2f tris=%zu->%zu (surface %zu, "
                "skirts %zu)\n",
                lvl, (double)eps, tris.size(), geo.size(), surface.size(),
                tris.size() - surface.size());
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
