#include "lod_bake.h"
#include "../../MatterSurfaceLib/include/mesh_simplifier.hpp"
#include "../../MatterSurfaceLib/include/mesh_indexed.hpp"
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
                    BLASManager& blas, const std::vector<TriEx>* triex) {
    LodLevels out;
    for (size_t lvl = 0; lvl < targets.keep_ratio.size(); ++lvl) {
        float keep = targets.keep_ratio[lvl];
        bool full = (keep >= 0.999f);
        // Perf fix: for the undecimated (full) level, avoid copying `tris` by
        // passing its data directly via const_cast (register_triangles reads only).
        // For decimated levels, the QEM output is already a fresh vector.
        std::vector<Tri> decimated;
        if (!full) {
            decimated = decimate_tris(tris, keep);
            if (decimated.empty()) decimated = tris;  // never register empty geometry
        }
        const std::vector<Tri>& geo = full ? tris : decimated;
        // Per-triangle TriEx (materialId/tint/normals/AO) is only valid for the
        // undecimated level: `geo` is then the input triangle set in original order,
        // so triex[i] still describes geo[i]. Decimation reorders/merges triangles,
        // so those levels pass nullptr and fall back to the instance material.
        const TriEx* ex = (full && triex && triex->size() == geo.size())
                          ? triex->data() : nullptr;
        // register_triangles may deduplicate (returning an existing handle), so we
        // must NOT pre-record entries().size() as the index — it would be off-by-N
        // if prior identical geometry already occupies that slot. Look up the returned
        // handle's actual position in the entries array after registration instead.
        // register_triangles reads but does not modify the Tri array; const_cast safe.
        BLASHandle h = blas.register_triangles(const_cast<Tri*>(geo.data()), (int)geo.size(), ex);
        uint32_t idx = UINT32_MAX;
        const auto& entries = blas.get_entries();
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i]->handle == h) { idx = (uint32_t)i; break; }
        }
        LodLevel L;
        L.screen_size_threshold = targets.threshold[lvl];
        if (idx != UINT32_MAX) L.blas_indices.push_back(idx);
        out.push_back(std::move(L));
    }
    return out;
}

} // namespace lod_bake
