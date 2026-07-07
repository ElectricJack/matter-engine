#include "../include/lod_bake.h"
#include "../../MatterSurfaceLib/include/mesh_simplifier.hpp"
extern "C" {
#include "raylib.h"
}
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace lod_bake {

// Pack a Tri vector into a non-indexed raylib Mesh (3 verts per tri).
static Mesh tris_to_mesh(const std::vector<Tri>& tris) {
    Mesh m{};
    m.triangleCount = (int)tris.size();
    m.vertexCount = (int)tris.size() * 3;
    if (m.vertexCount == 0) return m;
    m.vertices = (float*)MemAlloc(sizeof(float) * 3 * m.vertexCount);
    for (size_t i = 0; i < tris.size(); ++i) {
        const Tri& t = tris[i];
        float* v = m.vertices + i * 9;
        v[0]=t.vertex0.x; v[1]=t.vertex0.y; v[2]=t.vertex0.z;
        v[3]=t.vertex1.x; v[4]=t.vertex1.y; v[5]=t.vertex1.z;
        v[6]=t.vertex2.x; v[7]=t.vertex2.y; v[8]=t.vertex2.z;
    }
    return m;
}

// Unpack an indexed-or-not raylib Mesh back into Tri (recompute centroid).
static std::vector<Tri> mesh_to_tris(const Mesh& m) {
    std::vector<Tri> out;
    auto vert = [&](int idx) {
        return make_float3(m.vertices[idx*3+0], m.vertices[idx*3+1], m.vertices[idx*3+2]);
    };
    auto emit = [&](float3 a, float3 b, float3 c) {
        Tri t; t.vertex0=a; t.vertex1=b; t.vertex2=c;
        t.centroid = make_float3((a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3);
        out.push_back(t);
    };
    if (m.indices) {
        for (int i = 0; i < m.triangleCount; ++i)
            emit(vert(m.indices[i*3+0]), vert(m.indices[i*3+1]), vert(m.indices[i*3+2]));
    } else {
        for (int i = 0; i < m.triangleCount; ++i)
            emit(vert(i*3+0), vert(i*3+1), vert(i*3+2));
    }
    return out;
}

std::vector<Tri> decimate_tris(const std::vector<Tri>& tris, float keep_ratio) {
    if (tris.empty()) return {};
    Mesh in = tris_to_mesh(tris);
    SimplifyOptions opts; opts.target_ratio = keep_ratio; opts.lock_boundary = false;
    Mesh out = simplify_mesh(in, opts, nullptr);
    std::vector<Tri> result = (out.vertexCount > 0) ? mesh_to_tris(out) : tris;
    // simplify_mesh allocates with MemAlloc; free both scratch meshes.
    if (in.vertices) MemFree(in.vertices);
    if (out.vertices) MemFree(out.vertices);
    if (out.indices) MemFree(out.indices);
    return result;
}

std::vector<Tri> decimate_to_error(const std::vector<Tri>& tris, float epsilon,
                                   bool use_aabb_bounds) {
    if (tris.empty()) return {};
    Mesh in = tris_to_mesh(tris);
    SimplifyOptions opts;
    // target_ratio 0 -> targetTri clamps to 1, so the collapse loop runs until
    // the min heap cost exceeds max_error (the error bound is the ONLY stop).
    opts.target_ratio  = 0.0f;
    opts.max_error     = epsilon * epsilon;  // QEM cost is squared distance
    // lock_boundary=true activates the topological boundary lock (Task 8):
    // open-edge vertices are never collapsed, regardless of CellBounds.
    opts.lock_boundary = true;
    if (use_aabb_bounds) {
        // Also pass the mesh's own AABB so vertices on its face planes
        // (tile borders, sheet rims) are frozen — correct for terrain tiles and
        // whole-mesh flattens where the boundary IS the world border.
        float minx=1e30f,maxx=-1e30f,miny=1e30f,maxy=-1e30f,minz=1e30f,maxz=-1e30f;
        for (const Tri& t : tris) {
            const float3* vs[3] = { &t.vertex0, &t.vertex1, &t.vertex2 };
            for (const float3* v : vs) {
                if (v->x < minx) minx = v->x; if (v->x > maxx) maxx = v->x;
                if (v->y < miny) miny = v->y; if (v->y > maxy) maxy = v->y;
                if (v->z < minz) minz = v->z; if (v->z > maxz) maxz = v->z;
            }
        }
        CellBounds cb;
        cb.min_bound = { minx, miny, minz };
        cb.max_bound = { maxx, maxy, maxz };
        Mesh out = simplify_mesh(in, opts, &cb);
        std::vector<Tri> result = (out.vertexCount > 0) ? mesh_to_tris(out) : tris;
        if (in.vertices) MemFree(in.vertices);
        if (out.vertices) MemFree(out.vertices);
        if (out.indices) MemFree(out.indices);
        return result;
    } else {
        // use_aabb_bounds=false: pass bounds=nullptr so ONLY the topological
        // boundary lock (open-edge vertices) is active.  Face-plane locking is
        // suppressed — correct for cluster interiors whose AABB does NOT
        // represent a world border.
        Mesh out = simplify_mesh(in, opts, nullptr);
        std::vector<Tri> result = (out.vertexCount > 0) ? mesh_to_tris(out) : tris;
        if (in.vertices) MemFree(in.vertices);
        if (out.vertices) MemFree(out.vertices);
        if (out.indices) MemFree(out.indices);
        return result;
    }
}

// NOTE (Task 8, Phase 5 autoremesher integration): `reproject_triex` moved to
// MatterSurfaceLib (see MatterSurfaceLib/src/mesh_transform.cpp). Callers that
// still work in Tri/TriEx vectors wrap through MSL's from_tri/to_tri; those
// wrappers will collapse in Task 11 when lod_bake itself is refactored to
// MeshIndexed at its boundary.

LodLevels bake_lods(const std::vector<Tri>& tris, const BakeTargets& targets,
                    BLASManager& blas, const std::vector<TriEx>* triex) {
    LodLevels out;
    for (size_t lvl = 0; lvl < targets.keep_ratio.size(); ++lvl) {
        float keep = targets.keep_ratio[lvl];
        bool full = (keep >= 0.999f);
        std::vector<Tri> geo = full ? tris : decimate_tris(tris, keep);
        if (geo.empty()) geo = tris;     // never register empty geometry
        std::vector<Tri> copy = geo;     // register_triangles takes Tri*
        // Per-triangle TriEx (materialId/tint/normals/AO) is only valid for the
        // undecimated level: `geo` is then the input triangle set in original order,
        // so triex[i] still describes copy[i]. Decimation reorders/merges triangles,
        // so those levels pass nullptr and fall back to the instance material.
        const TriEx* ex = (full && triex && triex->size() == copy.size())
                          ? triex->data() : nullptr;
        // register_triangles may deduplicate (returning an existing handle), so we
        // must NOT pre-record entries().size() as the index — it would be off-by-N
        // if prior identical geometry already occupies that slot. Look up the returned
        // handle's actual position in the entries array after registration instead.
        BLASHandle h = blas.register_triangles(copy.data(), (int)copy.size(), ex);
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
