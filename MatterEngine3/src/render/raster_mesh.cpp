#include "raylib.h"
#include "raster_mesh.h"

#include <cstring>

namespace viewer {

RasterMeshData build_raster_mesh_data(const Tri* tris, const TriEx* triex, int tri_count,
                                      float default_mat_id) {
    return build_indexed_part_geometry(tris, triex, tri_count, default_mat_id);
}

RasterMeshData expand_indexed(const RasterMeshData& in) {
    if (in.indices.empty()) return in;
    RasterMeshData out;
    out.vertex_count = static_cast<int>(in.indices.size());
    out.vertices.reserve(in.indices.size() * 3);
    for (uint32_t idx : in.indices) {
        const size_t p = static_cast<size_t>(idx) * 3, c = static_cast<size_t>(idx) * 4,
                     uv = static_cast<size_t>(idx) * 2;
        if (p + 2 < in.vertices.size()) out.vertices.insert(out.vertices.end(), {in.vertices[p], in.vertices[p+1], in.vertices[p+2]});
        else out.vertices.insert(out.vertices.end(), {0.0f, 0.0f, 0.0f});
        if (p + 2 < in.normals.size()) out.normals.insert(out.normals.end(), {in.normals[p], in.normals[p+1], in.normals[p+2]});
        else out.normals.insert(out.normals.end(), {0.0f, 1.0f, 0.0f});
        if (c + 3 < in.colors.size()) out.colors.insert(out.colors.end(), {in.colors[c], in.colors[c+1], in.colors[c+2], in.colors[c+3]});
        else out.colors.insert(out.colors.end(), {255, 255, 255, 255});
        if (uv + 1 < in.texcoords.size()) out.texcoords.insert(out.texcoords.end(), {in.texcoords[uv], in.texcoords[uv+1]});
        else out.texcoords.insert(out.texcoords.end(), {0.0f, 0.0f});
        if (!in.surface_uvs.empty() && uv + 1 < in.surface_uvs.size()) out.surface_uvs.insert(out.surface_uvs.end(), {in.surface_uvs[uv], in.surface_uvs[uv+1]});
        if (!in.material_ids.empty() && idx < in.material_ids.size()) out.material_ids.push_back(in.material_ids[idx]);
        if (!in.baked_ao.empty() && idx < in.baked_ao.size()) out.baked_ao.push_back(in.baked_ao[idx]);
    }
    return out;
}

Matrix row_major_to_matrix(const float t[16]) { Matrix m; std::memcpy(&m, t, sizeof(Matrix)); return m; }

} // namespace viewer
