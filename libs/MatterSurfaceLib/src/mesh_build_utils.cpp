// libs/MatterSurfaceLib/src/mesh_build_utils.cpp
//
// Small helpers shared by the meshing algorithms and by Cell's commit path.
// Two functions, both CPU-only and safe on worker threads:
//
// - convert_mesh_to_triangles: raylib Mesh -> BVH `Tri` array, the form
//   BLASManager::register_triangles and the ray tracer want. Optionally emits a
//   parallel `TriEx` array carrying the three per-vertex shading normals.
// - unload_cpu_mesh: free a mesh's heap arrays with NO GL call. Read its
//   comment before reaching for raylib's UnloadMesh anywhere off the main
//   thread.
//
// raylib note: `Mesh` here is a POD vertex-array container only. The renderer
// is Vulkan-only; raylib survives as headers for types like this one.
#include "mesh_build_utils.h"
#include "mesh_memory.h"
#include <cmath>
#include <cstdio>

// Handles both indexed and non-indexed meshes (`mesh.indices` null means
// vertices are consumed three at a time). Triangles with an out-of-range index,
// a non-finite vertex, or a non-finite centroid are SKIPPED with a warning, so
// the returned vector may be shorter than `mesh.triangleCount` -- the two
// outputs stay parallel with each other, but not with the source mesh's
// triangle numbering.
//
// `out_triex` may be null (positions only). When it is non-null but the mesh
// carries no normals, each triangle gets its face normal replicated to all
// three vertices (flat shading). materialId and tint are left default-
// initialised and must be tagged by the caller.
//
// Helper function to convert Raylib Mesh to triangles for BLAS registration
std::vector<Tri> convert_mesh_to_triangles(const Mesh& mesh, std::vector<TriEx>* out_triex) {
    std::vector<Tri> triangles;

    if (mesh.vertexCount == 0 || mesh.triangleCount == 0 || !mesh.vertices) {
        return triangles;
    }

    triangles.reserve(mesh.triangleCount);
    if (out_triex) {
        out_triex->clear();
        out_triex->reserve(mesh.triangleCount);
    }
    // Per-vertex smooth normals are only available when the mesh carries them.
    const bool have_normals = (out_triex != nullptr) && (mesh.normals != nullptr);

    for (int i = 0; i < mesh.triangleCount; i++) {
        Tri tri;

        // Get vertex indices (either from indices array or sequential)
        int idx0, idx1, idx2;
        if (mesh.indices) {
            idx0 = mesh.indices[i * 3 + 0];
            idx1 = mesh.indices[i * 3 + 1];
            idx2 = mesh.indices[i * 3 + 2];
        } else {
            idx0 = i * 3 + 0;
            idx1 = i * 3 + 1;
            idx2 = i * 3 + 2;
        }

        // Bounds check to prevent segmentation fault
        if (idx0 >= mesh.vertexCount || idx1 >= mesh.vertexCount || idx2 >= mesh.vertexCount) {
            printf("Warning: Vertex index out of bounds in mesh conversion (triangle %d, vertices %d %d %d, max %d)\n",
                   i, idx0, idx1, idx2, mesh.vertexCount);
            continue;
        }

        // Extract vertex positions
        float v0x = mesh.vertices[idx0 * 3 + 0];
        float v0y = mesh.vertices[idx0 * 3 + 1];
        float v0z = mesh.vertices[idx0 * 3 + 2];
        float v1x = mesh.vertices[idx1 * 3 + 0];
        float v1y = mesh.vertices[idx1 * 3 + 1];
        float v1z = mesh.vertices[idx1 * 3 + 2];
        float v2x = mesh.vertices[idx2 * 3 + 0];
        float v2y = mesh.vertices[idx2 * 3 + 1];
        float v2z = mesh.vertices[idx2 * 3 + 2];

        // Check for invalid floating point values (NaN or infinity)
        if (!isfinite(v0x) || !isfinite(v0y) || !isfinite(v0z) ||
            !isfinite(v1x) || !isfinite(v1y) || !isfinite(v1z) ||
            !isfinite(v2x) || !isfinite(v2y) || !isfinite(v2z)) {
            printf("Warning: Triangle %d has invalid vertex coordinates, skipping\n", i);
            continue;
        }

        tri.vertex0 = make_float3(v0x, v0y, v0z);
        tri.vertex1 = make_float3(v1x, v1y, v1z);
        tri.vertex2 = make_float3(v2x, v2y, v2z);

        // Calculate centroid
        float cx = (tri.vertex0.x + tri.vertex1.x + tri.vertex2.x) / 3.0f;
        float cy = (tri.vertex0.y + tri.vertex1.y + tri.vertex2.y) / 3.0f;
        float cz = (tri.vertex0.z + tri.vertex1.z + tri.vertex2.z) / 3.0f;

        // Validate centroid
        if (!isfinite(cx) || !isfinite(cy) || !isfinite(cz)) {
            printf("Warning: Triangle %d has invalid centroid, skipping\n", i);
            continue;
        }

        tri.centroid = make_float3(cx, cy, cz);

        triangles.push_back(tri);

        if (out_triex) {
            TriEx ex{};
            if (have_normals) {
                ex.N0 = make_float3(mesh.normals[idx0 * 3 + 0], mesh.normals[idx0 * 3 + 1], mesh.normals[idx0 * 3 + 2]);
                ex.N1 = make_float3(mesh.normals[idx1 * 3 + 0], mesh.normals[idx1 * 3 + 1], mesh.normals[idx1 * 3 + 2]);
                ex.N2 = make_float3(mesh.normals[idx2 * 3 + 0], mesh.normals[idx2 * 3 + 1], mesh.normals[idx2 * 3 + 2]);
            } else {
                // Fall back to the face normal so all three vertices share it (flat shading).
                float3 fn = normalize(cross(tri.vertex1 - tri.vertex0, tri.vertex2 - tri.vertex0));
                ex.N0 = ex.N1 = ex.N2 = fn;
            }
            out_triex->push_back(ex);
        }
    }

    return triangles;
}

// Free a CPU-only mesh's heap arrays WITHOUT any GL call. raylib's UnloadMesh
// unconditionally calls rlUnloadVertexArray (glBindVertexArray/glDeleteVertexArrays
// once GL is initialized), which is illegal off the GL-context (main) thread and
// crashes. Meshes produced by the mesher/simplifier are never uploaded here
// (vaoId==0, vboId==NULL), so only their CPU buffers need freeing. Use this for
// any mesh discarded inside build_group_mesh (which runs on worker threads);
// uploaded meshes are still torn down with UnloadMesh on the main thread.
void unload_cpu_mesh(Mesh& m) {
    matter_surface::detail::mesh_free(m.vboId);
    matter_surface::detail::mesh_free(m.vertices);
    matter_surface::detail::mesh_free(m.texcoords);
    matter_surface::detail::mesh_free(m.normals);
    matter_surface::detail::mesh_free(m.colors);
    matter_surface::detail::mesh_free(m.tangents);
    matter_surface::detail::mesh_free(m.texcoords2);
    matter_surface::detail::mesh_free(m.indices);
    matter_surface::detail::mesh_free(m.animVertices);
    matter_surface::detail::mesh_free(m.animNormals);
    matter_surface::detail::mesh_free(m.boneWeights);
    matter_surface::detail::mesh_free(m.boneIds);
    matter_surface::detail::mesh_free(m.boneMatrices);
    m = Mesh{};
}
