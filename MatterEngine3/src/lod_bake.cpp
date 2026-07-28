#include <memory>
#include <cstdlib>
#include <cstdio>
#include "lod_bake.h"
#include "bake_trace.h"        // Bake Lab task 1.5: LOD ladder spans + counters
#include "bake_trace_names.h"  // kSpanLod, kSpanLodRung
#include "../../libs/MatterSurfaceLib/include/mesh_simplifier.hpp"
#include "../../libs/MatterSurfaceLib/include/mesh_indexed.hpp"
#include "../../libs/MatterSurfaceLib/include/mesh_transform.hpp"  // reproject_triex
#include <chrono>
#include <cmath>

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

LodLevels bake_lods(const std::vector<Tri>& tris, const BakeTargets& targets,
                    BLASManager& blas, const std::vector<TriEx>* triex,
                    BakeObserver* observer) {
    LodLevels out;
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
        // register_triangles may deduplicate (returning an existing handle), so we
        // must NOT pre-record entries().size() as the index — it would be off-by-N
        // if prior identical geometry already occupies that slot. Look up the returned
        // handle's actual position in the entries array after registration instead.
        // register_triangles reads but does not modify the Tri array; const_cast safe.
        BLASHandle h = blas.register_triangles(const_cast<Tri*>(geo.data()), (int)geo.size(), ex);
        t_register = lb_split();
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

        if (observer) {
            const double rung_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - rung_t0).count();
            observer->on_rung_ready((int)lvl, (int)geo.size(), rung_ms);
        }
    }
    return out;
}

} // namespace lod_bake
