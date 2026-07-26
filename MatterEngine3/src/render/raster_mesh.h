#ifndef VIEWER_RASTER_MESH_H
#define VIEWER_RASTER_MESH_H
#include "indexed_part_geometry.h"   // IndexedPartGeometry, Tri, TriEx
#include <cstdint>
#include <vector>

namespace viewer {

// CPU-side vertex arrays for one LOD level, raylib-Mesh channel layout.
// TriEx maps onto standard channels: normals <- N0/N1/N2, colors <- tint RGBA,
// texcoords <- (materialId, per-vertex AO). GL-free: upload happens in RasterComposer.
using RasterMeshData = IndexedPartGeometry;

RasterMeshData build_raster_mesh_data(const Tri* tris, const TriEx* triex, int tri_count,
                                      float default_mat_id = -1.0f);
// Unweld an indexed mesh back to soup (3 corners per triangle, indices empty).
// Legacy shim for the GL path; new code should consume indices directly.
RasterMeshData expand_indexed(const RasterMeshData& indexed);

} // namespace viewer
#endif
