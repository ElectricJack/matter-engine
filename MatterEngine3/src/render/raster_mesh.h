#ifndef VIEWER_RASTER_MESH_H
#define VIEWER_RASTER_MESH_H
#include "raylib.h"
#include "bvh.h"           // Tri, TriEx (MatterSurfaceLib)
#include <vector>

namespace viewer {

// CPU-side vertex arrays for one LOD level, raylib-Mesh channel layout.
// TriEx maps onto standard channels: normals <- N0/N1/N2, colors <- tint RGBA,
// texcoords <- (materialId, per-vertex AO). GL-free: upload happens in RasterComposer.
struct RasterMeshData {
    std::vector<float>         vertices;
    std::vector<float>         normals;
    std::vector<unsigned char> colors;
    std::vector<float>         texcoords;
    int vertex_count = 0;
};

RasterMeshData build_raster_mesh_data(const Tri* tris, const TriEx* triex, int tri_count,
                                      float default_mat_id = -1.0f);

// Row-major float[16] -> raylib Matrix. raylib's field declaration order
// (m0,m4,m8,m12,m1,...) IS row-major memory, so this is a straight copy.
Matrix row_major_to_matrix(const float t[16]);

} // namespace viewer
#endif
