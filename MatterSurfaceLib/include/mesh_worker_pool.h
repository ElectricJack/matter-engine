#ifndef MESH_WORKER_POOL_H
#define MESH_WORKER_POOL_H

#include "raylib.h"          // Mesh
#include "bvh.h"             // Tri, TriEx
#include <vector>
#include <cstdint>

// Forward declarations
struct Cell;
struct SurfaceScratch;

// CPU-only mesh build output for one merge group. Holds the raylib CPU Mesh
// (vertex/normal/index arrays, pre-UploadMesh) plus the BLAS-ready triangle
// arrays with per-triangle material/tint already resolved. Detached from any
// GL/BLAS state, so it can be produced on a worker thread and committed later
// on the main thread. The Mesh pointers are owned downstream by the Cell once
// committed; GroupMeshResult never frees them.
struct GroupMeshResult {
    uint32_t group_id = 0;
    Mesh mesh = {};                          // vertexCount == 0 => "no mesh, skip"
    std::vector<Tri> triangles;
    std::vector<TriEx> triangle_normals;     // materialId/tint filled during build
};

// CPU-only mesh build output for all merge groups in one cell.
struct CellMeshResult {
    std::vector<GroupMeshResult> groups;
};

#endif // MESH_WORKER_POOL_H
