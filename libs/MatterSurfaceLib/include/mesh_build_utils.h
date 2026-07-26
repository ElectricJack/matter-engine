#ifndef MESH_BUILD_UTILS_H
#define MESH_BUILD_UTILS_H

#include "raylib.h"   // Mesh
#include "bvh.h"      // Tri, TriEx
#include <vector>

// Convert a raylib Mesh's triangles into BVH Tri structs. When out_triex is
// non-null, fills per-triangle TriEx with the mesh's per-vertex normals (or the
// face normal when the mesh has none). materialId/tint are left default and must
// be tagged by the caller.
std::vector<Tri> convert_mesh_to_triangles(const Mesh& mesh, std::vector<TriEx>* out_triex);

// Free a CPU-only mesh's heap arrays WITHOUT any GL call (safe on worker
// threads). Zeroes the Mesh afterward.
void unload_cpu_mesh(Mesh& m);

#endif // MESH_BUILD_UTILS_H
