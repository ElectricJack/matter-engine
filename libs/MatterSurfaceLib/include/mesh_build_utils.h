#ifndef MESH_BUILD_UTILS_H
#define MESH_BUILD_UTILS_H

// libs/MatterSurfaceLib/include/mesh_build_utils.h
//
// Small CPU-side helpers that bridge raylib's `Mesh` — the mesher's native
// output format — and the BVH triangle types `Tri`/`TriEx` that live in
// SpatialQueryLib (`bvh.h`/`tri.h`). Neither function makes a GL call, which
// is the whole point: mesh building runs on `MeshWorkerPool` threads that
// have no GL context.
//
// Where it sits: MatterSurfaceLib, below MatterEngine3. Used by
// `src/cell.cpp` and `src/marching_cubes_algorithm.cpp` when a merge group's
// mesh is turned into BLAS-ready triangles, and by any worker-side path that
// has to discard a mesh it just built.
//
// Gotchas:
//   - `unload_cpu_mesh` exists because raylib's `UnloadMesh` unconditionally
//     issues GL calls (rlUnloadVertexArray) and crashes off the GL thread.
//     Use it only for meshes that were never uploaded (`vaoId == 0`); an
//     uploaded mesh must still go through `UnloadMesh` on the main thread.
//   - `convert_mesh_to_triangles` skips (rather than fails on) triangles
//     whose vertex indices fall outside the mesh, so the returned vector can
//     be shorter than `mesh.triangleCount`.
//   - It handles both indexed and non-indexed meshes: with `mesh.indices`
//     null it reads vertices sequentially, three per triangle.
//   - The renderer is Vulkan-only now; `raylib.h` survives here purely as the
//     POD declaration of `Mesh`.

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
