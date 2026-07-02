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

std::vector<Tri> decimate_to_error(const std::vector<Tri>& tris, float epsilon) {
    if (tris.empty()) return {};
    Mesh in = tris_to_mesh(tris);
    SimplifyOptions opts;
    // target_ratio 0 -> targetTri clamps to 1, so the collapse loop runs until
    // the min heap cost exceeds max_error (the error bound is the ONLY stop).
    // Caveat: on OPEN meshes the outline erodes for free (boundary collapses
    // are coplanar with the quadric planes, cost ~0), possibly down to nothing;
    // the empty-output fallback below then returns the input unchanged.
    opts.target_ratio  = 0.0f;
    opts.max_error     = epsilon * epsilon;  // QEM cost is squared distance
    opts.lock_boundary = false;
    Mesh out = simplify_mesh(in, opts, nullptr);
    std::vector<Tri> result = (out.vertexCount > 0) ? mesh_to_tris(out) : tris;
    if (in.vertices) MemFree(in.vertices);
    if (out.vertices) MemFree(out.vertices);
    if (out.indices) MemFree(out.indices);
    return result;
}

std::vector<TriEx> reproject_triex(const std::vector<Tri>& out_tris,
                                   const std::vector<Tri>& src_tris,
                                   const std::vector<TriEx>& src_triex) {
    if (src_tris.empty() || src_triex.size() != src_tris.size()) return {};

    // Uniform spatial hash over source centroids. Cell size from the source
    // AABB so an average cell holds a handful of centroids.
    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
    for (const auto& t : src_tris) {
        mn[0]=fminf(mn[0],t.centroid.x); mx[0]=fmaxf(mx[0],t.centroid.x);
        mn[1]=fminf(mn[1],t.centroid.y); mx[1]=fmaxf(mx[1],t.centroid.y);
        mn[2]=fminf(mn[2],t.centroid.z); mx[2]=fmaxf(mx[2],t.centroid.z);
    }
    float ext = fmaxf(fmaxf(mx[0]-mn[0], mx[1]-mn[1]), fmaxf(mx[2]-mn[2], 1e-6f));
    float cell = ext / fmaxf(1.0f, cbrtf((float)src_tris.size()));

    auto key = [&](float x, float y, float z) {
        int ix = (int)floorf((x - mn[0]) / cell);
        int iy = (int)floorf((y - mn[1]) / cell);
        int iz = (int)floorf((z - mn[2]) / cell);
        return ((uint64_t)(uint32_t)ix * 73856093ull) ^
               ((uint64_t)(uint32_t)iy * 19349663ull) ^
               ((uint64_t)(uint32_t)iz * 83492791ull);
    };
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    grid.reserve(src_tris.size());
    for (size_t i = 0; i < src_tris.size(); ++i)
        grid[key(src_tris[i].centroid.x, src_tris[i].centroid.y, src_tris[i].centroid.z)]
            .push_back((uint32_t)i);

    auto nearest_src = [&](const float3& c) -> uint32_t {
        float best_d2 = 1e30f;
        uint32_t best = 0;
        // Search the containing cell ring, growing until a hit is found. The
        // decimated surface stays near the source surface, so ring 0/1 almost
        // always suffices; the growth loop is a safety net for sparse regions.
        int cx = (int)floorf((c.x - mn[0]) / cell);
        int cy = (int)floorf((c.y - mn[1]) / cell);
        int cz = (int)floorf((c.z - mn[2]) / cell);
        // Scan outward shell by shell; after the first occupied shell, scan one
        // more (a neighbor cell can hold a closer centroid than the first cell
        // that happened to be occupied), then stop.
        int hit_ring = -1;
        for (int ring = 0; ring < 64; ++ring) {
            if (hit_ring >= 0 && ring > hit_ring + 1) break;
            bool any = false;
            for (int dz = -ring; dz <= ring; ++dz)
            for (int dy = -ring; dy <= ring; ++dy)
            for (int dx = -ring; dx <= ring; ++dx) {
                // Only the shell of this ring (inner rings already scanned).
                if (ring > 0 && abs(dx) != ring && abs(dy) != ring && abs(dz) != ring)
                    continue;
                uint64_t k = ((uint64_t)(uint32_t)(cx+dx) * 73856093ull) ^
                             ((uint64_t)(uint32_t)(cy+dy) * 19349663ull) ^
                             ((uint64_t)(uint32_t)(cz+dz) * 83492791ull);
                auto it = grid.find(k);
                if (it == grid.end()) continue;
                any = true;
                for (uint32_t i : it->second) {
                    const float3& s = src_tris[i].centroid;
                    float ddx=s.x-c.x, ddy=s.y-c.y, ddz=s.z-c.z;
                    float d2 = ddx*ddx + ddy*ddy + ddz*ddz;
                    if (d2 < best_d2) { best_d2 = d2; best = i; }
                }
            }
            if (any && hit_ring < 0) hit_ring = ring;
        }
        return best;
    };

    std::vector<TriEx> out;
    out.reserve(out_tris.size());
    for (const auto& t : out_tris) {
        const TriEx& src = src_triex[nearest_src(t.centroid)];
        TriEx ex = src;                       // materialId, tint, uv, AO
        // Geometric normal of the NEW triangle: decimation changed the surface,
        // so the source shading normals no longer describe it.
        float3 e1 = make_float3(t.vertex1.x-t.vertex0.x, t.vertex1.y-t.vertex0.y, t.vertex1.z-t.vertex0.z);
        float3 e2 = make_float3(t.vertex2.x-t.vertex0.x, t.vertex2.y-t.vertex0.y, t.vertex2.z-t.vertex0.z);
        float3 n = make_float3(e1.y*e2.z - e1.z*e2.y,
                               e1.z*e2.x - e1.x*e2.z,
                               e1.x*e2.y - e1.y*e2.x);
        float len = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
        if (len > 1e-12f) { n.x/=len; n.y/=len; n.z/=len; }
        else n = make_float3(0,1,0);
        ex.N0 = n; ex.N1 = n; ex.N2 = n;
        out.push_back(ex);
    }
    return out;
}

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
