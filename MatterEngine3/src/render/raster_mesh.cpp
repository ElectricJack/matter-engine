#include "raylib.h"
#include "raster_mesh.h"
#include <cstring>
#include <unordered_map>

namespace {

// Exact-bit weld key. The QEM unweld emits bit-identical floats for shared
// corners, so no epsilon: equal bits weld, anything else stays split.
struct VertexKey {
    float px, py, pz, nx, ny, nz;   // position, normal
    float tc0, tc1;                 // texcoords (mat_id, ao)
    float su, sv;                   // surface uv
    float ao;                       // baked_ao
    unsigned char rgba[4];
    uint32_t material_id;
};
static_assert(sizeof(VertexKey) == 52, "no padding: memcmp/hash over raw bytes");

struct VertexKeyHash {
    size_t operator()(const VertexKey& k) const {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&k);
        size_t h = 1469598103934665603ull;               // FNV-1a
        for (size_t i = 0; i < sizeof(VertexKey); ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }
};
struct VertexKeyEq {
    bool operator()(const VertexKey& a, const VertexKey& b) const {
        return std::memcmp(&a, &b, sizeof(VertexKey)) == 0;
    }
};

void append_vertex(viewer::RasterMeshData& d, const VertexKey& k) {
    d.vertices.insert(d.vertices.end(), {k.px, k.py, k.pz});
    d.normals.insert(d.normals.end(), {k.nx, k.ny, k.nz});
    d.colors.insert(d.colors.end(), {k.rgba[0], k.rgba[1], k.rgba[2], k.rgba[3]});
    d.texcoords.insert(d.texcoords.end(), {k.tc0, k.tc1});
    d.surface_uvs.insert(d.surface_uvs.end(), {k.su, k.sv});
    d.material_ids.push_back(k.material_id);
    d.baked_ao.push_back(k.ao);
}

} // namespace

namespace viewer {

static unsigned char to_u8(float v) {
    float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return (unsigned char)(c * 255.0f + 0.5f);
}

RasterMeshData build_raster_mesh_data(const Tri* tris, const TriEx* triex, int tri_count,
                                      float default_mat_id) {
    RasterMeshData d;
    const int est = tri_count * 2;  // rough unique-vertex estimate
    d.vertices.reserve(est * 3);    d.normals.reserve(est * 3);
    d.colors.reserve(est * 4);      d.texcoords.reserve(est * 2);
    d.surface_uvs.reserve(est * 2);
    d.material_ids.reserve(est);
    d.baked_ao.reserve(est);
    d.indices.reserve(tri_count * 3);

    std::unordered_map<VertexKey, uint32_t, VertexKeyHash, VertexKeyEq> weld;
    weld.reserve(tri_count * 2);

    for (int i = 0; i < tri_count; ++i) {
        const Tri& t = tris[i];
        const float3 vs[3] = { t.vertex0, t.vertex1, t.vertex2 };

        float3 ns[3];
        if (triex) { ns[0] = triex[i].N0; ns[1] = triex[i].N1; ns[2] = triex[i].N2; }
        else {
            float3 gn = normalize(cross(t.vertex1 - t.vertex0, t.vertex2 - t.vertex0));
            ns[0] = ns[1] = ns[2] = gn;
        }
        const float aos[3] = { triex ? triex[i].ao0 : 1.0f,
                               triex ? triex[i].ao1 : 1.0f,
                               triex ? triex[i].ao2 : 1.0f };
        const float mat_id = triex ? (float)triex[i].materialId : default_mat_id;
        const float2 uvs[3] = {triex ? triex[i].uv0 : make_float2(0.0f),
                               triex ? triex[i].uv1 : make_float2(0.0f),
                               triex ? triex[i].uv2 : make_float2(0.0f)};
        const uint32_t material_id = triex
                                         ? static_cast<uint32_t>(triex[i].materialId)
                                         : UINT32_MAX;
        unsigned char rgba[4] = { 255, 255, 255, 0 };            // neutral tint
        if (triex) {
            rgba[0] = to_u8(triex[i].tint.x); rgba[1] = to_u8(triex[i].tint.y);
            rgba[2] = to_u8(triex[i].tint.z); rgba[3] = to_u8(triex[i].tint.w);
        }
        for (int v = 0; v < 3; ++v) {
            VertexKey key{};
            key.px = vs[v].x; key.py = vs[v].y; key.pz = vs[v].z;
            key.nx = ns[v].x; key.ny = ns[v].y; key.nz = ns[v].z;
            key.tc0 = mat_id; key.tc1 = aos[v];
            key.su = uvs[v].x; key.sv = uvs[v].y;
            key.ao = aos[v];
            std::memcpy(key.rgba, rgba, 4);
            key.material_id = material_id;
            auto [it, inserted] = weld.try_emplace(key, (uint32_t)d.material_ids.size());
            if (inserted) append_vertex(d, key);
            d.indices.push_back(it->second);
        }
    }
    d.vertex_count = (int)d.material_ids.size();
    return d;
}

RasterMeshData expand_indexed(const RasterMeshData& in) {
    if (in.indices.empty()) return in;   // already soup
    RasterMeshData out;
    out.vertex_count = (int)in.indices.size();
    out.vertices.reserve(in.indices.size() * 3);
    for (uint32_t idx : in.indices) {
        const size_t p = (size_t)idx * 3, c = (size_t)idx * 4, uv = (size_t)idx * 2;
        if (p + 2 < in.vertices.size())
            out.vertices.insert(out.vertices.end(),
                                {in.vertices[p], in.vertices[p+1], in.vertices[p+2]});
        else
            out.vertices.insert(out.vertices.end(), {0.0f, 0.0f, 0.0f});
        if (p + 2 < in.normals.size())
            out.normals.insert(out.normals.end(),
                               {in.normals[p], in.normals[p+1], in.normals[p+2]});
        else
            out.normals.insert(out.normals.end(), {0.0f, 1.0f, 0.0f});
        if (c + 3 < in.colors.size())
            out.colors.insert(out.colors.end(),
                              {in.colors[c], in.colors[c+1], in.colors[c+2], in.colors[c+3]});
        else
            out.colors.insert(out.colors.end(), {255, 255, 255, 255});
        if (uv + 1 < in.texcoords.size())
            out.texcoords.insert(out.texcoords.end(), {in.texcoords[uv], in.texcoords[uv+1]});
        else
            out.texcoords.insert(out.texcoords.end(), {0.0f, 0.0f});
        if (!in.surface_uvs.empty() && uv + 1 < in.surface_uvs.size())
            out.surface_uvs.insert(out.surface_uvs.end(),
                                   {in.surface_uvs[uv], in.surface_uvs[uv+1]});
        if (!in.material_ids.empty() && idx < in.material_ids.size())
            out.material_ids.push_back(in.material_ids[idx]);
        if (!in.baked_ao.empty() && idx < in.baked_ao.size())
            out.baked_ao.push_back(in.baked_ao[idx]);
    }
    return out;
}

Matrix row_major_to_matrix(const float t[16]) {
    Matrix m;
    std::memcpy(&m, t, sizeof(Matrix));
    return m;
}

} // namespace viewer
