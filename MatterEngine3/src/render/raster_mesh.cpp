#include "raster_mesh.h"
#include <cstring>

namespace viewer {

static unsigned char to_u8(float v) {
    float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return (unsigned char)(c * 255.0f + 0.5f);
}

RasterMeshData build_raster_mesh_data(const Tri* tris, const TriEx* triex, int tri_count,
                                      float default_mat_id) {
    RasterMeshData d;
    d.vertex_count = tri_count * 3;
    d.vertices.reserve(d.vertex_count * 3);  d.normals.reserve(d.vertex_count * 3);
    d.colors.reserve(d.vertex_count * 4);    d.texcoords.reserve(d.vertex_count * 2);

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
        unsigned char rgba[4] = { 255, 255, 255, 0 };            // neutral tint
        if (triex) {
            rgba[0] = to_u8(triex[i].tint.x); rgba[1] = to_u8(triex[i].tint.y);
            rgba[2] = to_u8(triex[i].tint.z); rgba[3] = to_u8(triex[i].tint.w);
        }
        for (int v = 0; v < 3; ++v) {
            d.vertices.push_back(vs[v].x); d.vertices.push_back(vs[v].y); d.vertices.push_back(vs[v].z);
            d.normals.push_back(ns[v].x);  d.normals.push_back(ns[v].y);  d.normals.push_back(ns[v].z);
            d.colors.push_back(rgba[0]); d.colors.push_back(rgba[1]);
            d.colors.push_back(rgba[2]); d.colors.push_back(rgba[3]);
            d.texcoords.push_back(mat_id); d.texcoords.push_back(aos[v]);
        }
    }
    return d;
}

Matrix row_major_to_matrix(const float t[16]) {
    Matrix m;
    std::memcpy(&m, t, sizeof(Matrix));
    return m;
}

} // namespace viewer
